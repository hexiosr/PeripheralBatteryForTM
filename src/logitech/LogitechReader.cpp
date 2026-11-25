#include "LogitechReader.h"
#include "PluginConfig.h"
#include <windows.h>

// 光标位移即视为"设备被操作过"：罗技鼠标只在活动后的一小段时间内应答，
// 靠定时轮询很容易错过这个窗口
static bool CursorMoved(POINT& last)
{
    POINT pt{};
    GetCursorPos(&pt);
    const bool moved = (pt.x != last.x || pt.y != last.y);
    last = pt;
    return moved;
}

void LogitechReader::WorkerLoop()
{
    POINT last{};
    GetCursorPos(&last);
    ULONGLONG last_query = 0;

    while (!m_stop)
    {
        const RefreshConfig cfg = PluginConfig::Instance().Refresh();
        const bool moved = CursorMoved(last);
        const ULONGLONG now = GetTickCount64();

        // 鼠标刚动过 → 立刻查一次（设备此时被唤醒，才会回包）；
        // 空闲时按「查询间隔」兜底，避免一直朝睡着的设备发请求
        const bool due = (last_query == 0) || (now - last_query >= (ULONGLONG)cfg.lg_poll_ms);
        const bool trigger = (moved && now - last_query >= 800) || due;

        if (trigger)
        {
            if (!m_device.IsOpen() && !m_device.Open())
            {
                last_query = now;
                Sleep(120);
                continue;
            }

            MarkDeviceFound();
            BatteryReading reading;
            // 应答超时固定 250ms：鼠标休眠时接收器不回包，等太久只是白等
            if (m_device.QueryVoltage(reading, 250))
                UpdateSnapshot(reading, true, GetTickCount64());
            // 读不到：保留上次读数，鼠标一动就会刷新
            last_query = GetTickCount64();
        }

        // 空闲 tick：只做光标位移检测，不发任何请求
        Sleep(80);
    }

    m_device.Close();
}
