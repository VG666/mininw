// api_nw_screen.js —— nw.Screen。对应 SDK：src/resources/api_nw_screen.js。
// nw 的用法：先 Screen.Init()，之后读 Screen.screens（**属性**，返回数组）；显示器
// 变化（宿主的 WM_DISPLAYCHANGE 广播）时重拉并派发 displayBoundsChanged。
// displayAdded / displayRemoved 没有可靠的 Windows 消息源，如实不发。

  var Screen = new Emitter();
  var screenCache = [];
  var screenQueried = false;

  function refreshScreens() {
    var list = rpcSafe('screen.screens', {}, null);
    if (list) { screenCache = list; screenQueried = true; }
    return screenCache;
  }

  Screen.Init = function () { refreshScreens(); };
  Object.defineProperty(Screen, 'screens', {
    get: function () {
      // 没调 Init 就读也算合法（lazy 查一次）；Init 过了就回缓存，变化由事件驱动刷新。
      return screenQueried ? screenCache : refreshScreens();
    }
  });
  // native 的 WM_DISPLAYCHANGE 广播打到这里：先重拉显示器，再派发事件。
  window.__nmbScreenEvent = function (event) {
    refreshScreens();
    Screen.emit(event);
  };
