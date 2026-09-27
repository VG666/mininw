#include "nw_bridge.h"

#include <cstdio>
#include <vector>

#include "nw_package.h"     // ExecutableDirectory / JoinPath / PathExists / Utf8ToWide

namespace nmb {
namespace nw {

namespace {

// 必需导出：取不到就明确失败，不留半个能跑的桥。
template <typename T>
bool Take(HMODULE module, const char* name, T& target, std::vector<std::string>& missing) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!target) missing.push_back(name);
    return target != nullptr;
}

// 宿主窗口父链之外的候选都是相对 exe 的：bridge 模式的分发布局就是
// "exe 旁边放 NativeMediaBridge.dll + miniblink_x64.dll"。
std::vector<std::wstring> BridgeCandidates(const std::wstring& explicitPath) {
    std::vector<std::wstring> out;
    const std::wstring base = ExecutableDirectory();
    if (!explicitPath.empty()) {
        out.push_back(explicitPath);
        return out;
    }
    if (!base.empty()) {
        out.push_back(JoinPath(base, L"NativeMediaBridge.dll"));
        out.push_back(JoinPath(base, L"..\\NativeMediaBridge\\bin\\NativeMediaBridge.dll"));
    }
    return out;
}

// 桥模式内核候选：miniblink_x64.dll 为默认和第一优先级；其他真实内核仅作回退。
std::vector<std::wstring> MiniblinkCandidates(const std::wstring& explicitPath) {
    std::vector<std::wstring> out;
    const std::wstring base = ExecutableDirectory();
    if (!explicitPath.empty()) {
        out.push_back(explicitPath);
        return out;
    }
    if (!base.empty()) {
        out.push_back(JoinPath(base, L"miniblink_x64.dll"));
        out.push_back(JoinPath(base, L"miniblink\\miniblink_x64.dll"));
        out.push_back(JoinPath(base, L"mb108_x64.dll"));
        out.push_back(JoinPath(base, L"miniblink\\mb108_x64.dll"));
    }
    return out;
}

} // namespace

std::wstring FindBridgeDll(const std::wstring& explicitPath) {
    for (const std::wstring& candidate : BridgeCandidates(explicitPath)) {
        if (!candidate.empty() && PathExists(candidate)) return candidate;
    }
    return std::wstring();
}

std::wstring FindMiniblinkDll(const std::wstring& explicitPath) {
    for (const std::wstring& candidate : MiniblinkCandidates(explicitPath)) {
        if (!candidate.empty() && PathExists(candidate)) return candidate;
    }
    return std::wstring();
}

bool LoadBridge(const std::wstring& dllPath, BridgeApi& out, std::wstring& error) {
    if (dllPath.empty()) {
        error = L"NativeMediaBridge.dll not found (looked next to the exe, the annotation project, and ..\\NativeMediaBridge\\bin)";
        return false;
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH：桥的依赖（内核 dll 等）先在桥自己的目录里找。
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        wchar_t buffer[64]{};
        swprintf(buffer, 64, L" (GetLastError=%lu)", static_cast<unsigned long>(GetLastError()));
        error = L"failed to load " + dllPath + buffer;
        return false;
    }

    out = BridgeApi();
    out.module = module;
    std::vector<std::string> missing;
    Take(module, "NMB_Initialize", out.initialize, missing);
    Take(module, "NMB_CreateBrowser", out.createBrowser, missing);
    Take(module, "NMB_GetWebView", out.getWebView, missing);
    Take(module, "NMB_SetDebugConfig", out.setDebugConfig, missing);
    Take(module, "NMB_GetWindow", out.getWindow, missing);
    Take(module, "NMB_Resize", out.resize, missing);
    Take(module, "NMB_Show", out.show, missing);
    Take(module, "NMB_DestroyBrowser", out.destroyBrowser, missing);
    Take(module, "NMB_Shutdown", out.shutdown, missing);
    Take(module, "NMB_GetLastError", out.lastError, missing);
    Take(module, "NMB_SetHostHooks", out.setHostHooks, missing);
    // 可选导出：2026-09 引入的媒体 LUB 链式转发。缺了不失败——桥模式其余功能照常，
    // 只是媒体接管断链（宿主启动时会打一行 stderr 提醒）。
    out.mediaLoadUrlBegin = reinterpret_cast<decltype(out.mediaLoadUrlBegin)>(
        GetProcAddress(module, "NMB_MediaLoadUrlBegin"));

    if (!missing.empty()) {
        std::string joined;
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i) joined += ", ";
            joined += missing[i];
        }
        error = L"bridge " + dllPath + L" is missing required exports: " + Utf8ToWide(joined);
        UnloadBridge(out);
        return false;
    }
    return true;
}

void UnloadBridge(BridgeApi& api) {
    if (api.module) FreeLibrary(api.module);
    api = BridgeApi();
}

} // namespace nw
} // namespace nmb
