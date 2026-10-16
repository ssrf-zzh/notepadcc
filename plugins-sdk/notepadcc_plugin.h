#pragma once

/*
 * notepadcc 插件 SDK（设计文档 2.10 第 1 条 / ADR-06）。
 *
 * 设计原则：
 *  - **纯 C 接口**：没有 C++ 名字修饰、没有 STL 跨模块，插件用哪个编译器都能写。
 *  - **版本化函数表**：NpccHostServices 只允许在尾部追加字段，已有字段的类型、顺序、
 *    语义都不得变更；宿主与插件通过 size 与 abiVersion **双向校验**，不匹配就拒绝加载
 *    并给出可读原因（而不是崩在某个野指针上）。
 *  - **跨模块内存一律用宿主的 Alloc/Free**：插件里 new 出来的东西不能在宿主里 delete
 *    （两边的 CRT 不是同一个）。
 *  - 所有回调都在 **UI 线程**上被调用：插件可以放心碰界面状态，但也别在里面做长活。
 */

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPCC_PLUGIN_ABI_VERSION 1u

/* 32 位 DLL 会被宿主拒绝（ADR-06 限定 x64） */
#if defined(_WIN64) || defined(__x86_64__)
#define NPCC_PLUGIN_ARCH_OK 1
#else
#define NPCC_PLUGIN_ARCH_OK 0
#endif

/* ------------------------------------------------------------------ 命令 */

/* 插件注册的命令。name 是界面显示文本，key 是稳定标识（宿主的快捷键、宏都按 key 找它）。 */
typedef struct NpccCommandDesc {
    const wchar_t* key;          /* 例如 "hello.insert"，全局唯一（宿主会加上插件名前缀） */
    const wchar_t* name;         /* 菜单里显示的名字 */
    const wchar_t* category;     /* 菜单归类（现在只有 "插件"）*/
    void (*execute)(void* pluginCookie);
    void* pluginCookie;
} NpccCommandDesc;

/* ------------------------------------------------------------------ 宿主服务 */

typedef struct NpccHostServices {
    uint32_t size;               /* 本结构体的字节数：两边都拿它做校验 */
    uint32_t abiVersion;         /* 必须是 NPCC_PLUGIN_ABI_VERSION */
    void*    hostCookie;         /* 传给下面每个函数的不透明句柄 */

    /* --- 以下为函数指针表，只允许在尾部追加 --- */

    /* 注册一条命令。插件被禁用时宿主不会调用 Init，所以这里不必自己判断。 */
    void     (*RegisterCommand)(void* hostCookie, const NpccCommandDesc* desc);

    /* 跨模块内存：插件申请、宿主释放（或反过来）都要走这两个 */
    void*    (*Alloc)(void* hostCookie, size_t bytes);
    void     (*Free)(void* hostCookie, void* pointer);

    /* 配置目录（插件的配置文件应该放这儿）：返回写进缓冲区的字符数，buf 为空时返回所需长度 */
    size_t   (*GetConfigPath)(void* hostCookie, wchar_t* buf, size_t cch);

    /* 当前文档的文本（UTF-8）。返回写进缓冲区的字节数；buf 为空时返回所需长度。
       没有打开的文档时返回 0。缓冲区要自己准备（先用 buf=nullptr 问长度）。 */
    size_t   (*GetEditorText)(void* hostCookie, char* buf, size_t bytes);
    /* 把光标处的选区替换成给定文本（UTF-8）。返回是否成功。 */
    int      (*SetEditorText)(void* hostCookie, const char* utf8, size_t bytes);

    /* 弹一个信息框（UI 线程上调用；插件不该自己调 MessageBox——那样宿主管不了它的模态） */
    void     (*ShowMessage)(void* hostCookie, const wchar_t* text);

    /* 写进宿主的日志（级别：0 错误 / 1 警告 / 2 信息） */
    void     (*Log)(void* hostCookie, int level, const wchar_t* text);

    /* 原样发一条 Scintilla 消息。**本版返回 -1 并记一条日志**：只有 edit/EditView 允许
       碰 Scintilla 消息（设计文档 4.1 的分层约定），要开这个口子得先把"哪些消息安全"
       定清楚。字段留着是为了 ABI 向前兼容——将来实现它不必改结构体布局。 */
    intptr_t (*ScintillaSend)(void* hostCookie, intptr_t viewId, uint32_t msg, uintptr_t w,
                              intptr_t l);
} NpccHostServices;

/* ------------------------------------------------------------------ 插件信息 */

typedef struct NpccPluginInfo {
    uint32_t       abiVersion;    /* 必须等于 NPCC_PLUGIN_ABI_VERSION */
    const wchar_t* name;          /* 稳定标识（也是命令 key 的前缀、配置目录名） */
    const wchar_t* version;       /* 插件自己的版本号，给人看 */
    const wchar_t* author;
    const wchar_t* description;
} NpccPluginInfo;

/* ------------------------------------------------------------------ 导出符号 */

/* 三个函数是宿主唯一会找的符号；缺一个就拒载并说明缺哪个。 */
typedef const NpccPluginInfo* (*NpccPluginGetInfoFn)(void);
typedef int (*NpccPluginInitFn)(const NpccHostServices* host);
typedef void (*NpccPluginShutdownFn)(void);

#define NPCC_PLUGIN_GET_INFO_NAME "NpccPlugin_GetInfo"
#define NPCC_PLUGIN_INIT_NAME "NpccPlugin_Init"
#define NPCC_PLUGIN_SHUTDOWN_NAME "NpccPlugin_Shutdown"

#ifdef __cplusplus
}  /* extern "C" */
#endif
