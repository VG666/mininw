#pragma once
// 桥模式绑定层 —— 浏览器整体外包给 NativeMediaBridge.dll（miniblink 内核 + ffmpeg
// 媒体接管 + 离屏合成）时的最小胶水。
//
// 旧模式（无桥）里宿主自己 LoadKernel(mb*_x64.dll) 并挂满内核回调槽；桥模式下四个
// per-view 单槽被桥占用（onDocumentReady / onDidCreateScriptContext / onJsQuery /
// onPaintUpdated），宿主的注入时机与异步 RPC 由桥经 NMB_SetHostHooks 转发回来，
// 见 nw_host.cpp 匿名命名空间里的 Bridge*Hook 三个适配器。
//
// 刻意不 include 桥的头文件（NativeMediaBridge\api\native_media_bridge.h）：
// 宿主不依赖桥的源码树。下面 BridgeHostHooks 是那份头里 NMB_HostHooks 的逐字段
// 镜像，BridgeApi 是导出表的手写镜像——改那边要同步这里。
//
// 内核共享的关键点：宿主先把 miniblink_x64.dll 自己 LoadLibrary 进来并绑定
// KernelApi（页面 loadURL/runJs 等照旧直调），再把**同一个绝对路径**交给
// NMB_Initialize —— 桥命中同一模块，mbInit 全进程只发生一次（由桥做，
// 宿主的 BindKernelModule 只绑定不 init）。

#include <windows.h>

#include <string>

namespace nmb {
namespace nw {

// 与桥的 NMB_HostHooks 逐字段对齐（ABI 镜像，见文件头说明）。
struct BridgeHostHooks {
    void* param;
    void (__cdecl* onDocumentReady)(void* view, void* param, void* frame);
    void (__cdecl* onScriptContext)(void* view, void* param, void* frame, void* context,
                                    int extensionGroup, int worldId);
    // 返回 1 = 宿主已应答（mbResponseQuery），桥不再回包。
    int (__cdecl* onJsQuery)(void* view, void* param, void* execState, long long queryId,
                             int customMsg, const char* request);
};

// 桥导出的最小函数表（GetProcAddress 按名取，名字与桥的 def 完全一致）。
struct BridgeApi {
    HMODULE module{};   // NativeMediaBridge.dll 的模块句柄

    bool (__cdecl* initialize)(const wchar_t* kernelPath, const wchar_t* dataPath){};
    void* (__cdecl* createBrowser)(HWND parent, int x, int y, int width, int height){};
    void* (__cdecl* getWebView)(void* browser){};
    void (__cdecl* setDebugConfig)(void* browser, const char* name, const char* value){};
    HWND (__cdecl* getWindow)(void* browser){};
    void (__cdecl* resize)(void* browser, int width, int height){};
    void (__cdecl* show)(void* browser, int visible){};
    void (__cdecl* destroyBrowser)(void* browser){};
    void (__cdecl* shutdown)(){};
    const wchar_t* (__cdecl* lastError)(){};
    void (__cdecl* setHostHooks)(const BridgeHostHooks* hooks){};
    // 媒体下载拦截（可选导出，老桥没有）：内核 onLoadUrlBegin 是 per-view 单槽，
    // 桥模式下宿主把它让给自己的同步 RPC 退路，非 RPC 请求经此链式转发给桥——
    // 缺这个导出时媒体字节流会落回内核（无解码器，媒体元素永远起不来）。
    int (__cdecl* mediaLoadUrlBegin)(void* view, void* param, const char* url, void* job){};
};

// 定位桥 DLL。explicitPath 非空直接用（--nw-bridge=...）；否则依次找
// exe 目录、exe\..\NativeMediaBridge\bin（开发树布局）。找不到返回空串。
std::wstring FindBridgeDll(const std::wstring& explicitPath);

// 定位桥模式内核，miniblink_x64.dll 为第一优先；显式路径始终优先。
// 找不到时再尝试其它真实内核路径。
std::wstring FindMiniblinkDll(const std::wstring& explicitPath);

// 加载桥 DLL 并绑定导出。必需导出缺任何一个都算失败（模块会被释放）。
// 成功后宿主还应注册宿主钩子（BridgeApi::setHostHooks）再开始 CreateBrowser。
bool LoadBridge(const std::wstring& dllPath, BridgeApi& out, std::wstring& error);

void UnloadBridge(BridgeApi& api);

} // namespace nw
} // namespace nmb
