# nw_host.h —— 宿主的接口与数据结构声明

- 源文件：[nw_host.h](../../src/nw_host.h)
- 角色：声明全局宿主 `Host` 单例、一个窗口的全部状态 `NwWindow`、启动参数 `HostOptions`、通道枚举 `Host::Channel`、托盘表 `TrayEntry`。实现见 [nw_host.cpp](nw_host.cpp.md)。

## 关键结构

### `struct HostOptions`
启动入参：`appPath`（空=自动找）、`args`（不含 exe 名的命令行）、`kernelPath`（空=LoadKernel 自己找）、`disableNode`。

### `struct NwWindow`
**一个 nw 窗口 = 一个 Win32 框架窗口（HWND）+ 一个内核视图（WebView）+ 一份 nw.Window 状态。**
字段即能力清单：
- 身份/句柄：`id`、`hwnd`、`view`、`opener`、`ownsView`；
- 窗口形态：`frameless`、`fullscreen`、`kiosk`（= 全屏+置顶）、`maximized/minimized/closing`、`alwaysOnTop/onTopSetByUser`、`resizable`、`showMenuBar`、`nodeEnabled`；
- 几何：`savedRect/savedStyle`（全屏/最大化前保存）、`minW/minH/maxW/maxH`（setMinimum/MaximumSize 镜像，0 不限；WM_GETMINMAXINFO 拦拖拽 + 编程式 resize 两处都钳）；
- 标题：`title`、`titleOverride`（页面 setTitle 后 document.title 不再覆盖标题栏，直到新文档——即 nw.js 的 title_override）；
- 其它镜像：`zoom`（内核只有 setZoomFactor 没有 get，win.state 报的是这里自记的值）、`url`、`menu`（HMENU）、`hotkeys`（热键 id → JS 回调名）；
- `apiScript`：该窗口的注入脚本原文（每窗口一份，因要带窗口号）。

### `class Host`（单例，`Instance()`）
- 生命周期：`Start()` / `Run()`（消息循环）/ `Quit(exitCode)` / **`Shutdown()`**（停 RpcServer，join 线程）；
- 只读访问器：`manifest()`、`kernel()`、`instance()`、`appPath()`、`args()`、`nodeEnabled()`；
- 窗口：`OpenWindow(urlJson, opener)` 返回窗口号（0 失败）、`CloseWindow(id)`、`Find(id)`、`AllWindows()`；
- 脚本：`EvalInWindow(id, code)`（不等结果）、`EvalInWindowSync(id, code)`、`InstallScripts(window, frame)`、`RootApiScript()`；
- **通道入口**：`Dispatch(window, requestJson, Channel)`——页面→native 所有 JSON 命令的唯一收口；
  - `enum class Channel { Async, Sync }`：Async=mbQuery 回调在 UI 线程可碰 HWND；Sync=同步通道，线程语义依运输层而定，只许碰数据白名单；
- 应答信封：静态 `Ok(Json)` / `Fail(message)`；
- 私有 10 组 API：`ApiApp / ApiWindow / ApiMenu / ApiTray / ApiClipboard / ApiShell / ApiScreen / ApiShortcut / ApiFs / ApiProcess`；
- 私有状态：内核、manifest、路径（`dataPath_` 对应用户数据目录）、窗口表 `map<int, unique_ptr<NwWindow>>`、`nextWindowId_`（从 1 起）、`apiScript_`/`nodeScript_`、退出码与退出标志。

### `struct TrayEntry` + `FindTray(id)`
托盘是"没有窗口也要活着"的进程级资源：`id`、所属窗口、`NOTIFYICONDATAW`、加载的 `HICON`（销毁时要 DestroyIcon）。

## 编译注意（文件头注释）

`_WIN32_WINNT=0x0601`、`NTDDI_VERSION=0x06010000` 必须在任何 Windows 头之前定义：mingw-w64 的 shellapi.h 里 `NOTIFYICONDATAW` 成员随版本变化；并显式 `#include <shellapi.h>`（WIN32_LEAN_AND_MEAN 会把它从 windows.h 摘掉）。
