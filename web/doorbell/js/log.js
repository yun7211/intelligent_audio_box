/* 页面日志 —— 只输出**事件名和状态**，永不输出内容。
 *
 * 为什么要专门一个模块：设计里有一条硬约束"页面不得把鉴权信息、SDP、ICE/TURN
 * 凭证或完整控制消息打进控制台"。把输出集中到一个函数上，这条约束就只需要在
 * 这里守住一次，而不是指望每个 console.log 的调用点都记得脱敏。
 *
 * 所以本模块只接受**短字符串**，不接受对象。想打对象的时候，请先在调用点把它
 * 归纳成一句人话（"offer 已应答"而不是把 offer 打出来）—— 那一步归纳本身就是脱敏。
 */

const MAX_LINES = 200;

let sink = null;   // 由 app.js 注入的页面日志区域；没注入就只留控制台

export function bindLogView(el) {
    sink = el;
}

/**
 * @param {string} text 一句已经脱敏的人话。不要传入 SDP / candidate / 凭证 / 原始消息体。
 */
export function log(text) {
    const line = `[${new Date().toLocaleTimeString()}] ${text}`;
    // 控制台留一份，方便在没有页面日志区域时（比如刚加载就出错）也能看到。
    // 这里只会有事件名，不含任何敏感内容。
    console.info(line);
    if (sink === null) {
        return;
    }
    const div = document.createElement('div');
    div.textContent = line;   // textContent 而非 innerHTML：日志里可能有对端来的字样
    sink.appendChild(div);
    while (sink.childElementCount > MAX_LINES) {
        sink.removeChild(sink.firstElementChild);
    }
    sink.scrollTop = sink.scrollHeight;
}

/* 错误的**类型**可以打，错误的 message 不一定 —— fetch/WebSocket 的报错里可能带
 * 完整 URL（含房间号和 client_id）。所以这里只取 name，具体语境由调用点补一句。 */
export function logError(what, err) {
    log(`${what}失败${err && err.name ? `（${err.name}）` : ''}`);
}
