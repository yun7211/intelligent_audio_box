#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

// 设备全局业务状态。状态值只描述“当前处于什么阶段”，合法跳转由 DeviceStateMachine 统一约束。
enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError
};

#endif // _DEVICE_STATE_H_ 
