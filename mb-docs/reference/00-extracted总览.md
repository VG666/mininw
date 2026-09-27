# reference/ —— 上游 nw.js 提取原件（只读对照，不参与编译）

- 目录：[reference/](../../reference/)（整理前目录名是 `extracted\`）
- 性质：从 nw.js 上游源码中提取的**参考原件**，带原始版权头。本目录只用来"对照语义、对齐 API 形状"，
  **不要修改、不参与 nw.exe 编译、运行时也不读取**。当前实现（[nw_host.cpp](../../src/nw_host.cpp)、
  [js/api/](../../js/api/)）是对这些原件在 miniblink 上的重新实现，不是拷贝——原件依赖 Chromium base/IDL 编译链，
  这里没有那套东西。

分三组：`cc/`（C++ 原生侧）、`idl/`（接口定义）、`js/`（SDK 页面侧脚本）。

---

## 一、cc/ —— 原生侧 C++ 原件

| 文件 | 内容 | 本项目对应的重新实现 |
|---|---|---|
| [nw_package.h](../../reference/cc/nw_package.h) / [nw_package.cc](../../reference/cc/nw_package.cc) | nw.js 的应用清单：package.json 解析、main 解析为启动 URL、应用目录定位、nodejs/single-instance 等字段 | [nw_package.h](../src/nw_package.h.md) / [nw_package.cpp](../src/nw_package.cpp.md)（base::Value → 自带 [nw_json.h](../src/nw_json.h.md)；去掉 zip .nw 解包） |
| [shell_switches.h](../../reference/cc/shell_switches.h) / [shell_switches.cc](../../reference/cc/shell_switches.cc) | content shell 的命令行开关名（`--nwapp`、`--url`、`--remote-debugging-port` 等） | 散见于 [nw_main.cpp](../src/nw_main.cpp.md) 的参数解析与 [10_api_nw_app.js](../api/10_api_nw_app.js.md) 的过滤正则 |

## 二、idl/ —— 接口定义（声明每个 nw 对象暴露哪些方法/事件/属性）

这些 .idl 是判断"某方法在 nw 里到底同步还是异步、属于静态还是实例、有没有对应事件"的权威依据。
当前实现没有 IDL 代码生成，人工对照其形状：

- [nw_app.idl](../../reference/idl/nw_app.idl) → [10_api_nw_app.js](../api/10_api_nw_app.js.md)
- [nw_window.idl](../../reference/idl/nw_window.idl) → [20_api_nw_window.js](../api/20_api_nw_window.js.md)
- [nw_current_window_internal.idl](../../reference/idl/nw_current_window_internal.idl) → [15_api_nw_internal.js](../api/15_api_nw_internal.js.md)（binding 层）
- [nw_menu.idl](../../reference/idl/nw_menu.idl) → [41_api_nw_menu.js](../api/41_api_nw_menu.js.md)
- [nw_menuitem.idl](../../reference/idl/nw_menuitem.idl) → [40_api_nw_menuitem.js](../api/40_api_nw_menuitem.js.md)
- [nw_clipboard.idl](../../reference/idl/nw_clipboard.idl) → [50_api_nw_clipboard.js](../api/50_api_nw_clipboard.js.md)
- [nw_shell.idl](../../reference/idl/nw_shell.idl) → [51_api_nw_shell.js](../api/51_api_nw_shell.js.md)
- [nw_screen.idl](../../reference/idl/nw_screen.idl) → [52_api_nw_screen.js](../api/52_api_nw_screen.js.md)
- [nw_shortcut.idl](../../reference/idl/nw_shortcut.idl) → [53_api_nw_shortcut.js](../api/53_api_nw_shortcut.js.md)
- [nw_tray.idl](../../reference/idl/nw_tray.idl) → [54_api_nw_tray.js](../api/54_api_nw_tray.js.md)
- [nw_object.idl](../../reference/idl/nw_object.idl)、[nw_test.idl](../../reference/idl/nw_test.idl)：nw 基对象与测试接口（本项目只取其语义参考，无独立模块）。

## 三、js/ —— SDK 页面侧脚本原件

两套并存的历史形态都在：

- **新名（模块化）**：`api_nw_app.js`、`api_nw_window.js`、`api_nw_newwin.js`、`api_window_internal.js`、
  `api_nw_menu.js`、`api_nw_menuitem.js`、`api_nw_clipboard.js`、`api_nw_shell.js`、
  `api_nw_screen.js`、`api_nw_shortcut.js`、`api_nw_tray.js`、`api_nw_object.js`、`api_nw_test.js`；
- **旧名/ browser 侧**：`app.js`、`base.js`、`menu.js`、`menuitem.js`、`clipboard.js`、`shell.js`、
  `screen.js`、`shorcut.js`（上游原拼写）、`tray.js`；
- [nw_pre13_shim.js](../../reference/js/nw_pre13_shim.js)：nw 0.13 之前旧 API 的兼容垫片（本项目不实现旧 API）。

与当前 [js/api/](../../js/api/) 模块的对应关系已在每篇 api 文档开头的"对应 SDK"一行注明；
[00_base.js](../api/00_base.js.md) 对应这里的 `base.js` + bindingUtil 的角色，
[90_install.js](../api/90_install.js.md) 对应 `api_nw_object.js`/装配部分。

## 用法约定

改当前实现的 API 形状前，先在这里查上游原件确认语义（尤其事件名、同步/异步、静态/实例），
但**不要把原件里的 Chromium 专有调用**（`chrome.*`、bindingUtil 编译产物）照抄进来——
本项目页面↔native 只有 [00_base.js](../api/00_base.js.md) 定义的 rpc/rpcAsync/tell 三个口子。
