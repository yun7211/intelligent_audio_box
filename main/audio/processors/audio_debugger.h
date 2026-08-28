#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <vector>
#include <cstdint>

#include <sys/socket.h>
#include <netinet/in.h>

// 音频调试辅助类：把采集到的 PCM 通过 UDP 发给局域网分析工具。
// 仅传输调试数据，不参与正常的音频编解码与通信链路。
class AudioDebugger {
public:
    AudioDebugger();
    ~AudioDebugger();

    void Feed(const std::vector<int16_t>& data);

private:
    int udp_sockfd_ = -1;
    struct sockaddr_in udp_server_addr_;
};

#endif 
