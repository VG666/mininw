#pragma once
// 应用清单（package.json）—— 移植自 nw.js 的 src/nw_package.{h,cc}（原件见 extracted/cc/）。
//
// 移植时保留了 nw.js 的判定顺序与字段语义，去掉的只有 Chromium 依赖：
//   * base::Value / JSONFileValueDeserializer -> 自带的 nw_json.h
//   * base::FilePath / PathService               -> Win32 路径 + 自己拼
//   * zip::Unzip（.nw 打包）                     -> 没做，只认目录形式的应用
//   * CommandLine::AppendSwitch                  -> 记进 Manifest::chromiumArgs 字符串，
//                                                   由宿主/NMB_NwStart 决定要不要认
//
// 字段名和 nw.js 一致（main / name / window / chromium-args / js-flags / single-instance ...），
// 这样原本给 nw.js 写的项目不用改 package.json 就能被本模块跑起来。

#include <string>
#include <vector>

#include "nw_json.h"

namespace nmb {
namespace nw {

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);
std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf);
bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool ReadTextFile(const std::wstring& path, std::string& out);
std::wstring ExecutablePath();
std::wstring ExecutableDirectory();
// 把 "C:\a\b\index.html" 变成 "file:///C:/a/b/index.html"（nw.js 的 RelativePathToURI 等价物）。
// 带 scheme 的输入（http://、file://）原样返回。
std::string ToFileUrl(const std::wstring& path);

struct Manifest {
    bool ok = false;
    std::string error;                     // ok=false 时的原因（nw.js 是弹错误页，这里只记下来）
    std::wstring appPath;                  // 应用根目录
    std::wstring manifestPath;             // package.json 全路径
    std::string name = "nwjs";
    std::string version;
    std::string main = "nw:blank";
    std::string startupUrl = "nw:blank";   // main 解析后的地址（相对路径已转 file://）
    std::string chromiumArgs;              // manifest 的 chromium-args + NW_PRE_ARGS
    std::string jsFlags;
    std::string userAgent;
    bool useNode = true;
    bool singleInstance = false;
    bool disableDevTools = false;
    Json root;                             // 原始 manifest（给 nw.App.manifest）
    Json window;                           // window 配置；缺省时补 {"position":"center"}
    std::string source;                    // package.json 原文，直接透给页面
};

// 定位并读取应用目录下的 package.json。path 为空时按 nw.js 的顺序自动找：
//   1) exe 自身（.nw 包，本实现直接跳过）  2) exe 所在目录
//   3) exe 目录\package.nw                 4) --nwapp=<dir>  5) 命令行第一个参数
bool LoadManifest(const std::wstring& path, Manifest& out);
bool AutoLocateApp(const std::vector<std::wstring>& args, std::wstring& appPath);

} // namespace nw
} // namespace nmb
