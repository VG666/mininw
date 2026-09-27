# 20_api_nw_window.js —— nw.Window 与 WindowState

- 源文件：[js/api/20_api_nw_window.js](../../js/api/20_api_nw_window.js)（页面层最大模块，约 21KB）
- 对应 SDK：`api_nw_window.js` + `api_nw_newwin.js`。

## 核心数据结构

- `windowById`：id → WindowState；`currentId = window.__nwWindowId`（preamble 预注入）；
- `WindowState(id)`：一个 nw 窗口的 JS 镜像。**id 允许为 null**——`Window.open` 的真实 id 是异步回来的，先交对象、命令排队，`__resolve(id)` 结算后补发（见下）。

## 两个必须理解的访问器

`title` 与 `zoomLevel` 在 SDK 里是访问器（读走 currentWindowInternal，写走 setXxxInternal），不是普通字段：
- 内部值存 `__title/__zoom`，构造期只能写内部字段（写 this.title 会在还没 pending 队列时就发命令）；
- setter = 设**覆盖值**：发 `win.setTitle/win.setZoom`；此后 document.title 不再改标题栏，直到导航到新文档（native 的 title_override）。
- native 推回来的状态只能写 `__title/__zoom`，直接赋 this.title 会造成"回声"（状态又被当本地改动送回去）。

## 状态合并：`sync(state)`

把 native `win.state`/事件推送合并进对象并转成 nw 事件：x/y/width/height 直接填（不发 move/resize，编程调用自己已发过）；maximized/minimized/fullscreen 变化发对应/leave- 事件；title/url 变化发事件；zoom 只更新不发事件（nw 无 zoom 事件）。

## 命令三个口子 + id 排队

- `read/readSafe` → [internalRead/ReadSafe](15_api_nw_internal.js.md)（目前唯一能同步读的窗口命令是 win.state；canGoBack/canGoForward 走 Safe，拿不到老实回 false 并留错，不编值）；
- `act` → internalTell；**id===null 时命令进 `__pending` 队列**，`__resolve(id)` 时按序补发；
- `close()` 例外不排队（窗口还没结算就关，语义是"别开了"）；`close(false)` 先问 `close` 事件处理器可否决。

## 实现的窗口方法（均映射到 win.* 命令）

- 显隐/焦点/状态：show/hide/focus/blur/close/destroy/maximize/unmaximize/minimize/restore；
- 全屏/kiosk：enterFullscreen/leaveFullscreen/toggleFullscreen、enterKioskMode/leaveKioskMode/toggleKioskMode；
- 几何：moveTo/resizeTo/moveBy/resizeBy（负数抛错）、setMinimumSize/setMaximumSize、setPosition('center'|'mouse')；
- 样式/行为：setResizable、setAlwaysOnTop、setShowInTaskbar、setVisibleOnAllWorkspaces（Windows=TOPMOST；canSet 恒真）、setIgnoreMouseEvents、setTransparent、requestAttention（FlashWindow）、setBadgeLabel（Mac only，Windows no-op）；
- 导航/视图：setTitle、setZoom/zoomLevel、reload/reloadIgnoringCache、back/forward、load/navigate、canGoBack/canGoForward（同步读历史栈，红线命令拿不到就 false）、showDevTools/closeDevTools；
- `eval(code)`：同步取最后表达式值——它在 native Blink 重入红名单上，会被明确拒绝，注释提示应用改用事件/回调；
- 菜单：setMenu(menu)，id 未结算时把 menu.set 也排队；
- 任务栏：setProgressBar（<0 移除 / >1 跑马灯 / 0..1 百分比）；
- `capturePage(cb, type)`：走异步 win.capturePage，native 回 RGBA 裸像素 base64，页面 canvas 组 dataURL；无 callback 时返回 Promise；
- `getPrinters(cb)`：native 回名字数组，页面补成 `{name}` 对象；`print()` 转发（内核无打印栈，会 Fail）；
- `stop()` 调本页面 window.stop；`syncNow()` 拉一次 win.state 合并；`get()` 返回自身。

## 静态 API

- `Window.get(id)`：命中表就返回；否则新建 WindowState 并立刻用 readSafe('win.state') 尽量填充（被拒也能得到"只知道 id"的对象）；
- `Window.open(url, options)`：先 new WindowState(null)，异步 `win.create`（config 序列化携带 url），成功后 `__resolve(value.id)` → 补 state → emit('loaded')；失败 emit('error')。**同步返回句柄、id 异步结算**就是靠 pending 队列；
- `getAll()`：win.list 没进同步白名单，拿不到就退化为只给当前窗口；`getCurrent()`。
