# embed-js.ps1 —— 把页面 JS 烤进 C++ 内置副本

- 源文件：[tools/embed-js.ps1](../../tools/embed-js.ps1)
- 角色：构建期由 [build-nw.bat](build-nw.bat.md) 调用，读 `js\api\*.js` 与 `js\nw_node.js`，
  生成 [nw_script_generated.cpp](../src/nw_script_generated.cpp.md)（磁盘路径 `generated\`）。

## 干了什么事

1. 入参：`-ApiDir`（页面 api 目录，构建时传 `js\api`）、`-NodeJs`（nw_node.js 路径，构建时传 `js\nw_node.js`）、`-Output`（生成的 .cpp，构建时传 `generated\nw_script_generated.cpp`）；
2. `Read-Utf8`：按字节读文件，**剥除 UTF-8 BOM**（否则注入脚本开头多三个杂散字节）；
3. **必须切片**：MSVC 单个字符串字面量超过 **16380 字节会被静默截断（C2026）**——
   截断的脚本表现为"注入了半个文件、语法错、还没人立刻发现"。这里按 **8000 字符**切片，
   每片一个 C++ 原始字面量 `R"NWJS(...)NWJS",`，运行时在生成的函数里把各片 `+=` 拼回整串；
   若脚本原文含字面量终止串 `)NWJS"`，直接报错让你换分隔符；
4. **api 装配与运行时逐字节一致**：`Get-ApiText` 按文件名排序枚举 `*.js`，
   先写 prologue（`(function(){ 'use strict'; if(window.__nmbInstalled)return; window.__nmbInstalled=true;`），
   每个模块前加 `// ---- 文件名 ----`，最后 epilogue `})();`。
   注释明确：这套包装必须与 [nw_host.cpp](../../src/nw_host.cpp) 的 `AssembleApiModules()`
   （kApiPrologue/kApiEpilogue）保持一致，否则"烤进 exe 的"和"exe 旁边磁盘读的"两条路径行为分叉；
5. 输出两个函数 `BuiltinApiScript()`、`BuiltinNodeScript()`（nw_node.js 不存在时为空串），
   用 UTF-8 无 BOM 写盘，CRLF 行尾。

## 工程注意

- **全文件 ASCII only**：PowerShell 5.1 在中文系统上把无 BOM 的 .ps1 当 GBK 读，
  非 ASCII 会被错码到解析失败（"Missing ')' in method call"，文件头注释）。
