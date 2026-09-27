# nw_script.h —— 内置注入脚本的访问口（声明）

- 源文件：[nw_script.h](../../src/nw_script.h)
- 角色：只声明两个函数，返回"烤进 exe 的 JS 内置副本"：
  - `std::string BuiltinApiScript();`（页面 nw.* 层，即 api\*.js 拼接体）
  - `std::string BuiltinNodeScript();`（node 桥 nw_node.js）

## 副本机制（为什么需要）

- 运行期 `Host::LoadScripts()`（[nw_host.cpp](nw_host.cpp.md)）**优先读 exe 旁边的 `api\*.js` 与 `nw_node.js`**——开发期改脚本不用重编译；
- 磁盘上读不到时才用这里的内置副本，保证单文件分发也能跑；
- 内置副本由 [tools/embed-js.ps1](../build/embed-js.ps1.md) 在每次构建时重新生成到
  [nw_script_generated.cpp](nw_script_generated.cpp.md)；
- 之所以"生成"而非手写字符串：MSVC 单个字符串字面量超过 16380 字节会被静默截断（C2026），
  必须切成多段相邻原始字面量拼接（脚本总量数十 KB）；
- 尚未生成过时，编译的是 [nw_script_stub.cpp](nw_script_stub.cpp.md) 的空实现（这种情况下 exe 旁边必须有 JS）。
