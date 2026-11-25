#pragma once
// ============================================================
// DeviceList.h —— 设备清单（唯一需要手工维护的地方）
// 新增一台设备：
//   1) 实现对应的 Device（协议）+ Reader（线程，继承 BatteryReader）
//   2) 在本文件把 kDeviceCount +1，并补 kDefaultOrder / kDefaultDeviceNames
//   3) Plugin.cpp 的 kDeviceMeta 补一项，Plugin 的 m_readers 补一个成员
// 数组一律不写死长度：数量与 kDeviceCount 不一致会在这里直接编译失败，
// 不会再出现「只改常量、别处还是 4」的静默越界。
// ============================================================

constexpr int kDeviceCount = 5;

// 默认显示顺序（设备索引）：按型号名首字母 A → Z
constexpr int kDefaultOrder[] = { 1, 3, 2, 0, 4 };

// 默认显示名（与设备索引一一对应，玩家可在“显示设置”里改）
constexpr const wchar_t* kDefaultDeviceNames[] = { L"G87", L"A5 Ultra", L"G7 Pro", L"DS5", L"M230" };

static_assert(sizeof(kDefaultOrder) / sizeof(kDefaultOrder[0]) == kDeviceCount,
              "DeviceList.h: kDefaultOrder 的项数与 kDeviceCount 不一致");
static_assert(sizeof(kDefaultDeviceNames) / sizeof(kDefaultDeviceNames[0]) == kDeviceCount,
              "DeviceList.h: kDefaultDeviceNames 的项数与 kDeviceCount 不一致");
