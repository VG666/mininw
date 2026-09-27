#pragma once
// nw 宿主：把"浏览器内核"（miniblink）与"页面能看见的 nw.* API"接起来的那一层。
//
// 这一层就是题目里说的"浏览器内核跟 node 的桥梁"——nw.js 里与之对应的东西散落在
// src/browser/*.cc（NativeWindow、WindowBindings、MenuBindings、TrayBindings...）
// 和 src/api/*/ 的 native handler 里；本文件把它们收敛成一条通道：
//
//   页面 JS  ──mbQuery(id, cmd)──▶ Host::OnQuery ──▶ 各 Api 实现
//   页面 JS  ◀──mbResponseQuery──  Host         ◀── 返回值(JSON)
//
// 之所以走 query 而不是 wke.jsBindFunction 那套同步绑定：mb132 已经没有同步绑定了
// （见 native_media_bridge.cpp 里 "页面 → native 的唯一通道" 那条注释）。
//
// 本层**不做任何渲染/排版/网络**——那些全部由内核负责；它只做：
//   1. 窗口生命周期（Win32 框架窗口 + 内核子窗口）
//   2. nw.* API 的 native 实现（App / Window / Menu / Tray / Clipboard / Shell / Screen / Shortcut）
//   3. node 桥的 syscall 后端（fs / process / path 用的文件与环境原语）
//   4. 注入 nw_api.js / nw_node.js（用内核的脚本上下文回调）

// 这几个版本宏必须在任何 Windows 头之前定下来：mingw-w64 的 shellapi.h 里
// NOTIFYICONDATAW 的成员随 NTDDI_VERSION 变化，而 WIN32_LEAN_AND_MEAN 又把
// shellapi.h 从 windows.h 里摘掉了，所以要自己显式带上。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include <windows.h>
#include <shellapi.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nw_bridge.h"
#include "nw_kernel.h"
#include "nw_package.h"

namespace nmb {
namespace nw {

struct HostOptions {
    std::wstring appPath;                 // 空 = 按 nw.js 顺序自动找
    std::vector<std::wstring> args;       // 命令行（不含 exe 名）
    std::wstring kernelPath;              // 空 = LoadKernel 自己找
    std::wstring bridgePath;              // 桥模式：NativeMediaBridge.dll 路径（空 = exe 旁找）
    bool disableNode = false;             // 关掉 node 桥（等价于 manifest 的 nodejs:false）
    bool splashMode = false;              // 没给应用：起内置启动页，不读 package.json
    bool noBridge = false;                // --nw-no-bridge：跳过桥模式，直接用旧内核路径
};

// 一个 nw 窗口 = 一个 Win32 框架窗口 + 一个内核视图 + 一份 nw.Window 状态。
struct NwWindow {
    int id{};
    HWND hwnd{};
    WebView view{};
    bool ownsView{true};
    // 桥模式：NMB_HANDLE。视图与桥的离屏合成子窗口都归桥所有（NMB_DestroyBrowser
    // 统一销毁），宿主绝不能对 window->view 调 mbDestroyWebView。
    void* browser{};
    bool bridgeMode{};
    bool frameless{};
    bool showMenuBar{true};
    bool fullscreen{};
    bool alwaysOnTop{};
    bool onTopSetByUser{};
    bool resizable{true};
    bool maximized{};
    bool minimized{};
    bool closing{false};
    bool nodeEnabled{true};
    bool kiosk{};                         // kiosk = fullscreen + 置顶（nw 的 enterKioskMode 语义）
    // 原生拖动（win.startDrag）。本实现的无边框窗口是 WS_POPUP 且没有 caption，
    // 系统不给这类窗口走标题栏拖动（SC_MOVE 不生效），所以由宿主自己 SetCapture +
    // SetWindowPos 跟随鼠标。
    // 这里记的是按下瞬间的"光标屏幕坐标 + 窗口矩形"，之后每帧都用两者的差值做绝对定位；
    // 累加每帧位移会把取整误差攒成可见漂移。
    bool dragging{};
    POINT dragOrigin{};
    RECT dragStartRect{};
    HMENU menu{};
    RECT savedRect{};                     // 进全屏/最大化前的窗口矩形
    LONG savedStyle{};
    // setMinimumSize/setMaximumSize 的镜像（0 = 不限制）。WM_GETMINMAXINFO 按它钳制
    // 用户拖拽；win.moveTo/resizeTo 的编程式缩放也要钳（Win32 不会自动管编程式）。
    int minW{0};
    int minH{0};
    int maxW{0};
    int maxH{0};
    std::wstring title;
    // 页面显式 win.setTitle 过：此后 document.title 不再动标题栏，直到导航到新文档。
    // nw.js 里这叫 title_override（v8 的 setTitleInternal 就是设它）。
    bool titleOverride{};
    // 最近一次 win.setZoom 的值。**这是镜像不是读回**：内核只导出了 setZoomFactor，
    // 没有 getZoomFactor，所以除了自己记没有第二个来源（nw.state 报的就是它）。
    double zoom{1.0};
    std::wstring url;
    std::string apiScript;                // 注入脚本原文（每窗口一份，因为要带窗口号）
    std::map<int, std::string> hotkeys;   // 热键 id -> JS 回调名
    NwWindow* opener{};
};

// 全局宿主。单例，因为 Win32 的窗口类/托盘/剪贴板本来就是进程级资源。
class Host {
public:
    static Host& Instance();

    bool Start(HINSTANCE instance, const HostOptions& options, std::wstring& error);
    int Run();
    void Quit(int exitCode);
    // 消息循环结束后收尾：停掉 loopback RPC（join accept/worker 线程、WSACleanup）。
    void Shutdown();

    const Manifest& manifest() const { return manifest_; }
    const KernelApi& kernel() const { return kernel_; }
    // 桥模式时非空（BridgeApi::module != nullptr）：浏览器（离屏合成 + ffmpeg 媒体
    // 接管）整体外包给 NativeMediaBridge.dll，四个被桥占用的内核回调槽经宿主钩子转发。
    const BridgeApi& bridge() const { return bridge_; }
    HINSTANCE instance() const { return instance_; }
    const std::wstring& appPath() const { return appPath_; }
    const std::vector<std::wstring>& args() const { return args_; }
    bool nodeEnabled() const { return !disableNode_; }

    // 打开一个窗口；url 为空时用 manifest 的 main。返回窗口号，0 表示失败。
    int OpenWindow(const std::string& urlJson, NwWindow* opener);
    void CloseWindow(int id);
    NwWindow* Find(int id);
    std::vector<NwWindow*> AllWindows();

    // 在某个窗口上执行一段脚本（不带结果）。id=0 表示根窗口。
    void EvalInWindow(int id, const std::string& code);
    // 脚本执行结果（同步取回）。
    std::string EvalInWindowSync(int id, const std::string& code);

    // 页面 → native 的通道入口；request 是 JSON 文本，返回 JSON 文本。
    //
    // 两条通道的线程语义完全不同，所以必须区分：
    //   Async —— 页面调 mbQuery，内核在 UI 线程上回调进来（页面的 JS 也在这条线程上），
    //            可以随便碰 HWND / 窗口表 / 菜单 / 托盘。
    //   Sync  —— 页面发同步 XHR，内核在**网络线程**上调 onLoadUrlBegin，
    //            只许碰纯数据（文件、剪贴板、屏幕、进程信息），碰 UI 会和 UI 线程抢。
    enum class Channel { Async, Sync };
    std::string Dispatch(NwWindow* window, const std::string& request,
                         Channel channel = Channel::Async);

    // 应答信封的两个构造口。两条通道（异步 query / 同步 XHR）和各个 Api* 都要用，
    // 所以放公开区，别窝在 private 里让回调函数干瞪眼。
    static std::string Ok(Json payload);
    static std::string Fail(const std::string& message);

    // 注入脚本（脚本上下文一建立就调用一次；脚本自身幂等）。
    void InstallScripts(NwWindow* window, Frame frame);
    const std::string& RootApiScript() const { return apiScript_; }

    void WakeAll();

private:
    Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool PreparePaths(std::wstring& error);
    bool PrepareSplash(std::wstring& error);   // 无应用时的内置启动页（不依赖 package.json）
    bool LoadScripts(std::wstring& error);
    void BuildRootApiScript();
    void HandleWindowMessage(NwWindow* window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);

    // 桥模式的启动序列（NativeMediaBridge.dll）：加载桥 → 绑定 miniblink 内核（不 init）
    // → NMB_Initialize（桥共享同一模块并 mbInit）→ 注册宿主钩子。任何一步失败都要完整
    // 清理并返回 false，宿主随即落回旧的 mb*_x64.dll 路径，行为与旧版一致。
    bool TryStartBridge(std::wstring& error);

    // ---- nw.* API 的 native 实现（每个返回 JSON 文本）----
    std::string ApiApp(NwWindow* window, const Json& request);
    std::string ApiWindow(NwWindow* window, const Json& request);
    std::string ApiMenu(NwWindow* window, const Json& request);
    std::string ApiTray(NwWindow* window, const Json& request);
    std::string ApiClipboard(NwWindow* window, const Json& request);
    std::string ApiShell(NwWindow* window, const Json& request);
    std::string ApiScreen(NwWindow* window, const Json& request);
    std::string ApiShortcut(NwWindow* window, const Json& request);
    std::string ApiFs(NwWindow* window, const Json& request);       // node 桥的后端
    std::string ApiProcess(NwWindow* window, const Json& request);  // node 桥的后端

    HINSTANCE instance_{};
    KernelApi kernel_;
    BridgeApi bridge_;                    // 桥模式时 module 非空；旧模式全空
    Manifest manifest_;
    HostOptions options_;
    std::wstring appPath_;
    std::wstring dataPath_;               // 用户数据目录（nw.DataPath 的对应物）
    std::wstring exePath_;
    std::vector<std::wstring> args_;
    bool disableNode_{};
    int nextWindowId_{1};
    int exitCode_{0};
    bool quit_{false};
    std::map<int, std::unique_ptr<NwWindow>> windows_;
    std::string apiScript_;               // nw_api.js + nw_node.js 合并后的注入体
    std::string nodeScript_;

    friend LRESULT CALLBACK HostWndProc(HWND, UINT, WPARAM, LPARAM);
    friend void __cdecl HostJsQuery(WebView, void*, JsExec, int64_t, int, const char*);
};

// 托盘/全局热键这一类"没有窗口也要活着"的资源登记处。
struct TrayEntry {
    int id{};
    NwWindow* window{};
    NOTIFYICONDATAW data{};
    HICON icon{};                         // LoadImage 加载的文件图标：销毁托盘时要 DestroyIcon
};
TrayEntry* FindTray(int id);

} // namespace nw
} // namespace nmb
