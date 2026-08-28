/* 开门协议客户端（DataChannel 之上的一问一答）。
 *
 * 请求 {"type":"open_door","request_id":"<id>"}
 * 应答 {"type":"door_result","request_id":"<id>","status":"opened|busy|denied|error"}
 *
 * 三条不肯让步的规则：
 *  1. **绝不自动重发**。一次请求对应门上一次物理动作，超时只说"结果未知"，重发意味着
 *     门可能被开第二次。要不要再试一次，只能由人看着门再决定。
 *  2. 同时只允许一个在途请求。第二次点击在 busy 期间被挡掉，配合界面禁用按钮。
 *  3. 只认 request_id 完全相同的应答。设备对每个请求都回带同一个 id 的结果，靠它把
 *     "上一次超时后迟到的结果"和"这一次的结果"分开，否则会把旧结果显示成新结果。
 *
 * request_id 的字符集必须与固件的校验一致（[A-Za-z0-9_-]，非空，≤64）：固件会把它
 * 原样拼进应答 JSON，超出这个集合的字符会被直接判为非法请求。
 */
import { log } from './log.js';

const MSG_TYPE_REQUEST = 'open_door';
const MSG_TYPE_RESULT  = 'door_result';

// 设备回的 status -> 给人看的文字。表里没有的 status 一律按"执行失败"处理。
const STATUS_TEXT = {
    opened: '已开门',
    busy:   '设备忙，请稍后再试',
    denied: '已拒绝（通话未建立）',
    error:  '执行失败（设备端出错）',
};

function newRequestId() {
    // crypto.getRandomValues 而不是 Math.random：request_id 同时充当"这条应答是回给
    // 我这次请求的"的凭据，可预测的 id 会让迟到/伪造的应答更容易对上号。
    const bytes = new Uint8Array(8);
    crypto.getRandomValues(bytes);
    return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

export class DoorClient {
    /**
     * @param {object} opts
     * @param {(text:string)=>boolean} opts.send  经 DataChannel 发送，返回是否真的发出
     * @param {(pending:boolean)=>void} opts.onPending 在途状态变化（界面据此禁用按钮）
     * @param {(text:string)=>void} opts.onResult     结果文字
     * @param {number} opts.timeoutMs
     */
    constructor(opts) {
        this.opts_ = opts;
        this.pendingId_ = null;
        this.timer_ = null;
    }

    get pending() { return this.pendingId_ !== null; }

    /**
     * 发起一次开门请求。
     * @returns {boolean} 是否已发出。false 表示上一次还在途、或通道发不出去。
     */
    request() {
        if (this.pending) {
            return false;
        }
        const id = newRequestId();
        // 先置为在途再发送：send 是同步的，但应答理论上可以在 send 返回前就被投递，
        // 那时候必须已经能匹配上 pendingId_。
        this.pendingId_ = id;
        this.opts_.onPending(true);

        const ok = this.opts_.send(JSON.stringify({
            type: MSG_TYPE_REQUEST,
            request_id: id,
        }));
        if (!ok) {
            // 没发出去 = 门肯定没动，这是唯一可以明确告知"没开"的失败路径。
            this.clear_();
            this.opts_.onResult('发送失败，门未打开');
            return false;
        }
        log('已发送开门请求');   // 不打印消息体
        this.timer_ = setTimeout(() => this.onTimeout_(), this.opts_.timeoutMs);
        return true;
    }

    /**
     * 处理一条控制通道消息。
     * @returns {boolean} 是否是本模块认识并消费掉的消息
     */
    handleMessage(text) {
        let msg;
        try {
            msg = JSON.parse(text);
        } catch (_) {
            log('丢弃一条无法解析的控制消息');   // 不回显内容
            return false;
        }
        if (msg === null || typeof msg !== 'object' || msg.type !== MSG_TYPE_RESULT) {
            return false;
        }
        if (!this.pending || msg.request_id !== this.pendingId_) {
            // 迟到的、或者对不上号的结果：丢掉。绝不能把它显示出来 —— 用户会以为
            // 是刚才那次点击的结果。
            log('忽略一条对不上请求的开门结果');
            return true;
        }
        const status = typeof msg.status === 'string' ? msg.status : '';
        const text_ = STATUS_TEXT[status] || '执行失败（未知状态）';
        this.clear_();
        log(`收到开门结果：${text_}`);
        this.opts_.onResult(text_);
        return true;
    }

    /**
     * 复位在途状态。掉线、通话结束、页面隐藏超时都要调 —— 在途请求的结果已经不可能
     * 再收到了，留着它只会让按钮一直是禁用的。
     * @param {string} [reason] 传了就把它作为结果文字显示出来
     */
    reset(reason) {
        const wasPending = this.pending;
        this.clear_();
        if (wasPending && reason) {
            this.opts_.onResult(reason);
        }
    }

    // ---- 内部实现 --------------------------------------------------------

    onTimeout_() {
        this.clear_();
        // 关键措辞：不是"失败"，是"结果未知"。设备可能已经开了门只是应答丢了。
        this.opts_.onResult('结果未知：未收到设备应答，请确认门的实际状态后再决定是否重试');
        log('开门请求超时，结果未知（不自动重发）');
    }

    clear_() {
        if (this.timer_ !== null) {
            clearTimeout(this.timer_);
            this.timer_ = null;
        }
        if (this.pendingId_ !== null) {
            this.pendingId_ = null;
            this.opts_.onPending(false);
        }
    }
}
