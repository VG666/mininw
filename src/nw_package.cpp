#include "nw_package.h"

#include <windows.h>

#include <cstdio>

namespace nmb {
namespace nw {

namespace {

std::string EnvironmentText(const char* name) {
    char buffer[2048]{};
    const DWORD size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return size > 0 && size < sizeof(buffer) ? std::string(buffer, size) : std::string();
}

// 路径里的 '.' / '..' 就地折叠：manifest 里的 main 经常写 "./index.html"。
//
// 三种"根"必须分开对待，它们的根**本身**已经以分隔符结尾（或不以盘号结尾），
// 所以拼分段时不能再拿"结尾是不是盘号"当判据——踩过的坑：
//   "C:\Users\x" 里的根是 "C:"，若按"结尾是 ':' 就不加分隔符"处理，
//   结果成了 **盘相对路径** "C:Users\x"，落到 C: 的当前目录上（dataPath 就这么坏的）。
std::wstring NormalizePath(const std::wstring& path) {
    std::wstring text = path;
    for (wchar_t& c : text) {
        if (c == L'/') c = L'\\';
    }
    std::wstring out;
    size_t start = 0;
    if (text.size() >= 2 && text[1] == L':') {                    // 盘绝对：C:\...（根 "C:"）
        out = text.substr(0, 2);
        start = 2;
    } else if (text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\') {   // UNC：\\srv\shr
        out = L"\\\\";
        start = 2;
    } else if (!text.empty() && text[0] == L'\\') {                // 单反斜杠根：\foo
        out = L"\\";
        start = 1;
    }
    std::vector<std::wstring> parts;
    while (start <= text.size()) {
        const size_t cut = text.find(L'\\', start);
        const std::wstring part = text.substr(start, cut == std::wstring::npos ? std::wstring::npos : cut - start);
        if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        if (cut == std::wstring::npos) break;
        start = cut + 1;
    }
    for (const std::wstring& part : parts) {
        // 唯一的判据是"根/上一段末尾有没有分隔符"：UNC 的 "\\" 和单根 "\" 已经带着，
        // 盘号 "C:" 没有，所以这里会补上。
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        out += part;
    }
    return out.empty() ? L"." : out;
}

// file:// 里的不安全字符按 UTF-8 百分号编码：mbLoadURL 只吃 UTF-8 字符串，
// 而应用目录里带空格或中文（本机就是 F:\编程\...）时不编码会加载失败。
std::string PercentEncodePath(const std::wstring& path) {
    const std::string utf8 = WideToUtf8(path);
    std::string out;
    char buffer[8]{};
    for (unsigned char c : utf8) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':' || c == '\\';
        if (safe) {
            out.push_back(c == '\\' ? '/' : static_cast<char>(c));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}

bool HasScheme(const std::string& text) {
    const size_t at = text.find("://");
    if (at == std::string::npos || at == 0) return false;
    if (text == "nw:blank") return true;
    for (size_t i = 0; i < at; ++i) {
        const char c = text[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
        if (!ok) return false;
    }
    return true;
}

// manifest 的 main 可能是 "index.html"、"./app/main.html"，也可能是完整 URL。
std::string ResolveMain(const std::wstring& appPath, const std::string& main) {
    if (main.empty()) return "nw:blank";
    if (HasScheme(main) && main.find("://") != std::string::npos) return main;
    return ToFileUrl(JoinPath(appPath, Utf8ToWide(main)));
}

// 拆 "-a=1 -b 'c d'"：nw.js 用 base::StringTokenizer + quote '\''。
std::vector<std::string> Tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (char c : text) {
        if (c == '\'') { quoted = !quoted; continue; }
        if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            if (!current.empty()) { out.push_back(current); current.clear(); }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

// nw.js 的 ReadChromiumArgs：NW_PRE_ARGS 在前、manifest 的 chromium-args 在后，
// 只有开关形式（-x/--x）会被采纳，裸参数丢掉（那是给 Chromium 的）。
std::string CollectChromiumArgs(const Json& root) {
    const std::string fromEnv = EnvironmentText("NW_PRE_ARGS");
    const std::string fromManifest = root.text("chromium-args");
    if (fromEnv.empty() && fromManifest.empty()) return std::string();
    std::string joined = fromEnv;
    if (!joined.empty() && !fromManifest.empty()) joined += " ";
    joined += fromManifest;
    std::string kept;
    for (const std::string& token : Tokenize(joined)) {
        if (token.size() < 2 || token[0] != '-') continue;
        if (!kept.empty()) kept += " ";
        kept += token;
    }
    return kept;
}

} // namespace

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf) {
    if (leaf.empty()) return NormalizePath(base);
    // 绝对路径直接覆盖：main 允许写成 "C:\..." 或 "\\server\share"。
    if (leaf.size() >= 2 && leaf[1] == L':') return NormalizePath(leaf);
    if (base.empty()) return NormalizePath(leaf);
    return NormalizePath(base + L"\\" + leaf);
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 64 * 1024 * 1024) { CloseHandle(file); return false; }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(file, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    out.resize(read);
    // 去掉 UTF-8 BOM：package.json 常带，留着会让 JSON 解析在第一个字符就失败。
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

std::wstring ExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (size == 0) return std::wstring();
        if (size < buffer.size()) { buffer.resize(size); return buffer; }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ExecutableDirectory() {
    const std::wstring path = ExecutablePath();
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

std::string ToFileUrl(const std::wstring& path) {
    std::wstring absolute = path;
    const bool driveAbsolute = absolute.size() >= 2 && absolute[1] == L':';
    const bool uncAbsolute = absolute.size() >= 2 && absolute[0] == L'\\' && absolute[1] == L'\\';
    if (!driveAbsolute && !uncAbsolute) {
        std::wstring current(MAX_PATH, L'\0');
        const DWORD size = GetCurrentDirectoryW(static_cast<DWORD>(current.size()), &current[0]);
        current.resize(size);
        absolute = JoinPath(current, path);
    }
    absolute = NormalizePath(absolute);
    if (uncAbsolute) return "file:" + PercentEncodePath(absolute);
    return "file:///" + PercentEncodePath(absolute);
}

bool LoadManifest(const std::wstring& path, Manifest& out) {
    out = Manifest();
    if (path.empty()) {
        out.error = "no application directory specified";
        return false;
    }
    std::wstring target = path;
    if (IsDirectory(target)) {
        out.appPath = NormalizePath(target);
        out.manifestPath = JoinPath(target, L"package.json");
        if (!PathExists(out.manifestPath)) out.manifestPath = JoinPath(target, L"manifest.json");
    } else if (PathExists(target)) {
        // 直接给了 manifest 文件：nw.js 在这里会去解 .nw 包，本实现只接受 json。
        const size_t cut = target.find_last_of(L"\\/");
        out.appPath = cut == std::wstring::npos ? std::wstring() : target.substr(0, cut);
        out.manifestPath = NormalizePath(target);
    } else {
        out.error = "application directory does not exist: " + WideToUtf8(target);
        return false;
    }
    if (!PathExists(out.manifestPath)) {
        out.error = "no package.json / manifest.json in that directory";
        return false;
    }
    std::string text;
    if (!ReadTextFile(out.manifestPath, text)) {
        out.error = "failed to read package.json";
        return false;
    }
    if (!Json::parse(text, out.root) || !out.root.isObject()) {
        out.error = "package.json is not a valid JSON object";
        return false;
    }
    out.source = text;

    // nw.js 把 name 当必需字段、缺了整包作废。这里放宽成默认 "nwjs"：
    // 只有 {"main":"index.html"} 的目录也应该能跑，没必要为此拒开。
    out.name = out.root.text("name", "nwjs");
    out.version = out.root.text("version");
    out.main = out.root.text("main");
    out.useNode = out.root.boolean("nodejs", true);
    out.singleInstance = out.root.boolean("single-instance", false);
    out.disableDevTools = out.root.boolean("disable-devtools", false);
    out.jsFlags = out.root.text("js-flags");
    out.userAgent = out.root.text("user-agent");
    out.chromiumArgs = CollectChromiumArgs(out.root);

    // nw.js 强行保证 window 段存在，缺省是 {"position":"center"}。
    const Json* window = out.root.find("window");
    out.window = window && window->isObject() ? *window : Json::object();
    if (!out.window.find("position")) out.window.putString("position", "center");

    // --url 优先于 manifest 的 main（nw.js 的 Package::GetStartupURL）。
    std::string urlOverride;
    for (const std::string& token : Tokenize(out.chromiumArgs)) {
        const std::string key = "--url=";
        if (token.compare(0, key.size(), key) == 0) urlOverride = token.substr(key.size());
    }
    if (!urlOverride.empty()) {
        out.startupUrl = HasScheme(urlOverride) ? urlOverride : ("http://" + urlOverride);
    } else {
        out.startupUrl = ResolveMain(out.appPath, out.main);
    }
    out.ok = true;
    return true;
}

bool AutoLocateApp(const std::vector<std::wstring>& args, std::wstring& appPath) {
    const std::wstring exeDir = ExecutableDirectory();
    // 1) exe 所在目录本身就是一个应用（nw.js 的 GetSelfPath().DirName()）。
    if (IsDirectory(exeDir) && (PathExists(JoinPath(exeDir, L"package.json")) || PathExists(JoinPath(exeDir, L"manifest.json")))) {
        appPath = exeDir;
        return true;
    }
    // 2) exe 旁边的 package.nw 目录。
    const std::wstring packed = JoinPath(exeDir, L"package.nw");
    if (IsDirectory(packed)) {
        appPath = packed;
        return true;
    }
    // 3) --nwapp=<目录>。
    for (const std::wstring& arg : args) {
        const std::wstring prefix = L"--nwapp=";
        if (arg.compare(0, prefix.size(), prefix) == 0) {
            appPath = arg.substr(prefix.size());
            return !appPath.empty();
        }
    }
    // 4) 第一个不带开关前缀的参数。
    for (const std::wstring& arg : args) {
        if (arg.empty() || arg[0] == L'-' || arg[0] == L'/') continue;
        appPath = arg;
        return true;
    }
    return false;
}

} // namespace nw
} // namespace nmb
