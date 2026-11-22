#pragma once
// ============================================================
// DualSenseReader.h
// PS5 DualSense 后台读取线程
// DS5Dongle 桥接后手柄为 USB 形态（持续供电），无休眠顾虑，
// 采用固定间隔查询（每 60 秒一次，电量变化慢，无需高频）
// 线程启停与快照加锁由 BatteryReader 统一实现
// ============================================================
#include "DualSenseDevice.h"
#include "BatteryReader.h"

class DualSenseReader : public BatteryReader
{
public:
    ~DualSenseReader() override
    {
        Stop();       // 先发退出信号
        JoinWorker(); // 再等线程退出：之后析构期间不会再回到 WorkerLoop
    }

protected:
    void WorkerLoop() override;
};
