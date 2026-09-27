# nw_script_stub.cpp —— 内置脚本缺失时的空占位

- 源文件：[nw_script_stub.cpp](../../src/nw_script_stub.cpp)（全文件仅十余行）
- 角色：[nw_script.h](nw_script.h.md) 两个访问函数的**空实现**：`BuiltinApiScript()` 与 `BuiltinNodeScript()` 都返回空串。

## 什么时候编它

- 它是 [embed-js.ps1](../build/embed-js.ps1.md) 生成失败时的回退：[build-nw.bat](../build/build-nw.bat.md) 先尝试生成 [nw_script_generated.cpp](nw_script_generated.cpp.md)，失败（errorlevel 1）才把本文件放进编译列表（构建脚本里的 `SCRIPT_SRC` 切换）；
- 生成成功时本文件**不参与编译**（两个函数定义只在 generated 文件里），不会冲突。

## 后果

空内置副本意味着：exe 旁边必须存在 `api\*.js`（以及未禁 node 时的 `nw_node.js`），否则 `Host::LoadScripts()` 会以"缺少 api\\*.js"启动失败。
