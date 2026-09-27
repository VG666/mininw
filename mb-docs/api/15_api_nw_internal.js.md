# 15_api_nw_internal.js —— native 绑定分界层

- 源文件：[js/api/15_api_nw_internal.js](../../js/api/15_api_nw_internal.js)
- 对应 SDK：`src/resources/api_window_internal.js`（bindingUtil 一句话一个 native 请求那层）。

## 这个文件存在的意义

SDK 里这层只管"发请求"，对象语义全留给上层模块。本实现沿用同一条分界线，但底下换成 [00_base.js](00_base.js.md) 的通道：上层模块（主要是 [20_api_nw_window.js](20_api_nw_window.js.md)）只表达"我要读"还是"我要改"，**不直接调 rpc/tell**。

## 三个口子

- `internalRead(command, id, payload)`：要结果、失败就抛。自动补 `id`；交给 `rpc()`；
- `internalReadSafe(command, id, payload, fallback)`：要结果、拿不到用兜底值，原因经 `rpcSafe` 记进 `window.__nmbRpcErrors`；
- `internalTell(command, id, payload)`：只求做到，失败经 `tell` 记 `__nmbAsyncErrors` 并 warn。

## 同步白名单的"页面侧镜像"

`syncPrefixes = ['app.info','fs.','process.','clipboard.','screen.','shell.','win.state']`，暴露成 `window.__nmbSyncPrefixes`。

注意它**不是开关**：某条命令到底能不能同步，权威永远是 native（[nw_host.cpp](../../src/nw_host.cpp) 的 `SyncSafeCommand` 两道闸门）。这份清单只用于**对账**：若某命令不在镜像里却同步走通（或反之），`internalRead` 会把它记进 `window.__nmbSyncSurprises`，让 smoke 自检能发现两边漂移。
