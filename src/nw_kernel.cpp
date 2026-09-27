#include "nw_kernel.h"

#include <cstdio>
#include <vector>

#include "nw_package.h"     // Utf8ToWide：错误信息要回到宿主内部用的宽字符

namespace nmb {
namespace nw {

namespace {

bool g_kernelInitialized = false;
// 回调和 RunJsSync 必须从**实际加载的模块**取导出，不能把 DLL 名写死：
// miniblink/mb108/mb132 的文件名不同，但宿主所需的核心 ABI 是同一组 mb* 导出。
HMODULE g_kernelModule = nullptr;

// 内核导出全是 UTF-8 字符串；宿主内部一律用宽字符，所以进出各转一次。
std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
    return out;
}

// 必需导出：取不到就明确失败，不要留半个能跑的宿主。
template <typename T>
bool Take(HMODULE module, const char* name, T& target, std::vector<std::string>& missing, bool required = true) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!target && required) missing.push_back(name);
    return target != nullptr;
}

// 候选目录：显式路径 > NMB_MINIBLINK > exe 目录 > exe 目录\miniblink > exe 目录\..；每个目录优先 miniblink。
std::vector<std::wstring> KernelCandidates(const std::wstring& explicitPath) {
    std::vector<std::wstring> out;
    if (!explicitPath.empty()) {
        out.push_back(explicitPath);
        // 显式值既可以是 DLL，也可以是目录；目录下优先真实 miniblink 内核。
        out.push_back(explicitPath + L"\\miniblink_x64.dll");
        out.push_back(explicitPath + L"\\mb108_x64.dll");
        out.push_back(explicitPath + L"\\mb132_x64.dll");
        return out;
    }
    wchar_t buffer[4096]{};
    const DWORD envSize = GetEnvironmentVariableW(L"NMB_MINIBLINK", buffer, 4096);
    if (envSize > 0 && envSize < 4096) out.push_back(buffer);

    std::wstring dir(MAX_PATH, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, &dir[0], MAX_PATH);
    dir.resize(size);
    const size_t cut = dir.find_last_of(L"\\/");
    dir = cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);

    if (!dir.empty()) {
        out.push_back(dir + L"\\miniblink_x64.dll");
        out.push_back(dir + L"\\mb108_x64.dll");
        out.push_back(dir + L"\\mb132_x64.dll");
        out.push_back(dir + L"\\miniblink\\miniblink_x64.dll");
        out.push_back(dir + L"\\miniblink\\mb108_x64.dll");
        out.push_back(dir + L"\\miniblink\\mb132_x64.dll");
        out.push_back(dir + L"\\..\\..\\NativeMediaBridge\\bin\\miniblink_x64.dll");
        out.push_back(dir + L"\\..\\..\\NativeMediaBridge\\bin\\mb108_x64.dll");
        out.push_back(dir + L"\\..\\..\\NativeMediaBridge\\bin\\mb132_x64.dll");
    }
    out.push_back(L"miniblink_x64.dll"); // 交给系统按 PATH/当前目录找
    out.push_back(L"mb108_x64.dll");
    out.push_back(L"mb132_x64.dll");
    return out;
}

struct SyncRun {
    std::string* result{};
    bool ran{};
    int depth{};
};

void __cdecl OnRunJsDone(WebView, void* param, JsExec es, JsValue value) {
    SyncRun* run = static_cast<SyncRun*>(param);
    run->ran = true;
    if (!run->result) return;
    // 结果从 JsValue 换回字符串；null/undefined 会得到空串，调用方按"没结果"处理。
    if (!es) return;
    if (!g_kernelModule) return;
    using ToString = const char* (__cdecl*)(JsExec, JsValue);
    static ToString toString = nullptr;
    if (!toString) toString = reinterpret_cast<ToString>(GetProcAddress(g_kernelModule, "mbJsToString"));
    if (!toString) return;
    const char* value2 = toString(es, value);
    if (value2) *run->result = value2;
}

// 全部 mb* 导出的绑定清单（LoadKernel 与 BindKernelModule 共用；改这里两处同时生效）。
void BindExports(HMODULE module, KernelApi& out, std::vector<std::string>& missing) {
    Take(module, "mbCreateInitSettings", out.createInitSettings, missing);
    Take(module, "mbSetInitSettings", out.setInitSettings, missing, false);
    Take(module, "mbInit", out.init, missing);
    Take(module, "mbCreateWebView", out.createWebView, missing);
    Take(module, "mbCreateWebWindow", out.createWebWindow, missing);
    Take(module, "mbDestroyWebView", out.destroyWebView, missing);
    Take(module, "mbSetHandle", out.setHandle, missing);
    Take(module, "mbResize", out.resize, missing);
    Take(module, "mbShowWindow", out.showWindow, missing);
    Take(module, "mbLoadURL", out.loadURL, missing);
    Take(module, "mbLoadHtmlWithBaseUrl", out.loadHtmlWithBaseUrl, missing);
    Take(module, "mbRunJs", out.runJs, missing);
    Take(module, "mbJsToString", out.jsToString, missing);
    Take(module, "mbOnJsQuery", out.onJsQuery, missing);
    Take(module, "mbResponseQuery", out.responseQuery, missing);
    Take(module, "mbOnDocumentReady", out.onDocumentReady, missing);
    Take(module, "mbOnDidCreateScriptContext", out.onDidCreateScriptContext, missing);
    Take(module, "mbWake", out.wake, missing);
    Take(module, "mbSetFocus", out.setFocus, missing);
    Take(module, "mbKillFocus", out.killFocus, missing);
    Take(module, "mbWebFrameGetMainFrame", out.webFrameGetMainFrame, missing);
    Take(module, "mbIsMainFrame", out.isMainFrame, missing);
    Take(module, "mbSetDebugConfig", out.setDebugConfig, missing);
    Take(module, "mbSetNodeJsEnable", out.setNodeJsEnable, missing, false);
    Take(module, "mbSetNavigationToNewWindowEnable", out.setNavigationToNewWindowEnable, missing);
    // 同步通道的一条链，缺一环整个 fs.*Sync / nw.Clipboard.get 就没了，所以列为必需。
    Take(module, "mbOnLoadUrlBegin", out.onLoadUrlBegin, missing);
    Take(module, "mbNetSetData", out.netSetData, missing);
    Take(module, "mbNetSetMIMEType", out.netSetMIMEType, missing);

    // 可选导出：缺了只是少一条通知/开关，不影响宿主成立。
    Take(module, "mbSetHandleOffset", out.setHandleOffset, missing, false);
    Take(module, "mbSetAutoDrawToHwnd", out.setAutoDrawToHwnd, missing, false);
    Take(module, "mbSetTransparent", out.setTransparent, missing, false);
    Take(module, "mbSetZoomFactor", out.setZoomFactor, missing, false);
    Take(module, "mbSetUserAgent", out.setUserAgent, missing, false);
    Take(module, "mbSetCspCheckEnable", out.setCspCheckEnable, missing, false);
    Take(module, "mbReload", out.reload, missing, false);
    Take(module, "mbStopLoading", out.stopLoading, missing, false);
    Take(module, "mbGoBack", out.goBack, missing, false);
    Take(module, "mbGoForward", out.goForward, missing, false);
    Take(module, "mbCanGoBack", out.canGoBack, missing, false);
    Take(module, "mbCanGoForward", out.canGoForward, missing, false);
    Take(module, "mbGetUrl", out.getURL, missing, false);
    Take(module, "mbGetTitle", out.getTitle, missing, false);
    Take(module, "mbGetSize", out.getSize, missing, false);
    Take(module, "mbOnURLChanged", out.onURLChanged, missing, false);
    Take(module, "mbOnTitleChanged", out.onTitleChanged, missing, false);
    Take(module, "mbOnConsole", out.onConsole, missing, false);
    Take(module, "mbOnClose", out.onClose, missing, false);
    Take(module, "mbOnCreateView", out.onCreateView, missing, false);
    Take(module, "mbNetSetHTTPHeaderFieldUtf8", out.netSetHTTPHeaderFieldUtf8, missing, false);
    Take(module, "mbNetContinueJob", out.netContinueJob, missing, false);
    // prompt 同步通道：mb108 上 onLoadUrlBegin 不回调 XHR，页面 → native 的同步取数
    // 换成 window.prompt 拦截（见 nw_host.cpp 的 OnPromptBox）。全可选：缺了自动落回 XHR。
    Take(module, "mbOnPromptBox", out.onPromptBox, missing, false);
    Take(module, "mbCreateString", out.createString, missing, false);
    Take(module, "mbDeleteString", out.deleteString, missing, false);
    Take(module, "mbGetString", out.getString, missing, false);
    Take(module, "mbNetHookRequest", out.netHookRequest, missing, false);
}

// 统一的"缺必需导出"报错文案（LoadKernel / BindKernelModule 共用）。
std::wstring MissingExportsError(const std::wstring& dll, const std::vector<std::string>& missing) {
    std::string joined;
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) joined += ", ";
        joined += missing[i];
    }
    return L"kernel " + dll + L" is missing required exports: " + Utf8ToWide(joined);
}

} // namespace

bool KernelInitialized() { return g_kernelInitialized; }

bool LoadKernel(const std::wstring& dllPath, KernelApi& out, std::wstring& error) {
    std::vector<std::string> missing;
    std::vector<std::wstring> candidates = KernelCandidates(dllPath);
    HMODULE module = nullptr;
    std::wstring loadedFrom;
    std::string lastError = "kernel dll not found";
    for (const std::wstring& candidate : candidates) {
        // LOAD_WITH_ALTERED_SEARCH_PATH：内核旁边通常还有它的依赖（icudt、node 等），
        // 必须让系统先在内核自己的目录里找。
        module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module) { loadedFrom = candidate; break; }
        const DWORD code = GetLastError();
        if (code != ERROR_MOD_NOT_FOUND && code != ERROR_FILE_NOT_FOUND) {
            char buffer[256]{};
            snprintf(buffer, sizeof(buffer), " (GetLastError=%lu)", static_cast<unsigned long>(code));
            lastError = Narrow(candidate) + buffer;
        }
    }
    if (!module) {
        error = L"failed to load the miniblink kernel: " + Utf8ToWide(lastError);
        return false;
    }

    out = KernelApi();
    out.module = module;
    out.path = loadedFrom;
    g_kernelModule = module;

    BindExports(module, out, missing);
    if (!missing.empty()) {
        error = MissingExportsError(loadedFrom, missing);
        UnloadKernel(out);
        return false;
    }

    // mbInit 是全局一次的，重复调用会让内核状态错乱。
    if (!g_kernelInitialized) {
        void* settings = out.createInitSettings ? out.createInitSettings() : nullptr;
        if (settings && out.setInitSettings) out.setInitSettings(settings, "enableNodejs", "1");
        out.init(settings);
        g_kernelInitialized = true;
    }
    return true;
}

// 桥模式：显式路径加载 + 全量绑定，但**不 mbInit**——mbInit 由桥的 NMB_Initialize
// 做：宿主把同一个绝对路径交给桥，两边 LoadLibrary 命中同一个模块（refcount 各持
// 一次），全进程只初始化一次内核。成功后 KernelInitialized() 仍为 false。
bool BindKernelModule(const std::wstring& dllPath, KernelApi& out, std::wstring& error) {
    if (dllPath.empty()) {
        error = L"kernel dll path is empty";
        return false;
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH：依赖（icudt 等）先在内核自己的目录里找。
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        wchar_t buffer[64]{};
        swprintf(buffer, 64, L" (GetLastError=%lu)", static_cast<unsigned long>(GetLastError()));
        error = L"failed to load " + dllPath + buffer;
        return false;
    }
    out = KernelApi();
    out.module = module;
    out.path = dllPath;
    g_kernelModule = module;

    std::vector<std::string> missing;
    BindExports(module, out, missing);
    if (!missing.empty()) {
        error = MissingExportsError(dllPath, missing);
        UnloadKernel(out);
        return false;
    }
    return true;
}

void UnloadKernel(KernelApi& api) {
    if (g_kernelModule == api.module) g_kernelModule = nullptr;
    if (api.module) FreeLibrary(api.module);
    api = KernelApi();
}

std::string RunJsSync(WebView view, const char* code) {
    if (!view || !code || !g_kernelModule) return std::string();
    using RunJs = void (__cdecl*)(WebView, Frame, const char*, int, RunJsCallback, void*, void*);
    using MainFrame = Frame (__cdecl*)(WebView);
    static RunJs runJs = nullptr;
    static MainFrame mainFrame = nullptr;
    if (!runJs) runJs = reinterpret_cast<RunJs>(GetProcAddress(g_kernelModule, "mbRunJs"));
    if (!mainFrame) mainFrame = reinterpret_cast<MainFrame>(GetProcAddress(g_kernelModule, "mbWebFrameGetMainFrame"));
    if (!runJs || !mainFrame) return std::string();

    // mbRunJs 在本线程上会立刻把脚本跑完并同步回调（结果只能从回调里拿），
    // 所以这里用栈上的 SyncRun 接住结果即可——不要改成投递到别的线程等信号量，
    // 那样就跑到内核的非 GUI 线程上去了。
    std::string result;
    SyncRun run;
    run.result = &result;
    Frame frame = mainFrame(view);
    runJs(view, frame, code, 0, &OnRunJsDone, &run, nullptr);
    return run.ran ? result : std::string();
}

} // namespace nw
} // namespace nmb
