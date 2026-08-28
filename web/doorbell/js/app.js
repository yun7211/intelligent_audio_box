/* 界面装配层：把 DOM 事件接到 signaling / peer / door 三个模块上，并集中维护
 * "开门按钮此刻是否该可用"这一个判断。
 *
 * 这里是唯一碰 DOM 的文件。三个下层模块彼此不认识，也都不认识 DOM：换界面只改这一个
 * 文件，换信令只改 signaling.js，改开门协议只改 door.js。
 *
 * 开门按钮的可用条件（四个必须同时成立，缺一即禁用）：
 *   1. 处于通话中（state === 'incall'）
 *   2. PeerConnection 已 connected 且控制通道已 open（peer.ready）
 *   3. 没有在途的开门请求（!door.pending）
 *   4. 页面在前台（pageActive_）—— 页面被切走足够久就当作"人不在看着"，
 *      此时不该还留着一个能一键开门的按钮。
 *
 * 房间号**不做任何持久化**：它是"谁能和这台设备通话"的唯一门槛，而通话是远程开门的
 * 前提。存进 localStorage 就等于把门钥匙留在了浏览器里。
 */
import { CONFIG } from './config.js';
import { log, logError, bindLogView } from './log.js';
import { AppRtcSignaling } from './signaling.js';
import { DoorbellPeer } from './peer.js';
import { DoorClient } from './door.js';

const ROOM_PATTERN = /^[A-Za-z0-9_-]{1,64}$/;
// 页面切到后台后多久算"人不在看着"。给一点宽容度：切出去看一眼消息就回来是常见操作，
// 不该因此把开门按钮打灰。
const HIDDEN_GRACE_MS = 20000;

const el = {
    room:        document.getElementById('room'),
    joinBtn:     document.getElementById('joinBtn'),
    hangupBtn:   document.getElementById('hangupBtn'),
    callStatus:  document.getElementById('callStatus'),
    remoteVideo: document.getElementById('remoteVideo'),
    videoHint:   document.getElementById('videoHint'),
    unlockBtn:   document.getElementById('unlockBtn'),
    doorStatus:  document.getElementById('doorStatus'),
    logView:     document.getElementById('logView'),
};

bindLogView(el.logView);

let state = 'idle';          // 'idle' | 'joining' | 'waiting' | 'incall'
let signaling = null;
let peer = null;
let localStream = null;
let connectTimer = null;
let hiddenTimer = null;
let pageActive = !document.hidden;
// 已经显示过一条开门**结果**。置位后，refreshUnlock 不再用"可以开门/通话未接通"这类
// 提示去覆盖它 —— 尤其是"结果未知"：那条信息必须留在屏幕上让人看到并去确认门的状态，
// 而它出现的时机恰好就是通话被拆掉的时候，正好是提示文字最想插进来的时候。
// 只在"开始一通新通话"和"发起新的开门请求"这两个明确的时刻清掉。
let doorResultShown = false;

const door = new DoorClient({
    send: (text) => (peer !== null ? peer.sendControl(text) : false),
    onPending: () => refreshUnlock(),
    onResult: (text) => { doorResultShown = true; setDoorStatus(text); },
    timeoutMs: CONFIG.doorTimeoutMs,
});

// ---- 界面状态 -----------------------------------------------------------

function setCallStatus(text, kind) {
    el.callStatus.textContent = text;
    el.callStatus.className = 'status' + (kind ? ` is-${kind}` : '');
}

function setDoorStatus(text, kind) {
    el.doorStatus.textContent = text;
    el.doorStatus.className = 'status status-door' + (kind ? ` is-${kind}` : '');
}

/* 唯一决定开门按钮可用性的地方。任何可能影响四个条件之一的事件都必须调它，
 * 而不是各自去 disabled = ... —— 分散判断迟早会漏掉一种组合。 */
function refreshUnlock() {
    const ready = state === 'incall' && peer !== null && peer.ready &&
                  !door.pending && pageActive;
    el.unlockBtn.disabled = !ready;

    if (door.pending) {
        setDoorStatus('已发送，等待设备应答…');
        return;
    }
    // 已经有一条结果在显示时不做覆盖，见 doorResultShown 的说明。
    if (doorResultShown) {
        return;
    }
    if (ready) {
        setDoorStatus('可以开门');
    } else if (state !== 'incall') {
        setDoorStatus('通话未接通');
    } else if (!pageActive) {
        setDoorStatus('页面已切换到后台，回到前台后可用');
    } else {
        setDoorStatus('控制通道未就绪');
    }
}

function setJoinUi(joined) {
    el.joinBtn.disabled = joined;
    el.room.disabled = joined;
    el.hangupBtn.disabled = !joined;
}

// ---- 加入 / 挂断 ---------------------------------------------------------

el.joinBtn.addEventListener('click', () => { join(); });
el.hangupBtn.addEventListener('click', () => { hangup('已挂断'); });

async function join() {
    const room = el.room.value.trim();
    if (!ROOM_PATTERN.test(room)) {
        setCallStatus('房间号只能是字母、数字、下划线或短横线（1-64 位）', 'warn');
        return;
    }

    state = 'joining';
    setJoinUi(true);
    setCallStatus('正在获取麦克风…');
    doorResultShown = false;   // 新的一通通话，上一通的开门结果不再相关
    refreshUnlock();

    try {
        // 设备侧是“设备发音视频、页面回音频”，所以这里只要麦克风，不要摄像头。
        // 这里的 AEC 处理浏览器扬声器到浏览器麦克风的回声；ESP32 上的 MMR AEC
        // 独立处理 ES8311 扬声器到 ES7210 麦克风的回声，两端各自负责自己的声学环境。
        localStream = await navigator.mediaDevices.getUserMedia({
            audio: {
                echoCancellation: true,
                noiseSuppression: true,
                autoGainControl: true,
            },
            video: false,
        });
    } catch (err) {
        logError('获取麦克风', err);
        setCallStatus('麦克风获取失败，无法通话', 'warn');
        await teardown();
        return;
    }

    setCallStatus('正在加入房间…');
    signaling = new AppRtcSignaling(CONFIG.signalingBase);
    signaling.onMessage(onSignalingMessage);
    signaling.onClose(() => {
        // 信令断了并不必然导致媒体断（ICE 已经打通的话还能通话），但控制通道之后
        // 就没法再重新协商了，而且这通常是掉线的前兆 —— 直接按掉线处理，更好解释。
        if (state !== 'idle') {
            hangup('信令连接已断开');
        }
    });

    let joined;
    try {
        joined = await signaling.join(room);
    } catch (err) {
        logError('加入房间', err);
        setCallStatus('加入房间失败，请检查房间号与信令服务地址', 'warn');
        await teardown();
        return;
    }

    // 房间原本是空的 -> 设备还没进来。这时候如果留在房间里，设备随后加入会拿到
    // is_initiator=false，于是双方都在等对方的 offer，永远连不上。所以立刻退出房间，
    // 把"先在设备上按门铃"这一步明确交回给用户。
    if (joined.isInitiator) {
        setCallStatus('房间内还没有设备：请先在设备上按门铃，然后再点「加入」', 'warn');
        await teardown();
        return;
    }

    peer = new DoorbellPeer({
        iceServers: joined.iceServers,
        localStream,
        onState: onPeerState,
        onRemoteStream: onRemoteStream,
        onSignal: (msg) => { signaling.sendMessage(msg); },
        onControlState: (open) => {
            if (!open) {
                // 通道关掉了，在途请求的结果永远不会来了。
                door.reset('结果未知：控制通道已关闭');
            }
            refreshUnlock();
        },
        onControlMessage: (text) => {
            // door.js 不认识的控制消息直接静默丢弃，且**不回显内容**。
            if (!door.handleMessage(text)) {
                log('忽略一条未知类型的控制消息');
            }
        },
    });

    state = 'waiting';
    setCallStatus('已加入，等待设备接通…');
    refreshUnlock();

    // peer 建好之后才放开消息投递：设备的 offer 常常在页面加入之前就发出来了，由服务端
    // 在 join 应答里回放。放开得早一点，那条 offer 就会落在"peer 还是 null"的窗口里。
    signaling.flushPendingMessages();

    // 兜底超时：设备可能已经离开房间但服务端还没清理，那样 offer 永远不会来。
    // 没有这个定时器的话界面会一直卡在"等待设备接通"。
    connectTimer = setTimeout(() => {
        if (state !== 'incall') {
            hangup('接通超时');
        }
    }, CONFIG.connectTimeoutMs);
}

async function onSignalingMessage(msg) {
    switch (msg.type) {
    case 'offer':
        if (peer === null || typeof msg.sdp !== 'string') {
            return;
        }
        try {
            await peer.acceptOffer(msg.sdp);
        } catch (err) {
            logError('应答 offer', err);
            hangup('协商失败');
        }
        break;
    case 'candidate':
        if (peer !== null && typeof msg.candidate === 'string') {
            await peer.addRemoteCandidate(msg);
        }
        break;
    case 'answer':
        // 本页永远不发 offer（设备才是 Offerer），收到 answer 说明对端行为不符合约定。
        log('忽略一条意外的 answer');
        break;
    case 'bye':
        hangup('设备已挂断');
        break;
    case 'heartbeat':
        break;
    default:
        log('忽略一条未知类型的信令消息');   // 不打印 type 本身，它来自对端
        break;
    }
}

function onPeerState(peerState) {
    if (peerState === 'connected') {
        state = 'incall';
        if (connectTimer !== null) {
            clearTimeout(connectTimer);
            connectTimer = null;
        }
        setCallStatus('通话中', 'ok');
        // 刚接通时控制通道可能还没 open，refreshUnlock 会照实反映。
        refreshUnlock();
    } else if (peerState === 'closed') {
        if (state !== 'idle') {
            hangup('连接已断开');
        }
    } else {
        setCallStatus('正在建立连接…');
        refreshUnlock();
    }
}

function onRemoteStream(stream) {
    el.remoteVideo.srcObject = stream;
    el.videoHint.style.display = 'none';
    log('已收到设备媒体流');
}

/** 结束这一路通话并复位界面。reason 会显示在通话状态上。 */
async function hangup(reason) {
    if (state === 'idle') {
        return;
    }
    // 立刻置 idle 再做后面的异步收尾：下面有 await，期间可能再进来一次 hangup
    // （典型情况：连接断开和信令断开几乎同时发生），不挡住就会重复发 bye、重复拆。
    state = 'idle';
    // 先通知对端再拆本地：拆完了就没有通道可用了。设备侧收到 bye 会挂断并强制关锁。
    if (signaling !== null && signaling.joined) {
        await signaling.sendMessage({ type: 'bye' });
    }
    await teardown();
    setCallStatus(reason || '未连接');
}

/** 无条件回到 idle。任何失败路径都从这里收尾，保证不留半开状态。 */
async function teardown() {
    state = 'idle';
    if (connectTimer !== null) {
        clearTimeout(connectTimer);
        connectTimer = null;
    }
    // 在途开门请求的结果不可能再收到了，必须清掉，否则按钮会一直是禁用的。
    door.reset('结果未知：通话已结束');
    if (peer !== null) {
        peer.close();
        peer = null;
    }
    if (localStream !== null) {
        localStream.getTracks().forEach((t) => t.stop());
        localStream = null;
    }
    if (signaling !== null) {
        const s = signaling;
        signaling = null;
        await s.leave();
    }
    el.remoteVideo.srcObject = null;
    el.videoHint.style.display = '';
    setJoinUi(false);
    refreshUnlock();
}

// ---- 远程开门 -----------------------------------------------------------

el.unlockBtn.addEventListener('click', () => {
    // 再确认一次可用条件，不只依赖按钮的 disabled：状态可能在点击事件派发的间隙里变了。
    if (state !== 'incall' || peer === null || !peer.ready || door.pending || !pageActive) {
        refreshUnlock();
        return;
    }
    // 明确的二次确认。开门是不可撤销的物理动作，误点一次就是门被打开一次。
    if (!window.confirm('确认远程开门？\n门锁会保持开启约 0.8 秒后自动复位。')) {
        log('用户取消了开门');
        return;
    }
    // 必须在 request() 之前清：request() 的失败路径会同步回调 onResult 置位它，
    // 清在后面就会把"发送失败，门未打开"这条结果又擦掉。
    doorResultShown = false;
    door.request();
    refreshUnlock();
});

// ---- 页面前后台 ---------------------------------------------------------

document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
        if (hiddenTimer === null) {
            hiddenTimer = setTimeout(() => {
                hiddenTimer = null;
                pageActive = false;
                // 页面切走这么久，在途请求的结果就算回来了也没人看 —— 一并清掉，
                // 免得回到前台时把一条旧结果当成新结果读。
                door.reset('结果未知：页面已长时间切换到后台');
                log('页面长时间处于后台，已暂时禁用开门');
                refreshUnlock();
            }, HIDDEN_GRACE_MS);
        }
    } else {
        if (hiddenTimer !== null) {
            clearTimeout(hiddenTimer);
            hiddenTimer = null;
        }
        if (!pageActive) {
            pageActive = true;
            log('页面回到前台，开门恢复可用');
        }
        refreshUnlock();
    }
});

// 关页面/刷新时尽量把房间退掉，别把一个僵尸 client 留在房间里挡住下一次加入。
// pagehide 期间异步请求不保证发出，所以这是尽力而为，不作为正确性依赖。
window.addEventListener('pagehide', () => {
    if (state !== 'idle' && signaling !== null && signaling.joined) {
        signaling.sendMessage({ type: 'bye' });
        signaling.leave();
    }
});

// ---- 启动 ---------------------------------------------------------------

if (!navigator.mediaDevices || !window.RTCPeerConnection) {
    setCallStatus('当前浏览器不支持 WebRTC', 'warn');
    el.joinBtn.disabled = true;
} else {
    setCallStatus('未连接');
}
refreshUnlock();
log('页面已就绪');
