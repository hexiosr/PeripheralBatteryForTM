#pragma once
// ============================================================
// LogitechReader.h
// 罗技鼠标的后台线程：按固定间隔读电压
//
// 与键鼠不同，这里必须"问"才能拿到值——设备休眠时接收器直接不回包，
// 所以间隔不宜过短（默认 2 秒）；读不到只说明它在睡，不清读数。
// ============================================================
#include "BatteryReader.h"
#include "LogitechDevice.h"

class LogitechReader : public BatteryReader
{
public:
    ~LogitechReader() override
    {
        Stop();
        JoinWorker();
    }

protected:
    void WorkerLoop() override;

private:
    LogitechDevice m_device;
};
