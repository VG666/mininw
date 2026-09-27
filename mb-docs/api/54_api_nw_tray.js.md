# 54_api_nw_tray.js —— nw.Tray（系统托盘）

- 源文件：[js/api/54_api_nw_tray.js](../../js/api/54_api_nw_tray.js)
- 对应 SDK：`api_nw_tray.js`。

## 干了什么事

- `Tray(option)`：`{title, tooltip, icon, menu, iconsAreTemplates}`，继承 Emitter（click）。托盘是**进程级资源**（窗口全关托盘也活着），注册表 `trayRegistry` 在 [90_install.js](90_install.js.md) 全局装配。
- **异步创建 + id 排队**：与 Window.open 同理——`new Tray()` 先有对象，`__create` 异步发 `tray.create`（带 tooltip/图标），native 回 id 后 `__resolve(id)`；在此之前调用 setTooltip/setIcon 会排队（`__iconQueued`），结算后补发；
- `setTooltip(text)` / `setIcon(path)`：异步 `tray.setTooltip/setIcon`（图标路径解析在 native）；
- `remove()`：异步 `tray.remove`，native Shell_NotifyIcon(NIM_DELETE) 并 DestroyIcon；
- 派发口 `window.__nmbTrayEvent(id, event)`：native 托盘消息（左键单击等）经 EvalInWindow 调这里 → 查 registry → emit click；
- **右键弹菜单**：收到右键事件时若该托盘带 menu，页面调 `menu.popup` 或专门的托盘弹菜单路径，由 native TrackPopupMenu（挂在拥有托盘的窗口线程）；
- title/iconsAreTemplates 为 Mac 字段，Windows 上保持形状不生效。

native 侧见 [nw_host.cpp](../../src/nw_host.cpp) 的 `ApiTray` 与 `g_trays` 全局表。
