#ifndef PB_VERSION_H
#define PB_VERSION_H
// ============================================================
// Version.h —— 插件版本号的单一来源
// 同时被 C++（Plugin.cpp）与资源脚本（plugin/Plugin.rc）包含：
//   C++：L"" PB_VERSION_STRING 得到宽字符串，用于 GetInfo(TMI_VERSION)
//   RC ：PB_VERSION_MAJOR/MINOR/PATCH 填 FILEVERSION，
//        PB_VERSION_STRING 填 VALUE "FileVersion"
// 改版本只动下面四行；数字与字符串不一致会直接编译失败。
// ============================================================
#define PB_VERSION_MAJOR 0
#define PB_VERSION_MINOR 1
#define PB_VERSION_PATCH 0
#define PB_VERSION_STRING "0.1.0"

#ifdef __cplusplus
// 编译期校验：PB_VERSION_STRING 必须等于三个数字拼出来的字符串
// （C++11 constexpr，单条 return，递归比较字符数组）
#define PB_STR_INNER(x) #x
#define PB_STR(x) PB_STR_INNER(x)

constexpr bool PBVersionCharsMatch(const char* a, const char* b)
{
    return *a == *b && (*a == '\0' || PBVersionCharsMatch(a + 1, b + 1));
}

static_assert(PBVersionCharsMatch(PB_VERSION_STRING,
                                  PB_STR(PB_VERSION_MAJOR) "." PB_STR(PB_VERSION_MINOR) "." PB_STR(PB_VERSION_PATCH)),
              "Version.h: PB_VERSION_STRING 与 MAJOR/MINOR/PATCH 不一致");

#undef PB_STR
#undef PB_STR_INNER
#endif

#endif // PB_VERSION_H
