#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// 立创开发板硬件映射：音频、显示和 DVP 摄像头驱动只读取这些宏，不在业务层写死引脚。
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

/* WebRTC 设备端 AEC：ES7210 的 MIC1/MIC2 是近端麦克风，MIC3 接收经过耦合、
 * 分压后的 ES8311 播放参考。I2S 使用 4 个物理 TDM 时隙，但 AFE 只取前三路，
 * 因此布局是 MMR、有效时隙掩码是 0b0111。 */
#define WEBRTC_AEC_MIC_LAYOUT   "MMR"
#define WEBRTC_AEC_CHANNELS     4
#define WEBRTC_AEC_CHANNEL_MASK 0x07

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_38
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_12
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_45

#define AUDIO_CODEC_USE_PCA9557
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  0x82

#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY true

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

/* DVP 摄像头数据、同步与控制引脚。 */
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK GPIO_NUM_5
#define CAMERA_PIN_SIOD GPIO_NUM_1
#define CAMERA_PIN_SIOC GPIO_NUM_2

#define CAMERA_PIN_D7 GPIO_NUM_9
#define CAMERA_PIN_D6 GPIO_NUM_4
#define CAMERA_PIN_D5 GPIO_NUM_6
#define CAMERA_PIN_D4 GPIO_NUM_15
#define CAMERA_PIN_D3 GPIO_NUM_17
#define CAMERA_PIN_D2 GPIO_NUM_8
#define CAMERA_PIN_D1 GPIO_NUM_18
#define CAMERA_PIN_D0 GPIO_NUM_16
#define CAMERA_PIN_VSYNC GPIO_NUM_3
#define CAMERA_PIN_HREF GPIO_NUM_46
#define CAMERA_PIN_PCLK GPIO_NUM_7

#define XCLK_FREQ_HZ 20000000

/* 远程开门用的门锁控制脚（门铃模式，见 webrtc/doorbell/door_lock.h）。
 * GPIO21 在本板上未被音频、显示或摄像头占用。
 *
 * 这里只输出**逻辑控制信号**：实际电锁或继电器线圈必须经继电器模块 / MOSFET / 三极管
 * 驱动，并带续流保护，不能由 ESP32 的 GPIO 直接供电。驱动侧还应加**外部下拉**电阻，
 * 覆盖复位期间引脚处于高阻的那段时间。 */
#define DOOR_LOCK_GPIO         GPIO_NUM_21
#define DOOR_LOCK_ACTIVE_LEVEL 1
#define DOOR_LOCK_PULSE_MS     800

#endif // _BOARD_CONFIG_H_
