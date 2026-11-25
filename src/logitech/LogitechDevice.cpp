#include "LogitechDevice.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <vector>
#include <cwctype>

// hidclass.h 只声明了这个 GUID，这里给一份定义，免得依赖 INITGUID
static const GUID kHidInterfaceGuid = { 0x4D1E55B2, 0xF16F, 0x11CF,
                                        { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

namespace {

constexpr BYTE kReportLong = 0x11;      // HID++ 长报文
constexpr WORD kFeatureVoltage = 0x1000; // BATTERY_VOLTAGE

// 请求前把积压的输入帧读掉，避免把上一条响应当成这一次的
void Drain(HANDLE h, int timeout_ms)
{
    for (int i = 0; i < 8; ++i)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BYTE tmp[64]{};
        DWORD rd = 0;
        if (!ReadFile(h, tmp, sizeof(tmp), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
        {
            CloseHandle(ov.hEvent);
            return;
        }
        if (WaitForSingleObject(ov.hEvent, timeout_ms / 4 > 0 ? timeout_ms / 4 : 20) != WAIT_OBJECT_0)
        {
            CancelIo(h);
            CloseHandle(ov.hEvent);
            return;
        }
        GetOverlappedResult(h, &ov, &rd, FALSE);
        CloseHandle(ov.hEvent);
        if (rd == 0)
            return;
    }
}

} // namespace

LogitechDevice::LogitechDevice() = default;

LogitechDevice::~LogitechDevice()
{
    Close();
}

void LogitechDevice::Close()
{
    if (m_handle)
    {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
    m_slot = 0;
    m_feature = 0;
}

bool LogitechDevice::Open()
{
    Close();

    HDEVINFO info = SetupDiGetClassDevsW(&kHidInterfaceGuid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        return false;

    bool opened = false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &kHidInterfaceGuid, i, &ifd); ++i)
    {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;

        std::vector<BYTE> buf(needed);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, needed, nullptr, nullptr))
            continue;

        // 只认罗技：其他厂商的 HID 集合不必逐个开句柄
        std::wstring path = detail->DevicePath;
        std::wstring lower = path;
        for (auto& ch : lower)
            ch = (wchar_t)towlower(ch);
        if (lower.find(L"vid_046d") == std::wstring::npos)
            continue;

        if (OpenEndpoint(path.c_str()))
        {
            // 这里只探测一次；失败也不要紧——鼠标可能在休眠，
            // 真正的槽位探测放到 QueryVoltage 里每轮重试
            FindBatterySlot(150);
            opened = true;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    return opened;
}

bool LogitechDevice::OpenEndpoint(const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    HIDP_CAPS caps{};
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(h, &pp))
    {
        HidP_GetCaps(pp, &caps);
        HidD_FreePreparsedData(pp);
    }

    // HID++ 长报文端点：20 字节；短报文端点（7 字节）设备不应答
    if (caps.OutputReportByteLength != 20 || caps.InputReportByteLength < 20)
    {
        CloseHandle(h);
        return false;
    }

    HIDD_ATTRIBUTES attr{};
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != 0x046D)
    {
        CloseHandle(h);
        return false;
    }

    if (m_handle)
        CloseHandle(m_handle);
    m_handle = h;
    return true;
}

bool LogitechDevice::FindBatterySlot(int timeout_ms)
{
    // 优先 1、2 号槽：单设备接收器几乎都接在 1 号，全扫 6 个会白等 3 倍的超时
    static const BYTE kSlots[] = { 1, 2, 3, 4, 5, 6 };
    for (BYTE slot : kSlots)
    {
        const BYTE params[2] = { (BYTE)(kFeatureVoltage >> 8), (BYTE)(kFeatureVoltage & 0xFF) };
        BYTE resp[16]{};
        int len = 0;
        if (!Request((BYTE)slot, 0x00, 0x00, params, 2, resp, &len, timeout_ms))
            continue;
        if (len < 1 || resp[0] == 0)
            continue;

        m_slot = (BYTE)slot;
        m_feature = resp[0];
        return true;
    }
    m_slot = 0;
    m_feature = 0;
    return false;
}

bool LogitechDevice::Request(BYTE dev, BYTE feature, BYTE function, const BYTE* params, int plen,
                             BYTE* resp, int* rlen, int timeout_ms)
{
    if (!m_handle)
        return false;

    Drain(m_handle, timeout_ms);

    BYTE out[20]{};
    out[0] = kReportLong;
    out[1] = dev;
    out[2] = feature;
    out[3] = (BYTE)((function << 4) | 1); // swId 固定 1
    if (params && plen > 0)
    {
        if (plen > 16)
            plen = 16;
        for (int i = 0; i < plen; ++i)
            out[4 + i] = params[i];
    }

    OVERLAPPED wov{};
    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    if (!WriteFile(m_handle, out, sizeof(out), nullptr, &wov) && GetLastError() != ERROR_IO_PENDING)
    {
        CloseHandle(wov.hEvent);
        return false;
    }
    if (WaitForSingleObject(wov.hEvent, timeout_ms) != WAIT_OBJECT_0)
    {
        CancelIo(m_handle);
        CloseHandle(wov.hEvent);
        return false;
    }
    GetOverlappedResult(m_handle, &wov, &written, FALSE);
    CloseHandle(wov.hEvent);

    // 只接受与本次请求对得上的帧；其余（上一条残留、广播通知）丢掉
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        OVERLAPPED rov{};
        rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BYTE in[64]{};
        DWORD read = 0;
        if (!ReadFile(m_handle, in, sizeof(in), nullptr, &rov) && GetLastError() != ERROR_IO_PENDING)
        {
            CloseHandle(rov.hEvent);
            return false;
        }
        if (WaitForSingleObject(rov.hEvent, timeout_ms) != WAIT_OBJECT_0)
        {
            CancelIo(m_handle);
            CloseHandle(rov.hEvent);
            return false;
        }
        GetOverlappedResult(m_handle, &rov, &read, FALSE);
        CloseHandle(rov.hEvent);

        if (read < 6 || in[0] != kReportLong || in[1] != out[1])
            continue;
        if (in[2] == 0xFF) // HID++ 错误响应
            return false;
        if (in[2] != out[2] || (in[3] >> 4) != function)
            continue;

        const int n = (int)read - 4;
        if (rlen)
            *rlen = n > 16 ? 16 : n;
        if (resp)
        {
            for (int i = 0; i < n && i < 16; ++i)
                resp[i] = in[4 + i];
        }
        return true;
    }
    return false;
}

bool LogitechDevice::QueryVoltage(BatteryReading& out, int timeout_ms)
{
    if (!m_handle)
        return false;

    // 实测：设备刚被唤醒时头一两条请求会丢（接收器还没转发到设备），
    // 单轮查询经常白跑，所以这里连续试三次
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (!m_slot || !m_feature)
        {
            if (!FindBatterySlot(timeout_ms))
                continue;
        }

        BYTE resp[16]{};
        int len = 0;
        if (Request(m_slot, m_feature, 0x00, nullptr, 0, resp, &len, timeout_ms) && len >= 2)
        {
            const int mv = resp[0] | (resp[1] << 8);
            // 合理区间：AA 约 1.0~1.7 V，锂电最高约 4.3 V
            if (mv >= 800 && mv <= 5000)
            {
                out.valid = true;
                out.percent = -1; // 只报电压，不换算百分比
                out.millivolt = mv;
                out.charging = false;
                return true;
            }
        }
        Sleep(30);
    }
    return false;
}
