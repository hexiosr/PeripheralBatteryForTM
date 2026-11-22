#pragma once
// ============================================================
// G7Reader.h
// G7 Pro 后台监听线程：持续只读旁观 \\.\XboxGIP 总线，
// 解析设备主动上报的 Status 消息（电量档位 + 充电标志）。
// 短暂无消息保持最后读数；90 秒无消息视为手柄断开（显示"未连接"），
// 设备唤醒/插拔会自动收到新消息而更新。
// 线程启停与快照加锁由 BatteryReader 统一实现
// ============================================================
#include "G7Device.h"
#include "BatteryReader.h"

class G7Reader : public BatteryReader
{
public:
    ~G7Reader() override
    {
        Stop();       // 先发退出信号
        JoinWorker(); // 再等线程退出：之后析构期间不会再回到 WorkerLoop
    }

protected:
    void WorkerLoop() override;
};
