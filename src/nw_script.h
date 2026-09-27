#pragma once
// 注入脚本的"内置副本"入口。
//
// 运行期优先读 exe 旁边的 nw_api.js / nw_node.js（改脚本不用重编译，调试期很重要）；
// 两个文件都不在时才用这里编进去的副本。内置副本由 tools/embed-js.ps1 在构建时
// 从同目录的 .js 生成到 nw_script_generated.cpp —— 之所以要生成而不是手写，
// 是因为 MSVC 单个字符串字面量超过 16380 字节会被静默截断（C2026），
// 必须把脚本切成多段相邻字面量拼起来。

#include <string>

namespace nmb {
namespace nw {

// nw_api.js 的内置副本；没有内置时返回空串（此时必须能在 exe 旁边找到 .js）。
std::string BuiltinApiScript();
// nw_node.js 的内置副本。
std::string BuiltinNodeScript();

} // namespace nw
} // namespace nmb
