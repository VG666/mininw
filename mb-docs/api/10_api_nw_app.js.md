# 10_api_nw_app.js —— nw.App

- 源文件：[js/api/10_api_nw_app.js](../../js/api/10_api_nw_app.js)
- 对应 SDK：`src/resources/api_nw_app.js`（browser 侧 app.js）。

## 干了什么事

### 启动数据
优先用 native 预注入的 `window.__nmbBootAppReply`；没有就调 `probeSyncChannel()`（其 app.info 探测结果直接复用）。同步通道完全不可用时置 `syncUnavailable`，给空清单占位，并用 `rpcAsync('app.info')` 异步补齐 fullArgv/dataPath/manifest。

### 诊断出口
`window.__nmbChannel`：带 `threads`（native 实测线程拓扑）、`syncSharesUi`，并用 getter 暴露当前 `syncBase` 与 `__nmbSyncProbeError`；`window.__nmbAppInfo` 保留原始 app.info（含不出现在公开 API 里但自检需要的路径字段）。

### App 对象（继承 Emitter）
- `fullArgv`：native 原样命令行；
- `filteredArgv`：四条"宿主开关"正则（`--url=`、`--remote-debugging-port=`、`--renderer-cmd-prefix=`、`--nwapp=`），可读可写；
- `argv`：**只读 getter**，返回 fullArgv 过滤掉 filteredArgv 命中项的结果（语义照抄 SDK；strict 模式下给无 setter 的访问器赋值会抛 TypeError）；
- `dataPath`、`manifest`、`filter`（Emitter）；
- 生命周期（全走异步 `tell`）：`quit/exit(code)/restart`；
- `clearCache`：空转（缓存归内核管，无对外开关）；
- 崩溃测试口：`crashBrowser/crashRenderer` → `app.crash`（browser/renderer 同进程，即整个进程硬退）；`setCrashDumpDir` 空转（无 crashpad）；
- 代理/源白名单：内核未暴露网络栈配置，`setProxyConfig/addOriginAccessWhitelistEntry/removeOriginAccessWhitelistEntry` 如实空转，`getProxyForURL` 同步回 `'DIRECT'`——不假装成功；
- **全局热键**：`registerGlobalHotKey(shortcut)/unregisterGlobalHotKey(shortcut)` 只是转发到 53 号模块的 `shortcutRegistry`（与 SDK 一致，nw.App 只是转发层）。App 入口用 1..999 的 id 段，与 Shortcut 原型用的 1000+ 段错开，避免互相误删；注册失败调 `shortcut.failed`。
