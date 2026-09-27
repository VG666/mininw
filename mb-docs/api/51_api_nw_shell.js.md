# 51_api_nw_shell.js —— nw.Shell

- 源文件：[js/api/51_api_nw_shell.js](../../js/api/51_api_nw_shell.js)
- 对应 SDK：`api_nw_shell.js`。

## 干了什么事

薄封装层，四个静态方法（经 `exposeStatics` 同时提供实例同名方法），全部异步 `tell`，native 同步白名单里的 `shell.*`：

- `openExternal(uri)`：默认程序打开 URL；
- `openItem(path)`：系统关联程序打开文件；
- `showItemInFolder(path)`：资源管理器定位并选中；
- `beep()`：系统提示音。

native 实现在 [nw_host.cpp](../../src/nw_host.cpp) 的 `ApiShell`（ShellExecuteW / MessageBeep）。
