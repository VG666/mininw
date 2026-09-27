# 52_api_nw_screen.js —— nw.Screen

- 源文件：[js/api/52_api_nw_screen.js](../../js/api/52_api_nw_screen.js)
- 对应 SDK：`api_nw_screen.js`。

## 干了什么事

- `Screen.Init()`：同步 `rpc('screen.screens')`（白名单），缓存到 `screens` 属性：数组，每项 `{id, bounds:{x,y,width,height}, scaleFactor, isPrimary, work_area, ...}`，数据来自 native EnumDisplayMonitors（[nw_host.cpp](../../src/nw_host.cpp) 的 `EnumScreenProc/ApiScreen`）；
- 显示器变化：native 收到 WM_DISPLAYCHANGE 经 `window.__nmbScreenEvent` 回页面，重新 Init 后 emit `displayBoundsChanged`；
- **有意不发** `displayAdded/displayRemoved`：没有可靠的"哪块屏被加/拔"消息源，WM_DISPLAYCHANGE 只告诉"有变化"，注释明确拒绝猜，只做整体刷新。
- `Screen.screens` 缓存属性、`Screen.Init` 静态入口；事件走 Emitter。
