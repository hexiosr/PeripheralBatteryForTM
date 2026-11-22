#include "Plugin.h"
#include "PluginConfig.h"
#include "Version.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cstdio>

extern HMODULE g_hDllModule; // dllmain.cpp：本 DLL 模块句柄

// ============ 设备条目 ============
// 新增设备：1) 实现对应 Reader（继承 BatteryReader）
//           2) DeviceList.h 里 kDeviceCount +1，补默认名与默认顺序
//           3) 本文件 kDeviceMeta 补一项，Plugin 的 m_readers 补一个成员
// 显示名由玩家在“显示设置”里改，默认值见 DeviceList.h。
// 接收器 HID 产品字符串不一定是型号（G87 的接收器只报 "MCHOSE 2.4G Wireless"），故用固定名。

// 悬停提示的设备图标前缀（GDI 字体链接渲染为单色符号；
// emoji 为 UTF-16 代理对，\u 转义不允许代理区字符，须用 \x 逐值书写）
static const wchar_t* const kTipIconKb  = L"\u2328 ";       // ⌨
static const wchar_t* const kTipIconMs  = L"\xD83D\xDDB1 "; // 🖱
static const wchar_t* const kTipIconPad = L"\xD83C\xDFAE "; // 🎮

// 设备元数据（顺序 = 设备索引）：悬停图标 + 是否只有粗档读数。
// G7 Pro 只能读到 4 档（0/33/66/100），任务栏用 ▮▯ 电池图标而不是百分比。
struct DeviceMeta
{
    const wchar_t* tip_icon;
    bool bars_only;
};

const DeviceMeta kDeviceMeta[] = {
    { kTipIconKb,  false }, // 0 G87
    { kTipIconMs,  false }, // 1 A5 Ultra
    { kTipIconPad, true  }, // 2 G7 Pro
    { kTipIconPad, false }, // 3 DS5
};

static_assert(sizeof(kDeviceMeta) / sizeof(kDeviceMeta[0]) == kDeviceCount,
              "kDeviceMeta 的项数与 kDeviceCount 不一致");

// 全部取消勾选：条目不能真的消失，否则任务栏上没有点击区域，右键进不来设置
static const wchar_t* const kPlaceholderText = L"外设电量";
static const wchar_t* const kPlaceholderTip =
    L"四台设备都未勾选，电量暂不显示。\n右键打开「显示设置」重新勾选。";

Plugin Plugin::m_instance;

Plugin::Plugin()
{
    // 设备索引 → 读取器（数量变化由 Plugin.h 的 static_assert 拦下）
    m_readers[0] = &m_key_reader;   // G87
    m_readers[1] = &m_mouse_reader; // A5 Ultra
    m_readers[2] = &m_g7_reader;    // G7 Pro
    m_readers[3] = &m_ds_reader;    // DS5

    // 预填显示数组：首次 DataRequired 之前，自绘也能按完整布局计算宽度
    for (int i = 0; i < kDeviceCount; ++i)
    {
        m_show[i] = true;
        m_order[i] = kDefaultOrder[i];
        m_display[i].name = PluginConfig::Instance().Name(i);
    }
    m_display_count = kDeviceCount;
}

Plugin& Plugin::Instance()
{
    return m_instance;
}

IPluginItem* Plugin::GetItem(int index)
{
    switch (index)
    {
    case 0: return &m_item;    // 外设电量显示（组合键盘 / 鼠标 / 手柄）
    default: return nullptr;
    }
}

void Plugin::DataRequired()
{
    // 勾选才查询：按当前勾选启动各设备的后台读取线程
    SyncReaders();

    RefreshDisplay();
}

void Plugin::RefreshDisplay()
{
    const TextConfig tc = PluginConfig::Instance().Text();

    // 各设备快照与外观一次取齐（读取器内部加锁，UI 线程只拿副本）
    BatterySnapshot snaps[kDeviceCount];
    DeviceStyle styles[kDeviceCount];
    for (int d = 0; d < kDeviceCount; ++d)
    {
        snaps[d] = m_readers[d]->GetSnapshot();
        styles[d] = PluginConfig::Instance().Style(d);
    }

    // 组合显示一块，按选项设置的勾选与顺序过滤设备：
    // 文本、悬停提示、自绘数据三份输出都在这一轮里按同一顺序生成，不会错位。
    // 每个条目勾选即显示（含手柄）：已连接显示电量，未连接显示 --（与键鼠一致）。
    std::wstring val;
    std::wstring tip;
    m_display_count = 0;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        const int dev = m_order[i];
        if (!m_show[dev])
            continue;

        const BatterySnapshot& s = snaps[dev];
        const bool live = s.has_device && s.online && s.valid;

        std::wstring txt;
        BuildValueText(s, txt, styles[dev].no_read, kDeviceMeta[dev].bars_only);
        if (!val.empty())
            val += L" | ";
        val += tc.name[dev] + L": " + txt;

        BuildTooltip(tc.name[dev], kDeviceMeta[dev].tip_icon, s, tip);
        tip += L'\n';

        // 自绘数据：未连接 / 暂无读数 → 空圆环 + --（手柄与键鼠一致）
        DeviceDisplay& d = m_display[m_display_count++];
        d = DeviceDisplay{};
        d.name = tc.name[dev];
        d.style = styles[dev];
        if (live)
        {
            d.percent = s.percent;
            d.charging = s.charging;
        }
    }
    m_value_text = val;
    if (!tip.empty())
        tip.pop_back();
    m_tooltip = tip;

    // 宽度测量用示例文本：设备名同步为当前自定义名称
    m_sample_text.clear();
    for (int i = 0; i < kDeviceCount; ++i)
    {
        const int dev = m_order[i];
        if (!m_show[dev])
            continue;
        if (!m_sample_text.empty())
            m_sample_text += L" | ";
        m_sample_text += tc.name[dev] + L": ";
        m_sample_text += kDeviceMeta[dev].bars_only ? L"\u25AE\u25AE\u25AE\u25AE\u26A1" : L"100%\u26A1";
    }

    // 全部取消勾选：保留占位项，任务栏上仍有点击区域可右键打开设置
    m_placeholder = (m_display_count == 0);
    if (m_placeholder)
    {
        m_value_text = kPlaceholderText;
        m_tooltip = kPlaceholderTip;
    }
}

// 任务栏数值：普通设备显示百分比；只有粗档的设备（G7 Pro）用电池图标
//  ▮▮▮▮ = 满   ▮▮▮▯ = 高   ▮▮▯▯ = 中   ▮▯▯▯ = 低   ⚡ 后缀 = 充电中
void Plugin::BuildValueText(const BatterySnapshot& snap, std::wstring& out, const std::wstring& no_read,
                            bool bars_only)
{
    if (!snap.has_device || !snap.online || !snap.valid || snap.percent < 0)
    {
        out = no_read; // 未找到设备 / 离线 / 暂无有效读数
        return;
    }

    if (!bars_only)
    {
        wchar_t buf[32];
        if (snap.charging)
            swprintf_s(buf, L"%d%%\u26A1", snap.percent); // ⚡ 充电中
        else
            swprintf_s(buf, L"%d%%", snap.percent);
        out = buf;
        return;
    }

    int bars = 1; // 0% → 1 格
    if (snap.percent >= 100) bars = 4;
    else if (snap.percent >= 66) bars = 3;
    else if (snap.percent >= 33) bars = 2;

    out.clear();
    for (int i = 0; i < 4; ++i)
        out += (i < bars) ? L"\u25AE" : L"\u25AF"; // ▮ 实心 / ▯ 空心
    if (snap.charging)
        out += L"\u26A1"; // ⚡
}

// 悬停行尾部：秒级直显；分钟档带剩余秒；更大量级自动换算
static void AppendUpdatedAgo(const BatterySnapshot& snap, std::wstring& out)
{
    if (snap.last_update_ms == 0)
        return;
    ULONGLONG now = GetTickCount64();
    ULONGLONG diff = (now >= snap.last_update_ms) ? (now - snap.last_update_ms) / 1000 : 0;
    wchar_t buf[32];
    if (diff < 60)
        swprintf_s(buf, L" · %llu 秒前", diff);
    else if (diff < 3600)
        swprintf_s(buf, L" · %llu 分 %llu 秒前", diff / 60, diff % 60);
    else if (diff < 86400)
        swprintf_s(buf, L" · %llu 小时前", diff / 3600);
    else
        swprintf_s(buf, L" · %llu 天前", diff / 86400);
    out += buf;
}

// 悬停提示为一行式：图标 型号：状态 · 更新时间
// 四类设备的提示格式一致，只有图标不同，图标由 kDeviceMeta 传入
void Plugin::BuildTooltip(const std::wstring& label, const wchar_t* icon, const BatterySnapshot& snap,
                          std::wstring& out)
{
    out += icon;
    out += label;

    // 未检测到 / 离线 / 尚无上报，统一显示"未连接"
    if (!snap.has_device || !snap.online || !snap.valid)
    {
        out += L"：未连接";
        return;
    }

    wchar_t buf[32];
    if (snap.charging)
        swprintf_s(buf, L"：%d%% \u26A1充电中", snap.percent); // ⚡
    else
        swprintf_s(buf, L"：%d%%", snap.percent);
    out += buf;
    AppendUpdatedAgo(snap, out);
}

const wchar_t* Plugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:
        return L"外设电量显示";
    case TMI_DESCRIPTION:
        return L"在任务栏显示无线键盘、鼠标与手柄的电量";
    case TMI_AUTHOR:
        return L"ziphooo";
    case TMI_COPYRIGHT:
        return L"MIT License";
    case TMI_VERSION:
        return L"" PB_VERSION_STRING; // 版本号唯一来源：common/Version.h
    default:
        break;
    }
    return L"";
}

const wchar_t* Plugin::GetTooltipInfo()
{
    return m_tooltip.c_str();
}

void Plugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
    LoadConfig();
}

// ============ 选项配置（勾选显示哪些设备，存 ini） ============

// 老版本写出的 ini 没有 BOM，Windows 会按 ANSI（GBK）读写，UTF-8 编辑器打开时中文全是乱码。
// 首次加载时把文件整体转成带 BOM 的 UTF-16LE，之后 WritePrivateProfileStringW 也会按 Unicode 写。
static void EnsureIniUnicode(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        // 文件还不存在：先建一个只有 BOM 的空文件，后续写入即为 UTF-16
        h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(h, &bom, 2, &written, nullptr);
        CloseHandle(h);
        return;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(h);
        return;
    }
    std::string raw((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &raw[0], (DWORD)raw.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok || read < 2)
        return;

    const unsigned char b0 = (unsigned char)raw[0];
    const unsigned char b1 = (unsigned char)raw[1];
    if (b0 == 0xFF && b1 == 0xFE)
        return; // 已经是 UTF-16LE

    UINT cp = CP_ACP;
    int skip = 0;
    if (read >= 3 && b0 == 0xEF && b1 == 0xBB && (unsigned char)raw[2] == 0xBF)
    {
        cp = CP_UTF8;
        skip = 3;
    }
    int n = MultiByteToWideChar(cp, 0, raw.data() + skip, (int)read - skip, nullptr, 0);
    if (n <= 0)
        return;
    std::wstring text((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, raw.data() + skip, (int)read - skip, &text[0], n);
    text.insert(text.begin(), L'\uFEFF');

    h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

void Plugin::LoadConfig()
{
    if (!m_app)
        return;
    std::wstring path = m_app->GetPluginConfigDir();
    path += L"\\PeripheralBattery.ini";

    EnsureIniUnicode(path);

    // 玩家自定义的刷新 / 外观 / 文案参数（[Advanced] 段）
    PluginConfig::Instance().SetIniPath(path);
    PluginConfig::Instance().Load();

    // 勾选：show{id}=0/1（按设备 id 持久化，新增设备自动多一个 key）
    wchar_t buf[16];
    for (int d = 0; d < kDeviceCount; ++d)
    {
        wchar_t key[16];
        swprintf_s(key, L"show%d", d);
        m_show[d] = true;
        if (GetPrivateProfileStringW(L"Options", key, L"1", buf, 16, path.c_str()) > 0)
            m_show[d] = (buf[0] == L'1');
    }

    // 显示顺序：order=1,3,2,0（设备索引，逗号分隔；默认按型号字母序）
    wchar_t ord[64];
    for (int i = 0; i < kDeviceCount; ++i)
        m_order[i] = kDefaultOrder[i];
    if (GetPrivateProfileStringW(L"Options", L"order", L"", ord, 64, path.c_str()) > 0)
    {
        int idx = 0;
        wchar_t* ctx = nullptr;
        wchar_t* tok = wcstok_s(ord, L",", &ctx);
        while (tok && idx < kDeviceCount)
        {
            int dev = _wtoi(tok);
            if (dev >= 0 && dev < kDeviceCount)
                m_order[idx++] = dev;
            tok = wcstok_s(nullptr, L",", &ctx);
        }
        // 去重补全（保证每个设备恰好出现一次）
        bool seen[kDeviceCount] = {};
        int cnt = 0;
        for (int i = 0; i < kDeviceCount; ++i)
            if (!seen[m_order[i]])
            {
                seen[m_order[i]] = true;
                m_order[cnt++] = m_order[i];
            }
        for (int d = 0; d < kDeviceCount; ++d)
            if (!seen[d])
                m_order[cnt++] = d;
    }
}

void Plugin::SaveConfig()
{
    if (!m_app)
        return;
    std::wstring path = m_app->GetPluginConfigDir();
    path += L"\\PeripheralBattery.ini";
    EnsureIniUnicode(path); // 兜底：运行期间文件被删掉时重建，保证仍以 UTF-16 写入
    for (int d = 0; d < kDeviceCount; ++d)
    {
        wchar_t key[16];
        swprintf_s(key, L"show%d", d);
        WritePrivateProfileStringW(L"Options", key, m_show[d] ? L"1" : L"0", path.c_str());
    }
    wchar_t ord[64];
    swprintf_s(ord, L"%d", m_order[0]);
    for (int i = 1; i < kDeviceCount; ++i)
    {
        wchar_t seg[16];
        swprintf_s(seg, L",%d", m_order[i]);
        wcscat_s(ord, seg);
    }
    WritePrivateProfileStringW(L"Options", L"order", ord, path.c_str());
    PluginConfig::Instance().Save();
}

// ============ 选项对话框工作副本 ============

void Plugin::DlgBegin()
{
    memcpy(m_dlg_show, m_show, sizeof(m_show));
    memcpy(m_dlg_order, m_order, sizeof(m_order));
}

void Plugin::DlgApply()
{
    memcpy(m_show, m_dlg_show, sizeof(m_show));
    memcpy(m_order, m_dlg_order, sizeof(m_order));
    SaveConfig();
    ApplyOptionsNow();
    SyncReaders(); // 勾选才查询：按最新勾选启动/停止设备 Reader
}

void Plugin::SyncReaders()
{
    for (int d = 0; d < kDeviceCount; ++d)
    {
        if (m_show[d] == m_reader_started[d])
            continue;
        if (m_show[d])
            m_readers[d]->Start();
        else
            m_readers[d]->Stop(); // 非阻塞：线程自行退出，join 推迟到下次 Start / 析构
        m_reader_started[d] = m_show[d];
    }
}

void Plugin::DlgMoveUp(int dev)
{
    for (int i = 0; i < kDeviceCount; ++i)
    {
        if (m_dlg_order[i] == dev && i > 0)
        {
            std::swap(m_dlg_order[i], m_dlg_order[i - 1]);
            return;
        }
    }
}

void Plugin::DlgMoveDown(int dev)
{
    for (int i = 0; i < kDeviceCount; ++i)
    {
        if (m_dlg_order[i] == dev && i < kDeviceCount - 1)
        {
            std::swap(m_dlg_order[i], m_dlg_order[i + 1]);
            return;
        }
    }
}

void Plugin::DlgRestoreDefault()
{
    for (int d = 0; d < kDeviceCount; ++d)
    {
        m_dlg_show[d] = true;
        m_dlg_order[d] = kDefaultOrder[d];
    }
}

// ============ 选项设置对话框（Win11 风格自绘） ============
// 第一行：左「设备」（白色复选列表 + 上移 / 下移 / 改名）、
//         右「布局」（全局：设备间距 / 左右边距 / 显示分隔线），两组等高
// 第二、三行：2×2 四张设备卡片，每张卡 = 该设备的刷新策略（上半）+ 该设备自己的外观
//         （下半：阈值、数字缩放、环距、三档配色、百分号、无数据文案）
// 底部：恢复默认（左，连同顺序与勾选一起还原）/ 确定 / 应用 / 取消（右对齐）
// 按钮自绘（圆角、悬停/按下态，默认按钮主题蓝）；Win11 下窗口圆角
// 所有文本按当前 DPI 与实际字体度量排版，字体放大不会裁切。

namespace {

constexpr UINT IDC_DEVLIST = 500;  // 设备复选列表（自绘行）
constexpr UINT IDC_DEVWARN = 569;  // 全部取消勾选时的提示
constexpr UINT IDC_RENAMEHINT = 570; // 改名进行中的操作提示（与上面的提示同位置、互斥显示）
constexpr UINT IDC_UP = 600;
constexpr UINT IDC_DOWN = 601;
constexpr UINT IDC_RENAME = 603;
constexpr UINT IDC_DEVNAME = 590;  // 就地改名输入框（叠在选中行上）
constexpr UINT IDC_RESET = 602;
constexpr UINT IDC_OK = 700;
constexpr UINT IDC_CANCEL = 701;
constexpr UINT IDC_APPLY = 702;
constexpr UINT IDC_TAB_APP = 710;     // 页签：外观
constexpr UINT IDC_TAB_REF = 711;     // 页签：刷新策略
constexpr UINT IDC_DETAIL_NAME = 712; // 详情卡标题（设备名，随选中设备变化）

// 单实例标签：一次只显示一台设备、一个页签，标签不必按设备重复创建。
// 隐藏的输入框（其余设备 / 另一个页签）只是重叠堆在同一块区域里，切换时 ShowWindow。
constexpr UINT IDC_LBL_NUM0 = 720;      // 720..723：外观数值标签
constexpr UINT IDC_LBL_COLORROW0 = 724; // 724..726：高 / 中 / 低电量
constexpr UINT IDC_LBL_COLORHEAD0 = 727;// 727..728：深色 / 浅色任务栏
constexpr UINT IDC_LBL_NOREAD = 729;    // 「无数据时显示」
constexpr UINT IDC_LBL_REF0 = 730;      // 730..744：刷新策略标签（按 kRefFields 索引）
constexpr UINT IDC_LBL_CAP_COLOR = 745; // 「电量配色」小标题
constexpr UINT IDC_LBL_GLOBAL = 746;    // 「全局布局」分组标签
constexpr UINT IDC_REF0 = 800;     // 800..814：刷新策略（秒，按 kRefFields 索引）
constexpr UINT IDC_NUM0 = 820;     // 820..835：外观数值 dev*4 + i
constexpr UINT IDC_NOREAD0 = 836;  // 836..839：无数据时显示的文本（按设备）
constexpr UINT IDC_SHOW_SEP = 840; // 显示设备分隔线（全局）
constexpr UINT IDC_SHOWPCT0 = 841; // 841..844：显示百分号（按设备）
constexpr UINT IDC_LAY0 = 845;     // 845：设备间距 / 846：左右边距（全局）
constexpr UINT IDC_COLOR0 = 860;   // 860..883：外观配色 dev*6 + slot（RRGGBB）

constexpr COLORREF kDlgBg = RGB(247, 247, 248);

// 布局按 DPI 与实际字体度量计算：字体放大（125%/150%）时标签与输入框同步加宽加高，
// 不会出现文字被裁切。g_lay 在打开对话框时由 ComputeLayout 填充，绘制过程也读它。
int g_dpi = 96;

int S(int v) { return MulDiv(v, g_dpi, 96); }

int Max2(int a, int b) { return a > b ? a : b; }

// 显示设置的整体布局。
// 结构：浅灰画布上两张白色圆角卡片——
//   左「设备」：复选列表（勾选 = 任务栏显示）+ 上移 / 下移 / 改名；
//   右「详情」：当前选中设备的名称、类别，下面「外观 / 刷新策略」两个页签。
// 两张卡片等高，底部一行全局布局参数，最下面是按钮行。
// 所有纵向偏移都在 ComputeLayoutPass 里算好，创建控件与绘制共用同一组数值。
struct DlgLayout
{
    int w = 0, h = 0;
    int margin = 14;
    int field_h = 22, field_step = 26;
    int btn_h = 32, btn_w = 96, btn_action_w = 84;
    int pad = 14;       // 卡片内边距
    int card_gap = 12;  // 左右卡片间距
    int radius = 10;    // 卡片圆角

    // 左侧：设备卡片
    int left_x = 0, left_y = 0, left_w = 0, left_h = 0;
    int left_content = 0;                                // 卡片内容宽（列表与按钮列取较宽者）
    int left_cap_y = 0;                                  // 「设备」小标题
    int list_x = 0, list_y = 0, list_w = 0, list_h = 0, row_h = 32;
    int half_w = 0, updown_y = 0;                        // 上移 / 下移 并排
    int hint_y = 0, hint_w = 0, hint_x = 0;              // 列表下方的提示（两种状态互斥）

    // 右侧：详情卡片
    int right_x = 0, right_y = 0, right_w = 0, right_h = 0;
    int name_y = 0;                                      // 设备名
    int name_btn_x = 0, name_btn_y = 0, name_btn_w = 0;  // 设备名右侧的「重命名」
    int tab_y = 0, tab_h = 28, tab_w = 0;                // 两个页签（等宽）
    int body_y = 0;                                      // 页签内容区起点
    int body_btm = 0;                                    // 内容区底线（卡片内）

    // 页签内容：外观
    int num_lbl_w[2] = {}, num_edit_w = 56, num_col_gap = 14;
    int cap_color_y = 0, color_head_y = 0, color_row_y = 0;
    int color_lbl_w = 0, chip_w = 0, chip_off[3] = {};
    int pct_y = 0, pct_w = 0, noread_lbl_w = 0, noread_edit_w = 56;

    // 页签内容：刷新策略
    int ref_lbl_w = 0, ref_edit_w = 64, ref_col_gap = 14;
    int ref_y0 = 0;

    // 底部
    int global_y = 0, global_h = 34;
    int g_cap_x = 0, g_cap_w = 0, g_sep_x = 0;           // 「全局布局」与它后面的竖分隔线
    int g_lbl_x[2] = {}, g_lbl_w = 0, g_ed_x[2] = {}, g_ed_w = 0, g_chk_x = 0;
    int bottom_y = 0;
};
DlgLayout g_lay;

// 详情面板当前显示的设备与页签（0 = 外观，1 = 刷新策略）
int g_cur_dev = 0;
int g_cur_tab = 0;
HFONT g_name_font = nullptr; // 详情卡标题字体（就地改名时输入框沿用同一字号）

struct RefField
{
    const wchar_t* label;
    int lo, hi;
    int RefreshConfig::* member;
    int dev;             // 所属设备（0..3），决定落在哪一张设备卡片里
    const wchar_t* tip;  // 鼠标悬停时的说明
};

// 刷新策略：每个设备一张卡片，卡片标题用玩家自定义的设备名，其中每行一项
const RefField kRefFields[15] = {
    { L"休眠判定", 1, 86400, &RefreshConfig::kb_wake_ms, 0,
      L"闲置满这么多秒后的第一次按键算唤醒，并立刻查询一次电量；连续打字时不会误触发。默认 60 秒。" },
    { L"使用判定", 1, 86400, &RefreshConfig::kb_active_ms, 0,
      L"最近这么多秒内有按键就算正在使用，期间按「刷新间隔」查询。默认 30 秒。" },
    { L"刷新间隔", 1, 86400, &RefreshConfig::kb_refresh_ms, 0,
      L"使用中每隔这么多秒查询一次电量。默认 5 秒。" },
    { L"最短查询间隔", 1, 86400, &RefreshConfig::kb_cooldown_ms, 0,
      L"两次查询之间至少隔这么多秒，防止刚唤醒就连续查询。默认 2 秒。" },
    { L"重试间隔", 1, 86400, &RefreshConfig::kb_reconnect_ms, 0,
      L"设备打不开（接收器没插好或被占用）时隔多久重试；期间保留最后一次读数。默认 3 秒。" },
    { L"休眠判定", 1, 86400, &RefreshConfig::ms_wake_ms, 1,
      L"这么久没有移动或点击后的第一次操作算唤醒，并立刻采集一次电量。默认 120 秒。" },
    { L"使用判定", 1, 86400, &RefreshConfig::ms_active_ms, 1,
      L"最近这么多秒内有移动、滚轮或点击就算正在使用，期间按「刷新间隔」采集。默认 30 秒。" },
    { L"刷新间隔", 1, 86400, &RefreshConfig::ms_refresh_ms, 1,
      L"使用中每隔这么多秒采集一次电量。默认 5 秒。" },
    { L"最短查询间隔", 1, 86400, &RefreshConfig::ms_cooldown_ms, 1,
      L"两次采集之间至少隔这么多秒。默认 2 秒。" },
    { L"断开判定", 1, 86400, &RefreshConfig::g7_offline_ms, 2,
      L"超过这么多秒收不到手柄消息就认为已断开，任务栏显示 --。默认 90 秒。" },
    { L"重新识别", 1, 86400, &RefreshConfig::g7_reset_ms, 2,
      L"超过这么多秒没消息就重新识别手柄（换接收器或换手柄后能重新认上）。应小于「断开判定」。默认 60 秒。" },
    { L"重试间隔", 1, 86400, &RefreshConfig::g7_reconnect_ms, 2,
      L"GIP 总线打不开（驱动未就绪）时隔多久重试。默认 3 秒。" },
    { L"读取超时", 1, 60, &RefreshConfig::g7_read_timeout_ms, 2,
      L"单次等待手柄消息的最长时间，超时即视为这一轮没有消息。默认 1 秒。" },
    { L"连接检查", 1, 600, &RefreshConfig::ds_pnp_ms, 3,
      L"每隔这么多秒检查手柄是否还在系统中，不在则显示 --。默认 2 秒。" },
    { L"读取间隔", 1, 86400, &RefreshConfig::ds_read_ms, 3,
      L"手柄在位时每隔这么多秒读一次电量；刚接入会立刻读一次。默认 60 秒。" },
};

// 外观数值（每台设备各一份）
struct NumField
{
    const wchar_t* label;
    int lo, hi;
    int DeviceStyle::* member;
    const wchar_t* tip;
};

// 2 列 × 2 行，顺序决定格子位置
const NumField kNumFields[4] = {
    { L"高电量分界 %", 1, 100, &DeviceStyle::level_high,
      L"电量大于等于该值时使用高电量颜色。默认 60。" },
    { L"中电量分界 %", 0, 99, &DeviceStyle::level_low,
      L"电量大于等于该值（且低于高电量分界）时使用中电量颜色，再低为低电量颜色。默认 20。" },
    { L"数字缩放 ×0.1", 8, 40, &DeviceStyle::ring_scale_x10,
      L"数字大小相对字高的比例，数值 ×0.1；单行时圆环会随数字一起缩放，双行时圆环最大到任务栏行高。默认 15。" },
    { L"环与数字间距", 0, 20, &DeviceStyle::num_gap,
      L"任务栏分两行显示时，圆环与右侧数字的间隙。默认 2。" },
};
constexpr int kNumFieldsCount = 4;

// 全局布局参数（右上「布局」组）
struct LayoutField
{
    const wchar_t* label;
    int lo, hi;
    int LayoutConfig::* member;
    const wchar_t* tip;
};

const LayoutField kLayoutFields[2] = {
    { L"设备间距", 0, 60, &LayoutConfig::sep_w,
      L"相邻设备之间的间距，分隔线画在中间；设为 0 时设备紧挨在一起。所有设备共用。默认 14。" },
    { L"左右边距", 0, 20, &LayoutConfig::pad_lr,
      L"插件条目左右两侧的留白，用来与相邻任务栏项隔开。所有设备共用。默认 1。" },
};
constexpr int kLayoutFieldsCount = 2;

// 外观配色：每台设备 3 档电量 × 深/浅两套配色
// slot = 档位 * 2 + 主题（0=深色任务栏，1=浅色任务栏）
const wchar_t* const kColorRows[3] = { L"高电量", L"中电量", L"低电量" };
constexpr int kColorRowsCount = 3;
const wchar_t* const kColorHeads[2] = { L"深色任务栏", L"浅色任务栏" };
constexpr int kColorCount = 6;

COLORREF DeviceStyle::* const kColorMembers[kColorCount] = {
    &DeviceStyle::green_dark, &DeviceStyle::green_light,
    &DeviceStyle::yellow_dark, &DeviceStyle::yellow_light,
    &DeviceStyle::red_dark, &DeviceStyle::red_light,
};

const wchar_t* const kNoReadLabel = L"无数据时显示";
const wchar_t* const kColorCaption = L"电量配色";
const wchar_t* const kShowSepLabel = L"显示设备分隔线";
const wchar_t* const kShowPctLabel = L"显示百分号";
const wchar_t* const kAppCaption = L"外观";  // 卡片内的分段标题
constexpr size_t kNoReadMax = 15;
constexpr size_t kDevNameMax = 31;

// 对话框期间的设备名工作副本：列表行与「刷新策略」卡片标题都从这里取，
// 确定 / 应用时才随其余参数一起写回，取消则整体丢弃。
std::wstring g_dlg_names[kDeviceCount];
int g_rename_dev = -1;  // 正在改名的设备，-1 表示没有
HWND g_name_edit = nullptr;
HFONT g_list_font = nullptr;

// 四台都取消勾选时任务栏只剩占位项，设备卡片下方给出提示，免得被当成插件失效
const wchar_t* const kWarnAllUnchecked = L"全部未勾选，\n任务栏只剩占位项";
// 改名进行中占用同一位置，说明当前状态和如何结束
const wchar_t* const kRenameHint = L"输入新名称，回车确认；\nEsc 放弃改名";

int MeasureText(HDC hdc, HFONT font, const wchar_t* s, int* out_h)
{
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
    SelectObject(hdc, old);
    if (out_h)
        *out_h = sz.cy;
    return sz.cx;
}

// 定义在下方（EndRename 改名后需要用，先声明）
void UpdateDetailHeader(HWND hDlg);

// 按当前 DPI 与字体度量计算整套布局：字体放大（125%/150%）时标签与输入框同步放大，文字不裁切。
// compact = 屏幕高度放不下时用更紧的行距（是否启用由下面的 ComputeLayout 决定）。
void ComputeLayoutPass(HWND hDlg, HFONT font, bool compact)
{
    g_lay = DlgLayout{}; // 每轮从零算，避免 Max2 把上一轮的值带过来
    DlgLayout& L = g_lay;
    L.margin = S(compact ? 12 : 16);
    L.pad = S(compact ? 12 : 15);
    L.card_gap = S(compact ? 10 : 12);
    L.radius = S(10);
    L.btn_action_w = S(84);
    L.btn_h = S(compact ? 28 : 32);

    HDC hdc = GetDC(hDlg);
    int text_h = 0;
    MeasureText(hdc, font, L"国Ag", &text_h);
    L.field_h = Max2(text_h + S(8), S(22));
    L.field_step = Max2(text_h + S(12), S(compact ? 24 : 28));
    L.tab_h = Max2(text_h + S(12), S(28));
    L.row_h = Max2(text_h + S(16), S(30));
    L.btn_w = Max2(MeasureText(hdc, font, L"恢复默认", nullptr) + S(24), S(90));

    const int lbl_gap = S(6);
    const int col_gap = S(14);
    const int edit_min = Max2(MeasureText(hdc, font, L"86400", nullptr) + S(20), S(60));

    // ---- 左卡片：列表最小宽 与 两个并排按钮 取较宽者 ----
    const int list_min_w = Max2(MeasureText(hdc, font, L"外设电量显示", nullptr) + S(30), S(146));
    L.half_w = S(84);
    const int btn_col_w = L.half_w * 2 + S(8);
    L.left_content = Max2(list_min_w, btn_col_w);
    L.left_w = L.left_content + L.pad * 2;

    // ---- 右卡片：页签内各块的横向度量 ----
    for (int i = 0; i < kNumFieldsCount; ++i)
        L.num_lbl_w[i % 2] = Max2(L.num_lbl_w[i % 2], MeasureText(hdc, font, kNumFields[i].label, nullptr));
    L.num_lbl_w[0] += S(8);
    L.num_lbl_w[1] += S(8);
    L.num_edit_w = Max2(MeasureText(hdc, font, L"100", nullptr) + S(20), S(52));
    L.num_col_gap = col_gap;
    const int num_w = L.num_lbl_w[0] + lbl_gap + L.num_edit_w + L.num_col_gap +
                      L.num_lbl_w[1] + lbl_gap + L.num_edit_w;

    L.color_lbl_w = Max2(MeasureText(hdc, font, kColorHeads[0], nullptr),
                         MeasureText(hdc, font, kColorHeads[1], nullptr)) + S(8);
    const int chip_min = Max2(MeasureText(hdc, font, L"FFFFFF", nullptr) + S(16), S(66));
    const int chip_gap = S(8);
    const int color_w = L.color_lbl_w + lbl_gap + 3 * chip_min + 2 * chip_gap;

    int ref_lbl = 0;
    for (int i = 0; i < 15; ++i)
        ref_lbl = Max2(ref_lbl, MeasureText(hdc, font, kRefFields[i].label, nullptr));
    L.ref_lbl_w = ref_lbl + S(8);
    L.ref_edit_w = edit_min;
    L.ref_col_gap = col_gap;
    const int ref_w = 2 * (L.ref_lbl_w + lbl_gap + L.ref_edit_w) + L.ref_col_gap;

    L.pct_w = MeasureText(hdc, font, kShowPctLabel, nullptr) + S(28);
    L.noread_lbl_w = MeasureText(hdc, font, kNoReadLabel, nullptr) + S(8);
    L.noread_edit_w = Max2(MeasureText(hdc, font, L"--", nullptr) + S(20), S(56));
    const int bottom_w = L.pct_w + S(18) + L.noread_lbl_w + lbl_gap + L.noread_edit_w;

    int content_w = Max2(num_w, color_w);
    content_w = Max2(content_w, ref_w);
    content_w = Max2(content_w, bottom_w);
    content_w = Max2(content_w, S(296)); // 页签与标题至少留出这个宽度，避免挤成一条
    L.right_w = content_w + L.pad * 2;
    L.tab_w = content_w / 2;

    // 配色 chip 按内容宽三等分（至少容得下 6 位十六进制）
    L.chip_w = Max2(chip_min, (content_w - L.color_lbl_w - lbl_gap - 2 * chip_gap) / 3);
    L.chip_off[0] = L.color_lbl_w + lbl_gap;
    L.chip_off[1] = L.chip_off[0] + L.chip_w + chip_gap;
    L.chip_off[2] = L.chip_off[1] + L.chip_w + chip_gap;

    // ---- 横向定位：左卡片靠左，右卡片占满剩余 ----
    L.left_x = L.margin;
    L.left_y = L.margin;
    L.right_x = L.left_x + L.left_w + L.card_gap;
    L.right_y = L.left_y;
    L.w = L.right_x + L.right_w + L.margin;

    // ---- 纵向：详情卡片（绝对坐标，便于创建控件时直接用）----
    const int name_h = Max2(text_h + S(12), S(26));
    L.name_y = L.right_y + L.pad;
    L.name_btn_w = Max2(MeasureText(hdc, font, L"重命名", nullptr) + S(30), S(72));
    L.name_btn_x = L.right_x + L.right_w - L.pad - L.name_btn_w;
    L.name_btn_y = L.name_y + (name_h - L.field_h) / 2;
    L.tab_y = L.name_y + name_h + S(10);
    L.body_y = L.tab_y + L.tab_h + S(12);

    // 外观页签：数值 2×2 → 「电量配色」→ 表头 → 两行色块 → 底部开关
    L.cap_color_y = L.body_y + 2 * L.field_step + S(10);
    L.color_head_y = L.cap_color_y + S(22);
    L.color_row_y = L.color_head_y + S(22);
    L.pct_y = L.color_row_y + 2 * L.field_step + S(8);
    const int app_btm = L.pct_y + L.field_h;

    // 刷新页签：按项数最多的设备留位（键鼠各 5 项 → 3 行），下面一段说明
    int max_rows = 1;
    for (int d = 0; d < kDeviceCount; ++d)
    {
        int n = 0;
        for (int i = 0; i < 15; ++i)
            if (kRefFields[i].dev == d)
                ++n;
        max_rows = Max2(max_rows, (n + 1) / 2);
    }
    // 刷新页签与外观页签都从同一条基线开始，切页签时第一行不会跳位置
    L.ref_y0 = L.body_y;
    const int ref_btm = L.ref_y0 + max_rows * L.field_step;

    L.body_btm = Max2(app_btm, ref_btm);
    L.right_h = L.body_btm + L.pad;

    // ---- 纵向：左侧设备卡片（与右侧等高，列表吃掉多余高度）----
    const int cap_h = text_h + S(10);
    const int hint_h = S(38);
    const int fixed_h = L.pad + cap_h + S(8) + S(12) + L.btn_h + S(10) + hint_h + L.pad;
    const int row_min = Max2(text_h + S(16), S(30));
    L.list_h = Max2(L.right_h - fixed_h, kDeviceCount * row_min);
    L.row_h = L.list_h / kDeviceCount; // 4 行均分，填满列表区，不留半截空白
    L.left_h = fixed_h + L.list_h;
    L.right_h = Max2(L.right_h, L.left_h);

    L.left_cap_y = L.left_y + L.pad;
    L.list_x = L.left_x + L.pad;
    L.list_y = L.left_cap_y + cap_h + S(8);
    L.list_w = L.left_content;
    L.updown_y = L.list_y + L.list_h + S(12);
    L.hint_y = L.updown_y + L.btn_h + S(10);
    L.hint_x = L.list_x;
    L.hint_w = L.left_content;

    // ---- 底部：全局布局一行 + 按钮行 ----
    L.global_y = L.left_y + L.left_h + S(12);
    L.global_h = L.field_h + S(16);
    L.bottom_y = L.global_y + L.global_h + S(12);
    L.h = L.bottom_y + L.btn_h + L.margin;

    L.g_cap_w = MeasureText(hdc, font, L"全局布局", nullptr) + S(4);
    L.g_lbl_w = Max2(MeasureText(hdc, font, kLayoutFields[0].label, nullptr),
                     MeasureText(hdc, font, kLayoutFields[1].label, nullptr)) + S(8);
    L.g_ed_w = Max2(MeasureText(hdc, font, L"60", nullptr) + S(24), S(54));
    L.g_cap_x = L.margin + L.pad;
    L.g_sep_x = L.g_cap_x + L.g_cap_w + S(12); // 「全局布局」是分组标签，用竖线与参数隔开
    L.g_lbl_x[0] = L.g_sep_x + S(12);
    L.g_ed_x[0] = L.g_lbl_x[0] + L.g_lbl_w + lbl_gap;
    L.g_lbl_x[1] = L.g_ed_x[0] + L.g_ed_w + S(18);
    L.g_ed_x[1] = L.g_lbl_x[1] + L.g_lbl_w + lbl_gap;
    L.g_chk_x = L.g_ed_x[1] + L.g_ed_w + S(24);

    ReleaseDC(hDlg, hdc);
}

// 先按正常行距算一遍；整窗高度超过屏幕工作区时改用紧凑行距重算，
// 免得高 DPI / 小屏上底部按钮被裁到屏幕外。
void ComputeLayout(HWND hDlg, HFONT font)
{
    ComputeLayoutPass(hDlg, font, false);

    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        return;
    RECT wnd{ 0, 0, g_lay.w, g_lay.h };
    AdjustWindowRectEx(&wnd, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, FALSE, 0);
    if (wnd.bottom - wnd.top > wa.bottom - wa.top - S(8))
        ComputeLayoutPass(hDlg, font, true);
}

WNDPROC g_origBtnProc = nullptr;  // BUTTON 原窗口过程（子类化用）
HWND g_hover_btn = nullptr;       // 当前悬停的按钮（自绘悬停态）

// 勾选状态变化后刷新「全部取消勾选」提示的显隐
static void UpdateDeviceWarning(HWND hDlg)
{
    HWND warn = GetDlgItem(hDlg, IDC_DEVWARN);
    if (!warn)
        return;
    Plugin* self = (Plugin*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
    bool any = false;
    for (int d = 0; d < kDeviceCount && !any; ++d)
        any = self && self->DlgShowFor(d);
    ShowWindow(warn, (any || g_name_edit) ? SW_HIDE : SW_SHOWNA);
    // 隐藏时静态控件不会自己擦除，就地重绘这块背景（擦除走 WM_ERASEBKGND，会连白色卡片一起重画）
    const RECT rc{ g_lay.hint_x, g_lay.hint_y,
                   g_lay.hint_x + g_lay.hint_w, g_lay.hint_y + S(38) };
    InvalidateRect(hDlg, &rc, TRUE);
}

// 改名进行中的状态提示：标题行右侧换成操作说明，「改名」按钮置灰，
// 结束（提交 / 放弃）后还原。两个提示静态控件位置相同，任一时刻只显示一个。
static void SetRenameChrome(HWND hDlg, bool on)
{
    if (on)
        ShowWindow(GetDlgItem(hDlg, IDC_DEVWARN), SW_HIDE);
    ShowWindow(GetDlgItem(hDlg, IDC_RENAMEHINT), on ? SW_SHOWNA : SW_HIDE);
    EnableWindow(GetDlgItem(hDlg, IDC_RENAME), on ? FALSE : TRUE);
    if (!on)
        UpdateDeviceWarning(hDlg);
    const RECT rc{ g_lay.hint_x, g_lay.hint_y,
                   g_lay.hint_x + g_lay.hint_w, g_lay.hint_y + S(38) };
    InvalidateRect(hDlg, &rc, TRUE);
}

// 按当前顺序重建列表，并选中指定设备（-1 则保留首行）
static void RebuildDevList(HWND hDlg, int selectDev)
{
    Plugin* self = (Plugin*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (!self || !hList)
        return;
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (int row = 0; row < kDeviceCount; ++row)
    {
        const int dev = self->DlgOrderAt(row);
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)g_dlg_names[dev].c_str());
        SendMessageW(hList, LB_SETITEMDATA, row, dev);
        if (dev == selectDev)
            sel = row;
    }
    SendMessageW(hList, LB_SETCURSEL, sel, 0);
    EnableWindow(GetDlgItem(hDlg, IDC_UP), sel > 0);
    EnableWindow(GetDlgItem(hDlg, IDC_DOWN), sel < kDeviceCount - 1);
    UpdateDeviceWarning(hDlg);
}

static int SelectedDev(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (!hList)
        return -1;
    const int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return -1;
    return (int)SendMessageW(hList, LB_GETITEMDATA, sel, 0);
}

// 就地改名结束：keep=true 提交（空名 / 超长按边界处理），否则放弃
static void EndRename(HWND hDlg, bool keep)
{
    if (!g_name_edit)
        return;
    HWND e = g_name_edit;
    const int dev = g_rename_dev;
    g_name_edit = nullptr;
    g_rename_dev = -1;
    if (keep && dev >= 0)
    {
        wchar_t buf[128]{};
        GetWindowTextW(e, buf, 128);
        std::wstring v(buf);
        size_t b = 0, n = v.size();
        while (b < n && (v[b] == L' ' || v[b] == L'\t'))
            ++b;
        while (n > b && (v[n - 1] == L' ' || v[n - 1] == L'\t'))
            --n;
        v = v.substr(b, n - b);
        if (v.size() > kDevNameMax)
            v.resize(kDevNameMax);
        if (!v.empty())
            g_dlg_names[dev] = v;
        // 详情卡标题与设备名同源，改名后立即同步
        if (dev == g_cur_dev)
            UpdateDetailHeader(hDlg);
    }
    DestroyWindow(e);
    if (HWND title = GetDlgItem(hDlg, IDC_DETAIL_NAME))
        ShowWindow(title, SW_SHOW);
    SetRenameChrome(hDlg, false);

    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (hList)
    {
        // 列表行文字与标题同源：重建列表让它跟着变，并保持原选中行
        RebuildDevList(hDlg, dev >= 0 ? dev : SelectedDev(hDlg));
        InvalidateRect(hList, nullptr, TRUE);
        SetFocus(hList);
    }
}

// 重命名：输入框直接盖在详情卡标题上，回车 / 点到别处提交，Esc 放弃
static void BeginRename(HWND hDlg)
{
    if (g_name_edit)
        return;
    if (g_cur_dev < 0 || g_cur_dev >= kDeviceCount)
        return;
    HWND title = GetDlgItem(hDlg, IDC_DETAIL_NAME);
    if (!title)
        return;

    RECT rc{};
    GetWindowRect(title, &rc);
    MapWindowPoints(nullptr, hDlg, (POINT*)&rc, 2);

    g_rename_dev = g_cur_dev;
    g_name_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dlg_names[g_cur_dev].c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hDlg,
                                  (HMENU)(UINT_PTR)IDC_DEVNAME, GetModuleHandleW(nullptr), nullptr);
    if (!g_name_edit)
    {
        g_rename_dev = -1;
        return;
    }

    HFONT f = g_name_font ? g_name_font : (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
    SendMessageW(g_name_edit, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_name_edit, EM_SETLIMITTEXT, (WPARAM)kDevNameMax, 0);
    SendMessageW(g_name_edit, EM_SETSEL, 0, -1);

    ShowWindow(title, SW_HIDE); // 标题让位给输入框，避免两层文字叠在一起
    SetRenameChrome(hDlg, true);
    SetFocus(g_name_edit);
}

// 对话框底色画刷：分组框 / 标签的背景与 WM_ERASEBKGND 保持一致
HBRUSH DlgBgBrush()
{
    static HBRUSH br = CreateSolidBrush(kDlgBg);
    return br;
}

// 卡片内所有标签都用白底：擦除时父窗口已经把卡片画成白色，
// 标签自身若有背景也必须同色，否则会出现灰块。
HBRUSH WhiteBrush()
{
    static HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
    return br;
}

// 页签悬停时的淡淡蓝底
HBRUSH DlgHoverBrush()
{
    static HBRUSH br = CreateSolidBrush(RGB(243, 248, 255));
    return br;
}

void SetIntText(HWND edit, int v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    SetWindowTextW(edit, buf);
}

int SecFromMs(int ms)
{
    int s = (ms + 500) / 1000;
    return s < 1 ? 1 : s;
}

// 配色块：一个自绘按钮，底色就是当前颜色，点一下弹系统颜色选择器。
// 下标 = 设备 * kColorCount + slot；四台设备各存一份，同屏只有当前设备的 6 个可见。
constexpr int kChipCount = kDeviceCount * kColorCount;

COLORREF g_chip_color[kChipCount] = {};
COLORREF g_cust_colors[16] = {}; // 颜色对话框里的「自定义颜色」，会话内共用

// 底色太亮时描边压暗一点，避免浅色块在白卡片上看不出边界
COLORREF DarkenColor(COLORREF c)
{
    return RGB(GetRValue(c) * 3 / 4, GetGValue(c) * 3 / 4, GetBValue(c) * 3 / 4);
}

COLORREF ChipTextColor(COLORREF c)
{
    const int luma = ((int)GetRValue(c) * 299 + (int)GetGValue(c) * 587 + (int)GetBValue(c) * 114) / 1000;
    return luma > 150 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// 用指定配置填充全部输入框（设备卡片里的控件按设备索引编号）
void FillAdvancedFields(HWND hDlg, const RefreshConfig& r, const LayoutConfig& lay,
                        const DeviceStyle* styles, const TextConfig& t)
{
    for (int i = 0; i < 15; ++i)
        SetIntText(GetDlgItem(hDlg, IDC_REF0 + i), SecFromMs(r.*kRefFields[i].member));

    for (int i = 0; i < kLayoutFieldsCount; ++i)
        SetIntText(GetDlgItem(hDlg, IDC_LAY0 + i), lay.*kLayoutFields[i].member);
    CheckDlgButton(hDlg, IDC_SHOW_SEP, lay.show_sep ? BST_CHECKED : BST_UNCHECKED);

    for (int dev = 0; dev < kDeviceCount; ++dev)
    {
        const DeviceStyle& s = styles[dev];
        for (int i = 0; i < kNumFieldsCount; ++i)
            SetIntText(GetDlgItem(hDlg, IDC_NUM0 + dev * kNumFieldsCount + i), s.*kNumFields[i].member);
        CheckDlgButton(hDlg, IDC_SHOWPCT0 + dev, s.show_pct_sign ? BST_CHECKED : BST_UNCHECKED);
        for (int i = 0; i < kColorCount; ++i)
        {
            const int idx = dev * kColorCount + i;
            g_chip_color[idx] = s.*kColorMembers[i];
            InvalidateRect(GetDlgItem(hDlg, IDC_COLOR0 + idx), nullptr, TRUE);
        }
        SetWindowTextW(GetDlgItem(hDlg, IDC_NOREAD0 + dev), s.no_read.c_str());
    }

    for (int i = 0; i < kDeviceCount; ++i)
        g_dlg_names[i] = t.name[i];
}

// 用当前生效的配置填充
void FillAdvancedFields(HWND hDlg)
{
    DeviceStyle styles[kDeviceCount];
    for (int i = 0; i < kDeviceCount; ++i)
        styles[i] = PluginConfig::Instance().Style(i);
    FillAdvancedFields(hDlg, PluginConfig::Instance().Refresh(), PluginConfig::Instance().Layout(),
                       styles, PluginConfig::Instance().Text());
}

void WarnField(HWND hDlg, HWND edit, const std::wstring& msg)
{
    MessageBoxW(hDlg, msg.c_str(), L"显示设置", MB_OK | MB_ICONWARNING);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}

// 详情卡标题：设备名 + 类别，跟随左侧列表的选中项
void UpdateDetailHeader(HWND hDlg)
{
    if (g_cur_dev < 0 || g_cur_dev >= kDeviceCount)
        return;
    SetWindowTextW(GetDlgItem(hDlg, IDC_DETAIL_NAME), g_dlg_names[g_cur_dev].c_str());
}

// 按「当前设备 + 当前页签」切换控件显隐。
// 其余设备 / 另一个页签的输入框只是叠在同一块区域里被隐藏，不重建，
// 所以切换来回不会丢已经输入但尚未应用的值。
void SyncDetailVisible(HWND hDlg)
{
    const bool app = (g_cur_tab == 0);
    auto show = [&](UINT id, bool on) {
        if (HWND w = GetDlgItem(hDlg, id))
            ShowWindow(w, on ? SW_SHOW : SW_HIDE);
    };

    // 外观页签：标签只有一份，随页签整体显隐
    for (int i = 0; i < kNumFieldsCount; ++i)
        show(IDC_LBL_NUM0 + i, app);
    for (int i = 0; i < kColorRowsCount; ++i)
        show(IDC_LBL_COLORROW0 + i, app);
    for (int i = 0; i < 2; ++i)
        show(IDC_LBL_COLORHEAD0 + i, app);
    show(IDC_LBL_CAP_COLOR, app);
    show(IDC_LBL_NOREAD, app);

    // 刷新页签：标签按 kRefFields 索引，只显示当前设备那几项
    for (int i = 0; i < 15; ++i)
        show(IDC_LBL_REF0 + i, !app && kRefFields[i].dev == g_cur_dev);


    // 输入控件按设备分组，只有「当前设备 + 当前页签」那一组可见
    for (int dev = 0; dev < kDeviceCount; ++dev)
    {
        const bool on_app = app && dev == g_cur_dev;
        const bool on_ref = !app && dev == g_cur_dev;
        for (int i = 0; i < kNumFieldsCount; ++i)
            show(IDC_NUM0 + dev * kNumFieldsCount + i, on_app);
        for (int i = 0; i < kColorCount; ++i)
            show(IDC_COLOR0 + dev * kColorCount + i, on_app);
        show(IDC_NOREAD0 + dev, on_app);
        show(IDC_SHOWPCT0 + dev, on_app);
        for (int i = 0; i < 15; ++i)
            if (kRefFields[i].dev == dev)
                show(IDC_REF0 + i, on_ref);
    }

    // 页签按钮重绘选中态
    InvalidateRect(GetDlgItem(hDlg, IDC_TAB_APP), nullptr, TRUE);
    InvalidateRect(GetDlgItem(hDlg, IDC_TAB_REF), nullptr, TRUE);
}

bool ReadIntField(HWND hDlg, HWND edit, const wchar_t* label, int lo, int hi,
                  const std::wstring& unit, int& out)
{
    wchar_t buf[32]{};
    GetWindowTextW(edit, buf, 32);
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    while (*end == L' ' || *end == L'\t')
        ++end;
    if (end == buf || *end != L'\0' || v < lo || v > hi)
    {
        WarnField(hDlg, edit, std::wstring(L"“") + label + L"”" + unit + L"需要是 " +
                                   std::to_wstring(lo) + L" ~ " + std::to_wstring(hi) +
                                   L" 的整数。");
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool ReadTextField(HWND hDlg, HWND edit, const wchar_t* label, size_t maxlen, std::wstring& out)
{
    wchar_t buf[128]{};
    GetWindowTextW(edit, buf, 128);
    std::wstring v(buf);
    size_t b = 0;
    size_t e = v.size();
    while (b < e && (v[b] == L' ' || v[b] == L'\t'))
        ++b;
    while (e > b && (v[e - 1] == L' ' || v[e - 1] == L'\t'))
        --e;
    v = v.substr(b, e - b);
    if (v.empty())
    {
        WarnField(hDlg, edit, std::wstring(L"“") + label + L"” 不能为空。");
        return false;
    }
    if (v.size() > maxlen)
    {
        WarnField(hDlg, edit, std::wstring(L"“") + label + L"” 最多 " +
                                   std::to_wstring(maxlen) + L" 个字符。");
        return false;
    }
    out = v;
    return true;
}

// 颜色由选择器写入，这里只需要取当前值，不会出现非法输入
bool ReadColorField(HWND, int dev, int slot, COLORREF& out)
{
    out = g_chip_color[dev * kColorCount + slot];
    return true;
}

// 校验并写回配置
bool ReadAdvancedFields(HWND hDlg)
{
    RefreshConfig r;
    int sec[15]{};
    for (int i = 0; i < 15; ++i)
    {
        const std::wstring label =
            PluginConfig::Instance().Name(kRefFields[i].dev) + L" " + kRefFields[i].label;
        if (!ReadIntField(hDlg, GetDlgItem(hDlg, IDC_REF0 + i), label.c_str(),
                          kRefFields[i].lo, kRefFields[i].hi, L"（秒）", sec[i]))
            return false;
    }
    for (int i = 0; i < 15; ++i)
        r.*kRefFields[i].member = sec[i] * 1000;

    // 全局布局
    LayoutConfig lay = PluginConfig::Instance().Layout();
    for (int i = 0; i < kLayoutFieldsCount; ++i)
        if (!ReadIntField(hDlg, GetDlgItem(hDlg, IDC_LAY0 + i), kLayoutFields[i].label,
                          kLayoutFields[i].lo, kLayoutFields[i].hi, L" ", lay.*kLayoutFields[i].member))
            return false;
    lay.show_sep = IsDlgButtonChecked(hDlg, IDC_SHOW_SEP) == BST_CHECKED;

    // 每台设备各一份外观
    DeviceStyle styles[kDeviceCount];
    for (int dev = 0; dev < kDeviceCount; ++dev)
    {
        const std::wstring prefix = PluginConfig::Instance().Name(dev) + L" ";
        DeviceStyle& s = styles[dev];
        for (int i = 0; i < kNumFieldsCount; ++i)
        {
            const std::wstring label = prefix + kNumFields[i].label;
            if (!ReadIntField(hDlg, GetDlgItem(hDlg, IDC_NUM0 + dev * kNumFieldsCount + i),
                              label.c_str(), kNumFields[i].lo, kNumFields[i].hi, L" ",
                              s.*kNumFields[i].member))
                return false;
        }
        if (s.level_low >= s.level_high)
        {
            WarnField(hDlg, GetDlgItem(hDlg, IDC_NUM0 + dev * kNumFieldsCount + 1),
                      std::wstring(L"“") + prefix + L"”的「中电量分界 %」必须小于「高电量分界 %」。");
            return false;
        }
        s.show_pct_sign = IsDlgButtonChecked(hDlg, IDC_SHOWPCT0 + dev) == BST_CHECKED;
        for (int i = 0; i < kColorCount; ++i)
            if (!ReadColorField(hDlg, dev, i, s.*kColorMembers[i]))
                return false;
        if (!ReadTextField(hDlg, GetDlgItem(hDlg, IDC_NOREAD0 + dev), kNoReadLabel, kNoReadMax, s.no_read))
            return false;
    }

    TextConfig t;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        if (g_dlg_names[i].empty())
        {
            MessageBoxW(hDlg, (L"“" + PluginConfig::Instance().Name(i) + L"” 的名称不能为空。").c_str(),
                        L"显示设置", MB_OK | MB_ICONWARNING);
            return false;
        }
        t.name[i] = g_dlg_names[i];
    }

    PluginConfig::Instance().SetAll(r, lay, styles, t);
    return true;
}

WNDPROC g_origListProc = nullptr; // LISTBOX 原窗口过程（子类化用）
int g_hover_row = -1;             // 列表当前悬停行（自绘悬停态）

// LISTBOX 子类化：悬停跟踪 + 点击左侧复选框热区切换勾选（其余走默认选中逻辑）
LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        {
            const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
            int row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
            if (row != g_hover_row)
            {
                g_hover_row = row;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hover_row != -1)
        {
            g_hover_row = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDBLCLK:
        {
            // 双击设备名区域直接改名（复选框区域除外），省掉「先选中再点按钮」
            const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
            const int row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
            if (row >= 0 && row < kDeviceCount && GET_X_LPARAM(lp) >= S(40))
            {
                SendMessageW(hwnd, LB_SETCURSEL, row, 0);
                // 走一遍选中通知，让详情卡同步到这一台，再进入改名
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_DEVLIST, LBN_SELCHANGE),
                             (LPARAM)hwnd);
                BeginRename(GetParent(hwnd));
                return 0;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        {
            Plugin* self = (Plugin*)GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA);
            const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
            const int row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
            if (self && row >= 0 && row < kDeviceCount && GET_X_LPARAM(lp) < S(40))
            {
                self->DlgToggle((int)SendMessageW(hwnd, LB_GETITEMDATA, row, 0));
                UpdateDeviceWarning(GetParent(hwnd));
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    }
    return CallWindowProcW(g_origListProc, hwnd, msg, wp, lp);
}

// BUTTON 子类化：自绘按钮的悬停状态跟踪（WM_DRAWITEM 绘制时使用）
// 弹系统颜色选择器，选完直接更新色块（不需要手写十六进制）
void PickChipColor(HWND hDlg, int idx)
{
    if (idx < 0 || idx >= kChipCount)
        return;
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hDlg;
    cc.lpCustColors = g_cust_colors;
    cc.rgbResult = g_chip_color[idx];
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc))
    {
        g_chip_color[idx] = cc.rgbResult;
        InvalidateRect(GetDlgItem(hDlg, IDC_COLOR0 + idx), nullptr, TRUE);
    }
}

LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (g_hover_btn != hwnd)
        {
            g_hover_btn = hwnd;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hover_btn == hwnd)
        {
            g_hover_btn = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    return CallWindowProcW(g_origBtnProc, hwnd, msg, wp, lp);
}

LRESULT CALLBACK OptionDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Plugin* self = (Plugin*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_INITDIALOG:
        // 创建参数即 Plugin 实例；本过程经 DefDlgProc 转发，未处理的消息返回 FALSE
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, lp);
        return TRUE;
    case WM_ERASEBKGND:
        {
            // 浅灰画布 + 三张白色圆角卡片（左「设备」/ 右「详情」/ 底部「全局布局」）。
            // 卡片在这里画，任何局部重绘都会连带重画底下的卡片，不会留下灰块。
            const DlgLayout& L = g_lay;
            HDC hdc = (HDC)wp;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, DlgBgBrush());

            auto card = [&](int x, int y, int w, int h) {
                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, RGB(228, 228, 232)));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(RGB(255, 255, 255)));
                RoundRect(hdc, x, y, x + w, y + h, L.radius * 2, L.radius * 2);
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));
            };
            card(L.left_x, L.left_y, L.left_w, L.left_h);
            card(L.right_x, L.right_y, L.right_w, L.right_h);
            card(L.margin, L.global_y, L.w - L.margin * 2, L.global_h);

            // 底部「全局布局」标签与参数之间的竖分隔线
            {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(226, 226, 230));
                HGDIOBJ op = SelectObject(hdc, pen);
                const int y0 = L.global_y + (L.global_h - L.field_h) / 2;
                MoveToEx(hdc, L.g_sep_x, y0, nullptr);
                LineTo(hdc, L.g_sep_x, y0 + L.field_h);
                SelectObject(hdc, op);
                DeleteObject(pen);
            }

            // 页签底部的整条分隔线（选中页签会在自己身上盖一段主题蓝）
            {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(232, 232, 236));
                HGDIOBJ op = SelectObject(hdc, pen);
                const int y = L.tab_y + L.tab_h - 1;
                MoveToEx(hdc, L.right_x + L.pad, y, nullptr);
                LineTo(hdc, L.right_x + L.right_w - L.pad, y);
                SelectObject(hdc, op);
                DeleteObject(pen);
            }
            return 1;
        }
    case WM_PAINT:
        {
            // 背景与卡片都由 WM_ERASEBKGND 负责，这里只消化重绘区域
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return TRUE;
        }
    case WM_CTLCOLORSTATIC:
        {
            // 卡片内的标签：白底、透明字，与卡片同色；不同用途换文字色
            HDC hdc = (HDC)wp;
            const UINT sid = GetDlgCtrlID((HWND)lp);
            if (sid == IDC_DEVWARN)
                SetTextColor(hdc, RGB(176, 90, 28)); // 提示用暖色，与普通标签区分
            else if (sid == IDC_RENAMEHINT)
                SetTextColor(hdc, RGB(0, 90, 158)); // 改名提示用主题蓝
            else if (sid == IDC_DETAIL_NAME)
                SetTextColor(hdc, RGB(20, 20, 24));
            else
                SetTextColor(hdc, RGB(38, 38, 42));
            SetBkColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)WhiteBrush();
        }
    case WM_CTLCOLOREDIT:
        // 配色输入框以该颜色作背景，文字按亮度取黑白
        {
            HWND e = (HWND)lp;
            if (e == g_name_edit)
            {
                // 改名输入框：浅蓝底，配合行上的蓝框表明正在编辑
                static HBRUSH br = CreateSolidBrush(RGB(232, 243, 255));
                HDC hdc = (HDC)wp;
                SetTextColor(hdc, RGB(20, 20, 20));
                SetBkColor(hdc, RGB(232, 243, 255));
                return (LRESULT)br;
            }
        }
        break;
    case WM_DESTROY:
        g_name_edit = nullptr; // 子控件随对话框一起销毁，留指针只为避免误用
        g_rename_dev = -1;
        g_hover_row = -1;
        break;
    case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;

            // ---- 设备列表行：白卡片 + 圆角行高亮（选中淡蓝 / 悬停浅灰）+ 圆角复选框 ----
            if (dis->CtlID == IDC_DEVLIST && dis->itemID != (UINT)-1)
            {
                const int dev = (int)dis->itemData;
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                const bool sel = (dis->itemState & ODS_SELECTED) != 0;
                const bool hov = (g_hover_row == (int)dis->itemID) && !sel;

                const COLORREF bg =
                    sel ? RGB(233, 243, 255) : (hov ? RGB(244, 244, 246) : RGB(255, 255, 255));
                RECT rr = rc;
                InflateRect(&rr, -S(2), -S(2));
                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, bg));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(bg));
                RoundRect(hdc, rr.left, rr.top, rr.right, rr.bottom, S(6), S(6));
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                // 复选框：未选白底灰边、选中主题蓝底白勾
                const int bh = rc.bottom - rc.top;
                const int cs = S(18);
                const bool checked = self && self->DlgShowFor(dev);
                const RECT box{ rc.left + S(12), rc.top + (bh - cs) / 2,
                                rc.left + S(12) + cs, rc.top + (bh - cs) / 2 + cs };
                const COLORREF fillCol = checked ? RGB(0, 103, 192) : RGB(255, 255, 255);
                const COLORREF brdCol = checked ? RGB(0, 103, 192) : RGB(130, 130, 135);
                op = SelectObject(hdc, CreatePen(PS_SOLID, 1, brdCol));
                ob = SelectObject(hdc, CreateSolidBrush(fillCol));
                RoundRect(hdc, box.left, box.top, box.right, box.bottom, S(4), S(4));
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));
                if (checked)
                {
                    HGDIOBJ oh = SelectObject(hdc, CreatePen(PS_SOLID, Max2(S(2), 1), RGB(255, 255, 255)));
                    MoveToEx(hdc, box.left + cs * 4 / 20, box.top + cs * 9 / 20, nullptr);
                    LineTo(hdc, box.left + cs * 8 / 20, box.top + cs * 14 / 20);
                    LineTo(hdc, box.left + cs * 15 / 20, box.top + cs * 6 / 20);
                    DeleteObject(SelectObject(hdc, oh));
                }

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(20, 20, 20));
                RECT rcTxt = rc;
                rcTxt.left += S(40);
                DrawTextW(hdc, g_dlg_names[dev].c_str(), -1, &rcTxt,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                return TRUE;
            }

            // ---- 配色块：底色即当前颜色，文字是十六进制；点击弹颜色选择器 ----
            if (dis->CtlID >= IDC_COLOR0 && dis->CtlID < IDC_COLOR0 + (UINT)kChipCount)
            {
                const int idx = (int)dis->CtlID - (int)IDC_COLOR0;
                const COLORREF c = g_chip_color[idx];
                const bool hover = (g_hover_btn == dis->hwndItem);
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;

                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1,
                                                         hover ? RGB(0, 103, 192) : DarkenColor(c)));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(c));
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, S(8), S(8));
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                wchar_t buf[8]{};
                swprintf(buf, 8, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
                HGDIOBJ of = SelectObject(hdc, (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, ChipTextColor(c));
                DrawTextW(hdc, buf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, of);
                return TRUE;
            }

            // ---- 页签：选中项主题蓝字 + 2px 底线，未选中灰字 + 1px 浅线 ----
            if (dis->CtlID == IDC_TAB_APP || dis->CtlID == IDC_TAB_REF)
            {
                const bool on = (dis->CtlID == IDC_TAB_APP) ? (g_cur_tab == 0) : (g_cur_tab == 1);
                const bool hover = (g_hover_btn == dis->hwndItem) && !on;
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;

                FillRect(hdc, &rc, WhiteBrush());
                if (hover)
                {
                    RECT rh = rc;
                    rh.bottom -= 1;
                    FillRect(hdc, &rh, DlgHoverBrush());
                }

                // 底线：选中用主题蓝加粗，未选中补一条与页面同色的细线，保证整条线连贯
                RECT ln = rc;
                ln.top = ln.bottom - (on ? Max2(S(2), 2) : 1);
                HBRUSH b = CreateSolidBrush(on ? RGB(0, 103, 192) : RGB(232, 232, 236));
                FillRect(hdc, &ln, b);
                DeleteObject(b);

                wchar_t caption[32];
                int n = GetWindowTextW(dis->hwndItem, caption, 32);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, on ? RGB(0, 103, 192) : (hover ? RGB(60, 60, 66) : RGB(112, 112, 118)));
                DrawTextW(hdc, caption, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }

            // ---- 按钮：圆角 4px，白底灰边；默认（确定）主题蓝底白字 ----
            if (dis->CtlType == ODT_BUTTON)
            {
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                bool defBtn = (dis->CtlID == IDC_OK);
                bool disabled = (dis->itemState & ODS_DISABLED) != 0;
                bool hover = (g_hover_btn == dis->hwndItem) && !disabled;
                bool pressed = (dis->itemState & ODS_SELECTED) != 0;

                COLORREF fill, border, txt;
                if (defBtn)
                {
                    fill = pressed ? RGB(0, 89, 169) : (hover ? RGB(26, 120, 208) : RGB(0, 103, 192));
                    border = RGB(0, 80, 158);
                    txt = RGB(255, 255, 255);
                }
                else if (disabled)
                {
                    fill = kDlgBg;
                    border = RGB(225, 225, 228);
                    txt = RGB(178, 178, 184);
                }
                else
                {
                    fill = pressed ? RGB(229, 236, 245) : (hover ? RGB(245, 249, 255) : RGB(255, 255, 255));
                    border = hover || pressed ? RGB(0, 103, 192) : RGB(198, 198, 203);
                    txt = RGB(38, 38, 42);
                }

                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, border));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(fill));
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                wchar_t caption[64];
                int n = GetWindowTextW(dis->hwndItem, caption, 64);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, txt);
                DrawTextW(hdc, caption, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return FALSE;
        }
    case WM_COMMAND:
        {
            UINT id = LOWORD(wp);
            if (HIWORD(wp) == BN_CLICKED && id >= IDC_COLOR0 && id < IDC_COLOR0 + (UINT)kChipCount)
            {
                PickChipColor(hwnd, (int)id - (int)IDC_COLOR0);
                return TRUE;
            }
            // 回车 / Esc 在改名输入框里先结束改名，不触发确定 / 关窗
            if (id == IDOK && g_name_edit)
            {
                EndRename(hwnd, true);
                return TRUE;
            }
            if (id == IDCANCEL && g_name_edit)
            {
                EndRename(hwnd, false);
                return TRUE;
            }
            if (id == IDC_DEVNAME && HIWORD(wp) == EN_KILLFOCUS)
            {
                EndRename(hwnd, true);
                return TRUE;
            }
            // 设备列表：换行时同步 上移 / 下移 的可用性，并把详情面板切到该设备
            if (id == IDC_DEVLIST && HIWORD(wp) == LBN_SELCHANGE)
            {
                HWND hList = GetDlgItem(hwnd, IDC_DEVLIST);
                const int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                EnableWindow(GetDlgItem(hwnd, IDC_UP), sel > 0);
                EnableWindow(GetDlgItem(hwnd, IDC_DOWN), sel >= 0 && sel < kDeviceCount - 1);
                if (self && sel >= 0)
                {
                    g_cur_dev = self->DlgOrderAt(sel);
                    UpdateDetailHeader(hwnd);
                    SyncDetailVisible(hwnd);
                }
                return TRUE;
            }
            // 页签：外观 / 刷新策略
            if ((id == IDC_TAB_APP || id == IDC_TAB_REF) && HIWORD(wp) == BN_CLICKED)
            {
                const int tab = (id == IDC_TAB_APP) ? 0 : 1;
                if (g_cur_tab != tab)
                {
                    g_cur_tab = tab;
                    SyncDetailVisible(hwnd);
                }
                return TRUE;
            }
            if ((id == IDC_UP || id == IDC_DOWN) && HIWORD(wp) == BN_CLICKED)
            {
                const int dev = SelectedDev(hwnd);
                if (self && dev >= 0)
                {
                    if (id == IDC_UP)
                        self->DlgMoveUp(dev);
                    else
                        self->DlgMoveDown(dev);
                    RebuildDevList(hwnd, dev);
                }
                SetFocus(GetDlgItem(hwnd, IDC_DEVLIST));
                return TRUE;
            }
            if (id == IDC_RENAME && HIWORD(wp) == BN_CLICKED)
            {
                BeginRename(hwnd);
                return TRUE;
            }
            if (id == IDC_RESET)
            {
                // 一键还原：勾选、显示顺序、刷新策略、外观、文案全部回到出厂值（仍待确定 / 应用）
                if (self)
                {
                    EndRename(hwnd, false);
                    self->DlgRestoreDefault();
                    DeviceStyle styles[kDeviceCount]; // 默认构造即出厂外观
                    FillAdvancedFields(hwnd, RefreshConfig{}, LayoutConfig{}, styles, TextConfig{});
                    UpdateDetailHeader(hwnd);
                    SyncDetailVisible(hwnd);
                    RebuildDevList(hwnd, -1);
                }
                return TRUE;
            }
            switch (id)
            {
            case IDC_APPLY:
                // 立即保存并刷新显示，对话框保持打开（方便预览效果）
                EndRename(hwnd, true);
                if (self && ReadAdvancedFields(hwnd))
                {
                    self->DlgApply();
                    self->SetOptionChanged(true);
                    UpdateDetailHeader(hwnd); // 设备名可能已修改：同步标题
                    SyncDetailVisible(hwnd);
                    InvalidateRect(GetDlgItem(hwnd, IDC_DEVLIST), nullptr, TRUE);
                }
                return TRUE;
            case IDC_OK:
                EndRename(hwnd, true);
                if (self)
                {
                    if (!ReadAdvancedFields(hwnd))
                        return TRUE; // 输入非法：保持对话框打开
                    self->DlgApply(); // 写回配置、保存并立即刷新显示
                    self->SetOptionChanged(true);
                }
                {
                    HWND p = GetParent(hwnd);
                    if (p)
                    {
                        EnableWindow(p, TRUE);
                        SetActiveWindow(p);
                        SetForegroundWindow(p);
                    }
                }
                DestroyWindow(hwnd);
                return TRUE;
            case IDC_CANCEL:
                {
                    HWND p = GetParent(hwnd);
                    if (p)
                    {
                        EnableWindow(p, TRUE);
                        SetActiveWindow(p);
                        SetForegroundWindow(p);
                    }
                }
                DestroyWindow(hwnd);
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        {
            HWND p = GetParent(hwnd);
            if (p)
            {
                EnableWindow(p, TRUE);
                SetActiveWindow(p);
                SetForegroundWindow(p);
            }
        }
        DestroyWindow(hwnd);
        return TRUE;
    }
    return FALSE;
}

} // namespace

ITMPlugin::OptionReturn Plugin::ShowOptionsDialog(void* hParent)
{
    HWND parent = (HWND)hParent;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 用内存中的空对话框模板建窗（无控件、无图标）：模板对话框不会回退到系统默认图标，
    // 与其他插件的选项页一致；控件仍按像素坐标手工创建，最终尺寸由布局计算后设定。
    struct
    {
        DLGTEMPLATE tpl{};
        WORD menu = 0;
        WORD cls = 0;
        wchar_t title[16]{};
    } tmpl;
    tmpl.tpl.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    tmpl.tpl.cx = 420;
    tmpl.tpl.cy = 600;
    lstrcpynW(tmpl.title, L"显示设置", 16);

    HWND hDlg = CreateDialogIndirectParamW(hInst, &tmpl.tpl, parent, OptionDlgProc, (LPARAM)this);
    if (!hDlg)
        return OR_OPTION_UNCHANGED;

    // 按窗口所在显示器的 DPI 计算布局：字体放大时控件同步放大，文字不会被裁切
    {
        UINT dpi = 0;
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
        {
            typedef UINT(WINAPI * PFN_GDFW)(HWND);
            if (auto p = (PFN_GDFW)GetProcAddress(u32, "GetDpiForWindow"))
                dpi = p(hDlg);
        }
        if (!dpi)
        {
            HDC dc = GetDC(nullptr);
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
        }
        g_dpi = dpi ? (int)dpi : 96;
    }

    // Win11 圆角窗口（DWMWA_WINDOW_CORNER_PREFERENCE=33 / DWMWCP_ROUND=2，旧系统失败自动忽略）
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
    {
        typedef HRESULT(__stdcall* PFN_DSA)(HWND, DWORD, const void*, DWORD);
        if (auto pDSA = (PFN_DSA)GetProcAddress(dwm, "DwmSetWindowAttribute"))
        {
            int pref = 2; // DWMWCP_ROUND
            pDSA(hDlg, 33, &pref, sizeof(pref));
        }
    }

    // 工作副本：对话框期间所有勾选/排序只改副本，确定才写回
    DlgBegin();

    // 系统 UI 字体（Segoe UI / 微软雅黑），按当前显示器 DPI 取，对话框与所有控件统一设置
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    bool got_ncm = false;
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
    {
        typedef BOOL(WINAPI * PFN_SPIFD)(UINT, UINT, void*, UINT, UINT);
        if (auto p = (PFN_SPIFD)GetProcAddress(u32, "SystemParametersInfoForDpi"))
            got_ncm = p(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi) != FALSE;
    }
    if (!got_ncm)
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(hDlg, WM_SETFONT, (WPARAM)hFont, TRUE);

    // 设备列表字体比系统字体大两档，行文字更醒目
    LOGFONTW lfList = ncm.lfMessageFont;
    lfList.lfHeight = -S(16);
    g_list_font = CreateFontIndirectW(&lfList);

    // 详情卡标题（设备名）：半粗体、比正文大一号；就地改名时输入框沿用同一字号
    LOGFONTW lfName = ncm.lfMessageFont;
    lfName.lfHeight = -S(18);
    lfName.lfWeight = FW_SEMIBOLD;
    g_name_font = CreateFontIndirectW(&lfName);

    // 控件位置依赖字体度量，字体建好后再算
    ComputeLayout(hDlg, hFont);
    const DlgLayout& L = g_lay;

    // 由客户区尺寸反推整窗尺寸（标题栏/边框由系统计算，不手工留余量）
    RECT wnd{ 0, 0, L.w, L.h };
    AdjustWindowRectEx(&wnd, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, FALSE, 0);
    const int W = wnd.right - wnd.left, H = wnd.bottom - wnd.top;

    auto mk = [&](UINT id, const wchar_t* text, int x, int y, int w, int h, DWORD style) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text,
                                    WS_CHILD | WS_VISIBLE | style,
                                    x, y, w, h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
        if (ctrl)
        {
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
            // 自绘按钮的悬停状态需要子类化跟踪
            WNDPROC prev = (WNDPROC)SetWindowLongPtrW(ctrl, GWLP_WNDPROC, (LONG_PTR)BtnProc);
            if (!g_origBtnProc)
                g_origBtnProc = prev;
        }
        return ctrl;
    };

    // 参数悬停提示：鼠标停在参数名或输入框上显示该参数的含义
    HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                0, 0, 0, 0, hDlg, nullptr, hInst, nullptr);
    if (hTip)
    {
        SendMessageW(hTip, WM_SETFONT, (WPARAM)hFont, FALSE);
        SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, S(380)); // 超宽自动换行
        SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
        SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_RESHOW, 80);
    }
    auto addTip = [&](HWND ctl, const wchar_t* text) {
        if (!hTip || !ctl || !text)
            return;
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE; // 宿主无 v6 清单时是 comctl32 v5，只认 v1 大小
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hDlg;
        ti.uId = (UINT_PTR)ctl;
        ti.lpszText = const_cast<LPWSTR>(text);
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    };

    // 分组框（系统绘制边框，标签/底色由 WM_CTLCOLORSTATIC 统一）
    auto mkGroup = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                    x, y, w, h, hDlg, nullptr, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
    };
    // SS_NOTIFY：静态文本默认不接收鼠标消息，接受后提示才能响应悬停。
    // 标签带 ID 是为了页签切换时能 ShowWindow（同屏只有一份，不按设备重复）。
    auto mkStatic = [&](UINT id, const wchar_t* text, int x, int y, int w) {
        HWND ctrl = CreateWindowExW(0, L"STATIC", text,
                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
                                    x, y, w, L.field_h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };
    auto mkEdit = [&](UINT id, int x, int y, int w) {
        HWND ctrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    x, y, w, L.field_h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };
    auto mkCheck = [&](UINT id, const wchar_t* text, int x, int y, int w) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    x, y, w, L.field_h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };

    const int cx = L.right_x + L.pad;      // 详情卡内容区左边界
    const int cw = L.right_w - L.pad * 2;  // 详情卡内容区宽度

    // ---------- 左卡片：设备 ----------
    mkStatic(0, L"设备", L.left_x + L.pad, L.left_cap_y, L.left_content);
    {
        HWND hList = CreateWindowExW(0, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY |
                                         LBS_NOINTEGRALHEIGHT,
                                     L.list_x, L.list_y, L.list_w, L.list_h,
                                     hDlg, (HMENU)(UINT_PTR)IDC_DEVLIST, hInst, nullptr);
        if (hList)
        {
            SendMessageW(hList, WM_SETFONT, (WPARAM)g_list_font, TRUE);
            SendMessageW(hList, LB_SETITEMHEIGHT, 0, L.row_h);
            g_origListProc = (WNDPROC)SetWindowLongPtrW(hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
        }

        // 两个并排的排序按钮 + 整行的改名按钮
        mk(IDC_UP, L"上移", L.list_x, L.updown_y, L.half_w, L.btn_h, BS_OWNERDRAW | WS_TABSTOP);
        mk(IDC_DOWN, L"下移", L.list_x + L.half_w + S(8), L.updown_y, L.half_w, L.btn_h,
           BS_OWNERDRAW | WS_TABSTOP);

        // 列表下方的提示：全部未勾选 / 改名进行中，同一位置互斥显示
        HWND warn = CreateWindowExW(0, L"STATIC", kWarnAllUnchecked, WS_CHILD | SS_LEFT,
                                    L.hint_x, L.hint_y, L.hint_w, S(38), hDlg,
                                    (HMENU)(UINT_PTR)IDC_DEVWARN, hInst, nullptr);
        if (warn)
            SendMessageW(warn, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hint = CreateWindowExW(0, L"STATIC", kRenameHint, WS_CHILD | SS_LEFT,
                                    L.hint_x, L.hint_y, L.hint_w, S(38), hDlg,
                                    (HMENU)(UINT_PTR)IDC_RENAMEHINT, hInst, nullptr);
        if (hint)
            SendMessageW(hint, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    // ---------- 右卡片：标题 + 页签 ----------
    {
        HWND nm = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                  cx, L.name_y, cw - L.name_btn_w - S(10), S(26), hDlg,
                                  (HMENU)(UINT_PTR)IDC_DETAIL_NAME, hInst, nullptr);
        if (nm)
            SendMessageW(nm, WM_SETFONT, (WPARAM)g_name_font, TRUE);

        // 改名就改这个标题，入口放在标题右侧；双击左侧列表行同样可以进入
        if (HWND rb = mk(IDC_RENAME, L"重命名", L.name_btn_x, L.name_btn_y, L.name_btn_w, L.field_h,
                         BS_OWNERDRAW | WS_TABSTOP))
            addTip(rb, L"改的是任务栏上显示的型号名，双击左侧列表项也可以改名。");

        mk(IDC_TAB_APP, L"外观", cx, L.tab_y, L.tab_w, L.tab_h, BS_OWNERDRAW | WS_TABSTOP);
        mk(IDC_TAB_REF, L"刷新策略", cx + L.tab_w, L.tab_y, L.tab_w, L.tab_h,
           BS_OWNERDRAW | WS_TABSTOP);
    }

    // ---------- 外观页签：数值 2×2（标签一份，输入框按设备各一份叠放） ----------
    for (int i = 0; i < kNumFieldsCount; ++i)
    {
        const int col = i % 2, row = i / 2;
        const int lx = cx + (col == 0 ? 0 : L.num_lbl_w[0] + S(6) + L.num_edit_w + L.num_col_gap);
        const int y = L.body_y + row * L.field_step;
        HWND lbl = mkStatic(IDC_LBL_NUM0 + i, kNumFields[i].label, lx, y, L.num_lbl_w[col]);
        addTip(lbl, kNumFields[i].tip);
        for (int dev = 0; dev < kDeviceCount; ++dev)
            addTip(mkEdit(IDC_NUM0 + dev * kNumFieldsCount + i,
                          lx + L.num_lbl_w[col] + S(6), y, L.num_edit_w),
                   kNumFields[i].tip);
    }

    // ---------- 外观页签：电量配色（列 = 高/中/低，行 = 深/浅任务栏） ----------
    mkStatic(IDC_LBL_CAP_COLOR, kColorCaption, cx, L.cap_color_y, cw);
    for (int k = 0; k < kColorRowsCount; ++k)
    {
        HWND h = CreateWindowExW(0, L"STATIC", kColorRows[k],
                                 WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                 cx + L.chip_off[k], L.color_head_y, L.chip_w, L.field_h,
                                 hDlg, (HMENU)(UINT_PTR)(IDC_LBL_COLORROW0 + k), hInst, nullptr);
        if (h)
            SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    for (int r = 0; r < 2; ++r)
    {
        const int y = L.color_row_y + r * L.field_step;
        mkStatic(IDC_LBL_COLORHEAD0 + r, kColorHeads[r], cx, y, L.color_lbl_w);
        for (int k = 0; k < kColorRowsCount; ++k)
        {
            const int slot = k * 2 + r;
            for (int dev = 0; dev < kDeviceCount; ++dev)
                addTip(mk(IDC_COLOR0 + dev * kColorCount + slot, L"", cx + L.chip_off[k], y,
                          L.chip_w, L.field_h, BS_OWNERDRAW | WS_TABSTOP),
                       L"点击选择颜色");
        }
    }

    // ---------- 外观页签底部：显示百分号 + 无数据文案 ----------
    for (int dev = 0; dev < kDeviceCount; ++dev)
        addTip(mkCheck(IDC_SHOWPCT0 + dev, kShowPctLabel, cx, L.pct_y, L.pct_w),
               L"任务栏双行显示时生效，默认开启。只影响这一台设备。");
    {
        const int nx = cx + L.pct_w + S(18);
        mkStatic(IDC_LBL_NOREAD, kNoReadLabel, nx, L.pct_y, L.noread_lbl_w);
        for (int dev = 0; dev < kDeviceCount; ++dev)
        {
            HWND ne = mkEdit(IDC_NOREAD0 + dev, nx + L.noread_lbl_w + S(6), L.pct_y,
                             L.noread_edit_w);
            if (ne)
                SendMessageW(ne, EM_SETLIMITTEXT, (WPARAM)kNoReadMax, 0);
        }
    }

    // ---------- 刷新策略页签：该设备的各项间隔（2 列） ----------
    for (int i = 0; i < 15; ++i)
    {
        const int dev = kRefFields[i].dev;
        int idx = 0;
        for (int j = 0; j < i; ++j)
            if (kRefFields[j].dev == dev)
                ++idx;
        const int col = idx % 2, row = idx / 2;
        const int lx = cx + col * (L.ref_lbl_w + S(6) + L.ref_edit_w + L.ref_col_gap);
        const int y = L.ref_y0 + row * L.field_step;
        HWND lbl = mkStatic(IDC_LBL_REF0 + i, kRefFields[i].label, lx, y, L.ref_lbl_w);
        addTip(lbl, kRefFields[i].tip);
        addTip(mkEdit(IDC_REF0 + i, lx + L.ref_lbl_w + S(6), y, L.ref_edit_w),
               kRefFields[i].tip);
    }
    // ---------- 底部：全局布局（所有设备共用） ----------
    {
        const int y = L.global_y + (L.global_h - L.field_h) / 2;
        mkStatic(IDC_LBL_GLOBAL, L"全局布局", L.g_cap_x, y, L.g_cap_w);
        for (int i = 0; i < kLayoutFieldsCount; ++i)
        {
            HWND lbl = mkStatic(0, kLayoutFields[i].label, L.g_lbl_x[i], y, L.g_lbl_w);
            HWND ed = mkEdit(IDC_LAY0 + i, L.g_ed_x[i], y, L.g_ed_w);
            addTip(lbl, kLayoutFields[i].tip);
            addTip(ed, kLayoutFields[i].tip);
        }
        HWND sep = mkCheck(IDC_SHOW_SEP, kShowSepLabel, L.g_chk_x, y,
                           L.w - L.g_chk_x - L.margin - L.pad);
        addTip(sep, L"关闭后设备之间只保留间距，不画竖线。所有设备共用。");
    }

    FillAdvancedFields(hDlg);

    // 默认选中列表第一台设备，详情面板跟随
    g_cur_dev = DlgOrderAt(0);
    g_cur_tab = 0;
    UpdateDetailHeader(hDlg);
    SyncDetailVisible(hDlg);
    RebuildDevList(hDlg, g_cur_dev);

    // 底部：左侧恢复默认（连同顺序、勾选一起还原），右侧确定 / 应用 / 取消
    const int btnGap = S(8);
    const int footX = L.w - L.margin - L.btn_action_w;
    mk(IDC_RESET, L"恢复默认", L.margin, L.bottom_y, L.btn_w, L.btn_h, BS_OWNERDRAW);
    mk(IDC_OK, L"确定", footX - (L.btn_action_w + btnGap) * 2, L.bottom_y, L.btn_action_w, L.btn_h,
       BS_OWNERDRAW | BS_DEFPUSHBUTTON);
    mk(IDC_APPLY, L"应用", footX - (L.btn_action_w + btnGap), L.bottom_y, L.btn_action_w, L.btn_h,
       BS_OWNERDRAW);
    mk(IDC_CANCEL, L"取消", footX, L.bottom_y, L.btn_action_w, L.btn_h, BS_OWNERDRAW);

    g_hover_btn = nullptr; // 悬停状态复位（自绘悬停态）

    m_option_changed = false;
    if (parent)
        EnableWindow(parent, FALSE);

    // 屏幕工作区居中（不依赖父窗口位置，避免对话框被摆到屏幕外）
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - H) / 2;
    // 模板尺寸以对话框单位计，这里按像素设定精确窗口尺寸后显示
    SetWindowPos(hDlg, nullptr, x, y, W, H, SWP_NOZORDER | SWP_SHOWWINDOW);
    UpdateWindow(hDlg);

    // 模态消息循环：取本线程所有消息（子控件消息也在此队列），
    // 子控件导航交给 IsDialogMessage（Tab 切换、回车确认、Esc 取消），对话框销毁即退出。
    MSG msg;
    int gm = -1;
    while ((gm = (int)GetMessageW(&msg, nullptr, 0, 0)) > 0)
    {
        if (!IsDialogMessageW(hDlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hDlg))
            break;
    }
    // gm == 0：收到 WM_QUIT（主程序退出中）；gm == -1：GetMessage 出错。
    // 这两条路径循环提前结束，对话框可能尚未销毁，兜底 Destroy 防句柄泄漏。
    if (IsWindow(hDlg))
        DestroyWindow(hDlg);

    // 取消 / 叉掉：丢弃工作副本，不写回配置（任务栏不受影响）

    if (parent)
    {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent); // 关闭后把前台还给父窗口，避免焦点跳到其他应用
    }
    if (gm == 0)
        PostQuitMessage((int)msg.wParam); // 透传收到的 WM_QUIT，主程序的退出流程得以继续

    DeleteObject(hFont);
    if (g_name_font)
    {
        DeleteObject(g_name_font);
        g_name_font = nullptr;
    }
    if (g_list_font)
    {
        DeleteObject(g_list_font);
        g_list_font = nullptr;
    }
    return m_option_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

void* Plugin::GetPluginIcon()
{
    // 任务栏插件管理列表按小图标尺寸显示；按当前 DPI 取对应尺寸避免缩放发虚
    static HICON icon = (HICON)LoadImageW(g_hDllModule, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON),
                                         LR_DEFAULTCOLOR);
    return icon;
}

ITMPlugin* TMPluginGetInstance()
{
    return &Plugin::Instance();
}
