#pragma once
// ============================================================
// BatteryReader.h —— 四类设备读取线程的公共骨架
//  - IBatteryReader：Plugin 只按设备索引操作读取器，
//    新增设备不必再改 SyncReaders 里的 switch
//  - BatteryReader：线程启停、快照加锁、设备在位/离线标记的统一实现
//    （原来四份 Reader 各抄一遍，改一处容易漏三处）
//
// 启停语义（重要）：
//  - Start()：先回收上一轮线程，再复位停止位并开新线程。
//    复位必须在 join 之后，否则旧线程可能因为停止位被清零而永不退出。
//  - Stop()：只置停止位，不 join。DataRequired / 选项对话框都在 UI 线程上，
//    在那里 join 会把任务栏卡住几百毫秒；join 推迟到下次 Start 或析构。
// ============================================================
#include "BatteryCommon.h"
#include <atomic>
#include <mutex>
#include <thread>

class IBatteryReader
{
public:
    virtual ~IBatteryReader() = default;

    virtual void Start() = 0;                       // 启动后台线程（已在运行则不做任何事）
    virtual void Stop() = 0;                        // 只发退出信号，不等待线程结束
    virtual BatterySnapshot GetSnapshot() const = 0;
};

class BatteryReader : public IBatteryReader
{
public:
    ~BatteryReader() override
    {
        // 兜底：派生类析构里应已 Stop + JoinWorker，这里再收一次（join 已返回则无动作）
        m_stop = true;
        JoinWorker();
    }

    void Start() override
    {
        if (m_thread.joinable() && !m_stop.load())
            return; // 已在运行
        JoinWorker();       // 回收上一轮：它已收到停止信号，通常几十毫秒内退出
        m_stop = false;     // 必须在 join 之后复位
        m_thread = std::thread([this] { WorkerLoop(); });
    }

    void Stop() override
    {
        m_stop = true;      // 只发信号，join 交给 Start / 析构
    }

    BatterySnapshot GetSnapshot() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_snapshot;
    }

protected:
    virtual void WorkerLoop() = 0; // 由各设备实现

    // 等待线程退出（派生类析构里调用，保证析构完成前 WorkerLoop 已返回）
    void JoinWorker()
    {
        if (m_thread.joinable())
            m_thread.join();
    }

    // 有有效读数：更新电量并刷新时间戳；online 由调用方给（查询成功即 true）
    void UpdateSnapshot(const BatteryReading& reading, bool online, ULONGLONG now_ms)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.online = online;
        if (reading.valid)
        {
            m_snapshot.valid = true;
            m_snapshot.percent = reading.percent;
            m_snapshot.charging = reading.charging;
            m_snapshot.last_update_ms = now_ms;
        }
    }

    // 确认断开（手柄链路断开 / 设备从系统消失）：显示 --，不再保留末值
    void MarkOffline()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.valid = false;
        m_snapshot.online = false;
    }

    // 首次成功打开设备：之后才认为「找到过设备」
    void MarkDeviceFound()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.has_device = true;
    }

    // 线程与快照：派生类 WorkerLoop 用 m_stop 判断退出，用上面的成员函数改快照
protected:
    std::atomic<bool> m_stop{ false };
    std::thread m_thread;
    mutable std::mutex m_mutex;
    BatterySnapshot m_snapshot;
};
