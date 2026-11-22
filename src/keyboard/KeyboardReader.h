#pragma once
// ============================================================
// KeyboardReader.h
// 键盘后台线程：按需查询（初次/唤醒/使用中每 5 秒刷新）+ 按键检测 + 断线重连
// 不周期查询 -> 不会唤醒休眠中的键盘
// 线程启停与快照加锁由 BatteryReader 统一实现
// ============================================================
#include "KeyboardDevice.h"
#include "BatteryReader.h"

class KeyboardReader : public BatteryReader
{
public:
    ~KeyboardReader() override
    {
        Stop();       // 先发退出信号
        JoinWorker(); // 再等线程退出：之后析构期间不会再回到 WorkerLoop
    }

protected:
    void WorkerLoop() override;
};
