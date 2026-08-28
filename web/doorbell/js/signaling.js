/* AppRTC（collider）信令客户端。
 *
 * 职责边界：本模块只负责"把消息送到对端 / 把对端消息交出来"，完全不认识 SDP、
 * DataChannel 或开门协议。它对外只暴露四个动作（join / sendMessage / leave / close）
 * 和两个回调（onMessage / onClose），所以换信令服务时改动只落在这一个文件里。
 *
 * 这里**故意没有**"通过信令发送自定义数据"的接口。AppRTC 房间是明文转发的，谁猜到
 * 房间号就能往里发东西；开门命令只允许走 DTLS/SCTP 的 DataChannel。把信令侧的发送
 * 接口彻底不写出来，比写出来再要求"别用它开门"可靠得多 —— 不存在的接口不会被误用。
 *
 * 协议要点（与 esp-webrtc 的 apprtc_signal 实现对齐）：
 *   加入   POST   {base}/join/{room}        -> params{client_id,is_initiator,wss_url,
 *                                                     wss_post_url,room_id,ice_server_url,messages}
 *   凭证   POST   {ice_server_url}          -> {iceServers:[{urls,username,credential}]}
 *   长连   WSS    {wss_url}，首帧 {"cmd":"register","roomid":..,"clientid":..}
 *   发送   POST   {base}/message/{room}/{client}   由服务端转发给房间里的另一端
 *   离开   POST   {base}/leave/{room}/{client} + DELETE {wss_post_url}/{room}/{client}
 *
 * is_initiator 是**字符串** "true"/"false"，不是布尔 —— 直接 if(params.is_initiator)
 * 会把 "false" 当真。
 */
import { log, logError } from './log.js';

export class AppRtcSignaling {
    /** @param {string} baseUrl 信令基地址，不含 /join，允许带尾斜杠 */
    constructor(baseUrl) {
        this.base_ = baseUrl.replace(/\/+$/, '');
        this.ws_ = null;
        this.roomId_ = null;
        this.clientId_ = null;
        this.wssPostUrl_ = null;
        this.onMessage_ = () => {};
        this.onClose_ = () => {};
        this.closed_ = false;
        // join() 返回时调用方还没建好 PeerConnection（它需要 join 下发的 ICE 服务器），
        // 而设备的 offer 往往**早于**页面加入房间，会由服务端在 join 应答里回放。所以
        // 消息先在这里排队，等调用方显式 flushPendingMessages() 再按序投出去 ——
        // 否则那条 offer 会正好落在"还没有 peer"的窗口里被丢掉，这一路呼叫就此卡死。
        this.queue_ = [];
        this.flushed_ = false;
    }

    /** @param {(msg:object)=>void} cb 收到一条对端消息（已解出内层 JSON） */
    onMessage(cb) { this.onMessage_ = cb; }
    /** @param {()=>void} cb 信令连接断开（不区分正常/异常，调用方一律按掉线处理） */
    onClose(cb) { this.onClose_ = cb; }

    get joined() { return this.clientId_ !== null; }

    /**
     * 加入房间并建立 WSS 长连。
     * @param {string} room 房间号
     * @returns {Promise<{isInitiator: boolean, iceServers: Array}>}
     *          isInitiator 为 true 表示**房间原本是空的**（设备还没进来）。
     */
    async join(room) {
        const resp = await fetch(`${this.base_}/join/${encodeURIComponent(room)}`,
                                 { method: 'POST' });
        if (!resp.ok) {
            throw new Error(`join http ${resp.status}`);
        }
        const body = await resp.json();
        // 服务端用 result 字段表达业务失败（房间满等），HTTP 仍然是 200。
        if (body.result !== undefined && body.result !== 'SUCCESS') {
            throw new Error(`join rejected: ${body.result}`);
        }
        const p = body.params;
        if (!p || !p.client_id || !p.wss_url) {
            throw new Error('join response incomplete');
        }

        this.roomId_ = p.room_id || room;
        this.clientId_ = p.client_id;
        this.wssPostUrl_ = p.wss_post_url || null;
        const isInitiator = String(p.is_initiator) === 'true';
        log(`已加入信令房间（${isInitiator ? '房间原为空' : '房间内已有设备'}）`);

        const iceServers = await this.fetchIceServers_(p.ice_server_url);
        await this.openWebSocket_(p.wss_url);

        // join 之前对端就发出来的消息（典型情况：设备的 offer 先到了）由服务端在
        // params.messages 里一并回放，必须按顺序补投，否则这一路呼叫就永远等不到 offer。
        const pending = Array.isArray(p.messages) ? p.messages : [];
        if (pending.length > 0) {
            log(`已取回 join 之前的 ${pending.length} 条信令消息`);
        }
        for (const raw of pending) {
            this.dispatchRaw_(raw);   // 此时还没 flush，会进队列
        }
        return { isInitiator, iceServers };
    }

    /** 开始投递消息，并按序放出 join 期间排队的那些。调用方建好 peer 之后调一次。 */
    flushPendingMessages() {
        this.flushed_ = true;
        const queued = this.queue_;
        this.queue_ = [];
        for (const msg of queued) {
            this.onMessage_(msg);
        }
    }

    /**
     * 发一条信令消息给房间里的另一端（offer/answer/candidate/bye）。
     * 服务端负责转发，所以 URL 里带的是**自己**的 client id。
     * @param {object} message
     */
    async sendMessage(message) {
        if (!this.joined) {
            return;
        }
        const url = `${this.base_}/message/${encodeURIComponent(this.roomId_)}` +
                    `/${encodeURIComponent(this.clientId_)}`;
        try {
            await fetch(url, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(message),
            });
        } catch (err) {
            // 只报类型：err.message 里会带完整 URL（含房间号和 client_id）。
            logError(`发送 ${message.type} 信令`, err);
        }
    }

    /** 主动离开房间。幂等，可以在任何状态下调用。 */
    async leave() {
        if (!this.joined) {
            this.close();
            return;
        }
        const room = encodeURIComponent(this.roomId_);
        const client = encodeURIComponent(this.clientId_);
        // 先把标记清掉再发请求：离开过程中若有回调进来，不该再被当成"还在房间里"。
        const wssPostUrl = this.wssPostUrl_;
        this.roomId_ = null;
        this.clientId_ = null;
        this.close();
        try {
            await fetch(`${this.base_}/leave/${room}/${client}`, { method: 'POST' });
            if (wssPostUrl) {
                await fetch(`${wssPostUrl.replace(/\/+$/, '')}/${room}/${client}`,
                            { method: 'DELETE' });
            }
            log('已离开信令房间');
        } catch (err) {
            // 离开失败无所谓：服务端有自己的房间超时清理，这里不值得让界面报错。
            logError('离开房间', err);
        }
    }

    /** 只关长连，不通知服务端。leave() 会调它；掉线时也会走到这。 */
    close() {
        this.closed_ = true;
        this.queue_ = [];
        if (this.ws_ !== null) {
            const ws = this.ws_;
            this.ws_ = null;
            ws.onopen = ws.onmessage = ws.onerror = ws.onclose = null;
            try { ws.close(); } catch (_) { /* 已经关了就算了 */ }
        }
    }

    // ---- 内部实现 --------------------------------------------------------

    /* TURN/STUN 凭证由信令服务按房间下发，是有时效的，不落盘、不打印、不进日志：
     * 拿到就直接交给 RTCPeerConnection。取不到就退回公共 STUN —— 同一个 NAT 下
     * （家里同一个路由器）通常仍然能连通，只是跨 NAT 会失败，比整个页面用不了好。 */
    async fetchIceServers_(iceServerUrl) {
        const fallback = [{ urls: ['stun:stun.l.google.com:19302'] }];
        if (!iceServerUrl) {
            log('信令未下发 ICE 服务器，改用公共 STUN');
            return fallback;
        }
        try {
            const resp = await fetch(iceServerUrl, { method: 'POST' });
            if (!resp.ok) {
                throw new Error(`ice http ${resp.status}`);
            }
            const body = await resp.json();
            const servers = Array.isArray(body.iceServers) ? body.iceServers : [];
            if (servers.length === 0) {
                throw new Error('ice list empty');
            }
            log(`已获取 ${servers.length} 组 ICE 服务器凭证`);   // 只报数量
            return servers;
        } catch (err) {
            logError('获取 ICE 服务器凭证', err);
            log('改用公共 STUN，跨网络可能连不通');
            return fallback;
        }
    }

    openWebSocket_(wssUrl) {
        return new Promise((resolve, reject) => {
            let settled = false;
            const ws = new WebSocket(wssUrl);
            this.ws_ = ws;
            this.closed_ = false;

            ws.onopen = () => {
                // 首帧必须是 register，服务端据此把这条连接绑到房间里的这个 client。
                ws.send(JSON.stringify({
                    cmd: 'register',
                    roomid: this.roomId_,
                    clientid: this.clientId_,
                }));
                settled = true;
                log('信令长连已建立');
                resolve();
            };
            ws.onmessage = (ev) => this.onWsText_(ev.data);
            ws.onerror = () => {
                // WebSocket 的 error 事件不带可用信息，且紧跟着一定有 close，
                // 所以这里只在"还没连上"时用来失败掉 Promise。
                if (!settled) {
                    settled = true;
                    reject(new Error('signaling websocket failed'));
                }
            };
            ws.onclose = () => {
                if (!settled) {
                    settled = true;
                    reject(new Error('signaling websocket closed'));
                    return;
                }
                if (this.ws_ === ws && !this.closed_) {
                    this.ws_ = null;
                    log('信令长连已断开');
                    this.onClose_();
                }
            };
        });
    }

    onWsText_(text) {
        if (typeof text !== 'string') {
            return;
        }
        let outer;
        try {
            outer = JSON.parse(text);
        } catch (_) {
            log('丢弃一条无法解析的信令帧');   // 不打印内容
            return;
        }
        if (outer && typeof outer.error === 'string' && outer.error !== '') {
            log('信令服务返回错误');
            return;
        }
        // 服务端把对端消息塞在 msg 字段里，值是**一个 JSON 字符串**（嵌套编码）；
        // 有些实现直接把消息对象平铺在外层，两种都收。
        this.dispatchRaw_(outer && outer.msg !== undefined ? outer.msg : outer);
    }

    dispatchRaw_(raw) {
        let msg = raw;
        if (typeof msg === 'string') {
            if (msg === '') {
                return;
            }
            try {
                msg = JSON.parse(msg);
            } catch (_) {
                log('丢弃一条无法解析的信令消息');
                return;
            }
        }
        if (msg === null || typeof msg !== 'object' || typeof msg.type !== 'string') {
            return;
        }
        // flush 之前一律排队：WS 上的实时消息也要排，否则它会插到回放消息前面，
        // 出现"candidate 早于 offer"这种顺序，白白丢掉一批候选。
        if (!this.flushed_) {
            this.queue_.push(msg);
            return;
        }
        this.onMessage_(msg);
    }
}
