#pragma once
// ============================================================
// MouseReader.h
// 鼠标后台线程：按需采集（初次/唤醒/使用中每 5 秒刷新）+ 鼠标活动检测 + 断线重连
// 无周期轮询 -> 不会唤醒休眠中的鼠标/接收器
// 线程启停与快照加锁由 BatteryReader 统一实现
// ============================================================
#include "MouseDevice.h"
#include "BatteryReader.h"

class MouseReader : public BatteryReader
{
public:
    ~MouseReader() override
    {
        Stop();       // 先发退出信号
        JoinWorker(); // 再等线程退出：之后析构期间不会再回到 WorkerLoop
    }

protected:
    void WorkerLoop() override;
};
