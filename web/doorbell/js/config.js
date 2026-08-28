/* 部署配置 —— 这是整个页面里**唯一**需要按环境改动的文件。
 *
 * 安全约定（与固件同一条）：这里不放任何账号、令牌、TURN 密码。
 * 本文件会原样下发到浏览器，写进来就等于公开。TURN 凭证由信令服务在
 * join 之后按房间下发（见 signaling.js 的 fetchIceServers），页面只是转手
 * 交给 RTCPeerConnection，既不缓存也不打印。
 */
export const CONFIG = {
    // AppRTC 兼容信令服务的**基地址**，不含 /join。必须与固件的
    // CONFIG_WEBRTC_SIGNALING_URL 指向同一个服务，否则设备和页面进不了同一个房间。
    // 必须是 https：页面自身在 https 下加载时，浏览器会拦截混合内容的 http 请求。
    signalingBase: 'https://webrtc.espressif.com',

    // 开门应答的等待上限。超时只提示"结果未知"，**绝不自动重发** ——
    // 一次开门请求对应一次物理动作，重发意味着门可能被开第二次。
    doorTimeoutMs: 5000,

    // 通话建立的等待上限。超过这个时间没连上就当作失败并复位界面，
    // 避免"开门"按钮卡在一个永远不会变 connected 的状态上。
    connectTimeoutMs: 30000,
};
