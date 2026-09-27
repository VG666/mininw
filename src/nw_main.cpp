// nw.exe —— 只做三件事：解析命令行、起宿主、进消息循环。
//
// 对比 nw.js：那边这里是 content::BrowserMainRunner + nw::NWAppMainDelegate，
// 几百行才把 Chromium 拉起来；本实现把"浏览器"整体外包给 miniblink，
// 所以入口只剩下参数解析和宿主启动。
//
// 两个不需要内核的子命令在起宿主之前处理掉：
//   nw init <name>   在当前目录脚手架一个新项目（名称直接采用，不再追问）
//   不带应用启动     进内置启动页（splash），而不是报错退出

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nw_host.h"

namespace {

// --nw-no-dialog：启动失败时只把原因写 stderr，不弹模态框。
// 自动化（跑冒烟测试、CI）必须要有这个开关——模态框会把进程挂死在那儿等点击。
bool g_noDialog = false;

// 命令行拆成宽字符串数组。nw.js 的 nw.exe 参数规则是：
// 除了 --nwapp=<dir> 和第一个非开关参数（应用目录），其余原样透给 app 的 process.argv。
std::vector<std::wstring> SplitCommandLine() {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> out;
    if (!argv) return out;
    for (int i = 1; i < count; ++i) out.push_back(argv[i]);   // 跳过 exe 自己
    LocalFree(argv);
    return out;
}

void ShowError(const std::wstring& text) {
    OutputDebugStringW((text + L"\n").c_str());
    // stderr 也留一份：被脚本拉起时控制台/管道里能直接看到原因，
    // 不用去翻 DebugView，也不用点掉对话框才知道错在哪。
    // 文案一律英文 ASCII：cmd 按 OEM 代码页读字节，UTF-8 中文会变乱码。
    const std::string narrow = nmb::nw::WideToUtf8(text);
    std::fputs("[nw] startup failed: ", stderr);
    std::fputs(narrow.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (!g_noDialog) MessageBoxW(nullptr, text.c_str(), L"nw bridge startup error", MB_ICONERROR | MB_OK);
}

// ---- nw init <name> -------------------------------------------------------

bool IsValidProjectName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L"..") return false;
    // 只允许单层目录名（不准带子路径），并挡掉文件系统非法字符。
    static const wchar_t* kBad = L"\\/:*?\"<>|";
    if (name.find_first_of(kBad) != std::string::npos) return false;
    for (wchar_t c : name) if (c < 0x20) return false;
    return true;
}

// 名字来自命令行，进 JSON / HTML 文本前做最小转义。
std::string EscapeJson(std::string value) {
    std::string out;
    for (char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\r' || c == '\n') out += (c == '\r' ? "\\r" : "\\n");
        else out.push_back(c);
    }
    return out;
}

std::string EscapeHtml(std::string value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

bool WriteUtf8File(const std::wstring& path, const std::string& content) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // UTF-8 BOM：让页面/编辑器一眼认出编码，项目名允许非 ASCII。
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool ok = WriteFile(file, bom, 3, &written, nullptr) && written == 3;
    if (ok && !content.empty()) {
        ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
             written == content.size();
    }
    CloseHandle(file);
    return ok;
}

// 返回 0 成功；非 0 时 error 已填英文原因。
int RunInit(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        std::fputs("usage: nw init <project-name>\n", stderr);
        return 1;
    }
    const std::wstring name = args[1];
    if (!IsValidProjectName(name)) {
        std::fputs("[nw] init: invalid project name "
                   "(use one path segment, no \\ / : * ? \" < > |)\n", stderr);
        return 1;
    }

    wchar_t cwdBuffer[MAX_PATH]{};
    DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwdBuffer);
    if (cwdLen == 0 || cwdLen >= MAX_PATH) {
        std::fputs("[nw] init: cannot resolve the current directory\n", stderr);
        return 1;
    }
    const std::wstring target = nmb::nw::JoinPath(cwdBuffer, name);
    if (nmb::nw::PathExists(target)) {
        std::fputs("[nw] init: that path already exists (pick a name that does not exist)\n", stderr);
        return 1;
    }
    if (!CreateDirectoryW(target.c_str(), nullptr)) {
        std::fputs("[nw] init: failed to create the project directory (GetLastError=", stderr);
        std::fprintf(stderr, "%lu", static_cast<unsigned long>(GetLastError()));
        std::fputs(")\n", stderr);
        return 1;
    }

    const std::string utf8Name = nmb::nw::WideToUtf8(name);
    const std::string safeName = EscapeJson(utf8Name);

    const std::string packageJson =
        "{\n"
        "  \"name\": \"" + safeName + "\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"main\": \"index.html\",\n"
        "  \"window\": {\n"
        "    \"title\": \"" + safeName + "\",\n"
        "    \"width\": 800,\n"
        "    \"height\": 600\n"
        "  }\n"
        "}\n";
    const std::string indexHtml =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>" + EscapeHtml(utf8Name) + "</title>\n"
        "<style>\n"
        "  body { font-family: 'Segoe UI', Arial, sans-serif; margin: 48px; }\n"
        "  h1 { font-weight: 600; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>" + EscapeHtml(utf8Name) + "</h1>\n"
        "<p>This project was scaffolded by <code>nw init</code>. Edit index.html to start building.</p>\n"
        "<script>console.log('nw app ready: nw.App and nw.Window are available');</script>\n"
        "</body>\n"
        "</html>\n";

    if (!WriteUtf8File(nmb::nw::JoinPath(target, L"package.json"), packageJson) ||
        !WriteUtf8File(nmb::nw::JoinPath(target, L"index.html"), indexHtml)) {
        std::fputs("[nw] init: failed to write package.json / index.html\n", stderr);
        return 1;
    }

    // 名称自动确认：不弹任何交互，直接落成并给出运行方式。
    // 路径/名字可能含非 ASCII：真控制台走 WriteConsoleW，重定向走 UTF-8 字节。
    auto emitOut = [](const std::string& utf8) {
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
            const std::wstring wide = nmb::nw::Utf8ToWide(utf8);
            DWORD written = 0;
            if (!wide.empty()) WriteConsoleW(handle, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr);
        } else {
            std::fputs(utf8.c_str(), stdout);
        }
        std::fflush(stdout);
    };
    const std::string utf8Target = nmb::nw::WideToUtf8(target);
    emitOut("[nw] project '" + utf8Name + "' created at:\n");
    emitOut("  " + utf8Target + "\n");
    emitOut("[nw] run it with: nw.exe \"" + utf8Target + "\"\n");
    return 0;
}

} // namespace

// 用普通 main 而不是 wmain：wmain 需要 -municode 换启动入口，
// 而参数本来就从 GetCommandLineW 解析，没必要为入口点多担一个编译开关。
int main() {
    // DPI 感知：nw.js 默认按系统缩放，本实现显式声明，免得高分屏上界面发虚。
    // 用老的 SetProcessDPIAware 而不是 SetProcessDpiAwarenessContext：后者要 Win10 头文件，
    // 而这里只求"别被系统拉伸"，老接口完全够。
    SetProcessDPIAware();

    nmb::nw::HostOptions options;
    options.args = SplitCommandLine();

    // 子命令：不需要窗口/内核，处理完直接退出。
    if (!options.args.empty() && options.args[0] == L"init") {
        return RunInit(options.args);
    }

    for (size_t i = 0; i < options.args.size(); ++i) {
        const std::wstring& arg = options.args[i];
        if (arg.rfind(L"--nwapp=", 0) == 0) {
            options.appPath = arg.substr(8);
        } else if (arg.rfind(L"--nw-kernel=", 0) == 0) {
            options.kernelPath = arg.substr(12);
        } else if (arg.rfind(L"--nw-bridge=", 0) == 0) {
            options.bridgePath = arg.substr(12);
        } else if (arg == L"--nw-no-bridge") {
            options.noBridge = true;
        } else if (arg == L"--nw-no-node") {
            options.disableNode = true;
        } else if (arg == L"--nw-no-dialog") {
            g_noDialog = true;
        }
    }

    // nodeblink 会在宿主拿到控制权前读取进程命令行，并把 argv[1] 当入口模块。
    // 自动化可通过环境变量传宿主参数，使真实命令行只含 nw.exe，避免窗口闪退。
    wchar_t envApp[32768]{};
    const DWORD envAppSize = GetEnvironmentVariableW(L"NMB_NWAPP", envApp, 32768);
    if (options.appPath.empty() && envAppSize > 0 && envAppSize < 32768) options.appPath.assign(envApp, envAppSize);
    if (GetEnvironmentVariableW(L"NMB_NW_NO_DIALOG", envApp, 32768) > 0) g_noDialog = true;
    if (GetEnvironmentVariableW(L"NMB_NO_NODEJS", envApp, 32768) > 0) options.disableNode = true;

    // 应用目录解析权统一交给 AutoLocateApp（exe 旁 package.json / package.nw /
    // --nwapp / 第一个非开关参数，顺序与 nw.js 一致）。什么都没找到时不报错，
    // 进内置启动页。
    std::wstring located;
    if (options.appPath.empty() && !nmb::nw::AutoLocateApp(options.args, located)) {
        options.splashMode = true;
    } else if (options.appPath.empty()) {
        options.appPath = located;
    }

    nmb::nw::Host& host = nmb::nw::Host::Instance();
    std::wstring error;
    if (!host.Start(GetModuleHandleW(nullptr), options, error)) {
        ShowError(error);
        return 1;
    }

    if (!host.OpenWindow(std::string(), nullptr)) {
        ShowError(L"failed to open the main window");
        return 2;
    }
    int exitCode = host.Run();
    host.Shutdown();
    return exitCode;
}
