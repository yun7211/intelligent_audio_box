# WebRTC 通话回声消除（设备端 AEC）方案

> 本文基于代码实现整理，说明设备在 WebRTC 实时通话链路中如何做回声消除。
> 涉及文件：`main/webrtc/webrtc_media_bridge.c`、`main/webrtc/webrtc_call_service.cc`、`main/boards/lichuang-dev/config.h`、`main/audio/wake_words/afe_wake_word.cc`。

## 1. 回声问题从哪来

通话时扬声器播放对端语音，声音被麦克风重新拾取，再传回对端——对端会听到自己的回声。
消除的前提是：**拿到"我当前在播什么"作为参考信号**，从麦克风采集里对消掉播放的部分。

## 2. 硬件拓扑：本项目的参考信号来源

立创 ESP32-S3 板用 **ES7210 4-slot TDM** 采集，前三路依次是：

| Slot | 来源 | 作用 |
| --- | --- | --- |
| MIC1 / MIC2 | 门口人声（双麦克风） | 主拾音 |
| MIC3 | **ES8311 播放回采** | AEC 参考信号：扬声器正在播的声音 |

所以 AFE/采集源的输入布局是 `MMR`（Mic-Mic-Reference），配置见 [lichuang-dev/config.h:15-17](main/boards/lichuang-dev/config.h#L15-L17)：

```c
#define WEBRTC_AEC_MIC_LAYOUT   "MMR"   // 声明这块板有硬件参考（MIC3 回采）
#define WEBRTC_AEC_CHANNELS     4       // 4-slot TDM
#define WEBRTC_AEC_CHANNEL_MASK 0x07    // 取前三路（MIC1/2/3）
```

## 3. 两条独立链路的 AEC

本项目有两条会发声、会拾音的链路，各自做 AEC，互不干扰：

### 3.1 语音助手链路（ESP-SR AFE）

- 离线唤醒 + 对话阶段由 ESP-SR 的 AFE 做 AEC/VAD。
- 初始化时 `afe_config->aec_init = codec_->input_reference()`，AEC 模式 `AEC_MODE_SR_HIGH_PERF`，输入格式按 codec 参考通道拼成 `MMR`（[afe_wake_word.cc:73-77](main/audio/wake_words/afe_wake_word.cc#L73-L77)）。

### 3.2 WebRTC 通话链路（esp_capture AEC 源）

- 通话时助手被 `Suspend()`，AFE 不再运行，AEC 由 esp_capture 的 **AEC 采集源**接管。
- 只有声明了 `WEBRTC_AEC_MIC_LAYOUT` 的板型才走 AEC 源（[webrtc_media_bridge.c:58-72](main/webrtc/webrtc_media_bridge.c#L58-L72)）：

```c
esp_capture_audio_aec_src_cfg_t cfg = {
    .mic_layout   = WEBRTC_AEC_MIC_LAYOUT,   // "MMR"：三路输入
    .record_handle = rec,                    // 复用助手已初始化好的共享 codec handle
    .channel       = WEBRTC_AEC_CHANNELS,    // 4
    .channel_mask  = WEBRTC_AEC_CHANNEL_MASK,// 0x07
    .data_on_vad   = false,                  // 静音期也连续送帧，不吞用户开口
};
return esp_capture_new_audio_aec_src(&cfg);
```

AEC 源把 MIC1/2 人声 + MIC3 参考处理成**单声道** PCM，后续管线按协商结果编码为 G711A。

### 3.3 服务端 AEC（可选）

`CONFIG_USE_SERVER_AEC` 是助手链路的独立选项，与 WebRTC 无关，服务端侧再做一层对消。

## 4. 关键设计：时钟域统一是 AEC 生效的前提

AEC 对消要求**参考信号和主麦克风信号在同一个时钟域**（同采样率、同源），否则参考会持续漂移，对不干净。这是本项目最核心的处理：

1. **G711A 固定 8 kHz**。通话媒体配置为 8 kHz 单声道（[webrtc_call_service.cc:193-195](main/webrtc/webrtc_call_service.cc#L193-L195)），PCMA 编解码本身就在 8 kHz。
2. **采集端切到 8 kHz**：AEC 源启动时把共享 codec 的采集时钟切到 8 kHz。
3. **播放端跟随码流重配时钟**：渲染配置 `i2s_render_cfg_t { .fixed_clock = false }`（[webrtc_media_bridge.c:145](main/webrtc/webrtc_media_bridge.c#L145)）——G711A 解码输出是 8 kHz，渲染端允许 esp_codec_dev 随码流重配播放时钟。
4. **结果**：ES7210/ES8311 共用的**双工 I2S 保持同一 8 kHz 时钟域**，MIC3 参考不会持续漂移，AEC 才能有效对消。

切换时机：`Start()` 挂起助手（`AudioService::Suspend()`）→ `webrtc_media_bridge_build()` 创建 AEC 源并切 8 kHz → 通话 → `Cleanup()` → `webrtc_media_bridge_destroy()` → `AudioService::Resume()` 按板级配置恢复 24 kHz 助手链路。

## 5. 失败策略：硬件参考是必需能力，不静默降级

- **声明了 `WEBRTC_AEC_MIC_LAYOUT` 的板型**：`esp_capture_new_audio_aec_src` 创建失败时 `webrtc_media_bridge_build` 直接失败，**不退回可能产生强回声的原始音频**（[webrtc_media_bridge.c:56-57](main/webrtc/webrtc_media_bridge.c#L56-L57) 注释）。
- **没有硬件参考的板型**（如 esp-box-3）：退化为 `esp_capture_new_audio_dev_src` 普通采集源（无 AEC），日志明确打 `aec=0`（[webrtc_media_bridge.c:169-173](main/webrtc/webrtc_media_bridge.c#L169-L173)）。

## 6. 已知短板（诚实记录）

- `webrtc_media_bridge_build()` 的中途失败分支（如 `esp_capture_open` 失败）会让已分配的 `s_aud_src` 走不到 `esp_capture_close`，`destroy()` 只把它置 NULL，那一次分配就泄漏了（[webrtc_media_bridge.c:201-204](main/webrtc/webrtc_media_bridge.c#L201-L204)）。单次通话失败漏一次，量不大，正确做法是 build() 自己按分配顺序逆序回滚。
- 渲染端 FIFO `allow_drop_data = false` 在实时语音上是可商量的取舍：当前先保证音质可懂，压延迟时应该允许丢旧帧并把丢帧计数暴露到 `get_stats`（[webrtc_media_bridge.c:159-162](main/webrtc/webrtc_media_bridge.c#L159-L162)）。
