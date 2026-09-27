# nw_kernel.cpp —— 加载 miniblink 内核并绑定全部导出

- 源文件：[nw_kernel.cpp](../../src/nw_kernel.cpp)
- 角色：实现 [nw_kernel.h](nw_kernel.h.md) 声明的 `LoadKernel/UnloadKernel/RunJsSync`。宿主启动的第一步硬依赖：找不到内核或缺必需导出就直接启动失败。

## 干了什么事

### 1. 定位候选 dll：`KernelCandidates(explicitPath)`
按优先级产出一组路径：
1. `--nw-kernel=` 显式值本身；若是目录，再试其下 `mb108_x64.dll`、`mb132_x64.dll`；
2. 环境变量 `NMB_MINIBLINK`；
3. exe 同目录的 `mb108_x64.dll` / `mb132_x64.dll`；
4. exe 目录 `\miniblink\` 下两个名字；
5. exe 目录 `\..\NativeMediaBridge\bin\` 下两个名字；
6. 裸 dll 名交给系统按 PATH/当前目录找。

`LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`：内核旁边通常有自己的依赖（icudtl、node 等），必须先在 dll 自己的目录里找依赖。记最后一个非"文件不存在"的错误码，报错时带 `GetLastError`。

### 2. 取导出：模板 `Take(module, name, target, missing, required=true)`
- 必需导出缺失 → 名字进 `missing`，最后汇总成"内核 xxx 缺少必需导出：…"，卸载并失败；
- 可选导出缺失只是少功能（通知、透明、缩放、prompt 链等），不影响宿主成立；
- 必需链包括：init/webview/handle/load/runJs/jsQuery/responseQuery/documentReady/scriptContext/wake/focus/主帧/isMainFrame/debugConfig/新窗口开关，以及同步 XHR 退路的 `mbOnLoadUrlBegin + mbNetSetData + mbNetSetMIMEType`；
- 可选链含 `mbOnPromptBox/mbCreateString/mbDeleteString/mbGetString`（prompt 退路）、导航/历史/标题/URL/console/close/createView、CORS 头与 netHookRequest 等。
- 取导出必须从**实际加载的模块**取（保存于 `g_kernelModule`），不能把 dll 名写死——mb108/mb132 文件名不同但核心 ABI 同组。

### 3. `mbInit` 全局一次
`g_kernelInitialized` 守门：重复 mbInit 会让内核状态错乱。用 `mbCreateInitSettings()` 出默认设置后 `mbInit(settings)`。

### 4. `RunJsSync(view, code)`
- 用 `GetProcAddress` 懒取 `mbRunJs` 与 `mbWebFrameGetMainFrame`；
- 在**调用线程**上发起，内核会在本线程同步跑完脚本并回调 `OnRunJsDone`；
- `OnRunJsDone` 内再用 `mbJsToString(es, value)` 把 JsValue 换成字符串，null/undefined → 空串；
- 用栈上的 `SyncRun{result, ran, depth}` 接结果。注释明确警告：不要改成投到别的线程等信号量，那样会跑错到内核非 GUI 线程。

### 5. `UnloadKernel`
先摘掉 `g_kernelModule` 再 `FreeLibrary`，清空结构。

## 与谁协作

- 被 [nw_host.cpp](nw_host.cpp.md) 的 `Host::Start()` 调用一次；
- 错误信息经 `Utf8ToWide`（[nw_package.cpp](nw_package.cpp.md)）转成宿主内部宽字符。
