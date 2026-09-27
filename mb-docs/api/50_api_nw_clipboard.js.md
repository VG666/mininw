# 50_api_nw_clipboard.js —— nw.Clipboard

- 源文件：[js/api/50_api_nw_clipboard.js](../../js/api/50_api_nw_clipboard.js)
- 对应 SDK：`api_nw_clipboard.js`。

## 两种用法（SDK 语义）

- **实例**：`new Clipboard()` → 具名剪贴板选择器（本实现里只有系统剪贴板），实例方法 `.get(type)/.set(data, type)/.readList/writeList/clear`；
- **静态**：`Clipboard.get()` 返回全局单例（`exposeStatics` 把原型方法复制成静态方法时，静态 get 被显式覆盖为"返回单例"，否则静态 get() 会变成"取一个新选择器"，语义就错了）。

## 干了什么事

- `set(data, type)` / `get(type)`：type 支持 `text`（或 raw → `CF_UNICODETEXT`）、`html`（`CF_HTML`，其特殊头部由 native 回填/剥离）、`png`、`rtf`；
- png/rtf 在页面与 native 之间用 **base64** 承载二进制；text/html 直接字符串；
- `readAvailableTypes()` 读当前剪贴板里有哪些格式（`clipboard.availableTypes`，同步白名单）；
- `clear()`：`clipboard.clear`；
- 所有读写都走同步 `rpc/rpcSafe`（clipboard.* 在任意线程白名单），native 侧 OpenClipboard 带重试，见 [nw_host.cpp](../../src/nw_host.cpp) 的 `ApiClipboard`。
