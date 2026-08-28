#pragma once

#include <vector>
#include <deque>
#include <string>
#include <memory>
#include <optional>
#include <cmath>
#include "wifi_manager.h"
#include "application.h"

// 声波配网采用 AFSK：1800 Hz 表示 Mark（1），1500 Hz 表示 Space（0），以 100 bit/s 传输。
// 采样、频率检测、比特组帧和凭据解析构成一条完整的“音频 -> Wi-Fi 配置”链路。
const size_t kAudioSampleRate = 6400;
const size_t kMarkFrequency = 1800;
const size_t kSpaceFrequency = 1500;
const size_t kBitRate = 100;
const size_t kWindowSize = 64;

namespace audio_wifi_config
{
    // 配网入口：持续读取麦克风 PCM，解出 SSID/密码后交给 WifiManager 建立连接。
    void ReceiveWifiCredentialsFromAudio(Application *app, WifiManager *wifi_manager, Display *display, 
                                         size_t input_channels = 1);

    /**
     * Goertzel 单频检测器：用较低计算量估算一个分析窗口内目标频率的幅度。
     * AFSK 解调会各建一个实例，分别跟踪 Mark 和 Space 频率。
     */
    class FrequencyDetector
    {
    private:
        float frequency_;              // 归一化目标频率 f/fs
        size_t window_size_;           // 单次分析窗口的样本数
        float frequency_bin_;          // 对应的离散频率位置
        float angular_frequency_;      // 角频率
        float cos_coefficient_;        // cos(w)
        float sin_coefficient_;        // sin(w)
        float filter_coefficient_;     // 递推系数 2*cos(w)
        std::deque<float> state_buffer_;  // 保存递推状态 S[-1]、S[-2]

    public:
        /**
         * Constructor
         * @param frequency Normalized frequency (f / fs)
         * @param window_size Window size for analysis
         */
        FrequencyDetector(float frequency, size_t window_size);

        /**
         * Reset the detector state
         */
        void Reset();

        /**
         * Process one audio sample
         * @param sample Input audio sample
         */
        void ProcessSample(float sample);

        /**
         * Calculate current amplitude
         * @return Amplitude value
         */
        float GetAmplitude() const;
    };

    /**
     * Mark/Space 双频处理器：按窗口比较两种频率的能量，输出每个比特属于 Mark 的概率。
     */
    class AudioSignalProcessor
    {
    private:
        std::deque<float> input_buffer_;             // 跨调用保留的 PCM 样本
        size_t input_buffer_size_;                   // 检测窗口大小
        size_t output_sample_count_;                 // 当前比特已累计的样本数
        size_t samples_per_bit_;                     // 每个数据比特对应的样本数
        std::unique_ptr<FrequencyDetector> mark_detector_;   // 数字 1 检测器
        std::unique_ptr<FrequencyDetector> space_detector_;  // 数字 0 检测器

    public:
        /**
         * Constructor
         * @param sample_rate Audio sampling rate
         * @param mark_frequency Mark frequency for digital '1'
         * @param space_frequency Space frequency for digital '0'
         * @param bit_rate Data transmission bit rate
         * @param window_size Analysis window size
         */
        AudioSignalProcessor(size_t sample_rate, size_t mark_frequency, size_t space_frequency,
                           size_t bit_rate, size_t window_size);

        /**
         * Process input audio samples
         * @param samples Input audio sample vector
         * @return Vector of Mark probability values (0.0 to 1.0)
         */
        std::vector<float> ProcessAudioSamples(const std::vector<float> &samples);
    };

    /**
     * 数据接收状态机：先搜索帧头，确认后收集负载，遇到帧尾完成一次消息。
     */
    enum class DataReceptionState
    {
        kInactive,  // 尚未检测到起始标识
        kWaiting,   // 检测到候选起始序列，等待完整确认
        kReceiving  // 正在接收正文比特
    };

    /**
     * AFSK 数据组帧器：把概率判决成比特，识别起止标识，再转换为文本并可选校验和。
     */
    class AudioDataBuffer
    {
    private:
        DataReceptionState current_state_;       // Current reception state
        std::deque<uint8_t> identifier_buffer_;  // Buffer for start/end identifier detection
        size_t identifier_buffer_size_;          // Identifier buffer size
        std::vector<uint8_t> bit_buffer_;        // Buffer for storing bit stream
        size_t max_bit_buffer_size_;             // Maximum bit buffer size
        const std::vector<uint8_t> start_of_transmission_;  // Start-of-transmission identifier
        const std::vector<uint8_t> end_of_transmission_;    // End-of-transmission identifier
        bool enable_checksum_validation_;       // Whether to validate checksum

    public:
        std::optional<std::string> decoded_text; // 一帧接收完成后保存解出的文本

        /**
         * Default constructor using predefined start and end identifiers
         */
        AudioDataBuffer();

        /**
         * Constructor with custom parameters
         * @param max_byte_size Expected maximum data size in bytes
         * @param start_identifier Start-of-transmission identifier
         * @param end_identifier End-of-transmission identifier
         * @param enable_checksum Whether to enable checksum validation
         */
        AudioDataBuffer(size_t max_byte_size, const std::vector<uint8_t> &start_identifier,
                      const std::vector<uint8_t> &end_identifier, bool enable_checksum = false);

        /**
         * Process probability data and attempt to decode
         * @param probabilities Vector of Mark probabilities
         * @param threshold Decision threshold for bit detection
         * @return true if complete data was successfully received and decoded
         */
        bool ProcessProbabilityData(const std::vector<float> &probabilities, float threshold = 0.5f);

        /**
         * Calculate checksum for ASCII text
         * @param text Input text string
         * @return Checksum value (0-255)
         */
        static uint8_t CalculateChecksum(const std::string &text);

    private:
        /**
         * Convert bit vector to byte vector
         * @param bits Input bit vector
         * @return Converted byte vector
         */
        std::vector<uint8_t> ConvertBitsToBytes(const std::vector<uint8_t> &bits) const;

        /**
         * Clear all buffers and reset state
         */
        void ClearBuffers();
    };

    // 默认帧头和帧尾，发送端必须使用相同模式。
    extern const std::vector<uint8_t> kDefaultStartTransmissionPattern;
    extern const std::vector<uint8_t> kDefaultEndTransmissionPattern;
}
