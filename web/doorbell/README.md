# 门铃控制页（远程通话 + 远程开门）

配合固件的 `CONFIG_WEBRTC_DOORBELL_MODE` 使用的独立网页客户端。设备通过公网
AppRTC 信令建立 WebRTC 通话，本页面在通话中通过 **WebRTC 数据通道** 下发开门命令。

这个页面配合 `CONFIG_WEBRTC_SIGNALING_APPRTC_WS`（固件默认的信令模式）使用，是纯静态
文件，放在哪台机器上都可以。

固件还有另一条信令路径 `CONFIG_WEBRTC_SIGNALING_LOCAL_HTTP`：设备自己起局域网 HTTPS
信令服务，并内置一个单文件调试页（`https://<设备IP>/webrtc/test`）。**那条路和本页面互斥**
—— 选了它就不需要本页面，反之亦然。它的门槛也明显更低：局域网信令端点没有鉴权，同网内
任何主机都能成为对端，而对端拿得到数据通道也就拿得到开门资格。所以固件默认不选它。

## 目录

```
web/doorbell/
├── index.html          # 只有结构，没有内联脚本
├── css/style.css       # 只有布局和配色
├── js/config.js        # 唯一需要按环境修改的文件
├── js/log.js           # 统一的脱敏日志出口
├── js/signaling.js     # AppRTC 信令（join / message / leave + WSS）
├── js/peer.js          # PeerConnection + 控制数据通道
├── js/door.js          # 开门协议（request_id、超时、不自动重发）
└── js/app.js           # 唯一碰 DOM 的文件，负责装配
```

分层约定：`signaling.js` 不认识 SDP 和开门协议，`peer.js` 不认识开门协议和 DOM，
`door.js` 不认识 DOM。换信令服务只改 `signaling.js`，改界面只改 `app.js`。

## 运行

页面用了 ES 模块和 `getUserMedia`，两者都要求"安全上下文"，因此**不能直接双击用
`file://` 打开**。用任意静态服务器起在 `http://localhost` 或 https 域名下即可：

```bash
cd web/doorbell
python -m http.server 8080
# 然后浏览器打开 http://localhost:8080
```

在手机等非 localhost 的设备上访问时必须用 **https**，否则浏览器不给麦克风权限。

## 配置

只需要改 `js/config.js` 里的 `signalingBase`，指向与固件
`CONFIG_WEBRTC_SIGNALING_URL` **同一个** AppRTC 信令服务的基地址（不含 `/join`）：

| 位置 | 形式 | 例子 |
| --- | --- | --- |
| 固件 Kconfig | 带 `/join/` 结尾 | `https://<你的服务>/join/` |
| 本页 config.js | 基地址，不含 `/join` | `https://<你的服务>` |

`signalingBase` 必须是 `https`：页面自身跑在 https 下时，浏览器会拦掉指向 http 的请求。

## 使用顺序（顺序很重要）

1. 在设备屏幕上用「设置」填好房间号。
2. **先在设备上按「门铃」发起呼叫。**
3. 再在本页面填入同一个房间号，点「加入」。
4. 接通后「远程开门」按钮才会变为可用。

第 2、3 步不能颠倒。AppRTC 用"谁先进房间"来决定谁发 offer，而设备侧固定是
offer 的一方；如果页面先进房间，双方就会互相等待对方的 offer。为了避免踩进这个
死锁，页面在发现自己加入的是**空房间**时会立刻退出房间并提示先按门铃。

## 远程开门的行为约定

按钮可用需要**同时**满足四个条件，缺一即禁用：

1. 通话已接通（`RTCPeerConnection` 到达 `connected`）；
2. 控制数据通道处于 `open`；
3. 没有在途的开门请求；
4. 页面在前台（切到后台超过 20 秒会暂时禁用，回到前台自动恢复）。

点击后还有一次明确的二次确认。发送的消息与设备的应答：

```
页面 -> 设备   {"type":"open_door","request_id":"<16 位十六进制>"}
设备 -> 页面   {"type":"door_result","request_id":"<同一个 id>","status":"opened|busy|denied|error"}
```

| status | 页面显示 | 含义 |
| --- | --- | --- |
| `opened` | 已开门 | 设备已开始输出开锁脉冲 |
| `busy` | 设备忙，请稍后再试 | 上一次脉冲还没结束，设备**没有**重新计时 |
| `denied` | 已拒绝（通话未建立） | 设备侧判定通话不成立 |
| `error` | 执行失败（设备端出错） | GPIO 或定时器失败，设备已强制回到关锁电平 |

**超时不会自动重发。** 超过 `doorTimeoutMs`（默认 5 秒）没收到应答，页面显示
"结果未知"，并要求人确认门的实际状态后再决定是否重试。一次请求对应门上一次物理
动作，自动重发意味着门可能被开第二次。只有"发送失败"这一种情况可以确定门没动。

只有 `request_id` 完全一致的应答才会被采纳。上一次超时后迟到的结果会被丢弃，不会
被显示成这一次的结果。

掉线、通话结束、页面长时间切到后台时，在途请求会被清空并禁用按钮。

## 安全约定

- 命令**只走数据通道**。`signaling.js` 里刻意没有"通过信令发自定义数据"的接口 ——
  AppRTC 房间是明文转发的，不存在的接口不会被误用。设备侧同样只接受数据通道来的
  开门命令，从信令来的一律丢弃且不回应答。
- 本页面不含任何账号、令牌、TURN 密码。TURN 凭证由信令服务在 join 之后按房间下发，
  页面拿到就直接交给 `RTCPeerConnection`，不缓存、不打印。
- 房间号**不做持久化**。它是"谁能和这台设备通话"的唯一门槛，而通话是远程开门的前提；
  存进 `localStorage` 等于把门钥匙留在浏览器里。
- 日志（页面上的和控制台里的）只记事件名与状态，不含 SDP、ICE 候选、凭证或控制消息
  内容。统一出口在 `js/log.js`。
- `index.html` 带了 CSP，不加载任何外部资源。

## 已知未验证项

本页面与对应的固件改动**尚未做过端到端联调**（未编译、未烧录、未接硬件）。特别是
公网 AppRTC 这条路在本仓库里此前没有实际跑通过的记录，`signalingBase` 是否与目标
服务的接口完全一致需要实测确认。
