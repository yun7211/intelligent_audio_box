# RTOS 任务清单（基于 FreeRTOS / ESP-IDF）

> 本文档梳理本项目（`main/` 目录）中全部 FreeRTOS 任务及所用 RTOS 原语。
> 统计范围：项目自有代码；`managed_components/` 与 `esp-webrtc/`（vendored）等第三方组件内部任务不在此列，仅在文末备注。
> 优先级数值为 ESP-IDF FreeRTOS 优先级（0 = idle，越高越优先）。

## 1. 任务总览

| 分类 | 任务名 | 创建点 | 栈大小 | 优先级 | 核心 | 生命周期 |
| --- | --- | --- | --- | --- | --- | --- |
| 常驻 | 业务主任务 | [main.cc:51](main/main.cc#L51) | 继承 app_main | — | 任意 | 整机运行 |
| 常驻 | `audio_input` | [audio_service.cc:142](main/audio/audio_service.cc#L142) | 6 KB（AFE）/ 4 KB | 8 | **固定核心 0**（AFE 模式） | Start 时创建，Stop 退出 |
| 常驻 | `audio_output` | [audio_service.cc:149](main/audio/audio_service.cc#L149) | 4 KB（AFE）/ 2 KB | 4 | 任意 | Start 时创建，Stop 退出 |
| 常驻 | `opus_codec` | [audio_service.cc:171](main/audio/audio_service.cc#L171) | 24 KB | 2 | 任意 | Start 时创建，Stop 退出 |
| 常驻 | `audio_detection` | [afe_wake_word.cc:83](main/audio/wake_words/afe_wake_word.cc#L83) | 4 KB | 3 | 任意 | 随唤醒词对象 |
| 常驻 | `audio_communication` | [afe_audio_processor.cc:81](main/audio/processors/afe_audio_processor.cc#L81) | 4 KB | 3 | 任意 | 随处理器对象 |
| 常驻 | `LedEvent` | [gpio_led.cc:81](main/led/gpio_led.cc#L81) | 2 KB | `tskIDLE_PRIORITY+2` = 2 | 任意 | 整机运行 |
| 按需 | `encode_wake_word` | [afe_wake_word.cc:184](main/audio/wake_words/afe_wake_word.cc#L184) / [custom_wake_word.cc:230](main/audio/wake_words/custom_wake_word.cc#L230) | 24 KB / 28 KB（**PSRAM 栈**） | 2 | 任意 | 唤醒触发一次，跑完自删 |
| 按需 | `activation` | [application.cc:357](main/application.cc#L357) | 8 KB | 2 | 任意 | 首次联网创建一次，跑完自删 |
| 按需 | `wifi_cfg_delay` | [wifi_board.cc:210](main/boards/common/wifi_board.cc#L210) | 4 KB | 2 | 任意 | 进配网模式时创建，跑完自删 |
| 条件 | `acoustic_wifi` | [wifi_board.cc:188](main/boards/common/wifi_board.cc#L188) | 4 KB | 2 | 任意 | `CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING` 开启时 |
| 条件 | `blufi_deinit` | [blufi.cpp:693](main/boards/common/blufi.cpp#L693) | 4 KB | 5 | 任意 | BLUFI 断开且已配网时 |
| 条件 | BLUFI 连接等待（匿名任务） | [blufi.cpp:764](main/boards/common/blufi.cpp#L764) | 4 KB | 5 | 任意 | 手机下发 Wi-Fi 凭据后 |
| 条件 | `signal_hdlr` | [webrtc_http_server.c:108](main/webrtc/signaling/http_local/webrtc_http_server.c#L108) | 4 KB | 5 | 任意 | 仅 WebRTC 本地 HTTP 信令开启时 |
| 条件 | 摄像头 JPEG 编码线程（`std::thread`） | [esp32_camera.cc:199](main/boards/common/esp32_camera.cc#L199) | 系统默认 | — | — | 有摄像头板型 |
| 归档 | 旧视频任务 | [esp_video.cc:333](main/boards/common/legacy/esp_video.cc#L333) | — | — | — | `legacy/` 目录，不进默认构建 |

默认构建（WebRTC 关闭、无摄像头）常驻 **8 个**任务；全功能开启时含按需/条件任务共约 **14 个**。

---

## 2. 任务详解

### 2.1 业务主任务（Application 主循环）

- **创建**：[main.cc:51](main/main.cc#L51) —— `app_main()` 初始化 NVS 和 Application 后直接转为业务主任务，不额外创建任务。
- **职责**：整机事件循环。`xEventGroupWaitBits` 永久阻塞等待跨任务事件位，被唤醒后**在单一任务中串行执行**：状态机副作用、UI/LED 刷新、`main_tasks_` 闭包队列消费、时钟节拍统计、每 10 秒堆内存统计。
- **同步设计**：网络、音频、定时器、WebRTC 回调等运行在不同任务，统一通过事件位或闭包队列把工作交还给主任务，避免回调线程直接操作共享业务对象造成竞态。

### 2.2 `audio_input` —— 音频采集任务

- **创建**：[audio_service.cc:142](main/audio/audio_service.cc#L142)（AFE 模式，`xTaskCreatePinnedToCore` 固定核心 0）；非 AFE 模式 [:156](main/audio/audio_service.cc#L156)。
- **优先级 8 / 栈 6 KB**：项目中最高优先级业务任务，固定核心 0 是为了降低实时处理抖动。
- **职责**：[audio_service.cc:260](main/audio/audio_service.cc#L260) `AudioInputTask()` 按事件位分支：
  - `AS_EVENT_AUDIO_TESTING_RUNNING`：配网态 BOOT 键录音测试，录满编码回放；
  - `AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING`：同一份 10 ms PCM（16 kHz / 160 点）同时喂给唤醒词和 AEC/VAD 处理器；
  - 无事件位时 `xEventGroupWaitBits` 阻塞等待；读超时 `vTaskDelay(10ms)` 后重试，不因瞬时错误退出。
- **退出**：`Stop()` 置 `service_stopped_` 并置位所有事件唤醒任务，任务自行 break 后 `vTaskDelete(NULL)`。

### 2.3 `audio_output` —— 音频播放任务

- **创建**：[audio_service.cc:149](main/audio/audio_service.cc#L149)（AFE 模式）/ [:163](main/audio/audio_service.cc#L163)。
- **优先级 4**：低于采集，高于编解码。
- **职责**：`std::condition_variable` 等待播放队列非空，弹出 TTS 解码后的音频写入扬声器。

### 2.4 `opus_codec` —— Opus 编解码任务

- **创建**：[audio_service.cc:171](main/audio/audio_service.cc#L171)，栈 24 KB。
- **优先级 2**：Opus 编解码计算量大，但实时性要求低于 PCM 输入/输出，因此放到独立低优先级任务。

### 2.5 `audio_detection` —— 唤醒词检测任务

- **创建**：[afe_wake_word.cc:83](main/audio/wake_words/afe_wake_word.cc#L83)，栈 4 KB，优先级 3。
- **职责**：从 `input_buffer_` 取 PCM 喂给 ESP-SR AFE（`afe_iface_->feed()`），检测唤醒词。
- **同步**：由 `DETECTION_RUNNING_EVENT` 事件位控制启停；`Feed()` 与 `Stop()` 用 `std::mutex` 保护缓冲区，**在锁内检查运行事件位**避免 TOCTOU 竞态。

### 2.6 `audio_communication` —— AEC/VAD 处理任务

- **创建**：[afe_audio_processor.cc:81](main/audio/processors/afe_audio_processor.cc#L81)，栈 4 KB，优先级 3。
- **职责**：AFE 的 AEC + VAD 处理，支撑 TTS 播放期间的自由打断检测（Speaking 状态持续分析麦克风但不上传）。

### 2.7 `LedEvent` —— LED 状态任务

- **创建**：[gpio_led.cc:81](main/led/gpio_led.cc#L81)，栈 2 KB，优先级 `tskIDLE_PRIORITY + 2`。
- **职责**：LED 状态机。**GPIO ISR 用 `xTaskNotifyFromISR` 唤醒**，任务侧 `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` 阻塞等待（[gpio_led.cc:197](main/led/gpio_led.cc#L197)、[:256](main/led/gpio_led.cc#L256)）——任务通知比信号量更轻量，是 ISR → 任务的标准通道。

### 2.8 `encode_wake_word` —— 唤醒词编码任务（静态创建）

- **创建**：`xTaskCreateStatic`（[afe_wake_word.cc:184](main/audio/wake_words/afe_wake_word.cc#L184) / [custom_wake_word.cc:230](main/audio/wake_words/custom_wake_word.cc#L230)）。
- **内存**：任务栈用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 分配在 **PSRAM**（24 KB / 28 KB），`StaticTask_t` 控制块在内部 RAM——静态创建 + PSRAM 栈的标准做法。
- **职责**：唤醒触发后**按需启动**，在后台创建临时 Opus 编码器，把唤醒前滚缓存 PCM 编码成 Opus 包供建连后发送，避免阻塞实时检测任务；完成后释放编码器、清空缓存、`vTaskDelete(NULL)` 自删。
- **同步**：结果放入 `wake_word_opus_`（`std::mutex` + `std::condition_variable` 通知消费者）。

### 2.9 `activation` —— 激活 / OTA 检查任务

- **创建**：[application.cc:357](main/application.cc#L357)，栈 8 KB，优先级 2。
- **触发**：仅在 `Starting` / `WifiConfiguring` 态的首次联网时创建一次（[application.cc:348](main/application.cc#L348)），已有任务在跑则直接返回。
- **职责**：包含 HTTP 重试和 Flash 下载，放低优先级后台任务避免阻塞主事件循环；只做耗时工作并置位事件，**收尾（进入 Idle、展示版本、播放提示音）全部回到主任务执行**。

### 2.10 `wifi_cfg_delay` —— 进配网模式延迟任务

- **创建**：[wifi_board.cc:210](main/boards/common/wifi_board.cc#L210)，栈 4 KB，优先级 2。
- **职责**：先 `vTaskDelay(1 s)` 让播报优雅结束，再停 Wi-Fi、启动配网模式。

### 2.11 `acoustic_wifi` —— 声学配网任务

- **创建**：[wifi_board.cc:188](main/boards/common/wifi_board.cc#L188)，栈 4 KB，优先级 2。
- **条件**：`CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING` 开启时；通过音频接收配网凭据。

### 2.12 BLUFI 配网相关任务（2 个）

- `blufi_deinit`：[blufi.cpp:693](main/boards/common/blufi.cpp#L693)，栈 4 KB，优先级 5。BLE 断开且已完成配网时异步执行反初始化。
- 匿名连接等待任务：[blufi.cpp:764](main/boards/common/blufi.cpp#L764)，栈 4 KB，优先级 5。手机经 BLUFI 下发 Wi-Fi 凭据后创建，以 200 ms 间隔轮询等待 Wi-Fi 连接（10 s 超时），回包后主动断开 BLE，结束一次配网会话。

### 2.13 `signal_hdlr` —— WebRTC 本地信令发送任务

- **创建**：[webrtc_http_server.c:108](main/webrtc/signaling/http_local/webrtc_http_server.c#L108)，栈 4 KB，优先级 5。
- **条件**：`CONFIG_WEBRTC_SIGNALING_LOCAL_HTTP` 开启时。
- **职责**：从 `signaling_queue`（`xQueueCreate`，[webrtc_http_server.c:309](main/webrtc/signaling/http_local/webrtc_http_server.c#L309)）取信令消息向浏览器 SSE 推送；`xQueueReceive` 100 ms 超时轮询，附带 5 s 心跳检测。

### 2.14 摄像头 JPEG 编码线程

- **创建**：[esp32_camera.cc:199](main/boards/common/esp32_camera.cc#L199)，`std::thread`。
- **职责**：将摄像头帧压缩为 JPEG chunk 放入 40 帧深的 `xQueueCreate` 队列（[esp32_camera.cc:192](main/boards/common/esp32_camera.cc#L192)）。

---

## 3. 优先级分层

| 优先级 | 任务 | 设计意图 |
| --- | --- | --- |
| 8 | `audio_input` | 实时性最高；AFE 模式固定核心 0 降低抖动 |
| 4 | `audio_output` | 播放不能饿死，但低于采集 |
| 3 | `audio_detection` / `audio_communication` | 唤醒词与 AEC/VAD 检测 |
| 2 | `opus_codec` / `encode_wake_word` / `activation` / `wifi_cfg_delay` / `acoustic_wifi` / `LedEvent` | 计算量大的编解码与后台业务放低优先级 |
| 5 | `signal_hdlr` / BLUFI 任务 | 配网与信令按需任务（相对较高，短促执行） |

## 4. 所用 RTOS 原语汇总

| 原语 | 用途 | 代表位置 |
| --- | --- | --- |
| `xTaskCreate` | 动态创建任务 | 全部常驻/按需任务 |
| `xTaskCreatePinnedToCore` | 任务固定核心 | [audio_service.cc:142](main/audio/audio_service.cc#L142) |
| `xTaskCreateStatic` | 静态创建 + PSRAM 栈 | [afe_wake_word.cc:184](main/audio/wake_words/afe_wake_word.cc#L184) |
| `xEventGroupCreate/WaitBits/SetBits/ClearBits` | **事件组：跨任务事件通知**（全项目 6 个：Application、AudioService、AfeWakeWord、AfeAudioProcessor、WebSocketProtocol、MqttProtocol） | [application.cc:36](main/application.cc#L36)、[audio_service.cc:262](main/audio/audio_service.cc#L262)、[websocket_protocol.cc:229](main/protocols/websocket_protocol.cc#L229) |
| `xQueueCreate / xQueueSend / xQueueReceive` | 队列：信令消息、摄像头 JPEG chunk | [webrtc_http_server.c:309](main/webrtc/signaling/http_local/webrtc_http_server.c#L309)、[esp32_camera.cc:192](main/boards/common/esp32_camera.cc#L192) |
| `xTaskNotifyFromISR / ulTaskNotifyTake` | 任务通知：GPIO ISR → LED 任务 | [gpio_led.cc:197](main/led/gpio_led.cc#L197)、[:256](main/led/gpio_led.cc#L256) |
| `std::mutex / std::condition_variable` | C++ 同步原语：音频数据队列、`main_tasks_` 闭包队列、协议通道 | [audio_service.cc:322](main/audio/audio_service.cc#L322)、[application.cc:318](main/application.cc#L318) |
| `esp_timer`（跑在专用 FreeRTOS 任务上） | 周期/单次定时：音频电源管理（1 s）、打断确认（200 ms）、LED 闪烁、门锁脉冲、Wi-Fi 连接超时 | [door_lock.cc:44](main/webrtc/doorbell/door_lock.cc#L44) |
| `vTaskDelay / pdMS_TO_TICKS` | 延时与时间换算 | 全局 |
| `xTaskGetTickCount / portTICK_PERIOD_MS` | 节拍计数 | [esp_video.cc:354](main/boards/common/legacy/esp_video.cc#L354) |

**设计要点**：

1. **事件组是跨任务通信的主通道**：回调/ISR/定时器任务只负责置位，所有状态修改回到主任务串行执行，是"主任务并发模型"的核心。
2. **音频数据队列刻意未用 FreeRTOS 队列**，而是 `std::mutex + std::condition_variable + std::deque`；全项目没有 `xSemaphoreCreateMutex`——C++ 标准库同步被用作数据队列，FreeRTOS 原语负责任务间事件通知。
3. **esp_timer 回调里绝对不能 `vTaskDelay()`**：所有回调在同一个高优先级任务上串行执行，阻塞会连带拖住整机（[door_lock.h:13](main/webrtc/doorbell/door_lock.h#L13)）。
4. **静态任务栈放 PSRAM**：`encode_wake_word` 的 24~28 KB 栈通过 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 分配，TCB 放内部 RAM，规避内部 RAM 紧张。

## 5. C++ 层并发原语（RTOS 职责的 C++ 实现）

ESP-IDF 的 C++ 运行时把 C++ 标准库并发原语（`std::mutex` / `std::condition_variable` / `std::thread` / `std::atomic`）映射到 FreeRTOS 内核对象上（pthread 兼容层）。项目在**数据通路和共享状态**上大量使用 C++ 原语，与第 4 节的 FreeRTOS 原生 API 形成互补分工：FreeRTOS 事件组/任务通知负责任务间**事件通知**，C++ 原语负责**数据传递与互斥**。

### 5.1 互斥锁 `std::mutex` + RAII 锁（`std::lock_guard` / `std::unique_lock`）

| 锁 | 保护对象 | 代表位置 |
| --- | --- | --- |
| `mutex_` | 主任务 `main_tasks_` 闭包队列 | [application.h:164](main/application.h#L164)、[application.cc:318](main/application.cc#L318) |
| `mutex_` | 设备状态机状态读写 | [device_state_machine.h:66](main/device_state_machine.h#L66)、[.cc:134](main/device_state_machine.cc#L134) |
| `audio_queue_mutex_` | 5 条音频数据队列 | [audio_service.h:173](main/audio/audio_service.h#L173) |
| `decoder_mutex_` | 解码器实例（并发解码互斥） | [audio_service.h:151](main/audio/audio_service.h#L151)、[.cc:395](main/audio/audio_service.cc#L395) |
| `input_resampler_mutex_` | 输入重采样器 | [audio_service.h:152](main/audio/audio_service.h#L152) |
| `input_buffer_mutex_` | 唤醒词 / AEC 输入 PCM 缓冲 | [afe_wake_word.h:50](main/audio/wake_words/afe_wake_word.h#L50)、[custom_wake_word.h:59](main/audio/wake_words/custom_wake_word.h#L59)、[afe_audio_processor.h:46](main/audio/processors/afe_audio_processor.h#L46)、[esp_wake_word.h:44](main/audio/wake_words/esp_wake_word.h#L44) |
| `wake_word_mutex_` | 唤醒词编码结果缓冲 | [afe_wake_word.h:57](main/audio/wake_words/afe_wake_word.h#L57)、[custom_wake_word.h:67](main/audio/wake_words/custom_wake_word.h#L67) |
| `channel_mutex_` | MQTT 协议通道状态 | [mqtt_protocol.h:57](main/protocols/mqtt_protocol.h#L57)、[.cc:196](main/protocols/mqtt_protocol.cc#L196) |
| `data_if_mutex_` | 音频 codec 数据接口 | [es8311_audio_codec.h:25](main/audio/codecs/es8311_audio_codec.h#L25)、[box_audio_codec.h:24](main/audio/codecs/box_audio_codec.h#L24)、[no_audio_codec.h:14](main/audio/codecs/no_audio_codec.h#L14) |
| LED 驱动各自的 `mutex_` | LED 亮度 / 状态寄存器 | [gpio_led.h:27](main/led/gpio_led.h#L27)、[circular_strip.h:35](main/led/circular_strip.h#L35)、[single_led.h:20](main/led/single_led.h#L20) |

统一用 RAII 锁（`std::lock_guard` / `std::unique_lock`）加锁，保证异常安全、避免忘记解锁。

### 5.2 条件变量 `std::condition_variable`

| 条件变量 | 语义 | 位置 |
| --- | --- | --- |
| `audio_queue_cv_` | 播放/编解码任务阻塞等待对应队列非空，`wait(lock, predicate)` 带谓词防虚假唤醒 | [audio_service.h:174](main/audio/audio_service.h#L174)、[.cc:322](main/audio/audio_service.cc#L322)、[:359](main/audio/audio_service.cc#L359) |
| `wake_word_cv_` | 唤醒词编码任务完成后通知消费者取结果 | [afe_wake_word.h:58](main/audio/wake_words/afe_wake_word.h#L58)、[.cc:256](main/audio/wake_words/afe_wake_word.cc#L256) |

注意 `main_tasks_` 闭包队列**不配条件变量**：它的唤醒靠主循环的 `xEventGroupWaitBits` 事件位，闭包只在主任务单线程消费（[application.cc:317](main/application.cc#L317)）。

### 5.3 C++ 数据队列（FreeRTOS 队列的替代）

音频数据通路刻意不用 `xQueue`，改用 `std::deque` + 互斥锁 + 条件变量，并**用 `std::unique_ptr` 转移所有权实现零拷贝**（避免整个 `AudioStreamPacket`/`AudioTask` 被复制）：

| 队列 | 元素 | 用途 | 位置 |
| --- | --- | --- | --- |
| `audio_encode_queue_` / `audio_playback_queue_` | `std::unique_ptr<AudioTask>` | 采集 → 编码 / 解码 → 播放 | [audio_service.h:178](main/audio/audio_service.h#L178)、[:179](main/audio/audio_service.h#L179) |
| `audio_decode_queue_` / `audio_send_queue_` / `audio_testing_queue_` | `std::unique_ptr<AudioStreamPacket>` | 下行解码 / 上行发送 / 录音测试 | [audio_service.h:175](main/audio/audio_service.h#L175)、[:176](main/audio/audio_service.h#L176)、[:177](main/audio/audio_service.h#L177) |
| `timestamp_queue_` | `std::deque<uint32_t>` | 时间戳配对 | [audio_service.h:181](main/audio/audio_service.h#L181) |
| `main_tasks_` | `std::deque<std::function<void()>>` | 主任务闭包队列（跨任务回调回投主线程） | [application.h:165](main/application.h#L165) |
| `wake_word_pcm_` / `wake_word_opus_` | `std::deque<std::vector<...>>` | 唤醒前滚 PCM / 编码后 Opus 包 | [afe_wake_word.h:55](main/audio/wake_words/afe_wake_word.h#L55)、[:56](main/audio/wake_words/afe_wake_word.h#L56) |

### 5.4 原子变量 `std::atomic`（无锁共享状态）

多任务间只读/置位的**轻量状态**用原子变量，避免加锁：

| 原子量 | 语义 | 位置 |
| --- | --- | --- |
| 通话状态与统计（`state_`、`end_reason_`、`data_channel_ready_`、`t_start_`/`t_paired_`/`t_connected_`/`last_talk_ms_`、`n_calls_`/`n_connected_`/`n_connect_failed_`/`n_peer_disconnected_`） | WebRTC 状态机与计时计数，全部 32 位原子——避免 Xtensa 上 64 位原子引入额外锁和辅助库 | [webrtc_call_service.h:98-113](main/webrtc/webrtc_call_service.h#L98-L113) |
| `current_state_` | 设备状态机当前状态 | [device_state_machine.h:63](main/device_state_machine.h#L63) |
| `conversation_suspended_` | 通话期间助手对话挂起标志 | [application.h:182](main/application.h#L182) |
| `voice_detected_` / `voice_processing_output_enabled_` | 打断检测的人声标志与上行使能 | [audio_service.h:185](main/audio/audio_service.h#L185)、[:187](main/audio/audio_service.h#L187) |
| `is_speaking_` / `is_running_` / `running_` | AEC 处理器 / 无处理器 / 唤醒词的运行状态 | [afe_audio_processor.h:44](main/audio/processors/afe_audio_processor.h#L44)、[esp_wake_word.h:39](main/audio/wake_words/esp_wake_word.h#L39) |
| `shared_ptr<atomic<bool>> alive_` | MQTT 会话存活标志（可被多个对象共享） | [mqtt_protocol.h:49](main/protocols/mqtt_protocol.h#L49) |
| `busy_` | 门锁忙标志（避免重复触发脉冲） | [door_lock.h:68](main/webrtc/doorbell/door_lock.h#L68) |

### 5.5 线程 `std::thread`

- 摄像头 JPEG 编码线程：`std::thread`（[esp32_camera.cc:199](main/boards/common/esp32_camera.cc#L199)）。在 ESP-IDF 中 `std::thread` 底层就是 pthread → FreeRTOS 任务，与 `xTaskCreate` 创建的任务同属 FreeRTOS 调度器管理。

### 5.6 C++ 与 FreeRTOS 的分工

| 职责 | 选型 | 理由 |
| --- | --- | --- |
| 事件通知 / 跨任务唤醒 | FreeRTOS 事件组、任务通知 | 内核级阻塞语义、ISR 安全 |
| 大数据传递 | C++ `deque` + `unique_ptr` | 所有权转移零拷贝，接口顺手 |
| 互斥 / 条件同步 | C++ `std::mutex` / `condition_variable` | RAII 异常安全，谓词等待 |
| 轻量共享状态 | C++ `std::atomic` | 免锁、免上下文切换 |

## 6. 近期改动与待加强点

### 6.1 本次优化改动记录（2026-08）

1. **`Suspend()` 固定 50 ms 延时 → 任务退出确认**（[audio_service.cc:194](main/audio/audio_service.cc#L194)）
   - 原实现：`Stop()` 后 `vTaskDelay(50)` 硬等任务退出，是隐式时序假设——任务读 codec 卡住时 50 ms 内未退，WebRTC 就会打开被占用的设备。
   - 现实现：轮询 `eTaskGetState(input/output_task_handle) == eDeleted` 确认 codec 的直接使用者已退出，500 ms 超时兜底后照常移交。
   - 关联：本条即 6.3 中待加强点的落地，解决了 `AudioService::Suspend()` 的固定延时问题。

2. **`signal_hdlr` 100 ms 轮询 → 心跳定时器入队 + 任务纯阻塞**（[webrtc_http_server.c](main/webrtc/signaling/http_local/webrtc_http_server.c) 的 `signaling_msg_send_task`）
   - 原实现：`xQueueReceive` 带 100 ms 超时轮询，任务内比对墙钟做 5 s 心跳，心跳时机被轮询窗口量化。
   - 现实现：周期 `esp_timer`（5 s）把心跳 JSON 投递进 `signaling_queue`，任务 `xQueueReceive(portMAX_DELAY)` 纯阻塞；心跳与信令统一由任务发送，避免在 esp_timer 任务上做网络 I/O。deinit 用 NULL 哨兵唤醒阻塞任务退出。

3. **唤醒词前滚窗口写死"约 2 秒" → 按实际样本数裁剪**（[afe_wake_word.cc:163](main/audio/wake_words/afe_wake_word.cc#L163)、[custom_wake_word.cc:209](main/audio/wake_words/custom_wake_word.cc#L209)）
   - 原实现：`wake_word_pcm_.size() > 2000 / 30`，30 ms/块是"512 点、16 kHz"的写死假设，换模型/换 AFE chunksize 时窗口时长会漂。
   - 现实现：新增 `wake_word_samples_` 累计样本数，按 `16000 × 2` 精确裁剪；编码任务结束后归零。

### 6.2 评估后未做的改动（读码后判定）

- **WebRTC 资源所有权 RAII 守卫**：读完整 `webrtc_call_service.cc` 后判定，现有 `FailStart() → Cleanup()` 幂等单一出口 + `camera_released_` 标志已经实现了"任何失败分支都归还资源"的目标，强行 RAII 化是过度工程，不做。
- **摄像头 preview 双缓冲**：读码确认 `LvglAllocatedImage` 析构会 `heap_caps_free(buffer)`（[lvgl_image.cc:59](main/display/lvgl_display/lvgl_image.cc#L59)），且 `LcdDisplay::SetPreviewImage` 替换时释放旧图（[lcd_display.cc:898](main/display/lcd_display.cc#L898)），复用一个固定缓冲会 UAF；双缓冲需协调显示端生命周期，成本收益不成比例，不做。

### 6.3 仍待加强

- `AudioService::Suspend()` 的任务退出确认目前用 `eTaskGetState` 轮询（见 6.1.1），可进一步改为"任务退出时置位事件组位 + 事件组阻塞等待"，消除忙等，语义与全项目"事件组通知"模式一致。

## 7. 备注：第三方组件内部任务

- `esp_webrtc` / `esp_peer`（vendored，仅 `CONFIG_USE_WEBRTC_CALL` 开启时进入依赖图）：内部有独立的 ICE/媒体传输任务。
- ESP-IDF 系统任务：`esp_timer` 定时器服务、`ipc0/ipc1` 跨核 IPC、`esp_netif`/LWIP TCP/IP、Wi-Fi/BT 驱动任务、`esp_websocket_client` 与 MQTT client 各自的收发任务。
- 唤醒词引擎（ESP-SR AFE）内部按 `afe_perferred_core / afe_perferred_priority` 提示的调度偏好由库自行处理。
