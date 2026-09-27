# nw_kernel.h —— miniblink 内核 ABI 面

- 源文件：[nw_kernel.h](../../src/nw_kernel.h)
- 角色：把 mb108/mb132 内核 dll 的裸 C 导出收敛成一个 `KernelApi` 结构（一组函数指针），并声明需要的回调签名、`LoadKernel/UnloadKernel/RunJsSync`。加载与取导出的实现在 [nw_kernel.cpp](nw_kernel.cpp.md)。

## 干了什么事

### 回调 typedef（宿主实现，注册给内核）
- `JsQueryCallback`：页面调 `mbQuery` 时内核回调（**异步通道**落点 `OnJsQuery`）；
- `LoadUrlBeginCallback`：每个资源加载前回调（同步 XHR 退路 `OnLoadUrlBegin` 的落点；返回 TRUE=本请求宿主接管）；
- `PromptBoxCallback`：`window.prompt` 同步回调（**兼容退路**；主通道已是 loopback HTTP）；
- `CloseCallback`、`CreateViewCallback`（新窗口请求）、`DocumentReadyCallback`、`ScriptContextCallback`（两个注入时机）、`UrlChangedCallback`、`TitleChangedCallback`、`ConsoleCallback`；
- `RunJsCallback`：`mbRunJs` 的同步结果回调。
- 不透明句柄：`WebView`、`Frame`、`JsExec`、`JsValue`。

### `struct KernelApi`
函数指针**按职责分组**，全部默认初始化为空指针：
- 生命周期/视图：`createInitSettings`、`init`、`createWebView`、`createWebWindow`、`destroyWebView`、`setHandle`、`resize`、`showWindow`；
- 导航/脚本：`loadURL`、`loadHtmlWithBaseUrl`、`runJs`、`jsToString`、`reload/stopLoading/goBack/goForward/canGoBack/canGoForward`、`getURL/getTitle/getSize`；
- query 通道：`onJsQuery`、`responseQuery`；
- **同步通道三条链的导出**：`onLoadUrlBegin` + `netSetData/netSetMIMEType/netSetHTTPHeaderFieldUtf8/netContinueJob/netHookRequest`；以及退路用的 `onPromptBox` + `createString/deleteString/getString`（mbCreateString/mbDeleteString/mbGetString）；
- 注入时机/通知：`onDocumentReady`、`onDidCreateScriptContext`、`onURLChanged`、`onTitleChanged`、`onConsole`、`onClose`、`onCreateView`；
- 杂项开关：`setDebugConfig`、`setNavigationToNewWindowEnable`、`setHandleOffset`、`setAutoDrawToHwnd`、`setTransparent`、`setZoomFactor`、`setUserAgent`、`setCspCheckEnable`、`wake`、`setFocus/killFocus`、`webFrameGetMainFrame`、`isMainFrame`。
- 另存 `HMODULE module` 与实际加载路径 `std::wstring path`。

### 顶层函数
- `LoadKernel(dllPath, out, error)` / `UnloadKernel(api)`；
- `RunJsSync(view, code)`：在本线程立刻跑脚本并从回调拿回字符串结果。

## 设计要点（文件头注释）

内核没有任何"同步绑定 JS 函数"的导出（mb132 时代 wkeJsBindFunction 已删），mbQuery 应答又是异步投递，所以文件头把三条同步候选的来源写得很明确：① loopback HTTP（主，见 [nw_rpc.h](nw_rpc.h.md)）；② prompt（退路，createString 回程有内核级竞态）；③ onLoadUrlBegin 拦截（更老内核退路）。
