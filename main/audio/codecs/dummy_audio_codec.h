#ifndef _DUMMY_AUDIO_CODEC_H
#define _DUMMY_AUDIO_CODEC_H

#include "audio_codec.h"

// 无实际硬件的占位 codec：保留音频框架接口，但读写均不产生数据。
// 主要用于没有音频外设的板型或只需跑通上层流程的场景。
class DummyAudioCodec : public AudioCodec {
private:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    DummyAudioCodec(int input_sample_rate, int output_sample_rate);
    virtual ~DummyAudioCodec();
};

#endif // _DUMMY_AUDIO_CODEC_H
