#ifndef WEBRTC_MCP_TOOLS_H
#define WEBRTC_MCP_TOOLS_H

// 注册 WebRTC 通话控制的 MCP 工具（self.video_call.*）。
// 在 MCP 初始化期间调用一次即可；**门控写在调用点**：
//   #if CONFIG_USE_WEBRTC_CALL
// 这样功能关闭时一个工具都不会注册 —— 声明本身不需要门控，因为关闭态下这个翻译单元
// 压根不参与编译（见 main/CMakeLists.txt）。
void RegisterWebrtcMcpTools();

#endif // WEBRTC_MCP_TOOLS_H
