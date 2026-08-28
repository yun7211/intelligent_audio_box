#include "dummy_audio_codec.h"

DummyAudioCodec::DummyAudioCodec(int input_sample_rate, int output_sample_rate) {
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
}

DummyAudioCodec::~DummyAudioCodec() {
}

int DummyAudioCodec::Read(int16_t* dest, int samples) {
    // 没有输入设备，因此始终报告未读取到样本。
    return 0;
}

int DummyAudioCodec::Write(const int16_t* data, int samples) {
    // 丢弃输出数据，使调用方无需针对无声板型增加特殊分支。
    return 0;
}
