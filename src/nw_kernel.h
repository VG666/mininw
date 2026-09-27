#pragma once
// 浏览器内核绑定层 —— nw.js 原本这层是 content/ 那一整套 Chromium（BrowserMainParts、
// RenderViewHost、WebContents...），本模块把它们全部换成 miniblink 的 mb* C 接口。
//
// 这里刻意不 include miniblink 的头文件：内核按名字从 mb108_x64.dll / mb132_x64.dll 动态取
// （和 native_media_bridge.cpp 里 MbApi 的做法一致），好处是本项目不必带内核 SDK，
// 也避免和主桥的内核版本绑死。
//
// 只声明"作为一个浏览器宿主必须有的"那部分导出；缺哪个由 LoadKernel 如实报错，
// 不当成可选（可选导出另有说明）。
//
// 字段名一律照抄内核的真实导出名，别凭直觉拼：当前 mb108/mb132 里是
//   mbResize（不是 mbSetSize）、mbGetSize（没有 mbGetWidth/mbGetHeight）、
//   mbGetUrl（不是 mbGetURL）、mbOnDidCreateScriptContext（不是 mbDidCreateScriptContext）、
//   mbOnURLChanged（不是 mbOnURLChange）。
// 名字写错不会编译报错、只会让 LoadKernel 在启动时列"缺少必需导出"，
// 所以要改这里之前先用 tools 扫一遍导出表（f:\ffbuild\kernel_exports.py）。

#include <windows.h>

#include <cstdint>
#include <string>

namespace nmb {
namespace nw {

using WebView = void*;
using Frame = void*;
using JsExec = void*;
using JsValue = int64_t;

// miniblink SDK 的 mbWindowType ABI；数值必须与 mb108/mb132 的 mb.h 完全一致。
enum KernelWindowType {
    kWindowPopup = 0,
    kWindowTransparent = 1,
    kWindowControl = 2,      // 作为子窗口挂进宿主 HWND，本模块用这个
};

using DocumentReadyCallback = void(__cdecl*)(WebView, void*, Frame);
using JsQueryCallback = void(__cdecl*)(WebView, void*, JsExec, int64_t, int, const char*);
using ScriptContextCallback = void(__cdecl*)(WebView, void*, Frame, void*, int, int);
using RunJsCallback = void(__cdecl*)(WebView, void*, JsExec, JsValue);
using UrlChangedCallback = void(__cdecl*)(WebView, void*, const char*, BOOL);
using TitleChangedCallback = void(__cdecl*)(WebView, void*, const char*);
using ConsoleCallback = void(__cdecl*)(WebView, void*, int, const char*);
// 返回值 = "这个请求我接手了，内核别再自己去加载"（wke 的老约定，mb 沿用）。
// 它是本桥做出"页面 → native 同步取数"的支点：同步 XHR 打到自定义 scheme 上，
// 这里当场把应答塞进 job。详见 nw_host.cpp 的 OnLoadUrlBegin。
using LoadUrlBeginCallback = BOOL(__cdecl*)(WebView, void*, const char*, void*);
// window.prompt 的同步回调（mbPromptBoxCallback）：页面 JS 阻塞在 prompt() 上，
// 宿主在回调里作答，返回值（mbStringPtr）就是 prompt() 的返回值，*handled=FALSE
// 表示“我没处理”（内核走默认行为）。它是同步通道的兼容退路（主通道是
// nw_rpc.cpp 的 loopback HTTP），loopback 起不来时才落到这里。
using PromptBoxCallback = void* (__cdecl*)(WebView, void*, const char*, const char*, BOOL*);
using CloseCallback = void(__cdecl*)(WebView, void*, void*);
// 页面请求开新窗口：nw.js 由 WebContentsDelegate::AddNewContents 接住，
// 这里由 mbOnCreateView 接住并转成 nw.Window.open 的语义。
using CreateViewCallback = WebView(__cdecl*)(WebView, void*, int, const char*, const void*, const void*);

// 内核导出的函数表。字段命名保留 mb* 原名，方便和内核头文件对照。
struct KernelApi {
    HMODULE module{};
    std::wstring path;                                    // 实际加载到的内核路径

    void* (__cdecl* createInitSettings)(){};
    void (__cdecl* setInitSettings)(void* settings, const char* name, const char* value){};
    void (__cdecl* init)(void* settings){};
    void (__cdecl* setDebugConfig)(WebView, const char*, const char*){};
    void (__cdecl* setNodeJsEnable)(WebView, int){};
    WebView (__cdecl* createWebView)(){};
    WebView (__cdecl* createWebWindow)(int type, HWND parent, int x, int y, int w, int h){};
    void (__cdecl* destroyWebView)(WebView){};
    void (__cdecl* setHandle)(WebView, HWND){};
    void (__cdecl* setHandleOffset)(WebView, int, int){};
    void (__cdecl* setAutoDrawToHwnd)(WebView, int){};
    void (__cdecl* resize)(WebView, int, int){};          // 导出名 mbResize
    void (__cdecl* showWindow)(WebView, int){};
    void (__cdecl* setFocus)(WebView){};
    void (__cdecl* killFocus)(WebView){};
    void (__cdecl* wake)(WebView){};
    void (__cdecl* setZoomFactor)(WebView, float){};
    void (__cdecl* setUserAgent)(WebView, const char*){};
    void (__cdecl* setNavigationToNewWindowEnable)(WebView, int){};
    void (__cdecl* setCspCheckEnable)(WebView, int){};
    void (__cdecl* setTransparent)(WebView, int){};
    bool (__cdecl* isMainFrame)(WebView, Frame){};
    Frame (__cdecl* webFrameGetMainFrame)(WebView){};

    void (__cdecl* loadURL)(WebView, const char*){};
    void (__cdecl* loadHtmlWithBaseUrl)(WebView, const char*, const char*){};
    void (__cdecl* reload)(WebView){};
    void (__cdecl* stopLoading)(WebView){};
    void (__cdecl* goBack)(WebView){};
    void (__cdecl* goForward)(WebView){};
    bool (__cdecl* canGoBack)(WebView){};
    bool (__cdecl* canGoForward)(WebView){};
    const char* (__cdecl* getURL)(WebView){};             // 导出名 mbGetUrl
    const char* (__cdecl* getTitle)(WebView){};
    void (__cdecl* getSize)(WebView, int*, int*){};       // 导出名 mbGetSize

    void (__cdecl* runJs)(WebView, Frame, const char*, int, RunJsCallback, void*, void*){};
    // mbJsToString 取回脚本结果：mbRunJs 的结果只能通过回调里的 JsValue 拿，
    // 而 JsValue 本身是内核侧的句柄，必须回到 ExecState 上换成字符串。
    const char* (__cdecl* jsToString)(JsExec, JsValue){};

    void (__cdecl* onJsQuery)(WebView, JsQueryCallback, void*){};
    void (__cdecl* responseQuery)(WebView, int64_t, int, const char*){};

    // ---- 同步通道 ----
    // 内核没有任何"同步绑定 JS 函数"的导出（mb132 里 wkeJsBindFunction 那套已经没了），
    // mbQuery 的应答又是在后续任务里投递的。目前页面同步取数有三条候选，按探测顺序：
    //   1. loopback HTTP（主）：宿主在 127.0.0.1 随机端口起 RpcServer（nw_rpc.cpp），
    //      页面同步 XHR POST 过去，worker 线程里 Dispatch，应答走 HTTP 响应回来；
    //   2. prompt（退路）：onPromptBox —— 页面阻塞在 prompt() 上，回调里 Dispatch 后用
    //      createString 把应答 JSON 带回去；mb108 的 createString 回程有内核级竞态，
    //      所以只作为退路；
    //   3. onLoadUrlBegin 拦截（更老内核的退路）：同步 XHR 发到伪 URL，回调里当场作答。
    void (__cdecl* onLoadUrlBegin)(WebView, LoadUrlBeginCallback, void*){};
    void (__cdecl* netSetData)(void* job, void* buffer, int length){};
    void (__cdecl* netSetMIMEType)(void* job, const char* mime){};
    void (__cdecl* netSetHTTPHeaderFieldUtf8)(void* job, const char* key, const char* value, BOOL response){};
    void (__cdecl* netContinueJob)(void* job){};
    void (__cdecl* netHookRequest)(void* job){};
    // prompt 退路（可选导出：缺了它，同步通道就只剩 loopback HTTP / XHR 拦截）。
    void (__cdecl* onPromptBox)(WebView, PromptBoxCallback, void*){};
    void* (__cdecl* createString)(const char*, size_t){};   // mbCreateString
    void (__cdecl* deleteString)(void*){};                  // mbDeleteString
    const char* (__cdecl* getString)(void*){};              // mbGetString
    void (__cdecl* onDocumentReady)(WebView, DocumentReadyCallback, void*){};
    void (__cdecl* onDidCreateScriptContext)(WebView, ScriptContextCallback, void*){};
    // 下面几个是可选导出：缺了只是少了页面标题/地址变化通知，宿主不会因此起不来。
    void (__cdecl* onURLChanged)(WebView, UrlChangedCallback, void*){};
    void (__cdecl* onTitleChanged)(WebView, TitleChangedCallback, void*){};
    void (__cdecl* onConsole)(WebView, ConsoleCallback, void*){};
    void (__cdecl* onClose)(WebView, CloseCallback, void*){};
    void (__cdecl* onCreateView)(WebView, CreateViewCallback, void*){};
};

// 按名字从 dll 取全部必需导出。dllPath 为空时依次找：
// 环境变量 NMB_MINIBLINK、exe 目录、exe 目录\miniblink、PATH。
bool LoadKernel(const std::wstring& dllPath, KernelApi& out, std::wstring& error);
void UnloadKernel(KernelApi& api);

// 桥模式专用：加载显式路径的内核并绑定全部导出，但**不 mbInit**——
// mbInit 由桥的 NMB_Initialize 做（宿主把同一路径交给桥，两边共享一个模块）。
// 失败时模块已释放、out 已清空；成功后 KernelInitialized() 仍为 false。
bool BindKernelModule(const std::wstring& dllPath, KernelApi& out, std::wstring& error);

// 在指定视图的主框架上同步跑一段脚本，返回结果字符串（空串表示没拿到结果）。
// 内核的 mbRunJs 是异步回调式的，这里用内核的同步执行标志 + 回调取回。
std::string RunJsSync(WebView view, const char* code);

// 内核是否已经初始化过（mbInit 全局只允许一次）。
bool KernelInitialized();

} // namespace nw
} // namespace nmb
