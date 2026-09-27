# 40_api_nw_menuitem.js —— nw.MenuItem

- 源文件：[js/api/40_api_nw_menuitem.js](../../js/api/40_api_nw_menuitem.js)
- 对应 SDK：`api_nw_menuitem.js`。

## 干了什么事

### 菜单项对象
`MenuItem(options)`：字段 label/tooltip/icon/checked/enabled/type（normal|checkbox|separator|normal）/key/modifiers/submenu/click。
- checkbox 点击时由 41 号模块在派发 click 前翻转 checked；
- separator 没有 label/key/click；
- submenu 接受 Menu 实例；
- `click` 是直接回调；菜单弹出的命令路径在下面。

### 点击注册表（核心）
- 全局 `menuClickRegistry`（id → MenuItem）+ `menuClickSeq = 1001`（id 从 1001 起，避开 Win32 常用低 id 与热键段）；
- `registerMenuItemClick(item)` 给每个可点项分配一个**页面侧数字 id**；
- `serializeMenu(items)`：递归把 Menu 的 items 数组转成可过 JSON 线的结构——每项 `{id, label, type, checked, enabled, key, modifiers, submenu:[...]}`。这个结构就是 native `BuildMenuItems` 建 HMENU 时用的契约；
- 派发口 `window.__nmbMenuClick(id)`：native TrackPopupMenu/WM_COMMAND 回来的数字 id 经 EvalInWindow 调这里 → 查 registry → 翻转 checkbox → 触发 item.click 与其 menu 的 itemclick 事件。
- 构造时即登记，移除菜单由 41 号模块处理。
