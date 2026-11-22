#include "PluginConfig.h"
#include <cstdlib>
#include <cwchar>

namespace {

constexpr const wchar_t* kSection = L"Advanced"; // 刷新策略 / 布局 / 设备名
constexpr int kStyleSlots = 6;                   // 每台设备：3 档电量 × 深/浅两套配色

// 每台设备外观里的整型项（键名 → 成员），加载 / 保存 / 清理共用一张表
struct StyleIntField
{
    const wchar_t* key;
    int DeviceStyle::* member;
    int lo;
    int hi;
};

const StyleIntField kStyleInts[] = {
    { L"level_high",     &DeviceStyle::level_high,     1, 100 },
    { L"level_low",      &DeviceStyle::level_low,      0, 99 },
    { L"ring_scale_x10", &DeviceStyle::ring_scale_x10, 8, 40 },
    { L"num_gap",        &DeviceStyle::num_gap,        0, 20 },
};

struct StyleColorField
{
    const wchar_t* key;
    COLORREF DeviceStyle::* member;
};

const StyleColorField kStyleColors[kStyleSlots] = {
    { L"color_green_dark",   &DeviceStyle::green_dark },
    { L"color_green_light",  &DeviceStyle::green_light },
    { L"color_yellow_dark",  &DeviceStyle::yellow_dark },
    { L"color_yellow_light", &DeviceStyle::yellow_light },
    { L"color_red_dark",     &DeviceStyle::red_dark },
    { L"color_red_light",    &DeviceStyle::red_light },
};

int Clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
        ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        --e;
    return s.substr(b, e - b);
}

// 每台设备的外观存自己的段：Appearance0..Appearance3
std::wstring StyleSection(int dev)
{
    return L"Appearance" + std::to_wstring(dev);
}

bool ParseInt(const wchar_t* buf, int& out)
{
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    if (end == buf)
        return false;
    while (*end == L' ' || *end == L'\t')
        ++end;
    if (*end != L'\0')
        return false;
    out = static_cast<int>(v);
    return true;
}

// ---- 读取：带“键不存在”反馈，供新旧配置回落判断 ----

bool ReadIntIn(const std::wstring& path, const wchar_t* section, const wchar_t* key, int& out)
{
    wchar_t buf[32]{};
    if (GetPrivateProfileStringW(section, key, L"", buf, 32, path.c_str()) == 0)
        return false;
    return ParseInt(buf, out);
}

bool ReadStrIn(const std::wstring& path, const wchar_t* section, const wchar_t* key, std::wstring& out)
{
    wchar_t buf[128]{};
    if (GetPrivateProfileStringW(section, key, L"", buf, 128, path.c_str()) == 0)
        return false;
    out = buf;
    return true;
}

bool ReadColorIn(const std::wstring& path, const wchar_t* section, const wchar_t* key, COLORREF& out)
{
    std::wstring s;
    if (!ReadStrIn(path, section, key, s))
        return false;
    return PluginConfig::ParseColor(s, out);
}

// ---- 读取：全局键（缺省即用默认值）----

std::wstring ReadStrValue(const std::wstring& path, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[128]{};
    DWORD n = GetPrivateProfileStringW(kSection, key, def, buf, 128, path.c_str());
    if (n == 0 && def[0] != L'\0')
        return def;
    return buf;
}

int ReadIntValue(const std::wstring& path, const wchar_t* key, int def, int lo, int hi)
{
    int v = 0;
    if (!ReadIntIn(path, kSection, key, v))
        return def;
    return Clamp(v, lo, hi);
}

// ---- 写入 ----

void WriteInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}

void WriteColor(const std::wstring& path, const wchar_t* section, const wchar_t* key, COLORREF c)
{
    WritePrivateProfileStringW(section, key, PluginConfig::FormatColor(c).c_str(), path.c_str());
}

void DeleteKey(const std::wstring& path, const wchar_t* section, const wchar_t* key)
{
    WritePrivateProfileStringW(section, key, nullptr, path.c_str());
}

} // namespace

PluginConfig& PluginConfig::Instance()
{
    static PluginConfig instance;
    return instance;
}

void PluginConfig::SetIniPath(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = path;
}

void PluginConfig::Load()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.empty())
        return;

    // 先回到默认值，再逐项覆盖；缺失或非法项自动使用默认值
    m_refresh = RefreshConfig{};
    m_layout = LayoutConfig{};
    m_text = TextConfig{};
    for (int i = 0; i < kDeviceCount; ++i)
        m_style[i] = DeviceStyle{};

    const std::wstring& p = m_path;
    m_refresh.kb_wake_ms = ReadIntValue(p, L"kb_wake_ms", m_refresh.kb_wake_ms, 1000, 86400000);
    m_refresh.kb_active_ms = ReadIntValue(p, L"kb_active_ms", m_refresh.kb_active_ms, 1000, 86400000);
    m_refresh.kb_refresh_ms = ReadIntValue(p, L"kb_refresh_ms", m_refresh.kb_refresh_ms, 1000, 86400000);
    m_refresh.kb_cooldown_ms = ReadIntValue(p, L"kb_cooldown_ms", m_refresh.kb_cooldown_ms, 1000, 86400000);
    m_refresh.kb_reconnect_ms = ReadIntValue(p, L"kb_reconnect_ms", m_refresh.kb_reconnect_ms, 1000, 86400000);
    m_refresh.ms_wake_ms = ReadIntValue(p, L"ms_wake_ms", m_refresh.ms_wake_ms, 1000, 86400000);
    m_refresh.ms_active_ms = ReadIntValue(p, L"ms_active_ms", m_refresh.ms_active_ms, 1000, 86400000);
    m_refresh.ms_refresh_ms = ReadIntValue(p, L"ms_refresh_ms", m_refresh.ms_refresh_ms, 1000, 86400000);
    m_refresh.ms_cooldown_ms = ReadIntValue(p, L"ms_cooldown_ms", m_refresh.ms_cooldown_ms, 1000, 86400000);
    m_refresh.g7_offline_ms = ReadIntValue(p, L"g7_offline_ms", m_refresh.g7_offline_ms, 1000, 86400000);
    m_refresh.g7_reset_ms = ReadIntValue(p, L"g7_reset_ms", m_refresh.g7_reset_ms, 1000, 86400000);
    m_refresh.g7_reconnect_ms = ReadIntValue(p, L"g7_reconnect_ms", m_refresh.g7_reconnect_ms, 1000, 86400000);
    m_refresh.g7_read_timeout_ms = ReadIntValue(p, L"g7_read_timeout_ms", m_refresh.g7_read_timeout_ms, 100, 60000);
    m_refresh.ds_pnp_ms = ReadIntValue(p, L"ds_pnp_ms", m_refresh.ds_pnp_ms, 1000, 600000);
    m_refresh.ds_read_ms = ReadIntValue(p, L"ds_read_ms", m_refresh.ds_read_ms, 1000, 86400000);

    // 全局布局
    m_layout.sep_w = ReadIntValue(p, L"sep_w", m_layout.sep_w, 0, 60);
    m_layout.pad_lr = ReadIntValue(p, L"pad_lr", m_layout.pad_lr, 0, 20);
    m_layout.show_sep = ReadIntValue(p, L"show_sep", m_layout.show_sep ? 1 : 0, 0, 1) != 0;

    // 每台设备的外观：自己的段优先，缺项回落到 1.0.0 的 [Advanced] 全局值
    for (int dev = 0; dev < kDeviceCount; ++dev)
    {
        const std::wstring sect = StyleSection(dev);
        DeviceStyle& s = m_style[dev];

        for (const StyleIntField& f : kStyleInts)
        {
            int v = 0;
            if (ReadIntIn(p, sect.c_str(), f.key, v) || ReadIntIn(p, kSection, f.key, v))
                s.*f.member = Clamp(v, f.lo, f.hi);
        }
        {
            int v = 0;
            if (ReadIntIn(p, sect.c_str(), L"show_pct_sign", v) ||
                ReadIntIn(p, kSection, L"show_pct_sign", v))
                s.show_pct_sign = v != 0;
        }
        for (const StyleColorField& f : kStyleColors)
        {
            COLORREF c = 0;
            if (ReadColorIn(p, sect.c_str(), f.key, c) || ReadColorIn(p, kSection, f.key, c))
                s.*f.member = c;
        }
        {
            std::wstring v;
            if (ReadStrIn(p, sect.c_str(), L"no_read", v) || ReadStrIn(p, kSection, L"no_read", v))
                s.no_read = Trim(v);
        }
    }

    for (int i = 0; i < kDeviceCount; ++i)
    {
        wchar_t key[16];
        swprintf(key, 16, L"name%d", i);
        std::wstring v = Trim(ReadStrValue(p, key, m_text.name[i].c_str()));
        if (!v.empty())
            m_text.name[i] = v;
    }

    SanitizeLocked();
}

void PluginConfig::Save()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.empty())
        return;

    SanitizeLocked();

    const std::wstring& p = m_path;
    WriteInt(p, kSection, L"kb_wake_ms", m_refresh.kb_wake_ms);
    WriteInt(p, kSection, L"kb_active_ms", m_refresh.kb_active_ms);
    WriteInt(p, kSection, L"kb_refresh_ms", m_refresh.kb_refresh_ms);
    WriteInt(p, kSection, L"kb_cooldown_ms", m_refresh.kb_cooldown_ms);
    WriteInt(p, kSection, L"kb_reconnect_ms", m_refresh.kb_reconnect_ms);
    WriteInt(p, kSection, L"ms_wake_ms", m_refresh.ms_wake_ms);
    WriteInt(p, kSection, L"ms_active_ms", m_refresh.ms_active_ms);
    WriteInt(p, kSection, L"ms_refresh_ms", m_refresh.ms_refresh_ms);
    WriteInt(p, kSection, L"ms_cooldown_ms", m_refresh.ms_cooldown_ms);
    WriteInt(p, kSection, L"g7_offline_ms", m_refresh.g7_offline_ms);
    WriteInt(p, kSection, L"g7_reset_ms", m_refresh.g7_reset_ms);
    WriteInt(p, kSection, L"g7_reconnect_ms", m_refresh.g7_reconnect_ms);
    WriteInt(p, kSection, L"g7_read_timeout_ms", m_refresh.g7_read_timeout_ms);
    WriteInt(p, kSection, L"ds_pnp_ms", m_refresh.ds_pnp_ms);
    WriteInt(p, kSection, L"ds_read_ms", m_refresh.ds_read_ms);

    WriteInt(p, kSection, L"sep_w", m_layout.sep_w);
    WriteInt(p, kSection, L"pad_lr", m_layout.pad_lr);
    WriteInt(p, kSection, L"show_sep", m_layout.show_sep ? 1 : 0);

    // 每台设备的外观写自己的段
    for (int dev = 0; dev < kDeviceCount; ++dev)
    {
        const std::wstring sect = StyleSection(dev);
        const DeviceStyle& s = m_style[dev];
        for (const StyleIntField& f : kStyleInts)
            WriteInt(p, sect.c_str(), f.key, s.*f.member);
        WriteInt(p, sect.c_str(), L"show_pct_sign", s.show_pct_sign ? 1 : 0);
        for (const StyleColorField& f : kStyleColors)
            WriteColor(p, sect.c_str(), f.key, s.*f.member);
        WritePrivateProfileStringW(sect.c_str(), L"no_read", s.no_read.c_str(), p.c_str());
    }

    // 清掉 1.0.0 留在 [Advanced] 的全局外观键，避免两套值并存看不出哪个生效
    for (const StyleIntField& f : kStyleInts)
        DeleteKey(p, kSection, f.key);
    DeleteKey(p, kSection, L"show_pct_sign");
    for (const StyleColorField& f : kStyleColors)
        DeleteKey(p, kSection, f.key);
    DeleteKey(p, kSection, L"no_read");

    for (int i = 0; i < kDeviceCount; ++i)
    {
        wchar_t key[16];
        swprintf(key, 16, L"name%d", i);
        WritePrivateProfileStringW(kSection, key, m_text.name[i].c_str(), p.c_str());
    }
}

RefreshConfig PluginConfig::Refresh() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_refresh;
}

LayoutConfig PluginConfig::Layout() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_layout;
}

DeviceStyle PluginConfig::Style(int dev) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (dev < 0 || dev >= kDeviceCount)
        return DeviceStyle{};
    return m_style[dev];
}

TextConfig PluginConfig::Text() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_text;
}

std::wstring PluginConfig::Name(int dev) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (dev < 0 || dev >= kDeviceCount)
        return std::wstring();
    return m_text.name[dev];
}

void PluginConfig::SetAll(const RefreshConfig& refresh, const LayoutConfig& layout,
                          const DeviceStyle* styles, const TextConfig& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refresh = refresh;
    m_layout = layout;
    if (styles)
    {
        for (int i = 0; i < kDeviceCount; ++i)
            m_style[i] = styles[i];
    }
    m_text = text;
    SanitizeLocked();
}

void PluginConfig::SanitizeLocked()
{
    m_refresh.kb_wake_ms = Clamp(m_refresh.kb_wake_ms, 1000, 86400000);
    m_refresh.kb_active_ms = Clamp(m_refresh.kb_active_ms, 1000, 86400000);
    m_refresh.kb_refresh_ms = Clamp(m_refresh.kb_refresh_ms, 1000, 86400000);
    m_refresh.kb_cooldown_ms = Clamp(m_refresh.kb_cooldown_ms, 1000, 86400000);
    m_refresh.kb_reconnect_ms = Clamp(m_refresh.kb_reconnect_ms, 1000, 86400000);
    m_refresh.ms_wake_ms = Clamp(m_refresh.ms_wake_ms, 1000, 86400000);
    m_refresh.ms_active_ms = Clamp(m_refresh.ms_active_ms, 1000, 86400000);
    m_refresh.ms_refresh_ms = Clamp(m_refresh.ms_refresh_ms, 1000, 86400000);
    m_refresh.ms_cooldown_ms = Clamp(m_refresh.ms_cooldown_ms, 1000, 86400000);
    m_refresh.g7_offline_ms = Clamp(m_refresh.g7_offline_ms, 1000, 86400000);
    m_refresh.g7_reset_ms = Clamp(m_refresh.g7_reset_ms, 1000, 86400000);
    m_refresh.g7_reconnect_ms = Clamp(m_refresh.g7_reconnect_ms, 1000, 86400000);
    m_refresh.g7_read_timeout_ms = Clamp(m_refresh.g7_read_timeout_ms, 100, 60000);
    m_refresh.ds_pnp_ms = Clamp(m_refresh.ds_pnp_ms, 1000, 600000);
    m_refresh.ds_read_ms = Clamp(m_refresh.ds_read_ms, 1000, 86400000);

    m_layout.sep_w = Clamp(m_layout.sep_w, 0, 60);
    m_layout.pad_lr = Clamp(m_layout.pad_lr, 0, 20);

    for (int i = 0; i < kDeviceCount; ++i)
    {
        DeviceStyle& s = m_style[i];
        s.level_high = Clamp(s.level_high, 1, 100);
        s.level_low = Clamp(s.level_low, 0, 99);
        if (s.level_low >= s.level_high) // 中分界必须低于高电量分界
        {
            s.level_high = 60;
            s.level_low = 20;
        }
        s.ring_scale_x10 = Clamp(s.ring_scale_x10, 8, 40);
        s.num_gap = Clamp(s.num_gap, 0, 20);

        s.no_read = Trim(s.no_read);
        if (s.no_read.empty())
            s.no_read = L"--";
        else if (s.no_read.size() > 15)
            s.no_read.resize(15);
    }

    for (int i = 0; i < kDeviceCount; ++i)
    {
        m_text.name[i] = Trim(m_text.name[i]);
        if (m_text.name[i].empty())
            m_text.name[i] = kDefaultDeviceNames[i];
        else if (m_text.name[i].size() > 31)
            m_text.name[i].resize(31);
    }
}

bool PluginConfig::ParseColor(const std::wstring& text, COLORREF& out)
{
    std::wstring s = Trim(text);
    if (!s.empty() && s[0] == L'#')
        s.erase(0, 1);
    if (s.size() != 6)
        return false;

    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (end != s.c_str() + 6)
        return false;
    if (v > 0xFFFFFFul)
        return false;

    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

std::wstring PluginConfig::FormatColor(COLORREF color)
{
    wchar_t buf[8];
    const unsigned r = color & 0xFF;
    const unsigned g = (color >> 8) & 0xFF;
    const unsigned b = (color >> 16) & 0xFF;
    swprintf(buf, 8, L"%02X%02X%02X", r, g, b);
    return buf;
}
