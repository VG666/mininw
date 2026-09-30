#include "nw_host.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>                     // ITaskbarList3：win.setProgressBar
#include <winspool.h>                     // EnumPrintersW：win.getPrinters（windows.h 不自动带）
#include <winver.h>                       // GetFileVersionInfoW：启动页显示内核版本（version.lib）
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "nw_bridge.h"
#include "nw_kernel.h"
#include "nw_rpc.h"
#include "nw_script.h"

namespace nmb {
namespace nw {

namespace {

const wchar_t* kWndClass = L"NwBridgeHostFrame";

// 无边框窗口的边框缩放（实现在下方 BorderChildProc 一带）：把客户区子窗口类化，
// 让边缘带的命中测试交回框架窗口。开窗和 WM_PARENTNOTIFY 都要用，先在这里声明。
void InstallBorderHitTest(HWND frame, int depth);

// 一个内核视图只允许绑一个 param，而页面回调里要能反查窗口号。
std::map<WebView, NwWindow*> g_viewToWindow;
std::mutex g_viewMutex;

std::map<int, TrayEntry> g_trays;
int g_nextTrayId = 1000;

NwWindow* WindowOf(WebView view) {
    std::lock_guard<std::mutex> guard(g_viewMutex);
    const auto found = g_viewToWindow.find(view);
    return found == g_viewToWindow.end() ? nullptr : found->second;
}

void BindView(WebView view, NwWindow* window) {
    std::lock_guard<std::mutex> guard(g_viewMutex);
    if (window) g_viewToWindow[view] = window;
    else g_viewToWindow.erase(view);
}

// stderr 输出统一走这里。宿主自身的诊断一律用纯英文 ASCII（cmd 的 OEM 代码页
// 下也不会花）；只有转发页面 console 时可能带非 ASCII。真控制台用 WriteConsoleW
// 写 UTF-16（与控制台代码页无关，中文不再变乱码）；被重定向成管道/文件时
// WriteConsoleW 会失败，此时直接写 UTF-8 字节（测试脚本按 UTF-8 收）。
void EmitStderr(const std::string& utf8) {
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        const std::wstring wide = Utf8ToWide(utf8);
        DWORD written = 0;
        if (!wide.empty()) WriteConsoleW(handle, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr);
    } else {
        std::fputs(utf8.c_str(), stderr);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// 启动阶段耗时探针（默认关闭，NMB_TRACE_STARTUP=1 打开）。
//
// 「UI 加载慢」这种投诉最容易变成互相猜：宿主说内核慢、内核说页面慢。真相是
// 从双击到用户看见东西，中间串了六七段同步工作（读 manifest、加载桥/内核、读脚本、
// 起 RPC、建窗、注入约 88KB JS、loadURL、ShowWindow），光看代码分不出哪段是主因。
// 打开后每段耗时写到 stderr，和 tests\load_probe.ps1 的像素时间线（建窗→可见→首帧）
// 对齐就能直接定位。放在 EmitStderr 之后：探针本身也要走同一条 stderr 通道。
// ---------------------------------------------------------------------------
bool TraceStartupEnabled() {
    static const bool enabled = [] {
        wchar_t buffer[8]{};
        return GetEnvironmentVariableW(L"NMB_TRACE_STARTUP", buffer, 8) > 0;
    }();
    return enabled;
}

double TraceNowMs() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / frequency;
}

// 进程内相对时间：第一个调用点（Host::Start 入口）就是零点。
double TraceElapsedMs() {
    static const double start = TraceNowMs();
    return TraceNowMs() - start;
}

// 墙上时钟（epoch 毫秒）。宿主的时间线是进程相对时间，页面里的 performance.now()
// 是导航相对时间，两者单独看都对不上；两边各带一个 epoch 才能把「宿主 loadURL」
// 和「页面 scriptContext」钉在同一条轴上。
long long WallNowMs() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    // FILETIME 起点 1601-01-01，减掉到 1970 的偏移才是 Unix epoch。
    return static_cast<long long>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

void TraceMark(const char* label) {
    if (!TraceStartupEnabled()) return;
    char line[224]{};
    snprintf(line, sizeof(line), "[nw trace] %8.1f ms  %-42s wall=%lld\n",
             TraceElapsedMs(), label, WallNowMs());
    EmitStderr(line);
}

// 页面侧时间线（NMB_TRACE_PAGE=1）。宿主把这一小段探针拼在内核注入脚本最前面，
// 于是能拿到「脚本上下文创建 → DOMContentLoaded → load → 第一帧 rAF」四个时刻，
// 和宿主自己的 trace 对齐后就能回答"窗口已经显示了，里面为什么还没东西"——
// 到底是页面自己慢，还是画面没送到屏上。不改应用一行代码，任何 app 都适用。
bool TracePageEnabled() {
    static const bool enabled = [] {
        wchar_t buffer[8]{};
        return GetEnvironmentVariableW(L"NMB_TRACE_PAGE", buffer, 8) > 0;
    }();
    return enabled;
}

const char* kPageTraceBootstrap =
    ";try{(function(){"
    "var t0=Date.now();"
    "function emit(s){try{console.log('[nw pg] +'+(Date.now()-t0)+' ms  '+s);}catch(e){}}"
    // 每一步单独自保：脚本上下文刚建时 document 可能还不存在，整段包在一个 try 里
    // 会静默全军覆没（踩过：桥模式下 [nw pg] 一行都不出）。
    "try{emit('scriptContext ready='+document.readyState+' nw='+(typeof window.nw)+"
    "' require='+(typeof window.require)+' nodes='+(document.querySelectorAll('*').length));}"
    "catch(e){emit('scriptContext (no document yet) nw='+(typeof window.nw)+' require='+(typeof window.require));}"
    // 页面自己的脚本如果先于注入层跑起来，window.require 就是 undefined，
    // 应用会静默退回默认数据——这里包一层把每次 require 的耗时打出来。
    "try{var R=window.require;if(R){window.require=function(n){var t=Date.now();"
    "var m=R.apply(this,arguments);emit('require(\"'+n+'\") took '+(Date.now()-t)+' ms');return m;};}}"
    "catch(e){emit('require wrap failed '+e);}"
    "document.addEventListener('DOMContentLoaded',function(){emit('DOMContentLoaded');});"
    "window.addEventListener('load',function(){emit('load');"
    // 同步 RPC 往返是这套宿主的基础延迟，UI 里每个同步 fs 调用都要付一次，单独量出来。
    "try{var fs=window.require('fs');if(fs){var t=Date.now();for(var i=0;i<5;i++)fs.existsSync('C:/Windows');"
    "emit('fs.existsSync x5 '+(Date.now()-t)+' ms');}}catch(e){emit('fs bench failed '+e);}"
    "});"
    "if(window.requestAnimationFrame){requestAnimationFrame(function(){emit('raf1');"
    "requestAnimationFrame(function(){emit('raf2');});});}"
    "})();}catch(e){try{console.log('[nw pg] boom '+e);}catch(_){}};";

// 拼在 nw API + Node 兼容层之后，避免把“注入前尚未定义”误判成宿主脚本安装失败。
const char* kPageTraceAfterInstall =
    ";try{(function(){var t=Date.now();"
    "console.log('[nw pg] afterInstall nw='+(typeof window.nw)+' require='+(typeof window.require)+"
    "' ready='+document.readyState+' wall='+t);"
    "try{var fs=window.require&&window.require('fs');if(fs){var s=Date.now();"
    "for(var i=0;i<5;i++)fs.existsSync('C:/Windows');"
    "console.log('[nw pg] fs.existsSync x5 '+(Date.now()-s)+' ms');}}"
    "catch(e){console.log('[nw pg] fs bench failed '+e);}})();}catch(e){};";

// ---------------------------------------------------------------------------
// 无应用时的内置启动页（对标官方 nw.exe 无项目启动时的 NW.JS 大封面）
// ---------------------------------------------------------------------------

constexpr const char* kHostVersion = "0.1.0";   // 本宿主版本，启动页右下角展示

// Narrow / BuildSplashHtml 定义在本匿名命名空间靠后位置，这里前置声明。
std::string Narrow(const std::wstring& text);
std::string BuildSplashHtml(const std::string& kernelVersion, const std::string& kernelName);

// 取 dll 文件自身的四元版本号；取不到就返回 unknown。内核没有导出 mbGetVersion
// 之类的 API，文件版本资源是唯一可靠来源。
std::string FileVersionOf(const std::wstring& path) {
    if (path.empty()) return "unknown";
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return "unknown";
    std::string buffer(size, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, &buffer[0])) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(&buffer[0], L"\\", reinterpret_cast<LPVOID*>(&info), &length) || !info) {
        return "unknown";
    }
    auto part = [](DWORD value) {
        return std::to_string((value >> 16) & 0xFFFF) + "." + std::to_string(value & 0xFFFF);
    };
    return part(info->dwFileVersionMS) + "." + part(info->dwFileVersionLS);
}

// mb108_x64.dll 不带文件版本资源时，至少给出内核系列名（mb108 / mb132），
// 比孤零零一个 unknown 有信息量。
std::string KernelVersionLabel(const std::string& fileVersion, const std::string& fileName) {
    if (fileVersion != "unknown") return fileVersion;
    for (size_t i = 0; i + 2 < fileName.size(); ++i) {
        if ((fileName[i] == 'm' || fileName[i] == 'M') &&
            (fileName[i + 1] == 'b' || fileName[i + 1] == 'B')) {
            size_t j = i + 2;
            while (j < fileName.size() && fileName[j] >= '0' && fileName[j] <= '9') ++j;
            if (j > i + 2) return fileName.substr(i, j - i);
        }
    }
    return "unknown";
}

// 启动页落成数据目录下的 splash.html，返回它的 file:// URL。
// 不能用 data: URL —— mb108 和上游 Chromium 一样禁止顶层框架导航到 data:
// （控制台报 "Not allowed to navigate top frame to data URL"）。
bool WriteSplashPage(const KernelApi& kernel, const std::wstring& directory, std::string& urlOut) {
    std::string kernelName = "unknown";
    if (!kernel.path.empty()) {
        kernelName = Narrow(kernel.path.substr(kernel.path.find_last_of(L"\\/") + 1));
    }
    const std::string version = KernelVersionLabel(FileVersionOf(kernel.path), kernelName);
    const std::string html = BuildSplashHtml(version, kernelName);

    const std::wstring filePath = JoinPath(directory, L"splash.html");
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, html.data(), static_cast<DWORD>(html.size()), &written, nullptr) &&
                    written == html.size();
    CloseHandle(file);
    if (!ok) return false;
    urlOut = ToFileUrl(filePath);
    return true;
}

// 启动页 HTML：灰底、居中 NW . JS 大字、左下罗盘、右下版本行。全英文 ASCII，
// 与官方封面一个版式（版本信息如实写本宿主 + 实际内核，不冒充 Chromium 版本）。
std::string BuildSplashHtml(const std::string& kernelVersion, const std::string& kernelName) {
    // 内核系列名从文件名推（mb108_x64.dll -> mb108），不再写死 miniblink：
    // 宿主切到 mb108 后若仍写死会显示成 "miniblink … (mb108_x64.dll)"，自相矛盾。
    std::string family = kernelName;
    {
        size_t p = family.rfind("_x64.dll");
        if (p != std::string::npos) family = family.substr(0, p);
        else { size_t q = family.rfind(".dll"); if (q != std::string::npos) family = family.substr(0, q); }
    }
    const std::string kernelLine = (kernelVersion == family)
        ? (family + " (" + kernelName + ")")
        : (family + " " + kernelVersion + " (" + kernelName + ")");
    return
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<title>nw.js</title>\n"
"<style>\n"
"  html, body { margin: 0; height: 100%; overflow: hidden; }\n"
"  body { background: #525257; font-family: 'Segoe UI', Arial, sans-serif; }\n"
"  .word { position: absolute; top: 38%; left: 0; right: 0; text-align: center;\n"
"          font-weight: 900; font-size: 128px; letter-spacing: 0.10em; color: #2b2b2e; }\n"
"  .compass { position: absolute; left: 34px; bottom: 28px; width: 132px; height: 132px; }\n"
"  .versions { position: absolute; right: 30px; bottom: 26px; text-align: right;\n"
"             color: #e8e8ec; font-size: 22px; line-height: 1.45; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"  <div class=\"word\">NW . JS</div>\n"
"  <svg class=\"compass\" viewBox=\"0 0 100 100\">\n"
"    <polygon points=\"50,4 89,26 89,74 50,96 11,74 11,26\" fill=\"#39435a\" stroke=\"#c3cedf\" stroke-width=\"3\"/>\n"
"    <circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#232a3a\" opacity=\"0.55\"/>\n"
"    <g transform=\"rotate(25 50 50)\">\n"
"      <polygon points=\"50,16 57,50 50,84 43,50\" fill=\"#d7e0ee\"/>\n"
"      <polygon points=\"16,50 50,43 84,50 50,57\" fill=\"#6a7ea0\"/>\n"
"    </g>\n"
"    <circle cx=\"50\" cy=\"50\" r=\"4.5\" fill=\"#eef3fa\"/>\n"
"  </svg>\n"
"  <div class=\"versions\">\n"
"    <div>nw.js-compatible host v" + std::string(kHostVersion) + "</div>\n"
"    <div>" + kernelLine + "</div>\n"
"    <div>commit hash: local-development-build</div>\n"
"  </div>\n"
"</body>\n"
"</html>\n";
}

// 三条回调路径各自跑在哪个线程上：内核文档里没写，代码注释里那句"onLoadUrlBegin 在网络
// 线程"一直只是推断。这里把真值记下来，随 app.info 一并报给页面——它同时是本桥决定
// "UI 命令能不能走同步通道"的唯一依据（只有确认同线程，跨界问题才不存在）。
std::atomic<unsigned long> g_threadMain{0};
std::atomic<unsigned long> g_threadUi{0};
std::atomic<unsigned long> g_threadSync{0};

// 最近一次同步通道回程用的是哪条运输层。这个分类必须显式记，不能只比对线程号：
//   * Loopback —— HTTP worker 线程池，每个连接一个线程，永远不可能"恰好等于 UI 线程"，
//                 即使某次 TID 撞号也不能放行 UI 命令；
//   * Prompt   —— 同步模态，回调确定跑在页面 JS 的 UI 线程上；
//   * XhrHook  —— onLoadUrlBegin，内核网络线程。
// 另外注意：mb108 上普通资源加载也走 onLoadUrlBegin 且恰好发生在 UI 线程，
// 所以那里的线程号**不能**无条件写进 g_threadSync（见 OnLoadUrlBegin）。
enum class SyncTransport { Unknown = 0, Prompt = 1, XhrHook = 2, Loopback = 3 };
std::atomic<int> g_syncTransport{static_cast<int>(SyncTransport::Unknown)};

// 同步通道的回调和 UI 回调是否同线程。只有 prompt 退路满足；loopback/XhrHook
// 以及任一条还没跑过的情况一律按"不同线程"处理：宁可让页面收到一条明确的错误，
// 也不赌一个可能把窗口表踩坏的调用。
bool SyncChannelSharesUiThread() {
    if (g_syncTransport.load(std::memory_order_relaxed) !=
        static_cast<int>(SyncTransport::Prompt)) {
        return false;
    }
    const unsigned long ui = g_threadUi.load(std::memory_order_relaxed);
    const unsigned long sync = g_threadSync.load(std::memory_order_relaxed);
    return ui != 0 && sync != 0 && ui == sync;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
    return out;
}

// 把任意文本做成可以安全塞进 JS 源码的字符串字面量。
std::string QuoteForJs(const std::string& text) {
    Json array = Json::array();
    array.push(Json::fromText(text));
    const std::string dumped = array.dump();
    return dumped.substr(1, dumped.size() - 2);   // 去掉外层 []
}

// CreateProcessW 要的是一整行命令行，所以参数得按 Windows 的规则重新加引号：
// 只在含空白/引号时加引号，且反斜杠的加倍规则与引号的前后位置有关
// （末尾的 n 个反斜杠要变 2n，引号前的 n 个反斜杠要变 2n+1）。
// app.restart 用它把原命令行原样拼回去。
std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out;
    out.push_back(L'"');
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') out.append(backslashes * 2 + 1, L'\\');
        else                out.append(backslashes, L'\\');
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

const char* const kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// node 的 fs 读写要能过二进制：桥这边统一用 base64 承载（JSON 装不下裸字节）。
std::string Base64Encode(const std::string& input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t at = 0;
    while (at + 2 < input.size()) {
        const unsigned int packed = (static_cast<unsigned char>(input[at]) << 16) |
                                    (static_cast<unsigned char>(input[at + 1]) << 8) |
                                     static_cast<unsigned char>(input[at + 2]);
        out.push_back(kBase64Alphabet[(packed >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(packed >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(packed >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[packed & 0x3F]);
        at += 3;
    }
    const size_t left = input.size() - at;
    if (left == 1) {
        const unsigned int packed = static_cast<unsigned char>(input[at]) << 16;
        out.push_back(kBase64Alphabet[(packed >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(packed >> 12) & 0x3F]);
        out += "==";
    } else if (left == 2) {
        const unsigned int packed = (static_cast<unsigned char>(input[at]) << 16) |
                                    (static_cast<unsigned char>(input[at + 1]) << 8);
        out.push_back(kBase64Alphabet[(packed >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(packed >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(packed >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string Base64Decode(const std::string& input) {
    // 先建反查表：'=' 单独处理，其余非法字符一律跳过（页面侧已经做过一次校验，
    // 这里宽容一点，免得一个换行就把整段文件判死）。
    int table[256];
    for (int& slot : table) slot = -1;
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;

    std::string out;
    out.reserve(input.size() / 4 * 3);
    unsigned int packed = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=') break;
        const int value = table[c];
        if (value < 0) continue;
        packed = (packed << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((packed >> bits) & 0xFF));
        }
    }
    return out;
}

// nw.Shortcut 的 key 是 Electron 那套写法（"Ctrl+Shift+A" / "CommandOrControl+K"），
// 这里把它翻成 RegisterHotKey 要的 (modifiers, vk)。识别不了就返回 false，
// 让上层如实回一句"无法识别的按键组合"，而不是默默注册一个假热键。
bool ParseAccelerator(const std::string& spec, UINT& modifiers, UINT& key) {
    modifiers = 0;
    key = 0;
    size_t at = 0;
    while (at < spec.size()) {
        const size_t plus = spec.find('+', at);
        const size_t stop = plus == std::string::npos ? spec.size() : plus;
        std::string part = spec.substr(at, stop - at);
        at = stop + 1;    // 越过末尾时会自动退出循环

        // 去掉段内空白，再统一小写：用户手写的 "Ctrl + Shift + A" 也要认。
        std::string token;
        for (char c : part) {
            if (c == ' ' || c == '\t') continue;
            token.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        }
        if (token.empty()) continue;

        if (token == "ctrl" || token == "control" || token == "commandorcontrol" || token == "cmdorctrl") {
            modifiers |= MOD_CONTROL; continue;
        }
        if (token == "cmd" || token == "command" || token == "meta" || token == "super" || token == "win") {
            modifiers |= MOD_WIN; continue;
        }
        if (token == "alt" || token == "option") { modifiers |= MOD_ALT; continue; }
        if (token == "shift") { modifiers |= MOD_SHIFT; continue; }

        if (token.size() == 1) {
            const char c = token[0];
            if (c >= 'a' && c <= 'z') key = static_cast<UINT>('A' + (c - 'a'));
            else if (c >= '0' && c <= '9') key = static_cast<UINT>(c);
            else if (c == ' ') key = VK_SPACE;
            else return false;
            continue;
        }
        if (token[0] == 'f' && token.size() <= 3) {
            const int index = std::atoi(token.c_str() + 1);
            if (index >= 1 && index <= 24) { key = static_cast<UINT>(VK_F1 + index - 1); continue; }
        }
        if (token == "space") { key = VK_SPACE; continue; }
        if (token == "tab") { key = VK_TAB; continue; }
        if (token == "backspace") { key = VK_BACK; continue; }
        if (token == "delete" || token == "del") { key = VK_DELETE; continue; }
        if (token == "insert" || token == "ins") { key = VK_INSERT; continue; }
        if (token == "enter" || token == "return") { key = VK_RETURN; continue; }
        if (token == "escape" || token == "esc") { key = VK_ESCAPE; continue; }
        if (token == "up") { key = VK_UP; continue; }
        if (token == "down") { key = VK_DOWN; continue; }
        if (token == "left") { key = VK_LEFT; continue; }
        if (token == "right") { key = VK_RIGHT; continue; }
        if (token == "home") { key = VK_HOME; continue; }
        if (token == "end") { key = VK_END; continue; }
        if (token == "pageup") { key = VK_PRIOR; continue; }
        if (token == "pagedown") { key = VK_NEXT; continue; }
        if (token == "plus") { key = VK_OEM_PLUS; continue; }
        if (token == "minus") { key = VK_OEM_MINUS; continue; }
        if (token == "comma" || token == ",") { key = VK_OEM_COMMA; continue; }
        if (token == "period" || token == ".") { key = VK_OEM_PERIOD; continue; }
        if (token == "slash" || token == "/") { key = VK_OEM_2; continue; }
        return false;
    }
    return key != 0;
}

// 页面向 native 发查询：内核通过 mbOnJsQuery 回调进来，native 用 mbResponseQuery 应答。
// 这条通道就是 nw.js 里 node 上下文与浏览器上下文之间那座桥在 miniblink 上的落点
// （nw.js 走的是 renderer 里的 V8 binding，本实现走内核的 query 通道）。
void __cdecl OnJsQuery(WebView view, void* param, JsExec, int64_t queryId, int customMsg, const char* request) {
    g_threadUi.store(GetCurrentThreadId(), std::memory_order_relaxed);
    NwWindow* window = static_cast<NwWindow*>(param);
    if (!window) window = WindowOf(view);
    if (!window) return;
    const std::string reply = Host::Instance().Dispatch(window, request ? request : "");
    if (Host::Instance().kernel().responseQuery) {
        Host::Instance().kernel().responseQuery(view, queryId, customMsg, reply.c_str());
    }
}

// ---------------------------------------------------------------------------
// 同步通道·URL 拦截路径（更老内核的退路）：页面用同步 XHR 打到 nmb-rpc://
// （或退路 http://nmb-rpc），宿主在这里当场作答。主通道是 nw_rpc.cpp 的 loopback
// HTTP；这条路径只在更老内核（onLoadUrlBegin 会回调同步 XHR）上、且按页面探测顺序
// 排在 loopback/prompt 之后。
//
// 为什么非要绕这一圈：内核没有任何"同步绑定 JS 函数"的导出（mb132 里 wkeJsBindFunction
// 那套已经删了），而 mbQuery 的应答是内核在后续任务里调 __onMbQuery__ 投递的（见内核
// 注入脚本），所以页面调完 mbQuery 立刻读结果是读不到的。想让 fs.readFileSync /
// nw.Clipboard.get() 这类接口保持 nw.js 那样的同步语义，只能靠"同步 XHR"类通道。
//
// 注意：这个回调是内核的**网络线程**调的（内核为此专门有 mbNetGetRaw*InBlinkThread
// 这一对导出），所以这里只做数据类命令。凡是碰窗口/菜单/托盘的（要 UI 线程）都走
// 异步 query 通道，别往这儿加。
// ---------------------------------------------------------------------------

const std::string kSyncFileMarker = "/__nmb_rpc__/";
const std::string kSyncScheme = "nmb-rpc:";
const std::string kSyncHttpPrefix = "http://nmb-rpc/";

// 0-9a-fA-F → 0-15，别的回 -1（percent-decode 要靠它判断是否合法）。
int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const int hi = HexDigit(input[i + 1]);
            const int lo = HexDigit(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        // '+' 是表单编码的空格；本通道的载荷是 base64 再 percent-encode，不会出现裸 '+'，
        // 但别人手工拼 URL 时可能会，宽容处理。
        out.push_back(input[i] == '+' ? ' ' : input[i]);
    }
    return out;
}

BOOL __cdecl OnLoadUrlBegin(WebView view, void* param, const char* url, void* job) {
    if (!url || !job) return FALSE;

    // 注意：这里对**每个**资源加载都会回调（普通图片/脚本也算），mb108 上它还恰好
    // 跑在 UI 线程。线程拓扑值只能在确认拦截到 RPC 请求时再记，否则普通页面加载会把
    // g_threadSync 覆盖成 UI 线程号，骗 SyncChannelSharesUiThread 放行 UI 命令。
    Host& host = Host::Instance();
    const KernelApi& kernel = host.kernel();
    if (!kernel.netSetData) return FALSE;

    const std::string full(url);
    // 载荷一律取 "rpc/" 之后那一段：自定义 scheme 后面可能带 //、也可能被内核规范化成单个 /，
    // 所以不数斜杠，直接找路径段本身。
    size_t mark = std::string::npos;
    size_t payloadStart = std::string::npos;
    if (full.compare(0, 5, "file:") == 0) {
        mark = full.find(kSyncFileMarker);
        if (mark != std::string::npos) payloadStart = mark + kSyncFileMarker.size();
    } else if (full.compare(0, kSyncScheme.size(), kSyncScheme) == 0) {
        mark = full.find("rpc/");
        if (mark != std::string::npos) payloadStart = mark + 4;
    } else if (full.compare(0, kSyncHttpPrefix.size(), kSyncHttpPrefix) == 0) {
        // 旧内核退路：请求由 native 当场接管，不会实际访问这个主机名。
        mark = kSyncHttpPrefix.size();
        payloadStart = mark;
    }
    if (mark == std::string::npos || payloadStart == std::string::npos) {
        // 不是本通道的请求。桥模式：媒体字节流归桥的 ffmpeg 接管——内核 onLoadUrlBegin
        // 是单槽，这里（宿主同步 RPC 退路）覆盖了桥在 createBrowser 时挂的媒体拦截，
        // 必须对非 RPC 请求链式转发（NMB_MediaLoadUrlBegin）。RPC 与媒体 URL 判据互斥
        // （base64 载荷里没有点，桥按扩展名判媒体），顺序上宿主退路优先。
        const BridgeApi& bridge = host.bridge();
        if (bridge.module && bridge.mediaLoadUrlBegin) {
            return bridge.mediaLoadUrlBegin(view, nullptr, url, job) ? TRUE : FALSE;
        }
        return FALSE;
    }

    std::string payload = full.substr(payloadStart);
    const size_t query = payload.find('?');
    if (query != std::string::npos) payload = payload.substr(0, query);

    const std::string request = Base64Decode(PercentDecode(payload));
    NwWindow* window = static_cast<NwWindow*>(param);
    if (!window) window = WindowOf(view);

    // 确认是本通道的请求才记线程拓扑（普通资源加载不能碰这两个值）。
    g_syncTransport.store(static_cast<int>(SyncTransport::XhrHook), std::memory_order_relaxed);
    g_threadSync.store(GetCurrentThreadId(), std::memory_order_relaxed);

    const std::string reply = window ? host.Dispatch(window, request, Host::Channel::Sync)
                                     : Host::Fail("这个视图还没绑定到窗口");

    if (kernel.netSetMIMEType) kernel.netSetMIMEType(job, "text/plain; charset=utf-8");
    // 退路的 http:// 形式是跨源请求，得给 CORS 头，否则同步 XHR 会被 Blink 判死。
    if (kernel.netSetHTTPHeaderFieldUtf8) {
        kernel.netSetHTTPHeaderFieldUtf8(job, "Access-Control-Allow-Origin", "*", TRUE);
        kernel.netSetHTTPHeaderFieldUtf8(job, "Cache-Control", "no-store", TRUE);
    }
    std::string body = reply;
    kernel.netSetData(job, &body[0], static_cast<int>(body.size()));
    return TRUE;
}

// ---------------------------------------------------------------------------
// 同步通道·prompt 路径（兼容退路）：页面把 base64 载荷塞进 window.prompt() 的第一个
// 参数，JS 阻塞在 prompt 上；内核同步回调进来，这里 Dispatch 后把应答 JSON 包成
// mbString 返回 —— 它就是 prompt() 的返回值。同步语义由 JS 引擎自己保证，
// 完全不经过网络栈。
//
// 现状：mb108 把 mbCreateString 的句柄带回 JS 那一步有内核级竞态（native 应答正确，
// 页面仍偶发拿到 null/脏串，重试也不能根治），所以主同步通道已改为 127.0.0.1
// loopback HTTP（见 nw_rpc.cpp / RpcServer）。prompt 只在 loopback 起不来时作为
// 退路保留。
//
// 线程：prompt 是同步模态，回调必然跑在页面 JS 所在的线程（= UI 线程）。这里把
// 线程号记进 g_threadSync，于是 SyncChannelSharesUiThread() 成立 —— 和 loopback
// 工作线程路径"确认同线程才放行 UI 命令"的既有语义无缝衔接；fs 等纯数据命令本来
// 就在白名单。
//
// mbString 的生命周期：按 SDK 惯例，回调返回的 mbStringPtr 由内核使用后释放，
// 这里不再 delete（否则 double-free）。前缀不匹配的真实 prompt 一律放行。
// ---------------------------------------------------------------------------
const char kPromptMarker[] = "__nmb_rpc__:";

void* __cdecl OnPromptBox(WebView view, void* param, const char* msg, const char*, BOOL* handled) {
    if (!handled || !msg) return nullptr;
    *handled = FALSE;

    const KernelApi& kernel = Host::Instance().kernel();
    if (!kernel.createString) return nullptr;

    const size_t markerLen = sizeof(kPromptMarker) - 1;
    if (strncmp(msg, kPromptMarker, markerLen) != 0) {
        return FALSE;
    }

    NwWindow* window = static_cast<NwWindow*>(param);
    if (!window || !window->view) window = WindowOf(view);
    if (!window) return nullptr;

    g_syncTransport.store(static_cast<int>(SyncTransport::Prompt), std::memory_order_relaxed);
    g_threadSync.store(GetCurrentThreadId(), std::memory_order_relaxed);

    const std::string request = Base64Decode(PercentDecode(msg + markerLen));
    if (request.empty()) return nullptr;

    const std::string reply = Host::Instance().Dispatch(window, request, Host::Channel::Sync);
    // mb108 内核对短字符串返回有已知竞态；这里适度填充，避免短 JSON 走损坏的短串路径。
    std::string padded = reply;
    if (padded.size() < 256) padded.append(256 - padded.size(), ' ');
    void* result = kernel.createString(padded.data(), padded.size());
    if (!result) return nullptr;
    *handled = TRUE;
    return result;
}

// 脚本上下文一建立就注入：这是最早的时机，晚了页面的首屏脚本可能已经跑过。
void __cdecl OnScriptContext(WebView view, void* param, Frame frame, void*, int, int) {
    g_threadUi.store(GetCurrentThreadId(), std::memory_order_relaxed);
    NwWindow* window = static_cast<NwWindow*>(param);
    if (!window || !window->view) window = WindowOf(view);
    if (!window) return;
    // title_override 属于文档生命周期，不属于某一种导航 API：页面自己写 location.href、
    // 点链接、服务端重定向也都会创建新主文档。必须在主框架的新脚本上下文开始时清，
    // 而不能只在 win.loadURL/reload/back/forward 的命令处理里清；iframe 不能碰它。
    const KernelApi& kernel = Host::Instance().kernel();
    if (!kernel.isMainFrame || kernel.isMainFrame(view, frame)) window->titleOverride = false;
    TraceMark("hook: didCreateScriptContext");
    Host::Instance().InstallScripts(window, frame);
}

void __cdecl OnDocumentReady(WebView view, void* param, Frame) {
    g_threadUi.store(GetCurrentThreadId(), std::memory_order_relaxed);
    NwWindow* window = static_cast<NwWindow*>(param);
    if (!window) return;
    TraceMark("hook: documentReady");
    Host::Instance().InstallScripts(window, nullptr);
}

void __cdecl OnTitleChanged(WebView view, void*, const char* title) {
    NwWindow* window = WindowOf(view);
    if (!window || !title) return;
    Host::Instance().Dispatch(window, std::string("{\"c\":\"__title\",\"t\":") +
                                          QuoteForJs(title) + "}");
}

void __cdecl OnUrlChanged(WebView view, void*, const char* url, BOOL isMainFrame) {
    NwWindow* window = WindowOf(view);
    if (!window || !url || !isMainFrame) return;
    window->url = Utf8ToWide(url);
    Host::Instance().Dispatch(window, std::string("{\"c\":\"__url\",\"t\":") +
                                          QuoteForJs(url) + "}");
}

void __cdecl OnConsole(WebView view, void*, int level, const char* text) {
    NwWindow* window = WindowOf(view);
    if (!window || !text) return;
    char prefix[64]{};
    snprintf(prefix, sizeof(prefix), "[nw console %d] ", level);
    const std::string line = std::string(prefix) + text + "\n";
    OutputDebugStringA(line.c_str());
    // stderr 也留一份：从 cmd 直接跑起来时，页面的报错能立刻看见，
    // 不用挂 DebugView。页面里一句 console.log 常常就是唯一的线索。
    // 走 EmitStderr：页面内容可能含中文，直接 fputs UTF-8 在 cmd 里会花。
    EmitStderr(line);
}

// 页面里 window.open / target=_blank：nw.js 会开一个新的 nw 窗口而不是内核裸窗口。
// 这里返回 nullptr，让内核不要自建视图——真正的窗口由 JS 层的 __nmbOpenWindow 建。
WebView __cdecl OnCreateView(WebView view, void*, int, const char* url, const void*, const void*) {
    NwWindow* window = WindowOf(view);
    if (!window || !url) return nullptr;
    std::string code = "window.__nmbOpenWindow && window.__nmbOpenWindow(" + QuoteForJs(url) + ")";
    Host::Instance().EvalInWindow(window->id, code);
    return nullptr;
}

// ---- 桥模式的宿主钩子（经 NMB_SetHostHooks 注册，见 Host::TryStartBridge）--------
// 桥占用内核的 onDocumentReady / onDidCreateScriptContext / onJsQuery 三个 per-view
// 单槽后，宿主的注入时机与异步 RPC 由桥在恰当时机转发回来。param 一律传 nullptr，
// 各 On* 回调按既有习惯用 WindowOf(view) 反查窗口。
void __cdecl BridgeDocumentReadyHook(void* view, void*, void* frame) {
    OnDocumentReady(static_cast<WebView>(view), nullptr, static_cast<Frame>(frame));
}

void __cdecl BridgeScriptContextHook(void* view, void*, void* frame, void* context,
                                     int extensionGroup, int worldId) {
    OnScriptContext(static_cast<WebView>(view), nullptr, static_cast<Frame>(frame),
                    context, extensionGroup, worldId);
}

int __cdecl BridgeJsQueryHook(void* view, void*, void* execState, long long queryId,
                              int customMsg, const char* request) {
    OnJsQuery(static_cast<WebView>(view), nullptr, static_cast<JsExec>(execState),
              static_cast<int64_t>(queryId), customMsg, request);
    // OnJsQuery 内部必用 responseQuery 应答（哪怕是错误回包），返回 1 告诉桥
    // 不必再补 {"ok":false} 的默认应答。
    return 1;
}

} // namespace

// Start() 注册窗口类时要取 HostWndProc 的函数指针，而它的定义在下面几百行处。
// 头文件里的 friend 声明只把名字绑在类里（也只有在带实参调用时才能被 ADL 找到），
// 所以这里必须再前置声明一次。
LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

TrayEntry* FindTray(int id) {
    const auto found = g_trays.find(id);
    return found == g_trays.end() ? nullptr : &found->second;
}

Host& Host::Instance() {
    static Host host;
    return host;
}

std::string Host::Ok(Json payload) {
    Json root = Json::object();
    root.putString("s", "ok");
    root.put("v", std::move(payload));
    return root.dump();
}

std::string Host::Fail(const std::string& message) {
    Json root = Json::object();
    root.putString("s", "err");
    root.putString("m", message);
    return root.dump();
}

// ---------------------------------------------------------------------------
// 启动 / 主循环
// ---------------------------------------------------------------------------

bool Host::PreparePaths(std::wstring& error) {
    exePath_ = ExecutablePath();

    std::wstring located = options_.appPath;
    if (located.empty() && !AutoLocateApp(args_, located)) {
        error = L"no application directory found: pass one on the command line, "
                L"or place package.json / package.nw next to the exe";
        return false;
    }
    if (!LoadManifest(located, manifest_)) {
        error = Utf8ToWide(manifest_.error.empty() ? std::string("failed to parse package.json")
                                                   : manifest_.error);
        return false;
    }
    appPath_ = manifest_.appPath;
    disableNode_ = options_.disableNode || !manifest_.useNode;

    // nw.DataPath：%LOCALAPPDATA%\<厂商>\<应用名>，nw.js 拿它放 localStorage 之外的私有数据。
    wchar_t buffer[MAX_PATH]{};
    std::wstring root;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buffer))) root = buffer;
    if (root.empty()) root = ExecutableDirectory();
    const std::wstring parent = JoinPath(root, L"nw-bridge");
    dataPath_ = JoinPath(parent, Utf8ToWide(manifest_.name));
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(dataPath_.c_str(), nullptr);
    return true;
}

// 无应用启动：不读 package.json，用一份内置默认 manifest 把窗口开起来，
// 启动页 URL 在 Start() 里等内核加载后回填（见 BuildSplashDataUrl）。
bool Host::PrepareSplash(std::wstring& /*error*/) {
    exePath_ = ExecutablePath();
    appPath_ = ExecutableDirectory();
    disableNode_ = options_.disableNode;

    manifest_ = Manifest();
    manifest_.name = "nw.js";
    manifest_.version = kHostVersion;
    manifest_.appPath = appPath_;
    manifest_.useNode = !disableNode_;
    manifest_.startupUrl = "about:blank";
    manifest_.window.putString("title", "nw.js");
    manifest_.window.putNumber("width", 1024);
    manifest_.window.putNumber("height", 768);

    wchar_t buffer[MAX_PATH]{};
    std::wstring root;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buffer))) root = buffer;
    if (root.empty()) root = ExecutableDirectory();
    const std::wstring parent = JoinPath(root, L"nw-bridge");
    dataPath_ = JoinPath(parent, L"splash-welcome");
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(dataPath_.c_str(), nullptr);
    return true;
}

// 桥模式的启动序列：加载桥 → 宿主绑定内核（不 init，优先 miniblink；候选见 FindMiniblinkDll）→ 桥初始化
// （共享同一模块并 mbInit）→ 注册宿主钩子。任何一步失败都完整清理并返回 false。
bool Host::TryStartBridge(std::wstring& error) {
    const std::wstring bridgeDll = FindBridgeDll(options_.bridgePath);
    if (!LoadBridge(bridgeDll, bridge_, error)) return false;
    TraceMark("bridge: NativeMediaBridge.dll loaded");

    const std::wstring kernelDll = FindMiniblinkDll(options_.kernelPath);
    if (kernelDll.empty()) {
        error = L"miniblink_x64.dll not found (required for bridge mode; "
                L"looked next to the exe and in ..\\NativeMediaBridge\\bin)";
        UnloadBridge(bridge_);
        return false;
    }
    if (!BindKernelModule(kernelDll, kernel_, error)) {
        UnloadBridge(bridge_);
        return false;
    }
    TraceMark("bridge: miniblink module bound");

    // 内核同一个模块交给桥：桥内部 LoadLibraryW 同一路径命中同一模块（refcount 各
    // 持一次），mbInit 由桥做且全进程只做一次。
    if (!bridge_.initialize(kernelDll.c_str(), nullptr)) {
        error = (bridge_.lastError && *bridge_.lastError())
                    ? bridge_.lastError()
                    : L"bridge initialization failed";
        UnloadKernel(kernel_);
        UnloadBridge(bridge_);
        return false;
    }
    TraceMark("bridge: initialize (mbInit + hooks) done");

    // 宿主钩子：桥占用注入时机与页面 RPC 槽后经此转发回来（桥把字段拷进自己的
    // 全局表，这里的栈上临时对象用完即弃没有问题）。
    BridgeHostHooks hooks{};
    hooks.param = nullptr;
    hooks.onDocumentReady = &BridgeDocumentReadyHook;
    hooks.onScriptContext = &BridgeScriptContextHook;
    hooks.onJsQuery = &BridgeJsQueryHook;
    bridge_.setHostHooks(&hooks);
    return true;
}

bool Host::Start(HINSTANCE instance, const HostOptions& options, std::wstring& error) {
    TraceMark("Start: enter");
    instance_ = instance;
    options_ = options;
    args_ = options.args;
    g_threadMain.store(GetCurrentThreadId(), std::memory_order_relaxed);

    if (options_.splashMode) {
        if (!PrepareSplash(error)) return false;
    } else {
        if (!PreparePaths(error)) return false;
    }
    TraceMark("Start: manifest/paths ready");
    // 桥模式优先：NativeMediaBridge.dll = miniblink 内核 + ffmpeg 媒体接管 + 离屏合成。
    // 失败不挡路——stderr 留一行原因，落回旧的 mb*_x64.dll 路径，行为与旧版一致。
    if (!options_.noBridge) {
        std::wstring bridgeError;
        if (TryStartBridge(bridgeError)) {
            EmitStderr("[nw] bridge mode: browser handed to NativeMediaBridge.dll "
                       "(ffmpeg media playback enabled)\n");
        } else {
            EmitStderr("[nw] bridge mode unavailable, using the plain kernel: ");
            EmitStderr(WideToUtf8(bridgeError) + "\n");
        }
    }
    if (!bridge_.module && !LoadKernel(options.kernelPath, kernel_, error)) return false;
    TraceMark("Start: bridge/kernel loaded");
    // 启动页要显示内核版本：等内核加载完再生成页面文件，URL 指向数据目录里的 splash.html。
    if (options_.splashMode) {
        if (!WriteSplashPage(kernel_, dataPath_, manifest_.startupUrl)) {
            error = L"failed to write the built-in splash page into the data directory";
            return false;
        }
    }

    std::wstring scriptError;
    if (!LoadScripts(scriptError)) {
        error = scriptError;
        return false;
    }
    TraceMark("Start: scripts read from disk");

    // loopback HTTP 同步通道（mb108 主通道，见 nw_rpc.h）。起不来不是致命错误：
    // 页面探测失败后会自动退回 prompt 退路，只是那个退路有内核级回程竞态。
    {
        std::string rpcError;
        if (!RpcServer::Instance().Start(
                [this](int windowId, const std::string& encoded) -> std::string {
                    // 与 prompt / 同步 XHR 两条通道一样，载荷是 percent-encoded
                    // base64(UTF-8 JSON)，解码逻辑复用本文件同一份实现。
                    // worker 是独立线程池里的线程：显式标成 Loopback，让
                    // SyncChannelSharesUiThread() 恒为 false，只放行数据白名单命令。
                    g_syncTransport.store(static_cast<int>(SyncTransport::Loopback),
                                          std::memory_order_relaxed);
                    g_threadSync.store(GetCurrentThreadId(), std::memory_order_relaxed);
                    NwWindow* target = Find(windowId);
                    if (!target) return Fail("窗口不存在或已关闭（id=" + std::to_string(windowId) + "）");
                    const std::string request = Base64Decode(PercentDecode(encoded));
                    return Dispatch(target, request, Channel::Sync);
                }, rpcError)) {
            EmitStderr("[nw] loopback RPC failed to start; the sync channel falls back to prompt: ");
            EmitStderr(rpcError + "\n");
        }
    }

    TraceMark("Start: rpc server up");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &HostWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = L"failed to register the window class";
        return false;
    }
    TraceMark("Start: done");
    return true;
}

namespace {

// 页面侧脚本的外层包装。放在**这里**而不是每个模块里：模块之间要共享同一个函数作用域
// （直接用彼此的 var/function），各自再套一层 IIFE 就互相看不见了。幂等判断让"注入两次"
// 变成空操作。
// tools\embed-js.ps1 里有一份**逐字相同**的包装（烘焙进 exe 的那条路），改一处要改两处。
// 守卫名不能叫 __nmbInstalled：桥模式下桥自己的注入脚本先跑，也用 __nmb* 命名，
// 同名守卫会把后注入的一方整段顶掉（宿主的 nw API 就没了）。
const char* const kApiPrologue =
    "(function () {\n"
    "  'use strict';\n"
    "  if (window.__nwApiInstalled) return;\n"
    "  window.__nwApiInstalled = true;\n";
const char* const kApiEpilogue = "\n})();\n";

// 读 api\modules.txt —— 页面侧模块的显式引入链（契约写在 js\api\modules.txt 顶部）。
// 一行一个文件名，# 注释、空行忽略。文件不存在时返回空表，调用方退回字典序排序
// （只用于旧部署兜底；模块改名后没有 chain 文件顺序是不对的）。
std::vector<std::wstring> ReadModuleChain(const std::wstring& directory) {
    std::vector<std::wstring> order;
    std::string text;
    if (!ReadTextFile(JoinPath(directory, L"modules.txt"), text) || text.empty()) return order;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const size_t a = line.find_first_not_of(" \t");
        const size_t b = line.find_last_not_of(" \t");
        if (a != std::string::npos && b != std::string::npos && b >= a) {
            order.push_back(Utf8ToWide(line.substr(a, b - a + 1)));
        }
        pos = eol;
        while (pos < text.size() && (text[pos] == '\r' || text[pos] == '\n')) ++pos;
    }
    return order;
}

// 把 api\ 下的模块拼成一个脚本。加载顺序不再靠文件名字典序，而是同目录
// modules.txt 里写死的引入链；chain 没覆盖到的 .js 排在链尾（按名字排序）。
bool AssembleApiModules(const std::wstring& directory, std::string& out) {
    std::vector<std::wstring> disk;
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW(JoinPath(directory, L"*.js").c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) return false;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) disk.emplace_back(data.cFileName);
    } while (FindNextFileW(found, &data));
    FindClose(found);
    if (disk.empty()) return false;

    std::vector<std::wstring> names;
    const std::vector<std::wstring> chain = ReadModuleChain(directory);
    if (chain.empty()) {
        names = disk;
        std::sort(names.begin(), names.end());
    } else {
        std::vector<bool> used(disk.size(), false);
        for (const std::wstring& wanted : chain) {
            for (size_t i = 0; i < disk.size(); ++i) {
                if (!used[i] && _wcsicmp(disk[i].c_str(), wanted.c_str()) == 0) {
                    names.push_back(disk[i]);
                    used[i] = true;
                    break;
                }
            }
        }
        // chain 之外的新文件：按名字排序后追加，不会被静默吞掉。
        std::vector<std::wstring> leftovers;
        for (size_t i = 0; i < disk.size(); ++i) if (!used[i]) leftovers.push_back(disk[i]);
        std::sort(leftovers.begin(), leftovers.end());
        names.insert(names.end(), leftovers.begin(), leftovers.end());
    }

    out.assign(kApiPrologue);
    int count = 0;
    for (const std::wstring& name : names) {
        std::string body;
        if (!ReadTextFile(JoinPath(directory, name), body) || body.empty()) continue;
        // 每个模块的来历留在拼出来的脚本里：页面里报错时能一眼看出是哪个文件。
        out += "// ---- " + Narrow(name) + " ----\n";
        out += body;
        if (body[body.size() - 1] != '\n') out += "\n";
        ++count;
    }
    if (count == 0) {
        out.clear();
        return false;
    }
    out += kApiEpilogue;
    return true;
}

}  // namespace

bool Host::LoadScripts(std::wstring& error) {
    // 优先 exe 旁边的脚本（开发期改 JS 不用重编译），否则用编进 exe 的内置副本。
    const std::wstring base = ExecutableDirectory();
    std::string api;
    std::string node;
    if (!AssembleApiModules(JoinPath(base, L"api"), api)) api = BuiltinApiScript();
    if (!disableNode_ && !ReadTextFile(JoinPath(base, L"nw_node.js"), node)) node = BuiltinNodeScript();
    if (api.empty()) {
        error = L"missing api\\*.js: neither an api directory next to the exe nor a baked-in copy was found";
        return false;
    }
    apiScript_ = api;
    nodeScript_ = node;
    return true;
}

int Host::Run() {
    MSG message{};
    while (!quit_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return exitCode_;
}

void Host::Quit(int exitCode) {
    exitCode_ = exitCode;
    quit_ = true;
    PostQuitMessage(exitCode);
}

void Host::Shutdown() {
    // 必须在进程退出前同步停掉：worker 还会回调 Dispatch 碰窗口表，
    // 让静态单例先析构/CRT 卸载会踩到已释放内存。Stop 内部会 join 全部线程。
    RpcServer::Instance().Stop();
    // 桥模式：桥持有全部浏览器与内核（mbInit 也是它做的），让它统一收尾
    // （内部逐个销毁浏览器 + FreeLibrary 内核）。宿主不再 UnloadKernel——
    // 那是同一个模块句柄，桥的 shutdown 里已经放过了。
    if (bridge_.module && bridge_.shutdown) bridge_.shutdown();
    UnloadBridge(bridge_);
}

void Host::WakeAll() {
    for (NwWindow* window : AllWindows()) {
        if (kernel_.wake) kernel_.wake(window->view);
    }
}

void Host::InstallScripts(NwWindow* window, Frame frame) {
    if (!window || !window->view) return;
    // 脚本自身幂等（开头有 __nwApiInstalled 判断），所以 didCreateScriptContext 和
    // onDocumentReady 两条路都注入一遍，谁先来算谁的。
    // mb108 在脚本上下文创建回调内会拒绝发起同步 XHR。启动所需的两份纯数据由
    // native 直接随注入脚本带入；页面脚本真正运行后，fs 等接口再懒探测同步通道。
    const std::string appReply = Dispatch(window, "{\"c\":\"app.info\"}", Channel::Async);
    const std::string processReply = Dispatch(window, "{\"c\":\"process.info\"}", Channel::Async);
    // loopback RPC 的端口/令牌是进程级常量：只随注入脚本交给本进程页面，
    // 不写盘、不进命令行。端口为 0 表示服务没起来，页面侧据此跳过该通道。
    const int rpcPort = RpcServer::Instance().port();
    std::string rpcBoot;
    if (rpcPort > 0) {
        rpcBoot = ";window.__nmbRpcPort=" + std::to_string(rpcPort) +
                  ";window.__nmbRpcToken=" + QuoteForJs(RpcServer::Instance().token());
    }
    std::string preamble = "window.__nwWindowId=" + std::to_string(window->id) +
                           ";window.__nwNodeEnabled=" + (window->nodeEnabled ? "true" : "false") +
                           ";window.__nmbPromptAvailable=" +
                           ((kernel_.onPromptBox && kernel_.createString) ? "true" : "false") +
                           rpcBoot +
                           ";window.__nmbBootAppReply=" + appReply +
                           ";window.__nmbBootProcessReply=" + processReply + ";";
    const bool tracePage = TracePageEnabled();
    const std::string code = preamble + (tracePage ? kPageTraceBootstrap : "") +
                             apiScript_ + (window->nodeEnabled ? nodeScript_ : std::string()) +
                             (tracePage ? kPageTraceAfterInstall : "");
    KernelApi& k = kernel_;
    const double evalStart = TraceNowMs();
    if (k.runJs) {
        Frame target = frame ? frame : (k.webFrameGetMainFrame ? k.webFrameGetMainFrame(window->view) : nullptr);
        k.runJs(window->view, target, code.c_str(), 1 /* 闭包内执行 */, nullptr, nullptr, nullptr);
    }
    if (TraceStartupEnabled()) {
        static int calls = 0;
        ++calls;
        char line[224]{};
        snprintf(line, sizeof(line),
                 "[nw trace] %8.1f ms  InstallScripts #%d (%s, %d bytes, eval %.1f ms)\n",
                 TraceElapsedMs(), calls, frame ? "child-frame" : "main-frame", (int)code.size(),
                 TraceNowMs() - evalStart);
        EmitStderr(line);
    }
}

std::vector<NwWindow*> Host::AllWindows() {
    std::vector<NwWindow*> out;
    out.reserve(windows_.size());
    for (auto& entry : windows_) out.push_back(entry.second.get());
    return out;
}

NwWindow* Host::Find(int id) {
    const auto found = windows_.find(id);
    return found == windows_.end() ? nullptr : found->second.get();
}

void Host::EvalInWindow(int id, const std::string& code) {
    NwWindow* window = Find(id);
    if (!window || !window->view || !kernel_.runJs) return;
    Frame frame = kernel_.webFrameGetMainFrame ? kernel_.webFrameGetMainFrame(window->view) : nullptr;
    kernel_.runJs(window->view, frame, code.c_str(), 0, nullptr, nullptr, nullptr);
}

std::string Host::EvalInWindowSync(int id, const std::string& code) {
    NwWindow* window = Find(id);
    if (!window || !window->view) return std::string();
    return RunJsSync(window->view, code.c_str());
}

// ---------------------------------------------------------------------------
// 开窗
// ---------------------------------------------------------------------------

int Host::OpenWindow(const std::string& urlJson, NwWindow* opener) {
    TraceMark("OpenWindow: enter");
    Json config;
    if (!urlJson.empty()) Json::parse(urlJson, config);
    if (!config.isObject()) config = Json::object();

    const int id = nextWindowId_++;
    auto holder = std::make_unique<NwWindow>();
    NwWindow* window = holder.get();
    window->id = id;
    window->opener = opener;
    window->nodeEnabled = !disableNode_;

    // 取值优先级：本次调用的 config > manifest 的 window 段 > 内置默认值（与 nw.js 一致）。
    const Json& defaults = manifest_.window;
    const bool frameless = !config.boolean("frame", defaults.boolean("frame", true));
    const bool resizable = config.boolean("resizable", defaults.boolean("resizable", true));
    const bool showOnStart = config.boolean("show", defaults.boolean("show", true));
    const bool transparent = config.boolean("transparent", defaults.boolean("transparent", false));
    const bool fullscreen = config.boolean("fullscreen", defaults.boolean("fullscreen", false));
    const bool kiosk = config.boolean("kiosk", defaults.boolean("kiosk", false));

    auto numberOr = [&](const char* key, double fallback) {
        return config.find(key) ? config.number(key, fallback) : defaults.number(key, fallback);
    };

    int width = static_cast<int>(numberOr("width", 800));
    int height = static_cast<int>(numberOr("height", 600));
    if (width <= 0) width = 800;
    if (height <= 0) height = 600;

    window->frameless = frameless;
    window->resizable = resizable;
    window->alwaysOnTop = config.boolean("always-on-top", defaults.boolean("always-on-top", false));
    window->title = Utf8ToWide(config.text("title", defaults.text("title", manifest_.name)));
    // manifest 的 min_width/min_height/max_width/max_height（nw.js 的 window 段语义）：
    // 落进 minW/minH/maxW/maxH，由 WM_GETMINMAXINFO 拦拖拽、ApiWindow 拦编程式缩放。
    // 无边框窗口的边框缩放也走前者，所以这里必须填，否则能拖到远小于声明的最小尺寸。
    window->minW = static_cast<int>(numberOr("min_width", 0));
    window->minH = static_cast<int>(numberOr("min_height", 0));
    window->maxW = static_cast<int>(numberOr("max_width", 0));
    window->maxH = static_cast<int>(numberOr("max_height", 0));

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (frameless || kiosk) style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    int x = static_cast<int>(numberOr("x", INT_MIN));
    int y = static_cast<int>(numberOr("y", INT_MIN));
    const std::string position = config.text("position", defaults.text("position", "center"));
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (x == INT_MIN || y == INT_MIN || position == "center") {
        POINT cursor{};
        if (position == "mouse" && GetCursorPos(&cursor)) {
            x = cursor.x - width / 2;
            y = cursor.y - height / 2;
        } else {
            x = work.left + ((work.right - work.left) - width) / 2;
            y = work.top + ((work.bottom - work.top) - height) / 2;
        }
    }

    HWND hwnd = CreateWindowExW(0, kWndClass, window->title.c_str(), style, x, y, width, height,
                                opener ? opener->hwnd : nullptr, nullptr, instance_, nullptr);
    if (!hwnd) return 0;
    window->hwnd = hwnd;
    window->savedStyle = style;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));

    // 窗口图标：Window/manifest 的 icon 字段，相对路径相对应用根（nw 语义）。
    // 大小两档各加载一次，系统标题栏/任务栏各取所需。
    const std::string iconRel = config.find("icon") ? config.text("icon") : defaults.text("icon");
    if (!iconRel.empty()) {
        std::wstring iconPath = Utf8ToWide(iconRel);
        if (iconPath.size() < 2 || iconPath[1] != L':') iconPath = JoinPath(appPath_, iconPath);
        const HICON big = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
        const HICON small = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                                          GetSystemMetrics(SM_CXSMICON),
                                                          GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
        if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    }

    // 内核视图。MB_WINDOW_TYPE_CONTROL 让内核把浏览器做成宿主窗口的子窗口：
    // 输入、滚动、IME、光标全在核心里（原来这活是 Chromium 的 RenderWidgetHostView 干的）。
    RECT client{};
    GetClientRect(hwnd, &client);
    const int clientWidth = std::max(1L, client.right - client.left);
    const int clientHeight = std::max(1L, client.bottom - client.top);
    WebView view = nullptr;
    if (bridge_.module) {
        // 桥模式：整个浏览器（离屏合成 + ffmpeg 媒体接管）外包给桥。桥在宿主窗口
        // 客户区里自建一个子窗口（内部仍是 miniblink，但输入/绘制由桥接管），
        // 宿主只管壳（标题/菜单/托盘/RPC）和 nw.Window 的窗口几何。
        void* browser = bridge_.createBrowser(hwnd, 0, 0, clientWidth, clientHeight);
        if (!browser) {
            DestroyWindow(hwnd);
            return 0;
        }
        view = static_cast<WebView>(bridge_.getWebView(browser));
        if (!view) {
            bridge_.destroyBrowser(browser);
            DestroyWindow(hwnd);
            return 0;
        }
        window->browser = browser;
    } else if (kernel_.createWebWindow) {
        view = kernel_.createWebWindow(kWindowControl, hwnd, 0, 0, clientWidth, clientHeight);
    } else {
        view = kernel_.createWebView();
        if (view) {
            kernel_.setHandle(view, hwnd);
            kernel_.resize(view, clientWidth, clientHeight);
            if (kernel_.setAutoDrawToHwnd) kernel_.setAutoDrawToHwnd(view, 1);
        }
    }
    if (!view) {
        DestroyWindow(hwnd);
        return 0;
    }
    window->view = view;
    // 边框缩放：客户区里承载页面的子窗口要类化，命中测试才交得回框架窗口（见 BorderChildProc）。
    // 后建的子窗口由 HostWndProc 的 WM_PARENTNOTIFY 补装。
    InstallBorderHitTest(hwnd, 1);
    // mb108 的 Node bridge 是按 view 的 debug 配置开启的，必须早于首个页面加载。
    // nodejs:false / --nw-no-node 必须同时关掉 debug 配置和 view 开关；否则内核仍会
    // 把 nw.exe 的宿主参数误当成 Node 主模块，页面随即退出。
    const char* nodeSetting = disableNode_ ? "0" : "1";
    if (bridge_.module) {
        if (bridge_.setDebugConfig) bridge_.setDebugConfig(window->browser, "enableNodejs", nodeSetting);
    } else if (kernel_.setDebugConfig) {
        kernel_.setDebugConfig(view, "enableNodejs", nodeSetting);
    }
    if (kernel_.setNodeJsEnable) kernel_.setNodeJsEnable(view, disableNode_ ? FALSE : TRUE);
    BindView(view, window);
    TraceMark("OpenWindow: browser created, hooks bound");

    // 透明窗口是"内核直接画进宿主 HWND"的形态；桥模式的浏览器子窗口不支持，
    // 桥模式下静默忽略（nw.Window 语义降级为不透明）。
    if (!bridge_.module && transparent && kernel_.setTransparent) kernel_.setTransparent(view, 1);
    if (kernel_.setCspCheckEnable) kernel_.setCspCheckEnable(view, 0);   // nw 应用常用内联脚本
    if (bridge_.module) {
        // 桥模式：onDocumentReady / onDidCreateScriptContext / onJsQuery 三个槽被桥
        // 占用（onPaintUpdated 桥自己合成用），宿主的注入时机与异步 RPC 经
        // NMB_SetHostHooks 的宿主钩子转发回来（见 Bridge*Hook 适配器）。
        // 这里只挂桥不要的槽：标题/URL/console 通知、开新窗口、以及两条同步退路。
        if (kernel_.onTitleChanged) kernel_.onTitleChanged(view, &OnTitleChanged, window);
        if (kernel_.onURLChanged) kernel_.onURLChanged(view, &OnUrlChanged, window);
        if (kernel_.onConsole) kernel_.onConsole(view, &OnConsole, window);
        if (kernel_.onCreateView) kernel_.onCreateView(view, &OnCreateView, window);
        // 同步通道·URL 拦截退路（见 OnLoadUrlBegin）：必须在首个页面加载之前挂上，
        // 否则首屏脚本里的 fs.readFileSync 会打到一个没人接的 URL 上。
        if (kernel_.onLoadUrlBegin) kernel_.onLoadUrlBegin(view, &OnLoadUrlBegin, window);
        // 同步通道·prompt 退路（见 OnPromptBox）：主通道是 loopback HTTP。
        if (kernel_.onPromptBox) kernel_.onPromptBox(view, &OnPromptBox, window);
    } else {
        if (kernel_.onJsQuery) kernel_.onJsQuery(view, &OnJsQuery, window);
        // 同步通道·URL 拦截退路（见 OnLoadUrlBegin）：必须在首个页面加载之前挂上，否则
        // 首屏脚本里的 fs.readFileSync 会打到一个没人接的 URL 上。
        if (kernel_.onLoadUrlBegin) kernel_.onLoadUrlBegin(view, &OnLoadUrlBegin, window);
        // 同步通道·prompt 退路（见 OnPromptBox）：主通道是 loopback HTTP，这里同样要赶在
        // 页面脚本之前挂上，loopback 探测失败时页面还能落到它。
        if (kernel_.onPromptBox) kernel_.onPromptBox(view, &OnPromptBox, window);
        if (kernel_.onDidCreateScriptContext) kernel_.onDidCreateScriptContext(view, &OnScriptContext, window);
        if (kernel_.onDocumentReady) kernel_.onDocumentReady(view, &OnDocumentReady, window);
        if (kernel_.onTitleChanged) kernel_.onTitleChanged(view, &OnTitleChanged, window);
        if (kernel_.onURLChanged) kernel_.onURLChanged(view, &OnUrlChanged, window);
        if (kernel_.onConsole) kernel_.onConsole(view, &OnConsole, window);
        if (kernel_.onCreateView) kernel_.onCreateView(view, &OnCreateView, window);
    }
    if (!manifest_.userAgent.empty() && kernel_.setUserAgent) kernel_.setUserAgent(view, manifest_.userAgent.c_str());

    std::string url = config.text("url");
    if (url.empty()) url = manifest_.startupUrl;
    window->url = Utf8ToWide(url);

    windows_[id] = std::move(holder);

    // 先建好后注入，再加载：注入脚本要在页面首屏脚本之前就位。
    InstallScripts(window, nullptr);
    if (kernel_.loadURL) kernel_.loadURL(view, url.c_str());
    TraceMark("OpenWindow: injected, loadURL issued");

    if (kernel_.showWindow) kernel_.showWindow(view, showOnStart ? 1 : 0);
    if (showOnStart) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    TraceMark("OpenWindow: shown");
    if (fullscreen || kiosk) Dispatch(window, "{\"c\":\"win.fullscreen\",\"id\":" + std::to_string(id) + "}");
    return id;
}

void Host::CloseWindow(int id) {
    NwWindow* window = Find(id);
    if (!window) return;
    window->closing = true;
    // 托盘图标挂在窗口上：窗口关了必须一并摘掉，否则任务栏角落留僵尸图标。
    for (auto it = g_trays.begin(); it != g_trays.end();) {
        if (it->second.window == window) {
            Shell_NotifyIconW(NIM_DELETE, &it->second.data);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_trays.erase(it);
        } else {
            ++it;
        }
    }
    if (window->menu) {
        SetMenu(window->hwnd, nullptr);
        DestroyMenu(window->menu);
        window->menu = nullptr;
    }
    // 桥模式：先让桥销毁它的浏览器子窗口（内部连视图一起），再销毁壳。顺序反了
    // 壳先死，桥对着无效父子关系再拆一遍。view 已清空，WM_DESTROY 里不会再碰。
    if (window->browser && bridge_.module && bridge_.destroyBrowser) {
        bridge_.destroyBrowser(window->browser);
        window->browser = nullptr;
        window->view = nullptr;
    }
    if (window->hwnd) DestroyWindow(window->hwnd);
}

// ---------------------------------------------------------------------------
// 窗口过程：这一层只处理"窗口壳"的事（缩放、移动、关闭、托盘、热键），
// 页面的键鼠事件全部由内核子窗口自己消费，不经过这里。
// ---------------------------------------------------------------------------

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
// 启动原生拖动。不能在 ApiWindow 里内联做：SetCapture 之后要一路等到用户的
// WM_LBUTTONUP 才结束，而 RPC 回调必须立刻返回（否则页面的异步通道会卡住）。
constexpr UINT kStartDragMessage = WM_APP + 2;

void ApplyFullscreen(NwWindow* window, bool on) {
    if (!window || window->fullscreen == on) return;
    window->fullscreen = on;
    if (on) {
        GetWindowRect(window->hwnd, &window->savedRect);
        window->savedStyle = GetWindowLongW(window->hwnd, GWL_STYLE);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST), &info);
        SetWindowLongW(window->hwnd, GWL_STYLE, (window->savedStyle & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
        SetWindowPos(window->hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(window->hwnd, GWL_STYLE, window->savedStyle);
        SetWindowPos(window->hwnd, nullptr, window->savedRect.left, window->savedRect.top,
                     window->savedRect.right - window->savedRect.left,
                     window->savedRect.bottom - window->savedRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    ShowWindow(window->hwnd, SW_SHOW);
}

// kiosk = 全屏 + 置顶。nw 文档原话："full screen except the window is always on top"。
// 进去先置顶再吃边框；退出先还边框再摘 TOPMOST——顺序反过来会闪出一帧置顶的普通窗。
void ApplyKiosk(NwWindow* window, bool on) {
    if (!window || window->kiosk == on) return;
    window->kiosk = on;
    if (on) {
        SetWindowPos(window->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ApplyFullscreen(window, true);
    } else {
        ApplyFullscreen(window, false);
        SetWindowPos(window->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// 开始原生拖动：记下抓取点，把鼠标捕获抢到窗口上，后面的 WM_MOUSEMOVE 自己跟随。
void StartWindowDrag(NwWindow* window) {
    if (!window || !window->hwnd || window->dragging) return;
    // 最大化/最小化/全屏/kiosk 下没有可视的边框可以拉，直接忽略（nw 里这几种状态本来也不给拖）。
    if (window->maximized || window->minimized || window->fullscreen || window->kiosk) return;
    // 页面的 mousedown 到这条命令之间隔着一次 RPC。用户手快（按下就松）时按钮已经抬起了，
    // 这时再 SetCapture 会把窗口黏在鼠标上，直到下一次点击才松开。
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    RECT rect{};
    if (!GetWindowRect(window->hwnd, &rect)) return;
    window->dragOrigin = cursor;
    window->dragStartRect = rect;
    window->dragging = true;
    SetCapture(window->hwnd);
}

// ---------------------------------------------------------------------------
// 无边框窗口的边框缩放
//
// 无边框窗口是 WS_POPUP，系统不画可拖拽的边框，命中测试得由宿主自己答。答在
// **框架窗口**里（见 HostWndProc 的 WM_NCHITTEST）只是必要条件，不充分：内核/桥
// 会在客户区里铺一层（或多层）子窗口承载页面，真实鼠标的命中测试落在最深的那层
// 子窗口上，它默认回 HTCLIENT，框架窗口的 WM_NCHITTEST 在真实输入下根本收不到
// （直接 SendMessage 才会走到）。所以子窗口也要类化：只在边框带上返回
// HTTRANSPARENT —— 同线程下命中测试会继续往父窗口问，父窗口再给出 HT*，
// 系统就按原生方式开始缩放循环。其余位置一律原样放行，页面输入不受影响。
// ---------------------------------------------------------------------------

const wchar_t* kBorderProcProp = L"NmbBorderOriginalProc";
// 递归层数上限：够覆盖"浏览器子窗口 + 它自己的渲染/输入子窗口"，再深就不碰了。
constexpr int kBorderSubclassDepth = 4;

// 边框带宽度：SM_CXSIZEFRAME 是外框厚度，SM_CXPADDEDBORDER 是 DPI 附加量，
// 两者相加才是系统实际的缩放热区；下限 6px 保证高 DPI 缩放下也拖得住。
int BorderBandWidth() {
    return std::max(6, GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER));
}

// 屏幕坐标落在边框带上就返回对应的 HT*，否则 0。
// 只有"无边框 + 可缩放 + 不在最大化/最小化/全屏/kiosk"才给：其余状态没有可视边框，
// 给了会让页面边缘 8px 白白吃掉点击。这些标志都是运行时可变（setResizable 等），
// 所以每次命中测试都重新读，不缓存。
int BorderResizeHitTest(const NwWindow* window, POINT screenPoint) {
    if (!window || !window->hwnd || !window->frameless || !window->resizable) return 0;
    if (window->maximized || window->minimized || window->fullscreen || window->kiosk) return 0;
    RECT rect{};
    if (!GetWindowRect(window->hwnd, &rect)) return 0;
    const int edge = BorderBandWidth();
    const bool left = screenPoint.x >= rect.left && screenPoint.x < rect.left + edge;
    const bool right = screenPoint.x < rect.right && screenPoint.x >= rect.right - edge;
    const bool top = screenPoint.y >= rect.top && screenPoint.y < rect.top + edge;
    const bool bottom = screenPoint.y < rect.bottom && screenPoint.y >= rect.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return 0;
}

// 反查子窗口所属的 NwWindow：沿祖先链找到框架窗口，它的 USERDATA 就是 NwWindow*。
// 不能只看直接父窗口——内核可能再套一层自己的渲染子窗口，那一层的父窗口不是框架窗口。
// 类名也要核对：别的窗口的 USERDATA 未必是 NwWindow*，直接解引用就是野指针。
NwWindow* WindowOfChild(HWND child) {
    for (HWND parent = GetParent(child); parent; parent = GetParent(parent)) {
        wchar_t className[64]{};
        if (GetClassNameW(parent, className, 64) == 0) return nullptr;
        if (wcscmp(className, kWndClass) != 0) continue;
        NwWindow* window = reinterpret_cast<NwWindow*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
        return (window && window->hwnd == parent) ? window : nullptr;
    }
    return nullptr;
}

// 子窗口过程：命中测试落在边框带上就交出（HTTRANSPARENT），其余消息原样转给被换掉的原过程。
LRESULT CALLBACK BorderChildProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) {
        NwWindow* window = WindowOfChild(hwnd);
        if (window && BorderResizeHitTest(window, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}) != 0) {
            return HTTRANSPARENT;
        }
    }
    // 子窗口自己再建的窗口（内核的渲染层）收不到框架窗口的 WM_PARENTNOTIFY，得在这里补装。
    if (message == WM_PARENTNOTIFY && LOWORD(wparam) == WM_CREATE) {
        InstallBorderHitTest(hwnd, 1);
    }
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kBorderProcProp));
    if (!original) return DefWindowProcW(hwnd, message, wparam, lparam);
    return CallWindowProcW(original, hwnd, message, wparam, lparam);
}

// 类化一个子窗口。装过就跳过：属性在 = 这条链上已经有我们，**不管现在最外层是谁**。
// 内核/桥可能在之后又把窗口类化一次，那时若还照"最外层不是我们"就再装一遍，
// 会把它们的窗口过程从链上挤掉（我们只记得最初的原过程）。装过即跳过，
// 既保住它们的链，也让我们的命中测试继续留在链里。
void SubclassBorderWindow(HWND child, DWORD ownerThread) {
    if (!child) return;
    if (GetPropW(child, kBorderProcProp)) return;
    // 只碰本线程创建的窗口。内核/桥会把 IME 之类系统窗口也挂在框架窗口下，它们由
    // 自己的线程创建、消息也在那条线程上派发；跨线程换窗口过程是 Win32 明确不建议的
    // 做法，而且这些窗口不承载页面、永远收不到命中测试，类化它们没有收益只有风险。
    if (GetWindowThreadProcessId(child, nullptr) != ownerThread) return;
    const LONG_PTR previous = SetWindowLongPtrW(child, GWLP_WNDPROC,
                                                reinterpret_cast<LONG_PTR>(&BorderChildProc));
    if (!previous) return;
    if (!SetPropW(child, kBorderProcProp, reinterpret_cast<HANDLE>(previous))) {
        SetWindowLongPtrW(child, GWLP_WNDPROC, previous);   // 属性存不下就整个退回
    }
}

// 递归装：命中测试落在最深的那层子窗口上，所以整条子窗口链都要装。
void InstallBorderHitTest(HWND parent, int depth, DWORD ownerThread) {
    if (!parent || depth > kBorderSubclassDepth) return;
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SubclassBorderWindow(child, ownerThread);
        InstallBorderHitTest(child, depth + 1, ownerThread);
    }
}

// 从框架窗口起装：ownerThread 就是宿主 UI 线程（框架窗口属于它）。
void InstallBorderHitTest(HWND frame, int depth) {
    InstallBorderHitTest(frame, depth, GetWindowThreadProcessId(frame, nullptr));
}

} // namespace

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Host& host = Host::Instance();
    NwWindow* window = reinterpret_cast<NwWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // 托盘回调要先于 USERDATA 判断：托盘图标挂在创建它的窗口上，而那个窗口可能已经关了。
    if (message == kTrayMessage) {
        TrayEntry* tray = FindTray(static_cast<int>(wparam));
        if (!tray) return 0;
        const std::string event = lparam == WM_LBUTTONUP ? "click"
                                : lparam == WM_RBUTTONUP ? "right-click"
                                : lparam == WM_LBUTTONDBLCLK ? "double-click" : "event";
        std::string code = "window.__nmbTrayEvent&&window.__nmbTrayEvent(" + std::to_string(tray->id) +
                           "," + QuoteForJs(event) + ")";
        if (tray->window) host.EvalInWindow(tray->window->id, code);
        return 0;
    }

    if (message == kStartDragMessage) {
        StartWindowDrag(window);
        return 0;
    }

    // 拖动期间由这个窗口独占鼠标：SetCapture 之后 WM_MOUSEMOVE 只会送到这里，
    // 内核子窗口收不到，所以不会被页面的 hover 合成消息干扰。
    if (window && window->dragging) {
        if (message == WM_MOUSEMOVE) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                SetWindowPos(window->hwnd, nullptr,
                             window->dragStartRect.left + (cursor.x - window->dragOrigin.x),
                             window->dragStartRect.top + (cursor.y - window->dragOrigin.y),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        if (message == WM_LBUTTONUP) {
            window->dragging = false;   // 先清标志再 ReleaseCapture：后者会同步回调 WM_CAPTURECHANGED
            ReleaseCapture();
            return 0;
        }
    }
    // 捕获被别的窗口抢走（弹窗、任务栏、系统模态循环）必须停拖，否则窗口会一直跟着鼠标跑。
    if (message == WM_CAPTURECHANGED && window && window->dragging) {
        window->dragging = false;
    }

    if (message == WM_NCHITTEST) {
        // 边框缩放由框架窗口作答；命中测试本来会先落在客户区子窗口上，
        // 那些子窗口已被 InstallBorderHitTest 类化，在边框带上放行到这里。
        const int hit = BorderResizeHitTest(window, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (hit != 0) return hit;
    }

    switch (message) {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // 滚轮消息按键盘焦点窗口投递，而桥模式下焦点可能短暂落在顶层壳窗口，
        // 不能假设它一定命中承载页面的子窗口。保留 lParam 的屏幕坐标原样转发，
        // 由桥窗口统一 ScreenToClient 后调用 mbFireMouseWheelEvent。
        if (window && window->browser && host.bridge().getWindow) {
            HWND browserHwnd = host.bridge().getWindow(window->browser);
            if (browserHwnd && browserHwnd != hwnd && IsWindow(browserHwnd)) {
                SendMessageW(browserHwnd, message, wparam, lparam);
                return 0;
            }
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        // setMinimumSize/setMaximumSize：MINMAXINFO 管的是外框尺寸，与 nw 语义一致。
        // 这里只拦用户拖拽；编程式 moveTo/resizeTo 的钳制在 ApiWindow 里另做。
        if (window && (window->minW > 0 || window->minH > 0 || window->maxW > 0 || window->maxH > 0)) {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
            if (window->minW > 0) info->ptMinTrackSize.x = window->minW;
            if (window->minH > 0) info->ptMinTrackSize.y = window->minH;
            if (window->maxW > 0) info->ptMaxTrackSize.x = window->maxW;
            if (window->maxH > 0) info->ptMaxTrackSize.y = window->maxH;
            return 0;
        }
        break;
    }
    case WM_PARENTNOTIFY:
        // 内核/桥在页面加载过程中会陆续往客户区里挂子窗口；创建时补装一次边框命中测试，
        // 免得只有建窗那一刻存在的子窗口被类化、后建的又回到 HTCLIENT 吃掉边缘。
        if (LOWORD(wparam) == WM_CREATE) InstallBorderHitTest(hwnd, 1);
        break;
    case WM_SIZE: {
        if (!window || !window->view) break;
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        // 桥模式：几何由桥管理（浏览器子窗口 + 离屏合成），走 NMB_Resize；
        // 旧模式：直接驱动内核。resize 的调用点收在 WM_SIZE 一处。
        if (window->browser && host.bridge().resize) {
            host.bridge().resize(window->browser, width, height);
        } else if (host.kernel().resize) {
            host.kernel().resize(window->view, width, height);
        }
        if (wparam == SIZE_MAXIMIZED) window->maximized = true;
        else if (wparam == SIZE_RESTORED) window->maximized = false;
        break;
    }
    case WM_MOVE:
    case WM_EXITSIZEMOVE:
        // 子浏览器窗口是宿主窗口的 HWND 子窗口，宿主移动时会由 Windows 自动跟随。
        // 这里不能再次调用 bridge.resize：该调用会对桥窗口执行 SetWindowPos、重设内核
        // 尺寸并触发合成，在高频 WM_MOVE（页面拖动）期间会让可见内容短暂脱离宿主，
        // 表现为窗口从桌面消失而任务栏缩略图仍可见。真正的尺寸变化只在 WM_SIZE 处理。
        if (window && window->view) {
            if (host.kernel().wake) host.kernel().wake(window->view);
            InvalidateRect(hwnd, nullptr, FALSE);
            if (message == WM_EXITSIZEMOVE) UpdateWindow(hwnd);
        }
        break;
    case WM_SETFOCUS:
        if (window && window->view && host.kernel().setFocus) host.kernel().setFocus(window->view);
        break;
    case WM_ACTIVATE:
        // 失活时把内核的焦点收掉：不然内核还以为自己拿着键盘，切回来时输入法状态是错的。
        if (window && window->view && host.kernel().killFocus && LOWORD(wparam) == WA_INACTIVE) {
            host.kernel().killFocus(window->view);
        }
        break;
    case WM_HOTKEY: {
        if (!window) break;
        const auto found = window->hotkeys.find(static_cast<int>(wparam));
        if (found == window->hotkeys.end()) break;
        host.EvalInWindow(window->id, std::string("window.__nmbHotkey&&window.__nmbHotkey(") +
                                          QuoteForJs(found->second) + ")");
        break;
    }
    case WM_COMMAND: {
        // 菜单项：lparam==0 表示来自菜单（不是控件），低 16 位是菜单 id。
        if (lparam == 0 && window) {
            const int menuId = LOWORD(wparam);
            host.EvalInWindow(window->id, "window.__nmbMenuClick&&window.__nmbMenuClick(" +
                                          std::to_string(menuId) + ")");
            return 0;
        }
        break;
    }
    case WM_CLOSE: {
        if (!window) break;
        // 给页面一次反悔的机会：nw.js 的 win.on('close') 就是在这里被问的。
        // 页面若返回 "cancel" 就得自己决定何时真正关（比如异步处理完再调 win.close()）。
        const std::string verdict = host.EvalInWindowSync(window->id,
            "window.__nmbCloseVerdict?String(window.__nmbCloseVerdict()) : 'close'");
        if (verdict == "cancel") return 0;
        host.CloseWindow(window->id);
        return 0;
    }
    case WM_DESTROY: {
        if (window) {
            if (window->browser) {
                // 桥模式：视图与桥的子窗口都归桥销毁（正常路径已在 CloseWindow
                // 里销毁并把 browser 清空；走到这里说明壳被外部直接销毁，兜底一次）。
                BindView(window->view, nullptr);
                if (host.bridge().destroyBrowser) host.bridge().destroyBrowser(window->browser);
                window->browser = nullptr;
                window->view = nullptr;
            } else if (window->view && host.kernel().destroyWebView) {
                BindView(window->view, nullptr);
                host.kernel().destroyWebView(window->view);
                window->view = nullptr;
            }
            const int id = window->id;
            host.windows_.erase(id);
        }
        // 所有窗口都关了就退出——和 nw.js 的"最后一个窗口关闭即退出"一致。
        if (host.windows_.empty()) host.Quit(0);
        break;
    }
    case WM_ENDSESSION:
        host.Quit(0);
        break;
    case WM_DISPLAYCHANGE:
        // 显示器插拔/分辨率变化：广播给每个窗口，Screen 模块据此重拉 screens
        // 并派发 displayBoundsChanged（与 nw.Screen 的事件语义对齐）。
        for (const auto& item : host.windows_) {
            if (item.second->view) {
                host.EvalInWindow(item.second->id,
                    "window.__nmbScreenEvent&&window.__nmbScreenEvent('displayBoundsChanged')");
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// 桥的入口：页面发来的每条 JSON 在这里按 "c" 字段路由到对应 API。
// 前缀式路由而不是哈希表，是为了让"有哪些能力"在源码里一眼可见——
// nw.js 那边这些能力分散在 src/api/*/ 的各个 native handler 里，很难一眼看全。
// ---------------------------------------------------------------------------

// Blink 重入红线：这几条连"同线程"也救不了，同步通道上永远不放行。
// 同步 XHR 的应答就是在 UI 线程上内联产生的，此刻 Blink 正卡在嵌套里，
// 再回去调 mbRunJs / 发起导航 / 建视图，等于在别人的调用栈里重入它。
bool BlinkReentrancyUnsafe(const std::string& command) {
    static const std::string kUnsafe[] = {
        "win.eval",       // mbRunJs：重入正在执行的脚本
        "win.loadURL",    // 重入导航
        "win.reload",
        "win.back",
        "win.forward",
        "win.canGoBack",  // 读 Blink 的历史栈，仍在 Blink 里
        "win.canGoForward",
        "win.create",     // 在 Blink 内部建视图
        "win.devtools",
    };
    for (const std::string& item : kUnsafe) {
        if (command == item) return true;
    }
    return false;
}

// 同步通道的准入名单，分三档：
//
//   1) 任何线程都能跑（kAnywhere）—— 只碰数据（文件、剪贴板、显示器、进程信息），
//      或者只读 NwWindow 的字段 + IsWindowVisible 这类线程无关的 Win32 查询。
//   2) 要碰 UI 线程（窗口表 / 菜单 / 托盘 / 热键 / 消息循环）—— 只有当同步通道的回调
//      确认和 UI 回调同线程时才放行，见 SyncChannelSharesUiThread()。
//   3) Blink 重入红线 —— 见上面，同线程也不放行。
//
// 之所以按线程分档而不是"先验地只给数据命令"：哪条回调在哪个线程上，内核没写文档。
// 这条真值由本文件开头的三个 thread id 实测出来，随 app.info 报给页面，
// 于是"要不要把 UI 命令也放上同步通道"变成一个查表决定，而不是一个赌注。
bool SyncSafeCommand(const std::string& command) {
    static const std::string kAnywhere[] = {
        "app.info",     // 纯数据：清单 / argv / dataPath
        "fs.",          // node 桥的文件后端
        "process.",     // node 桥的进程信息
        "clipboard.",   // 剪贴板是会话级的，任何线程都能开
        "screen.",      // EnumDisplayMonitors 线程无关
        "shell.",       // ShellExecute 系列线程无关
        "win.state",    // 只读 NwWindow 字段 + IsWindowVisible：不碰 Blink，不碰消息循环
    };
    for (const std::string& prefix : kAnywhere) {
        if (command.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

std::string Host::Dispatch(NwWindow* window, const std::string& request, Channel channel) {
    Json payload;
    if (!request.empty() && !Json::parse(request, payload)) {
        return Fail("请求不是合法 JSON");
    }
    const std::string command = payload.text("c");
    if (command.empty()) return Fail("缺少命令名 c");

    if (channel == Channel::Sync) {
        if (BlinkReentrancyUnsafe(command)) {
            return Fail("命令 " + command + " 会重入 Blink，只能走异步通道");
        }
        // 同线程 = 同步回调本来就和 UI 回调跑在一条线上，UI 命令带内联作答是安全的。
        // 不同线程/未知 = 老实拒绝，让页面退到异步通道去。
        if (!SyncSafeCommand(command) && !SyncChannelSharesUiThread()) {
            return Fail("命令 " + command + " 动了 UI 线程的东西，只能走异步通道");
        }
    }

    // ---- 内核事件（native 主动推给页面的，页面只需要转成 nw 事件）----
    if (command == "__title" || command == "__url") {
        const bool isTitle = command == "__title";
        // document.title → 窗口标题（nw.js 的默认行为）。页面显式 setTitle 过之后就不跟了，
        // 那个值一直管到下一次导航（下面的 win.loadURL/reload/back/forward 会清掉它）。
        if (isTitle && !window->titleOverride) {
            window->title = Utf8ToWide(payload.text("t"));
            if (window->hwnd) SetWindowTextW(window->hwnd, window->title.c_str());
        }
        // 推给页面的是**生效值**：setTitle 过之后页面读 win.title 必须和标题栏一致，
        // 否则 JS 侧会显示一个窗口上并不存在的标题。
        const std::string value = isTitle ? Narrow(window->title) : payload.text("t");
        EvalInWindow(window->id, "window.__nmbEvent&&window.__nmbEvent(" + QuoteForJs(isTitle ? "title" : "url") +
                                 "," + QuoteForJs(value) + ")");
        return Ok(Json());
    }
    if (command.rfind("app.", 0) == 0) return ApiApp(window, payload);
    if (command.rfind("win.", 0) == 0) return ApiWindow(window, payload);
    if (command.rfind("menu.", 0) == 0) return ApiMenu(window, payload);
    if (command.rfind("tray.", 0) == 0) return ApiTray(window, payload);
    if (command.rfind("clipboard.", 0) == 0) return ApiClipboard(window, payload);
    if (command.rfind("shell.", 0) == 0) return ApiShell(window, payload);
    if (command.rfind("screen.", 0) == 0) return ApiScreen(window, payload);
    if (command.rfind("shortcut.", 0) == 0) return ApiShortcut(window, payload);
    if (command.rfind("fs.", 0) == 0) return ApiFs(window, payload);
    if (command.rfind("process.", 0) == 0) return ApiProcess(window, payload);
    return Fail("未知命令：" + command);
}

// ---------------------------------------------------------------------------
// nw.App —— 对应 nw.js 的 src/api/app/app.js + browser 侧的 AppBindings
// ---------------------------------------------------------------------------

std::string Host::ApiApp(NwWindow* window, const Json& request) {
    const std::string command = request.text("c");

    if (command == "app.info") {
        Json info = Json::object();
        info.putString("name", manifest_.name);
        info.putString("version", manifest_.version);
        info.putString("appPath", Narrow(appPath_));
        info.putString("dataPath", Narrow(dataPath_));
        info.putString("exePath", Narrow(exePath_));
        info.putString("manifestPath", Narrow(manifest_.manifestPath));
        info.putString("startupUrl", manifest_.startupUrl);
        info.putString("singleInstance", manifest_.singleInstance ? "true" : "false");
        Json argv = Json::array();
        argv.push(Json::fromText(Narrow(exePath_)));
        for (const std::wstring& arg : args_) argv.push(Json::fromText(Narrow(arg)));
        info.put("argv", std::move(argv));
        Json manifest;
        if (!manifest_.source.empty() && Json::parse(manifest_.source, manifest)) info.put("manifest", std::move(manifest));
        info.putString("rootWindowId", std::to_string(window ? window->id : 0));
        // 线程拓扑实测值：页面据此决定 UI 命令走同步还是异步（见 SyncSafeCommand 的注释）。
        Json threads = Json::object();
        threads.putNumber("main", static_cast<double>(g_threadMain.load(std::memory_order_relaxed)));
        threads.putNumber("ui", static_cast<double>(g_threadUi.load(std::memory_order_relaxed)));
        threads.putNumber("sync", static_cast<double>(g_threadSync.load(std::memory_order_relaxed)));
        threads.putBool("syncSharesUi", SyncChannelSharesUiThread());
        info.put("threads", std::move(threads));
        return Ok(std::move(info));
    }
    if (command == "app.quit" || command == "app.exit") {
        Quit(static_cast<int>(request.number("code", 0)));
        return Ok(Json());
    }
    if (command == "app.restart") {
        // nw 应用经常用"改完配置重启自己"这套；这里按 nw.js 的做法起一个新进程再退出。
        std::wstring line = QuoteArg(exePath_);
        for (const std::wstring& arg : args_) line += L" " + QuoteArg(arg);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> mutableLine(line.begin(), line.end());
        mutableLine.push_back(L'\0');
        if (CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           appPath_.c_str(), &startup, &process)) {
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
        Quit(0);
        return Ok(Json());
    }
    if (command == "app.crash") {
        // nw 文档：crashBrowser()/crashRenderer() 用于测试崩溃恢复。本宿主进程
        // 就是 browser + renderer 的合体，直接硬退（取 Chromium 常规崩溃码）。
        TerminateProcess(GetCurrentProcess(), 134);
        return Fail("进程未能如期终止");
    }
    if (command == "app.registerEvents") {
        // nw.js 在这里把 app 级事件（open/reopen/argv 变化）注册到 Browser process。
        // 本实现的窗口/进程关系是 1:1，没有需要跨进程转发的 app 事件，如实回一句。
        return Ok(Json::fromText("单窗口宿主，无跨进程 app 事件"));
    }
    return Fail("app 命令不支持：" + command);
}

// ---------------------------------------------------------------------------
// nw.Window —— 对应 src/api/window/window.js + browser 侧的 NativeWindow/WindowBindings
// ---------------------------------------------------------------------------

std::string Host::ApiWindow(NwWindow* window, const Json& request) {
    const std::string command = request.text("c");
    // 带 id 的命令操作指定窗口；不带 id 的操作发起窗口自己。
    NwWindow* target = window;
    if (request.find("id")) target = Find(static_cast<int>(request.number("id", 0)));
    if (!target && command != "win.list" && command != "win.create") return Fail("窗口不存在或已关闭");

    if (command == "win.list") {
        Json list = Json::array();
        for (NwWindow* item : AllWindows()) {
            if (!item->hwnd) continue;
            RECT rect{};
            GetWindowRect(item->hwnd, &rect);
            Json entry = Json::object();
            entry.putNumber("id", item->id);
            entry.putString("title", Narrow(item->title));
            entry.putString("url", Narrow(item->url));
            entry.putNumber("x", rect.left);
            entry.putNumber("y", rect.top);
            entry.putNumber("width", rect.right - rect.left);
            entry.putNumber("height", rect.bottom - rect.top);
            entry.putBool("maximized", item->maximized);
            entry.putBool("minimized", item->minimized);
            entry.putBool("fullscreen", item->fullscreen);
            entry.putBool("visible", IsWindowVisible(item->hwnd) != 0);
            list.push(std::move(entry));
        }
        return Ok(std::move(list));
    }
    if (command == "win.create") {
        const int id = OpenWindow(request.text("config"), target);
        if (!id) return Fail("开窗失败");
        Json result = Json::object();
        result.putNumber("id", id);
        return Ok(std::move(result));
    }
    if (command == "win.close") {
        CloseWindow(target->id);
        return Ok(Json());
    }
    if (command == "win.show") {
        ShowWindow(target->hwnd, SW_SHOW);
        if (kernel_.showWindow) kernel_.showWindow(target->view, 1);
        SetForegroundWindow(target->hwnd);
        return Ok(Json());
    }
    if (command == "win.hide") {
        ShowWindow(target->hwnd, SW_HIDE);
        if (kernel_.showWindow) kernel_.showWindow(target->view, 0);
        return Ok(Json());
    }
    if (command == "win.focus") {
        SetForegroundWindow(target->hwnd);
        if (kernel_.setFocus) kernel_.setFocus(target->view);
        return Ok(Json());
    }
    if (command == "win.blur") {
        if (kernel_.killFocus) kernel_.killFocus(target->view);
        return Ok(Json());
    }
    if (command == "win.startDrag") {
        if (!target->hwnd) return Fail("窗口已销毁");
        // 只投递不内联：拖动会一直阻塞到用户松手，
        // 而这里必须立刻返回（页面走的是异步通道，但宿主线程不能被模态循环占住）。
        PostMessageW(target->hwnd, kStartDragMessage, 0, 0);
        return Ok(Json());
    }
    if (command == "win.minimize") {
        ShowWindow(target->hwnd, SW_MINIMIZE);
        target->minimized = true;
        return Ok(Json());
    }
    if (command == "win.restore") {
        // 全屏状态下"还原"应该退回全屏前的矩形，而不是 SW_RESTORE 直接跳回最大化状态。
        if (target->fullscreen) ApplyFullscreen(target, false);
        ShowWindow(target->hwnd, SW_RESTORE);
        target->minimized = false;
        target->maximized = false;
        return Ok(Json());
    }
    if (command == "win.maximize") {
        ShowWindow(target->hwnd, SW_MAXIMIZE);
        target->maximized = true;
        return Ok(Json());
    }
    if (command == "win.unmaximize") {
        ShowWindow(target->hwnd, SW_RESTORE);
        target->maximized = false;
        return Ok(Json());
    }
    if (command == "win.fullscreen") {
        ApplyFullscreen(target, true);
        return Ok(Json());
    }
    if (command == "win.leaveFullscreen") {
        ApplyFullscreen(target, false);
        return Ok(Json());
    }
    if (command == "win.moveTo" || command == "win.resizeTo") {
        RECT rect{};
        GetWindowRect(target->hwnd, &rect);
        int x = request.find("x") ? static_cast<int>(request.number("x")) : rect.left;
        int y = request.find("y") ? static_cast<int>(request.number("y")) : rect.top;
        int width = request.find("width") ? static_cast<int>(request.number("width")) : rect.right - rect.left;
        int height = request.find("height") ? static_cast<int>(request.number("height")) : rect.bottom - rect.top;
        // setMinimumSize/setMaximumSize 对编程式缩放同样生效（WM_GETMINMAXINFO 只拦拖拽）。
        if (target->maxW > 0) width = std::min(width, target->maxW);
        if (target->maxH > 0) height = std::min(height, target->maxH);
        if (target->minW > 0) width = std::max(width, target->minW);
        if (target->minH > 0) height = std::max(height, target->minH);
        SetWindowPos(target->hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        return Ok(Json());
    }
    if (command == "win.setTitle") {
        const std::string title = request.text("title");
        target->title = Utf8ToWide(title);
        // 显式设过就是覆盖值：页面后面再改 document.title 也不动标题栏（nw.js 的 title_override）。
        target->titleOverride = true;
        SetWindowTextW(target->hwnd, target->title.c_str());
        return Ok(Json());
    }
    if (command == "win.setResizable") {
        LONG style = GetWindowLongW(target->hwnd, GWL_STYLE);
        if (request.boolean("value", true)) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        else style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongW(target->hwnd, GWL_STYLE, style);
        SetWindowPos(target->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        target->resizable = request.boolean("value", true);
        return Ok(Json());
    }
    if (command == "win.setAlwaysOnTop") {
        const bool on = request.boolean("value", true);
        SetWindowPos(target->hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        target->alwaysOnTop = on;
        target->onTopSetByUser = true;
        return Ok(Json());
    }
    if (command == "win.eval") {
        const std::string code = request.text("code");
        Json result = Json::object();
        result.putString("value", EvalInWindowSync(target->id, code));
        return Ok(std::move(result));
    }
    if (command == "win.state") {
        Json state = Json::object();
        state.putNumber("id", target->id);
        state.putBool("maximized", target->maximized);
        state.putBool("minimized", target->minimized);
        state.putBool("fullscreen", target->fullscreen);
        state.putBool("kiosk", target->kiosk);
        state.putBool("visible", IsWindowVisible(target->hwnd) != 0);
        state.putString("title", Narrow(target->title));
        state.putNumber("zoom", target->zoom);
        state.putString("url", Narrow(target->url));
        // 位置和尺寸一并报出去：GetWindowRect 线程无关，而 nw.Window.get() 的 width/height
        // 是同步属性，少了这几个字段页面只能读到 0（win.list 里本来就有）。
        RECT rect{};
        if (GetWindowRect(target->hwnd, &rect)) {
            state.putNumber("x", rect.left);
            state.putNumber("y", rect.top);
            state.putNumber("width", rect.right - rect.left);
            state.putNumber("height", rect.bottom - rect.top);
        }
        return Ok(std::move(state));
    }
    if (command == "win.devtools") {
        if (kernel_.setDebugConfig) {
            kernel_.setDebugConfig(target->view, "showDevTools", request.boolean("show", true) ? "1" : "0");
        }
        return Ok(Json());
    }
    if (command == "win.setZoom") {
        const double factor = request.number("factor", 1.0);
        // 先记下来再交给内核：内核没有 getZoomFactor，"记住了多少"是页面唯一能读回的口。
        target->zoom = factor;
        if (kernel_.setZoomFactor) kernel_.setZoomFactor(target->view, static_cast<float>(factor));
        return Ok(Json());
    }
    // 导航类命令一律清掉标题覆盖值：新文档的 document.title 应该重新说了算
    // （nw.js 里 title_override 也是随导航复位的）。
    if (command == "win.loadURL") {
        const std::string url = request.text("url");
        if (!url.empty()) {
            target->titleOverride = false;
            target->url = Utf8ToWide(url);
            if (kernel_.loadURL) kernel_.loadURL(target->view, url.c_str());
        }
        return Ok(Json());
    }
    if (command == "win.reload") {
        target->titleOverride = false;
        if (kernel_.reload) kernel_.reload(target->view);
        return Ok(Json());
    }
    if (command == "win.back") {
        target->titleOverride = false;
        if (kernel_.goBack) kernel_.goBack(target->view);
        return Ok(Json());
    }
    if (command == "win.forward") {
        target->titleOverride = false;
        if (kernel_.goForward) kernel_.goForward(target->view);
        return Ok(Json());
    }
    if (command == "win.canGoBack" || command == "win.canGoForward") {
        const bool back = command == "win.canGoBack";
        bool allowed = false;
        if (back && kernel_.canGoBack) allowed = kernel_.canGoBack(target->view);
        if (!back && kernel_.canGoForward) allowed = kernel_.canGoForward(target->view);
        Json result = Json::object();
        result.putBool("value", allowed);
        return Ok(std::move(result));
    }
    if (command == "win.enterKiosk") {
        ApplyKiosk(target, true);
        return Ok(Json());
    }
    if (command == "win.leaveKiosk") {
        ApplyKiosk(target, false);
        return Ok(Json());
    }
    if (command == "win.toggleKiosk") {
        ApplyKiosk(target, !target->kiosk);
        return Ok(Json());
    }
    if (command == "win.setMaximumSize" || command == "win.setMinimumSize") {
        const bool isMax = command == "win.setMaximumSize";
        if (isMax) {
            target->maxW = static_cast<int>(request.number("width", 0));
            target->maxH = static_cast<int>(request.number("height", 0));
        } else {
            target->minW = static_cast<int>(request.number("width", 0));
            target->minH = static_cast<int>(request.number("height", 0));
        }
        // 编程式缩放立即按新边界钳一次（0 = 不限制；拖拽走 WM_GETMINMAXINFO，不经过这里）。
        RECT rect{};
        GetWindowRect(target->hwnd, &rect);
        const int width = std::max(target->minW, target->maxW > 0
            ? std::min(static_cast<int>(rect.right - rect.left), target->maxW)
            : static_cast<int>(rect.right - rect.left));
        const int height = std::max(target->minH, target->maxH > 0
            ? std::min(static_cast<int>(rect.bottom - rect.top), target->maxH)
            : static_cast<int>(rect.bottom - rect.top));
        if (width != rect.right - rect.left || height != rect.bottom - rect.top) {
            SetWindowPos(target->hwnd, nullptr, rect.left, rect.top, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return Ok(Json());
    }
    if (command == "win.setPosition") {
        // nw 0.12 遗留方法：'center' / 'mouse' 两种预置位，锚点都是窗口中心。
        const std::string where = request.text("position");
        if (where == "center" || where == "mouse") {
            RECT rect{};
            GetWindowRect(target->hwnd, &rect);
            int x = 0;
            int y = 0;
            if (where == "center") {
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                GetMonitorInfoW(MonitorFromWindow(target->hwnd, MONITOR_DEFAULTTONEAREST), &info);
                x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - (rect.right - rect.left)) / 2;
                y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - (rect.bottom - rect.top)) / 2;
            } else {
                POINT cursor{};
                GetCursorPos(&cursor);
                x = cursor.x - (rect.right - rect.left) / 2;
                y = cursor.y - (rect.bottom - rect.top) / 2;
            }
            SetWindowPos(target->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return Ok(Json());
    }
    if (command == "win.requestAttention") {
        // nw 语义：requestAttention([boolean]) —— true 表示停止闪烁，缺省开始闪烁。
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = target->hwnd;
        if (request.boolean("value", false)) {
            flash.dwFlags = FLASHW_STOP;
        } else {
            flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;   // 一直闪到窗口拿到焦点
        }
        FlashWindowEx(&flash);
        return Ok(Json());
    }
    if (command == "win.setShowInTaskbar") {
        // WS_EX_APPWINDOW 强制出现任务栏按钮，WS_EX_TOOLWINDOW 强制隐藏；
        // 对可见窗口改 EXSTYLE 要先藏再显，任务栏按钮才会立即重算。
        const bool show = request.boolean("value", true);
        LONG ex = GetWindowLongW(target->hwnd, GWL_EXSTYLE);
        if (show) ex = (ex | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW;
        else ex = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
        const bool visible = IsWindowVisible(target->hwnd) != 0;
        if (visible) ShowWindow(target->hwnd, SW_HIDE);
        SetWindowLongW(target->hwnd, GWL_EXSTYLE, ex);
        if (visible) ShowWindow(target->hwnd, SW_SHOW);
        return Ok(Json());
    }
    if (command == "win.setProgressBar") {
        // 任务栏进度条：ITaskbarList3。v<0 移除，v>1 不确定态（跑马灯），0..1 显示百分比。
        const double value = request.number("value", -1);
        static ITaskbarList3* taskbar = nullptr;   // 只在 UI 线程用到，无需加锁
        if (!taskbar) {
            static const GUID kCLSID_TaskbarList =
                {0x56FDF344, 0xFD6D, 0x11d0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
            static const GUID kIID_ITaskbarList3 =
                {0xEA1AFB91, 0x9E28, 0x4B86, {0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF}};
            // COM 初始化失败/组件缺失只意味着"没有进度条"，不影响窗口本体。
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            CoCreateInstance(kCLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                             kIID_ITaskbarList3, reinterpret_cast<void**>(&taskbar));
        }
        if (taskbar) {
            if (value < 0) {
                taskbar->SetProgressState(target->hwnd, TBPF_NOPROGRESS);
            } else if (value > 1) {
                taskbar->SetProgressState(target->hwnd, TBPF_INDETERMINATE);
            } else {
                taskbar->SetProgressState(target->hwnd, TBPF_NORMAL);
                taskbar->SetProgressValue(target->hwnd,
                    static_cast<ULONGLONG>(value * 100.0 + 0.5), 100);
            }
        }
        return Ok(Json());
    }
    if (command == "win.setBadgeLabel") {
        // 文档标注 Mac only：Windows 的 nw.js 同样是 no-op，如实对齐。
        return Ok(Json());
    }
    if (command == "win.setVisibleOnAllWorkspaces") {
        // Windows 上"所有工作区可见"的等价物就是 TOPMOST；与 setAlwaysOnTop 共用
        // 同一位，用户已显式置顶时不重复动（避免把用户的置顶清掉）。
        if (!target->alwaysOnTop) {
            const bool on = request.boolean("value", true);
            SetWindowPos(target->hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return Ok(Json());
    }
    if (command == "win.canSetVisibleOnAllWorkspaces") {
        Json result = Json::object();
        result.putBool("value", true);
        return Ok(std::move(result));
    }
    if (command == "win.setIgnoreMouseEvents") {
        // 鼠标穿透：WS_EX_TRANSPARENT 让 WM_NCHITTEST 直接透给下层窗口。
        LONG ex = GetWindowLongW(target->hwnd, GWL_EXSTYLE);
        if (request.boolean("value", false)) ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
        else ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongW(target->hwnd, GWL_EXSTYLE, ex);
        return Ok(Json());
    }
    if (command == "win.setTransparent") {
        // nw 文档标注已弃用，但 0.14 仍可用：转发内核的视图透明。
        if (kernel_.setTransparent) {
            kernel_.setTransparent(target->view, request.boolean("value", false) ? 1 : 0);
        }
        return Ok(Json());
    }
    if (command == "win.capturePage") {
        // 内核视图是宿主窗口的子窗口（kWindowControl）：PrintWindow 抓它的客户区，
        // DIB 是 BGRA，就地翻成 RGBA 再 base64，页面侧组 canvas 后 toDataURL。
        HWND child = nullptr;
        EnumChildWindows(target->hwnd, [](HWND item, LPARAM out) -> BOOL {
            *reinterpret_cast<HWND*>(out) = item;
            return FALSE;                      // 第一个子窗口就是内核视图
        }, reinterpret_cast<LPARAM>(&child));
        if (!child) return Fail("找不到内核视图子窗口");
        RECT rect{};
        if (!GetClientRect(child, &rect)) return Fail("取内核视图客户区失败");
        const int width = static_cast<int>(rect.right - rect.left);
        const int height = static_cast<int>(rect.bottom - rect.top);
        if (width <= 0 || height <= 0) return Fail("内核视图客户区为空");
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;      // 负高 = 自上而下行序
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        HDC dcChild = GetDC(child);
        HDC dcMem = CreateCompatibleDC(dcChild);
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool captured = false;
        if (bitmap && bits) {
            const HGDIOBJ old = SelectObject(dcMem, bitmap);
            constexpr DWORD kPwRenderFullContent = 0x00000002;   // Win8.1+，老头文件缺此常量
            // PrintWindow 能抓被遮挡的内容；失败退回 BitBlt 屏拷贝。
            if (!PrintWindow(child, dcMem, PW_CLIENTONLY | kPwRenderFullContent) &&
                !PrintWindow(child, dcMem, PW_CLIENTONLY)) {
                BitBlt(dcMem, 0, 0, width, height, dcChild, 0, 0, SRCCOPY | CAPTUREBLT);
            }
            SelectObject(dcMem, old);
            memcpy(pixels.data(), bits, pixels.size());
            captured = true;
        }
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dcMem);
        ReleaseDC(child, dcChild);
        if (!captured) return Fail("抓屏失败");
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);  // BGRA→RGBA
        Json result = Json::object();
        result.putNumber("width", width);
        result.putNumber("height", height);
        result.putString("data", Base64Encode(std::string(
            reinterpret_cast<const char*>(pixels.data()), pixels.size())));
        return Ok(std::move(result));
    }
    if (command == "win.getPrinters") {
        // 本地 + 已连接的网络打印机，PRINTER_INFO_4 够轻（nw 文档也只要名字）。
        DWORD needed = 0;
        DWORD count = 0;
        EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &count);
        std::vector<unsigned char> buffer(needed > 0 ? needed : 1);
        Json list = Json::array();
        if (EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &needed, &count)) {
            const PRINTER_INFO_4W* printers = reinterpret_cast<const PRINTER_INFO_4W*>(buffer.data());
            for (DWORD i = 0; i < count; ++i) list.push(Json::fromText(Narrow(printers[i].pPrinterName)));
        }
        return Ok(std::move(list));
    }
    if (command == "win.print") {
        // 无打印栈：如实报错，不假装成功（页面可据此退级）。
        return Fail("宿主未实现打印（内核无打印栈）");
    }
    return Fail("win 命令不支持：" + command);
}

// ---------------------------------------------------------------------------
// nw.Menu / nw.MenuItem —— 对应 src/api/menu + MenuBindings。
// nw.js 那边菜单是"JS 对象 + native 侧一份镜像"，这里更进一步：
// native 只留 HMENU，点击时把菜单 id 丢回页面，由 JS 侧的注册表找回调。
// ---------------------------------------------------------------------------

namespace {

// 菜单 id 从 1001 起，避开 WM_COMMAND 里可能出现的系统值。
void BuildMenuItems(const Json& items, HMENU menu, int& nextId) {
    if (!items.isArray()) return;
    for (const Json& item : items.items) {
        const std::string type = item.text("type", "normal");
        if (type == "separator") {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        const std::wstring label = Utf8ToWide(item.text("label"));
        const bool enabled = item.boolean("enabled", true);
        const bool checked = item.boolean("checked", false);
        const Json* submenu = item.find("submenu");
        if (submenu && submenu->isArray()) {
            HMENU child = CreatePopupMenu();
            BuildMenuItems(*submenu, child, nextId);
            AppendMenuW(menu, MF_POPUP | (enabled ? MF_ENABLED : MF_GRAYED),
                        reinterpret_cast<UINT_PTR>(child), label.c_str());
            continue;
        }
        // id 优先用 JS 给的：页面侧的点击回调表是按 id 建的，native 自己编号就对不上了。
        const int id = item.find("id") ? static_cast<int>(item.number("id", nextId)) : nextId;
        if (id >= nextId) nextId = id + 1;
        UINT flags = MF_STRING;
        if (!enabled) flags |= MF_GRAYED;
        if (type == "checkbox") flags |= checked ? MF_CHECKED : MF_UNCHECKED;
        else if (type == "radio") flags |= checked ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(id), label.c_str());
    }
}

} // namespace

std::string Host::ApiMenu(NwWindow* window, const Json& request) {
    const std::string command = request.text("c");
    NwWindow* target = window;
    if (request.find("id")) target = Find(static_cast<int>(request.number("id", 0)));
    if (!target) return Fail("窗口不存在");

    if (command == "menu.set" || command == "menu.create") {
        const Json* items = request.find("items");
        HMENU menu = CreateMenu();
        int nextId = 1001;
        if (items) BuildMenuItems(*items, menu, nextId);
        HMENU old = target->menu;
        target->menu = menu;
        SetMenu(target->hwnd, menu);
        DrawMenuBar(target->hwnd);
        if (old) DestroyMenu(old);
        Json result = Json::object();
        result.putNumber("firstId", 1001);
        result.putNumber("lastId", nextId - 1);
        return Ok(std::move(result));
    }
    if (command == "menu.clear") {
        if (target->menu) {
            SetMenu(target->hwnd, nullptr);
            DestroyMenu(target->menu);
            target->menu = nullptr;
            DrawMenuBar(target->hwnd);
        }
        return Ok(Json());
    }
    if (command == "menu.popup") {
        HMENU menu = CreatePopupMenu();
        int nextId = 1001;
        const Json* items = request.find("items");
        if (items) BuildMenuItems(*items, menu, nextId);
        POINT point{};
        point.x = static_cast<LONG>(request.number("x", 0));
        point.y = static_cast<LONG>(request.number("y", 0));
        if (point.x == 0 && point.y == 0) GetCursorPos(&point);
        // TPM_RETURNCMD：菜单是模态的，等用户点完再返回选中的 id，
        // 这样点击回调能从当前这次查询里直接发出，不用额外存一份弹出菜单的映射。
        const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                           point.x, point.y, 0, target->hwnd, nullptr);
        DestroyMenu(menu);
        if (chosen) {
            EvalInWindow(target->id, "window.__nmbMenuClick&&window.__nmbMenuClick(" +
                                     std::to_string(chosen) + ")");
        }
        return Ok(Json());
    }
    if (command == "menu.setLabel") {
        if (target->menu) {
            const int id = static_cast<int>(request.number("itemId", 0));
            const std::wstring label = Utf8ToWide(request.text("label"));
            if (id) ModifyMenuW(target->menu, static_cast<UINT>(id), MF_BYCOMMAND | MF_STRING,
                                static_cast<UINT_PTR>(id), label.c_str());
        }
        return Ok(Json());
    }
    return Fail("menu 命令不支持：" + command);
}

// ---------------------------------------------------------------------------
// nw.Tray —— 对应 src/api/tray + TrayBindings（Shell_NotifyIcon）
// ---------------------------------------------------------------------------

std::string Host::ApiTray(NwWindow* window, const Json& request) {
    const std::string command = request.text("c");

    if (command == "tray.create") {
        const int id = g_nextTrayId++;
        TrayEntry entry;
        entry.id = id;
        entry.window = window;
        entry.data.cbSize = sizeof(NOTIFYICONDATAW);
        entry.data.hWnd = window->hwnd;
        entry.data.uID = static_cast<UINT>(id);
        entry.data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        entry.data.uCallbackMessage = kTrayMessage;
        const std::string iconRel = request.text("icon");
        HICON fileIcon = nullptr;
        if (!iconRel.empty()) {
            std::wstring iconPath = Utf8ToWide(iconRel);
            // 相对路径相对应用根（nw 语义）；手工判盘符/UNC 前缀，不引 shlwapi。
            if (iconPath.size() < 2 || (iconPath[1] != L':' && iconPath.compare(0, 2, L"\\\\") != 0)) {
                iconPath = JoinPath(appPath_, iconPath);
            }
            fileIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
        }
        entry.icon = fileIcon;
        entry.data.hIcon = fileIcon ? fileIcon : LoadIconW(nullptr, IDI_APPLICATION);
        const std::wstring tooltip = Utf8ToWide(request.text("tooltip", manifest_.name));
        wcsncpy_s(entry.data.szTip, tooltip.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_ADD, &entry.data)) return Fail("创建托盘图标失败");
        g_trays[id] = entry;
        Json result = Json::object();
        result.putNumber("id", id);
        return Ok(std::move(result));
    }

    const int id = static_cast<int>(request.number("id", 0));
    TrayEntry* tray = FindTray(id);
    if (!tray) return Fail("托盘图标不存在");

    if (command == "tray.setTooltip") {
        const std::wstring tooltip = Utf8ToWide(request.text("tooltip"));
        wcsncpy_s(tray->data.szTip, tooltip.c_str(), _TRUNCATE);
        tray->data.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &tray->data);
        return Ok(Json());
    }
    if (command == "tray.setIcon") {
        const std::string iconRel = request.text("icon");
        if (iconRel.empty()) return Fail("tray.setIcon 需要 icon 路径");
        std::wstring iconPath = Utf8ToWide(iconRel);
        if (iconPath.size() < 2 || (iconPath[1] != L':' && iconPath.compare(0, 2, L"\\\\") != 0)) {
            iconPath = JoinPath(appPath_, iconPath);
        }
        HICON fresh = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON),
                                                    GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
        if (!fresh) return Fail("托盘图标加载失败：" + iconRel);
        HICON stale = tray->icon;
        tray->icon = fresh;
        tray->data.uFlags = NIF_ICON;
        tray->data.hIcon = fresh;
        Shell_NotifyIconW(NIM_MODIFY, &tray->data);
        if (stale) DestroyIcon(stale);
        return Ok(Json());
    }
    if (command == "tray.remove") {
        Shell_NotifyIconW(NIM_DELETE, &tray->data);
        if (tray->icon) DestroyIcon(tray->icon);
        g_trays.erase(id);
        return Ok(Json());
    }
    return Fail("tray 命令不支持：" + command);
}

// ---------------------------------------------------------------------------
// nw.Clipboard / nw.Shell / nw.Screen / nw.Shortcut
// 对应 src/api/{clipboard,shell,screen,shortcut} 下的 native handler。
// ---------------------------------------------------------------------------

namespace {

// 剪贴板是在**同步通道**上开的（内核的网络线程，见 OnLoadUrlBegin）：别的进程/线程
// 正拿着剪贴板时 OpenClipboard 会直接失败（典型是 ERROR_ACCESS_DENIED），这是一次性的
// 竞争而不是权限问题，所以重试；重试还失败就把 GetLastError 报出去——只回一句
// "打开剪贴板失败"会让人以为是本进程没权限，白查半天。
bool OpenClipboardRetry() {
    const int kAttempts = 10;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        if (attempt + 1 < kAttempts) Sleep(20);
    }
    return false;
}

std::string ClipboardOpenFailure() {
    return "打开剪贴板失败（GetLastError=" + std::to_string(static_cast<long long>(GetLastError())) +
           "，重试 10 次仍被别的进程占着）";
}

} // namespace

std::string Host::ApiClipboard(NwWindow*, const Json& request) {
    const std::string command = request.text("c");
    // 注册格式：CF_HTML 和 PNG 都没有系统常量，首次使用时注册（进程内幂等）。
    // nw 的 readAvailableTypes 也按这几个类型名汇报。
    static const UINT htmlFormat = RegisterClipboardFormatW(L"HTML Format");
    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    static const UINT rtfFormat = RegisterClipboardFormatW(L"Rich Text Format");

    if (command == "clipboard.write") {
        const std::string type = request.text("type", "text");
        if (type != "text" && type != "raw" && type != "html" && type != "png" && type != "rtf") {
            return Fail("不支持的剪贴板类型：" + type + "（可用：text/raw/html/png/rtf）");
        }
        if (!OpenClipboardRetry()) return Fail(ClipboardOpenFailure());
        EmptyClipboard();
        bool ok = false;
        if (type == "text" || type == "raw") {
            const std::wstring text = Utf8ToWide(request.text("data"));
            const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* target = GlobalLock(handle)) {
                    memcpy(target, text.c_str(), bytes);
                    GlobalUnlock(handle);
                    ok = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
                }
            }
        } else if (type == "html") {
            // CF_HTML 是带头部的 HTML 片段：四个 10 位偏移量按实际内容回填，
            // 片段用 <!--StartFragment--> / <!--EndFragment--> 框住。
            const std::string fragment = request.text("data");
            const std::string prefix = "<html><body><!--StartFragment-->";
            const std::string suffix = "<!--EndFragment--></body></html>";
            auto pad10 = [](size_t value) {
                std::string out = std::to_string(value);
                if (out.size() < 10) out.insert(0, 10 - out.size(), '0');
                return out;
            };
            std::string head = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                               "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
            const size_t startHtml = head.size();
            const size_t startFragment = startHtml + prefix.size();
            const size_t endFragment = startFragment + fragment.size();
            head.replace(head.find("StartHTML:") + 10, 10, pad10(startHtml));
            head.replace(head.find("EndHTML:") + 8, 10, pad10(endFragment + suffix.size()));
            head.replace(head.find("StartFragment:") + 14, 10, pad10(startFragment));
            head.replace(head.find("EndFragment:") + 12, 10, pad10(endFragment));
            const std::string payload = head + prefix + fragment + suffix;
            if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, payload.size() + 1)) {
                if (void* target = GlobalLock(handle)) {
                    memcpy(target, payload.c_str(), payload.size() + 1);
                    GlobalUnlock(handle);
                    ok = SetClipboardData(htmlFormat, handle) != nullptr;
                }
            }
        } else {
            // 二进制（png/rtf）：页面侧传 base64（JSON 通道装不下裸字节），解码后
            // 原样放进注册格式，别的应用照常能读。GlobalSize 可能带分配粒度尾巴，
            // PNG 的解析器对 IEND 之后的垃圾宽容，可接受。
            const std::string bytes = Base64Decode(request.text("data"));
            const UINT format = type == "png" ? pngFormat : rtfFormat;
            if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size() + 1)) {
                if (void* target = GlobalLock(handle)) {
                    memcpy(target, bytes.data(), bytes.size());
                    static_cast<char*>(target)[bytes.size()] = '\0';
                    GlobalUnlock(handle);
                    ok = SetClipboardData(format, handle) != nullptr;
                }
            }
        }
        CloseClipboard();
        if (!ok) return Fail("写入剪贴板失败");
        return Ok(Json());
    }
    if (command == "clipboard.read") {
        const std::string type = request.text("type", "text");
        UINT format = CF_UNICODETEXT;
        std::string kind = type;
        if (type == "text" || type == "raw") {
            format = CF_UNICODETEXT;
        } else if (type == "html") {
            format = htmlFormat;
        } else if (type == "png") {
            format = pngFormat;
        } else if (type == "rtf") {
            format = rtfFormat;
        } else {
            return Fail("不支持的剪贴板类型：" + type);
        }
        if (!OpenClipboardRetry()) return Fail(ClipboardOpenFailure());
        std::string out;
        if (HANDLE handle = GetClipboardData(format)) {
            if (void* data = GlobalLock(handle)) {
                const SIZE_T size = GlobalSize(handle);
                if (format == CF_UNICODETEXT) {
                    out = Narrow(static_cast<const wchar_t*>(data));
                } else if (format == htmlFormat) {
                    // CF_HTML：取 <!--StartFragment--> 与 <!--EndFragment--> 之间。
                    const std::string all(static_cast<const char*>(data),
                                          strnlen(static_cast<const char*>(data), size));
                    const size_t begin = all.find("<!--StartFragment-->");
                    const size_t end = all.find("<!--EndFragment-->");
                    out = begin != std::string::npos && end != std::string::npos && end > begin
                              ? all.substr(begin + 20, end - begin - 20)
                              : all;
                } else {
                    // png/rtf 二进制：base64 回传（与 write 端的编码约定对称）。
                    out = Base64Encode(std::string(static_cast<const char*>(data), size));
                }
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        Json result = Json::object();
        result.putString("data", out);
        result.putString("type", kind);
        return Ok(std::move(result));
    }
    if (command == "clipboard.availableTypes") {
        // nw.readAvailableTypes：枚举当前剪贴板上的全部格式，映射成 nw 类型名。
        if (!OpenClipboardRetry()) return Fail(ClipboardOpenFailure());
        Json types = Json::array();
        UINT format = 0;
        while ((format = EnumClipboardFormats(format)) != 0) {
            if (format == CF_UNICODETEXT) types.push(Json::fromText("text"));
            else if (format == htmlFormat) types.push(Json::fromText("html"));
            else if (format == pngFormat) types.push(Json::fromText("png"));
            else if (format == rtfFormat) types.push(Json::fromText("rtf"));
        }
        CloseClipboard();
        return Ok(std::move(types));
    }
    if (command == "clipboard.clear") {
        if (!OpenClipboardRetry()) return Fail(ClipboardOpenFailure());
        EmptyClipboard();
        CloseClipboard();
        return Ok(Json());
    }
    return Fail("clipboard 命令不支持：" + command);
}

std::string Host::ApiShell(NwWindow*, const Json& request) {
    const std::string command = request.text("c");
    const std::wstring argument = Utf8ToWide(request.text("target"));

    if (command == "shell.openExternal") {
        // 外部浏览器/邮件客户端这一类：交给 ShellExecute，成败看返回值 > 32。
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", argument.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) return Fail("ShellExecute 打开失败");
        return Ok(Json());
    }
    if (command == "shell.openItem") {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", argument.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) return Fail("打开文件失败");
        return Ok(Json());
    }
    if (command == "shell.showItemInFolder") {
        std::wstring line = L"/select,\"" + argument + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", line.c_str(), nullptr, SW_SHOWNORMAL);
        return Ok(Json());
    }
    if (command == "shell.beep") {
        MessageBeep(MB_OK);
        return Ok(Json());
    }
    return Fail("shell 命令不支持：" + command);
}

namespace {

struct ScreenEnumContext {
    Json* screens{};
    int index{};
    double scale{1.0};
};

BOOL CALLBACK EnumScreenProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* context = reinterpret_cast<ScreenEnumContext*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        Json entry = Json::object();
        entry.putNumber("id", context->index++);
        entry.putString("name", Narrow(info.szDevice));
        Json bounds = Json::object();
        bounds.putNumber("x", info.rcMonitor.left);
        bounds.putNumber("y", info.rcMonitor.top);
        bounds.putNumber("width", info.rcMonitor.right - info.rcMonitor.left);
        bounds.putNumber("height", info.rcMonitor.bottom - info.rcMonitor.top);
        entry.put("bounds", std::move(bounds));
        Json work = Json::object();
        work.putNumber("x", info.rcWork.left);
        work.putNumber("y", info.rcWork.top);
        work.putNumber("width", info.rcWork.right - info.rcWork.left);
        work.putNumber("height", info.rcWork.bottom - info.rcWork.top);
        entry.put("work_area", std::move(work));
        entry.putNumber("scaleFactor", context->scale);
        entry.putBool("isPrimary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0);
        context->screens->push(std::move(entry));
    }
    return TRUE;
}

float PrimaryScaleFactor() {
    // 不依赖 GetDpiForMonitor/GetDpiForWindow：前者要额外链 shcore，后者要 Win10 SDK。
    // 桌面 HDC 的 LOGPIXELSX 在任何 Windows 上都能拿到，够用。
    HDC screen = GetDC(nullptr);
    if (!screen) return 1.0f;
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

} // namespace

std::string Host::ApiScreen(NwWindow*, const Json&) {
    ScreenEnumContext context;
    Json screens = Json::array();
    context.screens = &screens;
    context.scale = PrimaryScaleFactor();
    EnumDisplayMonitors(nullptr, nullptr, &EnumScreenProc, reinterpret_cast<LPARAM>(&context));
    // 某些无桌面/远程会话中 EnumDisplayMonitors 会返回空，但窗口仍有可用的虚拟桌面。
    // 用系统度量补出主屏，避免 nw.Screen.screens 错误地暴露为空数组。
    if (screens.items.empty()) {
        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (width > 0 && height > 0) {
            Json entry = Json::object();
            entry.putNumber("id", 0);
            entry.putString("name", "DISPLAY1");
            Json bounds = Json::object();
            bounds.putNumber("x", x);
            bounds.putNumber("y", y);
            bounds.putNumber("width", width);
            bounds.putNumber("height", height);
            entry.put("bounds", std::move(bounds));
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            Json workArea = Json::object();
            workArea.putNumber("x", work.left);
            workArea.putNumber("y", work.top);
            workArea.putNumber("width", work.right - work.left);
            workArea.putNumber("height", work.bottom - work.top);
            entry.put("work_area", std::move(workArea));
            entry.putNumber("scaleFactor", context.scale);
            entry.putBool("isPrimary", true);
            screens.push(std::move(entry));
        }
    }
    return Ok(std::move(screens));
}

// ---------------------------------------------------------------------------
// node 桥的 syscall 后端。
//
// 真正的 node 在这套架构里是缺位的：nw.js 把 V8 实例塞进 renderer，本模块没有 renderer
// 可塞，也不该为此拖一个 node 进来。所以 node 这一侧的能力（require / process / Buffer /
// path / fs）由页面的 nw_node.js 实现，凡是需要落到操作系统的地方（文件、环境、进程信息）
// 一律走下面这两个入口回到 native —— 这正是"内核跟 node 之间的桥梁"这句话的落点：
// 桥只暴露原语，不假装自己是 node。
// ---------------------------------------------------------------------------

std::string Host::ApiFs(NwWindow*, const Json& request) {
    const std::string command = request.text("c");
    const std::wstring path = Utf8ToWide(request.text("path"));

    if (command == "fs.exists") {
        Json result = Json::object();
        result.putBool("exists", PathExists(path));
        result.putBool("isDirectory", IsDirectory(path));
        return Ok(std::move(result));
    }
    if (command == "fs.stat") {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        Json result = Json::object();
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            result.putBool("exists", false);
            return Ok(std::move(result));
        }
        const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        FILETIME writes = data.ftLastWriteTime;
        ULARGE_INTEGER ticks{};
        ticks.LowPart = writes.dwLowDateTime;
        ticks.HighPart = writes.dwHighDateTime;
        result.putBool("exists", true);
        result.putBool("isDirectory", (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        result.putBool("isFile", (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
        result.putNumber("size", static_cast<double>(size));
        // FILETIME 的 100ns 起点是 1601-01-01，换算成 Unix 毫秒要减 11644473600 秒。
        result.putNumber("mtimeMs", static_cast<double>(ticks.QuadPart / 10000ULL) - 11644473600000.0);
        result.putBool("readonly", (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0);
        return Ok(std::move(result));
    }
    if (command == "fs.readFile") {
        std::string content;
        if (!ReadTextFile(path, content)) return Fail("读文件失败：" + request.text("path"));
        Json result = Json::object();
        const std::string encoding = request.text("encoding", "utf8");
        // base64 之外一律按 utf8 原样回；二进制文件应该明写 encoding:"base64"。
        result.putString("data", encoding == "base64" ? Base64Encode(content) : content);
        result.putString("encoding", encoding == "base64" ? "base64" : "utf8");
        return Ok(std::move(result));
    }
    if (command == "fs.writeFile" || command == "fs.appendFile") {
        std::string content = request.text("data");
        if (request.text("encoding", "utf8") == "base64") content = Base64Decode(content);
        const bool append = command == "fs.appendFile";
        HANDLE file = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return Fail("写文件失败：" + request.text("path"));
        DWORD written = 0;
        const bool ok = content.empty() || WriteFile(file, content.data(),
                                                     static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(file);
        if (!ok) return Fail("写文件失败（写入阶段）");
        return Ok(Json());
    }
    if (command == "fs.readdir") {
        const std::wstring pattern = path.empty() ? L"*" : JoinPath(path, L"*");
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &data);
        Json entries = Json::array();
        if (search != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..") continue;
                entries.push(Json::fromText(Narrow(name)));
            } while (FindNextFileW(search, &data));
            FindClose(search);
        }
        return Ok(std::move(entries));
    }
    if (command == "fs.mkdir") {
        if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            if (!request.boolean("recursive", true)) return Fail("建目录失败：" + request.text("path"));
            // 逐级创建：CreateDirectory 只建最后一段，父目录不存在时会直接失败，
            // 所以按分隔符一段段往上补（已经存在的段会返回 ERROR_ALREADY_EXISTS，忽略即可）。
            std::wstring current = path;
            std::vector<std::wstring> missing;
            while (!current.empty() && !PathExists(current)) {
                missing.push_back(current);
                const size_t cut = current.find_last_of(L"\\/");
                if (cut == std::wstring::npos || cut <= 2) break;
                current = current.substr(0, cut);
            }
            for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
                CreateDirectoryW(it->c_str(), nullptr);
            }
            if (!IsDirectory(path)) return Fail("建目录失败：" + request.text("path"));
        }
        return Ok(Json());
    }
    if (command == "fs.unlink") {
        if (IsDirectory(path)) {
            if (!RemoveDirectoryW(path.c_str())) return Fail("删目录失败（非空目录需用 fs.rmdir_recursive）");
        } else if (!DeleteFileW(path.c_str())) {
            return Fail("删文件失败：" + request.text("path"));
        }
        return Ok(Json());
    }
    if (command == "fs.rename") {
        const std::wstring to = Utf8ToWide(request.text("to"));
        if (!MoveFileExW(path.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            return Fail("重命名失败");
        }
        return Ok(Json());
    }
    if (command == "fs.realpath") {
        wchar_t buffer[4096]{};
        const DWORD size = GetFullPathNameW(path.c_str(), 4096, buffer, nullptr);
        Json result = Json::object();
        result.putString("path", size > 0 ? Narrow(buffer) : request.text("path"));
        return Ok(std::move(result));
    }
    return Fail("fs 命令不支持：" + command);
}

std::string Host::ApiProcess(NwWindow*, const Json& request) {
    const std::string command = request.text("c");
    if (command == "process.info") {
        wchar_t cwd[4096]{};
        GetCurrentDirectoryW(4096, cwd);
        Json info = Json::object();
        info.putString("cwd", Narrow(cwd));
        info.putString("execPath", Narrow(exePath_));
        info.putNumber("pid", GetCurrentProcessId());
        info.putString("platform", "win32");
        info.putString("arch", sizeof(void*) == 8 ? "x64" : "ia32");
        info.putString("dataPath", Narrow(dataPath_));
        info.putString("appPath", Narrow(appPath_));
        // 环境变量整块给出去：node 的 process.env 就是这个语义，
        // 页面侧的 JS 沙箱自己决定要暴露哪些（比如把 NW_ 开头的过滤掉）。
        std::string env;
        if (LPWCH block = GetEnvironmentStringsW()) {
            for (LPWCH at = block; *at; at += wcslen(at) + 1) {
                env += Narrow(at);
                env.push_back('\n');
            }
            FreeEnvironmentStringsW(block);
        }
        info.putString("env", env);
        std::string versions = "12.0.0";
        info.putString("nodeVersion", versions);
        info.putString("kernel", Narrow(kernel_.path));
        return Ok(std::move(info));
    }
    if (command == "process.chdir") {
        if (!SetCurrentDirectoryW(Utf8ToWide(request.text("path")).c_str())) return Fail("切换目录失败");
        return Ok(Json());
    }
    if (command == "process.exit") {
        Quit(static_cast<int>(request.number("code", 0)));
        return Ok(Json());
    }
    return Fail("process 命令不支持：" + command);
}

std::string Host::ApiShortcut(NwWindow* window, const Json& request) {
    const std::string command = request.text("c");
    NwWindow* target = window;
    if (request.find("windowId")) target = Find(static_cast<int>(request.number("windowId", 0)));
    if (!target) return Fail("窗口不存在");

    if (command == "shortcut.register") {
        UINT modifiers = 0;
        UINT key = 0;
        if (!ParseAccelerator(request.text("key"), modifiers, key)) return Fail("无法识别的按键组合");
        const int id = static_cast<int>(request.number("id", 0));
        if (!id) return Fail("缺少热键 id");
        // MOD_NOREPEAT：按住不放时不要连续触发；nw.Shortcut 的语义也是按一次来一次。
        if (!RegisterHotKey(target->hwnd, id, modifiers | MOD_NOREPEAT, key)) return Fail("注册热键失败（可能已被占用）");
        target->hotkeys[id] = request.text("callback", "__nmbHotkey");
        return Ok(Json());
    }
    if (command == "shortcut.unregister") {
        const int id = static_cast<int>(request.number("id", 0));
        UnregisterHotKey(target->hwnd, id);
        target->hotkeys.erase(id);
        return Ok(Json());
    }
    if (command == "shortcut.unregisterAll") {
        for (const auto& entry : target->hotkeys) UnregisterHotKey(target->hwnd, entry.first);
        target->hotkeys.clear();
        return Ok(Json());
    }
    return Fail("shortcut 命令不支持：" + command);
}

} // namespace nw
} // namespace nmb
