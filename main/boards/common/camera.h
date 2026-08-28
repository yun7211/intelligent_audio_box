#ifndef CAMERA_H
#define CAMERA_H

#include <string>

// 摄像头业务抽象：本地抓拍/识图与 WebRTC 视频通话共享同一外设。
class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // 可选的 RGB565 字节序调整
    virtual std::string Explain(const std::string& question) = 0;

    // WebRTC 通话需要独占摄像头时先 Release()，结束后 Reacquire()；默认实现为空，
    // 共享外设的具体摄像头必须覆写。
    virtual void Release() {}
    virtual void Reacquire() {}
};

#endif // CAMERA_H
