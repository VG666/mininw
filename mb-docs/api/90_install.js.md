# 90_install.js —— 装配层：挂 window.nw、接管事件与新窗口

- 源文件：[js/api/90_install.js](../../js/api/90_install.js)（api 模块最后一个）
- 对应 SDK：`api_nw.js` / `api_nw_nw.js` 的装配部分。

## 干了什么事（按序）

1. **取当前窗口镜像**：`Window.get(currentId)`，后续所有窗口级事件都挂它；
2. **window.nw 命名空间**：`window.nw = {}`（Object，没有自有方法；nw 对象本身只是命名空间占位）；挂上 App/Window/Menu/MenuItem/Clipboard/Shell/Screen/Shortcut/Tray；node 脚本随后回填 `nw.require`；
3. **native 事件总入口 `window.__nmbEvent`**：native EvalInWindow 推来的事件（窗口 maximize/minimize/restore/focus/blur/move/resize/enter-fullscreen/title/zoom 等）统一进这里 → 找 WindowState → `sync()` 合并/emit；
4. **新窗口 `window.__nmbOpenWindow(payload)`**：native 的 `mbOnCreateView` 回调（页面 target=_blank/window.open）经这里 → 用 `Window.open(url, options)` 建 JS 侧对象；
5. **关闭否决 `window.__nmbCloseVerdict`**：框架窗口收到 WM_CLOSE 时 native 同步问一句，这里看当前 WindowState 是否有 `close` 事件处理器（`close(false)` 语义）决定是否允许关闭；
6. **接管 window.open**：保存原始引用后，把页面 `window.open` 替换为走 nw 开窗流程（由 host 窗口承载，而非内核弹窗）；
7. 模块注册表（trayRegistry/shortcutRegistry/menuClickRegistry 由 40/53/54 定义、这里统一通过 `window.nw` 暴露）；
8. 幂等守卫：外层 IIFE 的 `__nmbInstalled`（所有 api 模块共享）保证重复注入不重复挂。

## 为什么它必须最后

所有 `window.__nmb*` 回调入口依赖前面模块定义好的构造器与注册表；native 回调可能在脚本注入后任意时刻到达，必须在本文件执行完那一刻全部就绪。
