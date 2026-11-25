#pragma once
// ============================================================
// LogitechDevice.h
// 罗技 2.4G 鼠标电量读取（HID++ 2.0，只报电压）
//
// 实测（VID_046D & PID_C52F "USB Receiver"，M230 接在设备槽 1）：
//   ① 打开 MI_01 里 OutputReportByteLength = 20 的 vendor 集合
//      （短报文集合 7 字节，设备不应答，必须用长报文 0x11）
//   ② root feature(0x00) function 0 查 BATTERY_VOLTAGE(0x1000) 的索引
//   ③ 对该索引发 function 0，响应前两字节是电压毫伏（小端）
//
// 注意：
//  - 设备空闲（深度休眠）时接收器不回包，必须靠超时跳过这一轮，
//    不能当成"设备离线"——鼠标一动就能读到。
//  - 电压不是百分比：碱性 AA 满电约 1.60 V，耗尽约 1.10 V。
// ============================================================
#include <windows.h>
#include <string>
#include "BatteryCommon.h"

class LogitechDevice
{
public:
    LogitechDevice();
    ~LogitechDevice();

    // 扫描罗技接收器的 HID++ 长报文端点并确认设备槽支持电压 feature
    bool Open();

    void Close();

    bool IsOpen() const { return m_handle != nullptr; }

    // 读一次电压（毫伏）；超时返回 false，不影响已持有的句柄
    bool QueryVoltage(BatteryReading& out, int timeout_ms);

private:
    bool OpenEndpoint(const wchar_t* path);
    bool FindBatterySlot(int timeout_ms);
    bool Request(BYTE dev, BYTE feature, BYTE function, const BYTE* params, int plen,
                 BYTE* resp, int* rlen, int timeout_ms);

private:
    HANDLE m_handle = nullptr;
    BYTE m_slot = 0;     // 接收器里的设备槽（1..6）
    BYTE m_feature = 0;  // BATTERY_VOLTAGE 的 feature 索引
};
