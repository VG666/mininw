# 41_api_nw_menu.js —— nw.Menu

- 源文件：[js/api/41_api_nw_menu.js](../../js/api/41_api_nw_menu.js)
- 对应 SDK：`api_nw_menu.js`。

## 干了什么事

`Menu(type)`（`'menubar' | 'contextmenu'`），继承 Emitter（事件 itemclick），内部 `items` 数组装 [MenuItem](40_api_nw_menuitem.js.md)。

- 结构操作（纯本地，同步）：`append(menuItem)/insert(index, item)/remove(item)/removeAt(index)`，并实现 items 的只读 getter；
- `attach(win)` / `win.setMenu(menu)`：异步 `menu.set`，把整个菜单（`serializeMenu` 递归序列化 + 窗口 id）发给 native 建/换 HMENU；菜单栏挂在框架窗口；
- `popup(x, y)`：异步 `menu.popup`，native TrackPopupMenu 弹出右键菜单；点击经 `window.__nmbMenuClick(id)`（定义在 40 号文件）回流；
- `createMacBuiltin/removeItem/items` 等 Mac-only 入口保持 API 形状但空转/有限实现（本宿主是 Windows）。

## 与 native 的契约线

命令：`menu.set/create`（建菜单）、`menu.popup`（弹菜单）、`menu.clear`（清除窗口菜单栏）、`menu.setLabel`（动态改项文字，更新本地镜像）。实现见 [nw_host.cpp](../../src/nw_host.cpp) 的 `BuildMenuItems/ApiMenu`。
