# nw_host.cpp —— 宿主全部实现（项目最大的文件）

- 源文件：[nw_host.cpp](../../src/nw_host.cpp)（约 2300 行）
- 角色：实现 [nw_host.h](nw_host.h.md) 的 `Host` 与所有内核回调。**浏览器内核与页面 nw.* API 之间的那一层**：窗口生命周期、命令路由、十组 nw/node API 的 native 实现、三条同步通道中的两条半（prompt、XHR 拦截）与异步通道。渲染/排版/网络不在这里，全归内核。

## 文件结构地图（按行序）

### A. 全局设施（匿名命名空间）
- `kWndClass`：Win32 框架窗口类名 `NwBridgeHostFrame`；
- `g_viewToWindow` + `g_viewMutex`：内核视图只允许绑一个 param，回调里靠它反查 `NwWindow*`（`WindowOf/BindView`，加锁）；
- `g_trays/g_nextTrayId`：托盘登记表（id 从 1000 起）；
- **线程拓扑原子量**：`g_threadMain/g_threadUi/g_threadSync` 记录三个线程的实测 TID；
  `enum class SyncTransport { Unknown, Prompt, XhrHook, Loopback }` + `g_syncTransport`。
  `SyncChannelSharesUiThread()` **仅当 transport==Prompt** 才比较 TID——loopback worker / XhrHook 即使 TID 撞号也绝不放行 UI 命令；
- 工具：`Narrow`（宽→UTF8）、`QuoteForJs`（拼进注入脚本的 JS 字符串转义）、`Base64Encode/Decode`、`HexDigit/PercentDecode`、`ParseAccelerator`（把 `Ctrl+Shift+A` 解析成修饰键+VK）。

### B. 内核回调
- `OnJsQuery`：**异步通道**入口。记 UI TID → 找窗口 → `Dispatch(Channel::Async)` → `mbResponseQuery` 应答；
- `OnLoadUrlBegin`：**同步 XHR 拦截退路**。对每个资源加载都会回调（mb108 上还恰好在 UI 线程），所以只有确认拦截到 RPC（`file:` 中的 `/__nmb_rpc__/`、`nmb-rpc:` scheme、`http://nmb-rpc/` 前缀）才写 transport=XhrHook 和 g_threadSync；解码后 `Dispatch(Channel::Sync)`，用 `mbNetSetData/SetMIMEType/SetHTTPHeaderField` 当场作答（带 CORS/Cache-Control）；
- `OnPromptBox`：**prompt 兼容退路**。识别前缀 `__nmb_rpc__:`，记 transport=Prompt/UI TID，Dispatch 后 `mbCreateString` 回应答；短于 256 字节填充到 256（规避短串路径的已知竞态）；非本通道的真实 prompt 一律 `*handled=FALSE` 放行；
- `OnScriptContext`：记 UI TID；主框架新上下文清 `titleOverride`（页面 location.href/链接/重定向都要清，不能只在导航命令里清）→ `InstallScripts`；
- `OnDocumentReady`：再注入一次（幂等）；
- `OnTitleChanged`/`OnUrlChanged`：主框架变化 → 转成 `__title/__url` 命令回 Dispatch；
- `OnConsole`：页面 console 转发到 stderr（外部可取证）；
- `HostWndProc`：框架窗口过程，处理尺寸变化（同步内核子窗口 mbResize）、菜单命令、WM_HOTKEY、托盘消息、WM_CLOSE（问页面 `__nmbCloseVerdict` 可否决）、WM_DISPLAYCHANGE、WM_GETMINMAXINFO（按 min/max 尺寸钳制）等；
- `ApplyFullscreen/ApplyKiosk`：全屏（保存矩形/样式，吃边框）与 kiosk（先置顶后全屏，顺序反了会闪一帧）。

### C. Host 生命周期
- `PreparePaths`：确定 exe/应用/用户数据目录；
- `Start`：LoadKernel → LoadManifest → LoadScripts → **RpcServer::Start**（失败只打 stderr，退回 prompt）→ 注册窗口类等；RPC handler lambda：标 Loopback + 记 worker TID → 按路径 windowId `Find()` → 解码 → `Dispatch(Channel::Sync)`；
- `Run`：GetMessage 标准循环；`Quit`：PostQuitMessage；**`Shutdown`：`RpcServer::Stop()`**（join 全部线程，防退出后 worker 踩单例）；
- `WakeAll`：对所有视图 mbWake；
- `InstallScripts`：拼 preamble（`__nwWindowId/__nwNodeEnabled/__nmbPromptAvailable/__nmbRpcPort/__nmbRpcToken/__nmbBootAppReply/__nmbBootProcessReply`，后两者是直接在 native 预跑 app.info/process.info 的结果——绕开 mb108 "脚本上下文回调内禁止同步 XHR"）+ api 脚本 + node 脚本，`mbRunJs(闭包内执行)` 注入；
- `AssembleApiModules`：枚举 exe 旁边 api\\*.js，排序、加模块来源注释、包与 embed-js 相同的 IIFE；
- `LoadScripts`：磁盘版优先，缺失才回落 `BuiltinApiScript/BuiltinNodeScript`；
- `Find/AllWindows/EvalInWindow(同步/异步)`。

### D. 开窗与关窗
- `OpenWindow(urlJson, opener)`：按 manifest window 段与传入 config 建框架窗口（位置/尺寸/frame/transparent/置顶/任务栏等），`mbCreateWebView` 建视图、`mbSetHandle` 绑定为真实子窗口，注册全部内核回调，挂菜单，`mbLoadURL`/loadHtml，窗口号入表；`win.create` 命令复用它开子窗；
- `CloseWindow`：销毁视图、摘绑定、关 HWND、清托盘/热键。

### E. 命令路由（桥的入口）
- `BlinkReentrancyUnsafe`：`win.eval/loadURL/reload/back/forward/canGoBack/canGoForward/create/devtools` 永远不许走同步通道（会在 Blink 嵌套调用栈里重入它）；
- `SyncSafeCommand`：任意线程可跑白名单 `app.info`、`fs.`、`process.`、`clipboard.`、`screen.`、`shell.`、`win.state`；其余 UI 命令需 `SyncChannelSharesUiThread()`；
- `Dispatch`：解析 JSON 取 `c`；同步通道先过两道闸门；`__title/__url` 内联处理（title_override 判定后 SetWindowText，并把"生效值"通过 `__nmbEvent` 推页面）；再按前缀分派到下面 10 组；未知命令 Fail；
- 信封：`Host::Ok(v)`→`{"s":"ok","v":...}`，`Host::Fail(m)`→`{"s":"err","m":...}`。

### F. 十组 API 实现（每组都是"解 JSON → 调 Win32/内核 → 回 JSON"）

| 实现函数 | 处理的命令（摘自源码，按出现序） |
|---|---|
| `ApiApp` | `app.info`（清单/argv/dataPath/线程拓扑）、`app.quit/exit/restart/crash/registerEvents` |
| `ApiWindow`（约 50 个） | `win.list/create/close/show/hide/focus/blur/minimize/restore/maximize/unmaximize/fullscreen/leaveFullscreen/moveTo/resizeTo/setTitle/setResizable/setAlwaysOnTop/eval/state/devtools/setZoom/loadURL/reload/back/forward/canGoBack/canGoForward/enterKiosk/leaveKiosk/toggleKiosk/setMinimumSize/setMaximumSize/setPosition/requestAttention/setShowInTaskbar/setProgressBar(ITaskbarList3)/setBadgeLabel/setVisibleOnAllWorkspaces/canSetVisibleOnAllWorkspaces/setIgnoreMouseEvents/setTransparent/capturePage(PrintWindow+RGBA)/getPrinters(EnumPrintersW)/print` |
| `ApiMenu` + `BuildMenuItems` | `menu.set/create`（建 HMENU，递归子菜单，点击 id 与页面注册表一一对应）、`menu.clear/popup(TrackPopupMenu)/setLabel` |
| `ApiTray` | `tray.create`（Shell_NotifyIcon + LoadImage 图标）、`setTooltip/setIcon/remove`；点击经 `__nmbTrayEvent` 回页面 |
| `ApiClipboard` | `clipboard.write/read/clear/availableTypes`。`OpenClipboardRetry` 带重试；text→CF_UNICODETEXT、html→CF_HTML（头部由 native 回填/剥离）、png/rtf→注册格式 + base64 |
| `ApiShell` | `shell.openExternal/openItem/showItemInFolder/beep`（ShellExecuteW/MessageBeep） |
| `ApiScreen` + `EnumScreenProc` | `screen.screens`：EnumDisplayMonitors 枚举每块屏的 bounds/scale/主显标志 |
| `ApiFs` | node 桥文件后端：`fs.exists/stat/readFile/writeFile/appendFile/readdir/mkdir/unlink/rename/realpath`（应用根内相对路径解析，utf8/base64 两种线格式） |
| `ApiProcess` | `process.info`（platform/arch/cwd/argv/env/versions/pid/appPath）、`process.chdir/exit` |
| `ApiShortcut` | `shortcut.register/unregister/unregisterAll`：RegisterHotKey 绑窗口线程，WM_HOTKEY 经 `__nmbHotkey` 回页面 |

## 关键设计约束（读这个文件时必须记住）

1. 同步通道能做什么由 native 两道闸门决定，页面侧 [15_api_nw_internal.js](../api/15_api_nw_internal.js.md) 只做镜像对账，不掌握最终决定权；
2. 标题/URL 推给页面的永远是**生效值**（setTitle 之后页面读 win.title 必须等于标题栏）；
3. 凡是需要页面回调的（菜单点击、热键、托盘、新窗口、关闭否决）都通过 EvalInWindow 调约定的 `window.__nmb*` 入口，这些入口在 [90_install.js](../api/90_install.js.md) 与各模块里挂接。
