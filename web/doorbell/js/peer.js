/* PeerConnection + 控制 DataChannel 的生命周期。
 *
 * 职责边界：本模块只管"连接通不通、控制通道开不开、字符串怎么收发"，不认识 open_door
 * 协议，也不碰任何 DOM。开门协议在 door.js，界面在 app.js。
 *
 * 角色约定：**设备是 Offerer**，页面只应答。所以控制 DataChannel 由设备创建，页面靠
 * ondatachannel 接收 —— 页面不主动 createDataChannel，否则会多出一条谁都不读的通道。
 *
 * ready 的定义是"PeerConnection 已 connected **且** DataChannel 处于 open"。开门按钮
 * 的可用性直接绑在它上面：只要有一条不满足，命令根本发不出去，按钮就不该是可点的。
 */
import { log, logError } from './log.js';

export class DoorbellPeer {
    /**
     * @param {object} opts
     * @param {Array}    opts.iceServers      信令下发的 ICE 服务器（含时效凭证，不留存）
     * @param {MediaStream} opts.localStream  本地麦克风流
     * @param {(state:string)=>void} opts.onState        'connecting'|'connected'|'closed'
     * @param {(stream:MediaStream)=>void} opts.onRemoteStream
     * @param {(msg:object)=>void} opts.onSignal         需要发给对端的 answer/candidate
     * @param {(open:boolean)=>void} opts.onControlState 控制通道开/关
     * @param {(text:string)=>void} opts.onControlMessage 控制通道收到的原始字符串
     */
    constructor(opts) {
        this.opts_ = opts;
        this.pc_ = null;
        this.channel_ = null;
        this.pendingCandidates_ = [];
        this.closed_ = false;
    }

    get ready() {
        return this.pc_ !== null && this.pc_.connectionState === 'connected' &&
               this.channel_ !== null && this.channel_.readyState === 'open';
    }

    /** 收到设备的 offer：建连接、挂本地音轨、回 answer。 */
    async acceptOffer(sdp) {
        if (this.pc_ !== null) {
            // 设备重发 offer（比如它那边重启了一次呼叫）时，旧连接必须先拆掉：
            // 在已有 remote description 的连接上再 setRemoteDescription('offer') 会抛。
            log('收到新的 offer，重建连接');
            this.closePeer_();
        }
        this.closed_ = false;
        const pc = this.createPeerConnection_();

        for (const track of this.opts_.localStream.getTracks()) {
            pc.addTrack(track, this.opts_.localStream);
        }

        await pc.setRemoteDescription({ type: 'offer', sdp });
        // remote description 之前到的 candidate 只能先排队，这时候才补进去。
        const queued = this.pendingCandidates_;
        this.pendingCandidates_ = [];
        for (const cand of queued) {
            await this.addIce_(pc, cand);
        }

        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        // 只报"已应答"，不打印 answer.sdp：SDP 里含 ICE 候选、指纹和内网地址。
        log('已应答设备的 offer');
        this.opts_.onSignal({ type: 'answer', sdp: answer.sdp });
    }

    /** 收到对端 candidate。remote description 还没设好就先排队。 */
    async addRemoteCandidate(msg) {
        const cand = {
            candidate: msg.candidate,
            sdpMid: msg.sdpMid !== undefined ? msg.sdpMid : '0',
            sdpMLineIndex: msg.sdpMLineIndex !== undefined ? msg.sdpMLineIndex : 0,
        };
        if (this.pc_ === null || this.pc_.remoteDescription === null) {
            this.pendingCandidates_.push(cand);
            return;
        }
        await this.addIce_(this.pc_, cand);
    }

    /**
     * 从控制通道发一条字符串。
     * @returns {boolean} 是否确实交给了通道。false 表示没连通 —— 调用方**不要**当成
     *          "可能发出去了"来处理，尤其不要因此重试开门。
     */
    sendControl(text) {
        if (!this.ready) {
            return false;
        }
        try {
            this.channel_.send(text);
            return true;
        } catch (err) {
            logError('控制通道发送', err);
            return false;
        }
    }

    /** 拆掉连接并停掉本地音轨之外的一切。幂等。 */
    close() {
        this.closed_ = true;
        this.closePeer_();
    }

    // ---- 内部实现 --------------------------------------------------------

    createPeerConnection_() {
        const pc = new RTCPeerConnection({
            iceServers: this.opts_.iceServers,
            bundlePolicy: 'max-bundle',
            rtcpMuxPolicy: 'require',
        });
        this.pc_ = pc;

        pc.onicecandidate = (ev) => {
            if (ev.candidate) {
                this.opts_.onSignal({
                    type: 'candidate',
                    candidate: ev.candidate.candidate,
                    sdpMid: ev.candidate.sdpMid,
                    sdpMLineIndex: ev.candidate.sdpMLineIndex,
                });
            }
        };

        pc.ontrack = (ev) => {
            if (ev.streams && ev.streams[0]) {
                this.opts_.onRemoteStream(ev.streams[0]);
            }
        };

        // 控制通道由设备（Offerer / SCTP 客户端）创建，这里只接。
        pc.ondatachannel = (ev) => this.attachChannel_(ev.channel);

        pc.onconnectionstatechange = () => {
            if (this.pc_ !== pc) {
                return;   // 已经被新连接替换掉的旧对象，忽略它的迟到事件
            }
            const s = pc.connectionState;
            if (s === 'connected') {
                log('媒体连接已建立');
                this.opts_.onState('connected');
            } else if (s === 'disconnected' || s === 'failed' || s === 'closed') {
                log(`媒体连接已断开（${s}）`);
                this.closePeer_();
                this.opts_.onState('closed');
            }
        };

        this.opts_.onState('connecting');
        return pc;
    }

    attachChannel_(channel) {
        this.channel_ = channel;
        // 控制消息是短 JSON，用文本模式收，省掉一层解码。
        channel.binaryType = 'arraybuffer';
        channel.onopen = () => {
            log('控制通道已打开');
            this.opts_.onControlState(true);
        };
        channel.onclose = () => {
            log('控制通道已关闭');
            if (this.channel_ === channel) {
                this.channel_ = null;
            }
            this.opts_.onControlState(false);
        };
        channel.onerror = () => {
            // error 事件不带可用信息，且后面一定跟 close，那里已经会通知上层。
            log('控制通道出错');
        };
        channel.onmessage = (ev) => {
            if (typeof ev.data !== 'string') {
                log('丢弃一条非文本控制消息');
                return;
            }
            this.opts_.onControlMessage(ev.data);
        };
        // ondatachannel 之后通道可能已经是 open 了（事件在 open 之前触发不保证），
        // 补一次判断，否则会漏掉这次开门可用的时机。
        if (channel.readyState === 'open') {
            this.opts_.onControlState(true);
        }
    }

    async addIce_(pc, cand) {
        try {
            await pc.addIceCandidate(cand);
        } catch (err) {
            // candidate 加失败通常不致命（ICE 还有别的候选），只记类型不记内容。
            logError('添加 ICE 候选', err);
        }
    }

    closePeer_() {
        this.pendingCandidates_ = [];
        if (this.channel_ !== null) {
            const ch = this.channel_;
            this.channel_ = null;
            ch.onopen = ch.onclose = ch.onerror = ch.onmessage = null;
            try { ch.close(); } catch (_) { /* 已经关了 */ }
            this.opts_.onControlState(false);
        }
        if (this.pc_ !== null) {
            const pc = this.pc_;
            this.pc_ = null;
            pc.onicecandidate = pc.ontrack = pc.ondatachannel = null;
            pc.onconnectionstatechange = null;
            try { pc.close(); } catch (_) { /* 已经关了 */ }
        }
    }
}
