# 00_base.js —— 页面侧地基：通道、Emitter、工具

- 源文件：[js/api/00_base.js](../../js/api/00_base.js)
- 加载顺序：api 模块第一个（数字前缀即顺序）。所有模块共享同一个外层 IIFE 函数作用域，本文件定义的东西直接被后续模块使用。

## 干了什么事

### 1. 通道变量与状态
- `queryChannel()`：用的时候再取 `window.mbQuery`（内核异步注入，顶部捕快照可能是 undefined）；
- `syncBase`：探测成功的通道标记（`http-loopback` / `prompt` / 具体 XHR 基址），null=不可用；
- `useLoopbackChannel/usePromptChannel`：sendSync 的分发开关。

### 2. 载荷编码（与 native 三路统一）
`utf8Binary`（TextEncoder 或 unescape(encodeURIComponent)）→ `encodePayload = encodeURIComponent(btoa(utf8))`。中文先转 UTF-8 字节再 base64，避免拼进 URL 时被百分号编码膨胀三倍。

### 3. 三条同步通道实现
- **`loopbackEndpoint()`**：读预注入的 `__nmbRpcPort/__nmbRpcToken` 与 `__nwWindowId`，拼 `http://127.0.0.1:<port>/w<id>/?t=<token>`；
- **`loopbackOnce(request)`（主通道）**：`new XMLHttpRequest(); open('POST', url, false)` 同步发送；`Content-Type: text/plain;charset=UTF-8`——刻意保持 CORS 简单请求（同步 XHR 不允许触发 OPTIONS 预检），令牌放 URL 而不放自定义头也是为此。连接级失败（send 抛异常）换新连接重试 1 次；HTTP 4xx/5xx 不重试（协议问题重试不变）；
- **`promptOnce(request)`（退路）**：`window.prompt("__nmb_rpc__:" + payload)`，最多 3 次；应答前后可能带 NUL/脏字符，截取第一个 `{` 到最后一个 `}` 再 JSON.parse 验真，失败再试；
- **`xhrOnce(base, request)`（老内核退路）**：同步 GET 到 `./__nmb_rpc__/`、`http://nmb-rpc/rpc/`、`nmb-rpc://rpc/` 三个候选；自定义 scheme 没有真实状态码，0 也算成功；
- `sendSync(base, request)` 按两个开关分发。

### 4. 通道探测：`probeSyncChannel()`
顺序：① loopback（用一条真实 `app.info` 探测，成功连结果一起返回，省一次往返）→ ② prompt（`__nmbPromptAvailable` 时两轮，每轮自带重试）→ ③ 三个 XHR 候选。失败原因累加进 `window.__nmbSyncProbeError`，成功通道名写 `window.__nmbSyncChannel`。

### 5. 对外的四个通道函数（后续模块只用它们）
- `rpc(command, payload)`：同步取数。首次调用懒探测（mb108 在脚本上下文创建回调内禁止同步 XHR，所以不能在加载时探测）；失败抛错；应答 JSON.parse 校验，`s==='err'` 抛 Error；挂在 `window.__nmbRpc`；
- `rpcSafe(command, payload, fallback)`：同上，失败把 `cmd: message` 记进 `window.__nmbRpcErrors` 并返回兜底值；
- `rpcAsync(command, payload, done)`：走 mbQuery 异步应答，done(error, value)；挂 `__nmbRpcAsync`；
- `tell(command, payload)`：只求做到的异步调用，失败记 `window.__nmbAsyncErrors` 并 console.warn；挂 `__nmbTell`。

### 6. Emitter 与工具
极简事件发射器 `Emitter`：on/once/removeListener/removeAllListeners/emit（emit 内对单个处理器 try/catch，不让一个坏处理器炸掉整轮）；`exposeStatics(ctor, skip)` 把原型方法复制为静态方法（nw 的 Clipboard/Shell 静态/实例同名用法靠它）。

## 自检钩子（供 tests/apps/smoke 使用）
`__nmbSyncChannel`、`__nmbSyncProbeError`、`__nmbRpcErrors`、`__nmbAsyncErrors`，以及 10 号文件挂的 `__nmbChannel`（含线程拓扑）。
