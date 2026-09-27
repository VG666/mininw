// api_nw_shortcut.js —— nw.Shortcut。对应 SDK：src/resources/api_nw_shortcut.js。
// registerGlobalHotKey / unregisterGlobalHotKey 是**静态**方法（SDK 里 nw.App 只是转发到这里）。

  function Shortcut() { this.__ids = {}; }
  Shortcut.prototype.register = function (accelerator, callback) {
    var id = (Object.keys(this.__ids).length + 1) * 1000;
    this.__ids[id] = { key: accelerator, callback: callback };
    shortcutRegistry[id] = callback;
    // RegisterHotKey 绑在窗口所属线程上，只能走异步通道；注册失败（组合键被占用等）
    // native 会 Fail 回来，这里撤掉该条目并 warn。nw 原型注册本身没有 failed 回调，
    // 带 failed 的入口是 App.registerGlobalHotKey（10 文件），那边自己接管错误。
    var self = this;
    rpcAsync('shortcut.register', { id: id, key: accelerator, callback: String(id), windowId: currentId }, function (error) {
      if (!error) return;
      delete self.__ids[id];
      delete shortcutRegistry[id];
      if (window.console && console.warn) console.warn('[nw] 热键注册失败 ' + accelerator + '：' + error.message);
    });
    return true;
  };
  Shortcut.prototype.unregister = function (accelerator) {
    // nw 的 unregister 收的是快捷键串，只撤匹配的那一条（原先实现会误撤全部）。
    for (var id in this.__ids) {
      if (this.__ids[id].key !== accelerator) continue;
      tell('shortcut.unregister', { id: parseInt(id, 10), windowId: currentId });
      delete shortcutRegistry[id];
      delete this.__ids[id];
    }
    return true;
  };
  Shortcut.prototype.unregisterAll = function () {
    tell('shortcut.unregisterAll', { windowId: currentId });
    this.__ids = {};
    return true;
  };

  var shortcutRegistry = {};
  window.__nmbHotkey = function (name) {
    if (typeof shortcutRegistry[name] === 'function') shortcutRegistry[name]();
  };
