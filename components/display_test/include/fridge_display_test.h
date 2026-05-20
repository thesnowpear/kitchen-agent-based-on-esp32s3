// 冰箱小精灵屏幕点亮测试公共接口。
// 该组件只用于 TS040HDS02CP-B1620A/TR230S 的 QSPI 安全点亮验证，不初始化触摸、背光 PWM 或业务网络。

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 运行独立屏幕测试。
// 注意：函数会循环显示测试图案；如果 WAIT# 长期为低或总线初始化失败，会停止发送 QSPI 并停留在错误日志状态。
void fridge_display_test_run(void);

#ifdef __cplusplus
}
#endif
