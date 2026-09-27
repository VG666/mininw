<div align="center">

# 📘 mb 技术文档总索引

**nw 改版项目** —— 用 `mb108_x64.dll`（miniblink）替代 nw.js 自带的 Chromium 大内核，产出小宿主 `bin\nw.exe`

`43 篇逐文件文档` · `一个源文件对应一篇` · `所有符号均锚定实际代码`

</div>

---

## 🧭 怎么用这套文档

| 你的目的 | 入口 |
|---|---|
| 🚀 第一次接触本项目 | 先读 **[00 · 架构总览](00-架构总览.md)**：模块索引（每个模块干什么、内含哪些文件）+ 启动链/通道/路由/线程流程图 |
| 🔍 知道"要干的事"想找文件 | 看下面的 **[一、功能速查表](#一功能速查表按事情找文件)** |
| 📂 想按目录逐篇逛 | 看 **[二、按目录分组](#二按目录分组的全部文档)** |
| 🖥️ 想用浏览器舒服地看 | 双击打开 **[index.html](index.html)**：左侧目录树 + 搜索 + 源码直达（无需联网、无需服务器） |

> 📌 每篇文档顶部都有可点击的"源文件"链接，直达对应的真实代码。

---

## 一、功能速查表（按事情找文件）

| 你想找的事 | 去哪个文件 | 文档 |
|---|---|---|
| 进程入口、命令行参数（`--nwapp` 等） | [nw_main.cpp](../src/nw_main.cpp) | [入口](src/nw_main.cpp.md) |
| 宿主主体：窗口生命周期、消息循环、API 路由 | [nw_host.cpp](../src/nw_host.cpp) | [宿主实现](src/nw_host.cpp.md) |
| 宿主的数据结构与接口声明 | [nw_host.h](../src/nw_host.h) | [宿主头文件](src/nw_host.h.md) |
| **loopback HTTP 同步通道（主同步通道）** | [nw_rpc.cpp](../src/nw_rpc.cpp) / [nw_rpc.h](../src/nw_rpc.h) | [RPC 服务](src/nw_rpc.cpp.md) / [声明](src/nw_rpc.h.md) |
| prompt 同步退路 / XHR 拦截退路 | [nw_host.cpp](../src/nw_host.cpp)（`OnPromptBox` / `OnLoadUrlBegin`） | [宿主实现](src/nw_host.cpp.md) |
| 加载 mb108_x64.dll、取全部 mb* 导出 | [nw_kernel.cpp](../src/nw_kernel.cpp) | [内核加载](src/nw_kernel.cpp.md) |
| mb* 导出函数指针表、回调类型 | [nw_kernel.h](../src/nw_kernel.h) | [内核 ABI](src/nw_kernel.h.md) |
| 读 package.json（manifest 字段、找应用目录） | [nw_package.cpp](../src/nw_package.cpp) | [清单解析](src/nw_package.cpp.md) |
| 自带 JSON 解析/序列化 | [nw_json.h](../src/nw_json.h) | [JSON](src/nw_json.h.md) |
| 注入脚本的内置副本机制 | [nw_script.h](../src/nw_script.h)、[nw_script_generated.cpp](../generated/nw_script_generated.cpp) | [脚本内置](src/nw_script.h.md) |
| 页面通道层（rpc/rpcAsync/tell、同步通道探测） | [js/api/00_base.js](../js/api/00_base.js) | [页面地基](api/00_base.js.md) |
| nw.App（argv/manifest/quit/全局热键） | [js/api/10_api_nw_app.js](../js/api/10_api_nw_app.js) | [App](api/10_api_nw_app.js.md) |
| native 绑定分界层（read/tell） | [js/api/15_api_nw_internal.js](../js/api/15_api_nw_internal.js) | [internal 层](api/15_api_nw_internal.js.md) |
| nw.Window（窗口全部 JS 接口） | [js/api/20_api_nw_window.js](../js/api/20_api_nw_window.js) | [Window](api/20_api_nw_window.js.md) |
| nw.Menu / nw.MenuItem | 40 / 41 两个文件 | [MenuItem](api/40_api_nw_menuitem.js.md) / [Menu](api/41_api_nw_menu.js.md) |
| nw.Clipboard / Shell / Screen / Shortcut / Tray | 50–54 | [Clipboard](api/50_api_nw_clipboard.js.md) · [Shell](api/51_api_nw_shell.js.md) · [Screen](api/52_api_nw_screen.js.md) · [Shortcut](api/53_api_nw_shortcut.js.md) · [Tray](api/54_api_nw_tray.js.md) |
| 装配 window.nw、接管 window.open | [js/api/90_install.js](../js/api/90_install.js) | [装配层](api/90_install.js.md) |
| 页面侧 node 桥（require/Buffer/process/fs…） | [nw_node.js](../js/nw_node.js) | [node 桥](src/nw_node.js.md) |
| 构建 nw.exe | [build-nw.bat](../build-nw.bat) | [构建脚本](build/build-nw.bat.md) |
| 把 JS 烤进 exe | [tools/embed-js.ps1](../tools/embed-js.ps1) | [embed-js](build/embed-js.ps1.md) |
| 21 项端到端冒烟 / 标题回归 / 窗口绑定回归 | tests\*.bat | [测试体系](tests/00-测试总览.md) |
| 上游 nw.js 提取的参考原件（不改、只对照） | reference\ | [参考原件](reference/00-extracted总览.md) |

---

## 二、按目录分组的全部文档

### 1. native 宿主（C++，编译为 `bin\nw.exe`）

| 源文件 | 文档 | 一句话职责 |
|---|---|---|
| [nw_main.cpp](../src/nw_main.cpp) | [文档](src/nw_main.cpp.md) | 进程入口：参数解析 → 起宿主 → 消息循环 → 收尾 |
| [nw_host.h](../src/nw_host.h) | [文档](src/nw_host.h.md) | `Host` 单例、`NwWindow`、`HostOptions`、`Channel` 声明 |
| [nw_host.cpp](../src/nw_host.cpp) | [文档](src/nw_host.cpp.md) | 宿主全部实现：窗口/菜单/托盘/剪贴板/文件等 10 组 API 与命令路由 |
| [nw_kernel.h](../src/nw_kernel.h) | [文档](src/nw_kernel.h.md) | miniblink ABI：`KernelApi` 函数指针表与回调签名 |
| [nw_kernel.cpp](../src/nw_kernel.cpp) | [文档](src/nw_kernel.cpp.md) | 定位并 LoadLibrary 内核、取导出、`mbInit`、同步取脚本结果 |
| [nw_rpc.h](../src/nw_rpc.h) | [文档](src/nw_rpc.h.md) | `RpcServer` 单例声明与安全/协议模型说明 |
| [nw_rpc.cpp](../src/nw_rpc.cpp) | [文档](src/nw_rpc.cpp.md) | 127.0.0.1 loopback HTTP RPC 完整实现（主同步通道） |
| [nw_json.h](../src/nw_json.h) | [文档](src/nw_json.h.md) | header-only 极简 JSON（解析+序列化，容忍注释/BOM） |
| [nw_package.h](../src/nw_package.h) | [文档](src/nw_package.h.md) | `Manifest` 结构与路径/编码工具函数声明 |
| [nw_package.cpp](../src/nw_package.cpp) | [文档](src/nw_package.cpp.md) | package.json 解析、应用目录自动定位、路径规范化 |
| [nw_script.h](../src/nw_script.h) | [文档](src/nw_script.h.md) | 内置注入脚本两个访问函数的声明 |
| [nw_script_generated.cpp](../generated/nw_script_generated.cpp) | [文档](src/nw_script_generated.cpp.md) | embed-js.ps1 生成的 JS 内置副本（勿手改） |
| [nw_script_stub.cpp](../src/nw_script_stub.cpp) | [文档](src/nw_script_stub.cpp.md) | 内置副本缺失时的空实现占位 |

### 2. 页面注入脚本（JS，随每个窗口注入）

| 源文件 | 文档 | 一句话职责 |
|---|---|---|
| [js/api/00_base.js](../js/api/00_base.js) | [文档](api/00_base.js.md) | 三条同步通道探测 + rpc/rpcAsync/tell + Emitter + 编码工具 |
| [js/api/10_api_nw_app.js](../js/api/10_api_nw_app.js) | [文档](api/10_api_nw_app.js.md) | nw.App：manifest/argv/dataPath/quit/代理空实现/全局热键 |
| [js/api/15_api_nw_internal.js](../js/api/15_api_nw_internal.js) | [文档](api/15_api_nw_internal.js.md) | 对应 SDK 的 binding 层：internalRead/ReadSafe/Tell |
| [js/api/20_api_nw_window.js](../js/api/20_api_nw_window.js) | [文档](api/20_api_nw_window.js.md) | nw.Window 与 WindowState：全部窗口方法、open 排队结算 |
| [js/api/40_api_nw_menuitem.js](../js/api/40_api_nw_menuitem.js) | [文档](api/40_api_nw_menuitem.js.md) | MenuItem 构造器与菜单树 JSON 序列化（分配回调 id） |
| [js/api/41_api_nw_menu.js](../js/api/41_api_nw_menu.js) | [文档](api/41_api_nw_menu.js.md) | Menu（menubar/popup）、点击回调入口 |
| [js/api/50_api_nw_clipboard.js](../js/api/50_api_nw_clipboard.js) | [文档](api/50_api_nw_clipboard.js.md) | 剪贴板读写（text/html/png/rtf，base64 约定） |
| [js/api/51_api_nw_shell.js](../js/api/51_api_nw_shell.js) | [文档](api/51_api_nw_shell.js.md) | openExternal/openItem/showItemInFolder/beep |
| [js/api/52_api_nw_screen.js](../js/api/52_api_nw_screen.js) | [文档](api/52_api_nw_screen.js.md) | 显示器枚举缓存与 displayBoundsChanged |
| [js/api/53_api_nw_shortcut.js](../js/api/53_api_nw_shortcut.js) | [文档](api/53_api_nw_shortcut.js.md) | 全局热键注册/注销与回调注册表 |
| [js/api/54_api_nw_tray.js](../js/api/54_api_nw_tray.js) | [文档](api/54_api_nw_tray.js.md) | 托盘图标：异步创建、图标排队、右键菜单 |
| [js/api/90_install.js](../js/api/90_install.js) | [文档](api/90_install.js.md) | 缝出 window.nw、接 native 事件、window.open 接管 |
| [nw_node.js](../js/nw_node.js) | [文档](src/nw_node.js.md) | 纯 JS CommonJS 运行时（require/Buffer/fs/path/process…） |
| [nw_api.js](../archive/nw_api.js) | [文档](src/nw_api.js.md) | **历史旧版**单体页面脚本，已被 js\api\ 拆分取代（当前不参与运行） |

### 3. 构建与工具

| 源文件 | 文档 | 一句话职责 |
|---|---|---|
| [build-nw.bat](../build-nw.bat) | [文档](build/build-nw.bat.md) | zig 编译 nw.exe、生成内置脚本、拷贝 api JS 与内核 dll |
| [tools/embed-js.ps1](../tools/embed-js.ps1) | [文档](build/embed-js.ps1.md) | 把 js\api\*.js + js\nw_node.js 切片成 C++ 字符串数组 |
| tools/build-doc-index.ps1 | 无单篇（见脚本头注释） | 把 mb-docs 全部 .md 打包成离线单页 [index.html](index.html)；文档增删改后重跑 |
| [archive/split_api.py](../archive/split_api.py) | [文档](build/split_api.py.md) | 一次性重构脚本：把旧 nw_api.js 拆成 js\api\ 多模块 |

### 4. 测试（tests\）

- [00 · 测试总览](tests/00-测试总览.md) 先看这篇，再看各脚本文档：

| 类型 | 文档 |
|---|---|
| 🏁 驱动脚本 | [run-smoke.bat](tests/run-smoke.bat.md) · [run-title.bat](tests/run-title.bat.md) · [run-window.bat](tests/run-window.bat.md) · [window_probe.ps1](tests/window_probe.ps1.md) · [run-fsapp.bat](tests/run-fsapp.bat.md) · [run-fsapp-auto.bat](tests/run-fsapp-auto.bat.md) |
| 🧪 被测应用 | [smoke 应用](tests/app-smoke.md) · [title 应用](tests/app-title.md) · [fsapp 应用](tests/app-fsapp.md) |

### 5. 上游参考原件（reference\，只读对照，不参与编译）

- [00 · reference 总览](reference/00-extracted总览.md)：cc（C++ 原件）/ idl（接口定义）/ js（SDK 页面脚本）三组对照说明。

---

## 三、想搞懂架构怎么跑起来？

直接进 **[00 · 架构总览（模块索引 + 流程图）](00-架构总览.md)**：

1. 上半部分是**模块索引**：九个模块各自的职责与边界，以及模块内每个文件的清单（一句话说明）；
2. 下半部分是**五张流程图**：进程启动链、页面↔native 四条通道、命令路由、线程模型、一次窗口操作往返；
3. 想挖细节时，点每篇文档顶部的"源文件"链接直达真实代码。
