#pragma once
// ============================================================
// BatteryCommon.h —— 键盘 / 鼠标两个设备共用的数据结构
// ============================================================
#include <windows.h>
#include <atomic>
#include <string>

// 单次电量读数（键鼠 0x13 / 0x11 协议、罗技 HID++ 电压共用）
struct BatteryReading
{
    bool   valid = false;    // 是否有有效读数
    int    percent = -1;     // 电量百分比 0~100
    int    millivolt = 0;    // 电压（mV）；只报电压的设备用它代替百分比
    bool   charging = false; // 是否充电中
};

// 供 UI 侧读取的快照（每个 Reader 各自维护一份）
struct BatterySnapshot
{
    bool   has_device = false;   // 是否找到并打开过设备
    bool   online = false;       // 最近查询是否成功
    bool   valid = false;        // 是否有有效读数
    int    percent = -1;         // 电量百分比
    int    millivolt = 0;        // 电压（mV）
    bool   charging = false;     // 充电中
    ULONGLONG last_update_ms = 0;// 最近一次成功读数时间（GetTickCount64）
};

// 可中断等待：stop 置位后提前返回。后台线程里的长延迟（重连重试等）
// 用它代替 Sleep，保证 Stop() 之后线程能尽快自行退出
// （join 在下次 Start() 或析构时做，UI 线程不等它）。
inline void SleepInterruptible(const std::atomic<bool>& stop, DWORD total_ms)
{
    for (DWORD waited = 0; waited < total_ms && !stop.load(); waited += 50)
        Sleep(50);
}
