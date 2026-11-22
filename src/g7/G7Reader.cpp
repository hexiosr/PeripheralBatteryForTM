#include "G7Reader.h"
#include "PluginConfig.h"
#include <windows.h>
#include <cstring>

// GIP Status 消息电池字节：低 2 位档位 0~3，bit4 充电标志（逆向实测）
static void ParseStatusBattery(BYTE battery_byte, int& percent, bool& charging)
{
    int level = battery_byte & 0x03;
    charging = (battery_byte & 0x10) != 0;

    switch (level)
    {
    case 0: percent = 0;   break;
    case 1: percent = 33;  break;
    case 2: percent = 66;  break;
    case 3: percent = 100; break;
    default: percent = -1; break;
    }
}

void G7Reader::WorkerLoop()
{
    G7Device device;
    BYTE dev_id[8] = { 0 };          // 当前关注的接收器设备 ID（首个 Status 消息学习）
    ULONGLONG last_msg_ms = 0;       // 最近一次收到该设备消息的时间

    while (!m_stop)
    {
        // 每轮取一次配置快照：玩家在选项里改完间隔后，最迟一个等待周期生效
        const RefreshConfig cfg = PluginConfig::Instance().Refresh();

        if (!device.IsOpen())
        {
            bool ok = device.Open();
            if (!ok)
            {
                // GIP 总线打不开（驱动未就绪/系统无 GIP 设备）→ 保持最后读数，延迟重试
                SleepInterruptible(m_stop, (DWORD)cfg.g7_reconnect_ms);
                continue;
            }
            MarkDeviceFound();
        }

        BYTE buf[512];
        int len = 0;
        ULONGLONG now = GetTickCount64();

        if (!device.ReadMessage(buf, len, (DWORD)cfg.g7_read_timeout_ms))
        {
            // 超时无消息
            if (last_msg_ms != 0 && now - last_msg_ms >= (ULONGLONG)cfg.g7_reset_ms)
                memset(dev_id, 0, sizeof(dev_id)); // 接收器可能被换插，重新学习设备 ID
            // 兜底：长时间无消息 → 视为手柄断开，显示 --（防止漏掉断开标志消息）
            if (last_msg_ms != 0 && now - last_msg_ms >= (ULONGLONG)cfg.g7_offline_ms)
                MarkOffline();
            continue;
        }

        if (len < 24)
            continue;

        BYTE type = buf[8];
        if (type != 0x03) // GipMessageStatus
            continue;

        // 设备 ID 学习/过滤（只认当前接收器）
        if (dev_id[0] == 0)
            memcpy(dev_id, buf, 8);
        else if (memcmp(dev_id, buf, 8) != 0)
            continue; // 其他设备的 Status，忽略

        // Status 消息 24B：12 字节头部 + 12 字节 payload
        // 电池字节 payload[8] = buf[20]（低 2 位档位 0~3，bit4 充电标志）
        // 链路状态字节 payload[11] = buf[23]：
        //   0x28 = 无线链路连接（bit3=1）；0x20 = 断开（bit3=0，实测断开瞬间）
        BYTE battery_byte = buf[20];
        BYTE link_byte    = buf[23];

        if ((link_byte & 0x08) == 0)
        {
            // 手柄断开（关闭/休眠）：显示 --（与其他设备"保持末值"不同，
            // G7 手柄按用户要求断开即显示未连接）
            MarkOffline();
            last_msg_ms = GetTickCount64(); // 刷新活动时间，避免 60s 闲置重置设备 ID
            continue;
        }

        int percent = -1;
        bool charging = false;
        ParseStatusBattery(battery_byte, percent, charging);
        if (percent < 0)
            continue;

        BatteryReading r;
        r.valid = true;
        r.percent = percent;
        r.charging = charging;

        last_msg_ms = GetTickCount64();
        UpdateSnapshot(r, true, GetTickCount64());
    }

    device.Close();
}
