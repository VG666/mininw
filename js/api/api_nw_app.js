// api_nw_app.js —— nw.App。对应 SDK：src/resources/api_nw_app.js（browser 侧 app.js）。
// argv / fullArgv / filteredArgv 三个的语义照抄 SDK：fullArgv 是 native 给的原样，
// argv 是过掉 filteredArgv 那组正则之后的，filteredArgv 本身可读可写。

  // mb108 在脚本上下文创建回调内不能发同步 XHR，优先使用 native 随脚本预注入的启动数据。
  // 旧宿主没有预注入值时仍保留原探测逻辑。
  var bootApp = window.__nmbBootAppReply;
  var appInfo = bootApp && bootApp.v ? bootApp.v : probeSyncChannel();
  var syncUnavailable = !appInfo;
  if (!appInfo) appInfo = { manifest: {}, argv: [], dataPath: '' };

  // 线程拓扑：宿主实测出来报给页面的（真值，不是推断）。这决定 UI 命令能不能走同步通道，
  // 自检脚本应该把它打出来。
  var threads = (appInfo.threads) || null;
  var channel = {
    threads: threads,
    syncSharesUi: !!(threads && threads.syncSharesUi)
  };
  Object.defineProperty(channel, 'syncBase', {
    enumerable: true,
    configurable: false,
    get: function () { return syncBase; }
  });
  Object.defineProperty(channel, 'syncProbeError', {
    enumerable: true,
    configurable: false,
    get: function () { return window.__nmbSyncProbeError || null; }
  });
  window.__nmbChannel = channel;
  // 原始 app.info 留在页面上：路径字段（appPath / startupUrl / manifestPath）不出现在
  // nw 的公开 API 里，但自检要能看见它们——路径是最容易悄悄坏掉的一类值。
  window.__nmbAppInfo = appInfo;

  var manifest = appInfo.manifest || { name: appInfo.name, version: appInfo.version, main: appInfo.startupUrl };

  var App = new Emitter();
  // argv 三段照抄 SDK（api_nw_app.js）：fullArgv 是 native 给的原样一条不删，argv 是过掉
  // filteredArgv 那组正则之后的，filteredArgv 本身可读可写（api_nw_app.js 里带 setter）。
  // 那四条正则都是"宿主自己的开关"（--url= / --remote-debugging-port= / --renderer-cmd-prefix=
  // / --nwapp=），nw 应用读 argv 本来不该看见它们。
  // 注意：argv 是**只读 getter**（nw 那边也是 __defineGetter__），别在别处给它赋值——
  // 脚本跑在 'use strict' 下，赋给没有 setter 的访问器会直接抛 TypeError。
  App.fullArgv = appInfo.argv || [];
  App.filteredArgv = [
    /^--url=/,
    /^--remote-debugging-port=/,
    /^--renderer-cmd-prefix=/,
    /^--nwapp=/
  ];
  Object.defineProperty(App, 'argv', {
    get: function () {
      return App.fullArgv.filter(function (arg) {
        return !App.filteredArgv.some(function (filter) { return filter.test(String(arg)); });
      });
    }
  });
  App.dataPath = appInfo.dataPath;
  App.manifest = manifest;
  App.filter = new Emitter();
  // 这三个动的是消息循环/进程，走异步通道（nw 里它们也不返回任何东西）。
  App.quit = function () { tell('app.quit'); };
  App.exit = function (code) { tell('app.exit', { code: code || 0 }); };
  App.restart = function () { tell('app.restart'); };
  App.clearCache = function () { /* 缓存由内核管理，无对外开关 */ };

  // 崩溃测试口（nw 文档：crashBrowser/crashRenderer 用来测崩溃恢复路径）。本宿主
  // browser/renderer 同进程，一条 app.crash 命令就是整个进程硬退（native 侧实现）。
  App.crashBrowser = function () { tell('app.crash'); };
  App.crashRenderer = App.crashBrowser;
  // 没有 crashpad：崩溃转储无处可配，如实空转。
  App.setCrashDumpDir = function () {};

  // 代理与源白名单：内核没暴露网络栈配置导出（本桥只接管渲染层）。这里如实空转，
  // 而不是假装设置成功——依赖代理的应用会走系统代理，getProxyForURL 如实回 DIRECT。
  App.setProxyConfig = function () {};
  App.getProxyForURL = function (url, callback) {
    // nw 语义：返回该 URL 使用的代理（"DIRECT" 或 "PROXY host:port"）。
    if (typeof callback === 'function') callback('DIRECT');
    return 'DIRECT';
  };
  App.addOriginAccessWhitelistEntry = function () {};
  App.removeOriginAccessWhitelistEntry = function () {};

  // 全局热键：SDK 里 App.registerGlobalHotKey 只是转发到 nw.Shortcut，这里同样委托
  // api_nw_shortcut.js 的注册表（跨文件共享作用域，调用时才解引用）。
  // shortcut 形如 { key: 'Ctrl+Shift+A', active: fn, failed: fn }。id 用 1..999 段，
  // 与 Shortcut 原型注册的 1000+ 段错开，unregister 不会互相误伤。
  var globalHotkeySeq = 0;
  App.registerGlobalHotKey = function (shortcut) {
    if (!shortcut || typeof shortcut.key !== 'string') return;
    shortcut.active = shortcut.active || function () {};
    var name = String(++globalHotkeySeq);
    shortcutRegistry[name] = shortcut.active;
    rpcAsync('shortcut.register', {
      id: globalHotkeySeq, key: shortcut.key, callback: name, windowId: currentId
    }, function (error) {
      if (!error) return;
      delete shortcutRegistry[name];
      if (shortcut.failed) shortcut.failed(error.message);
    });
  };
  App.unregisterGlobalHotKey = function (shortcut) {
    if (!shortcut || typeof shortcut.key !== 'string') return;
    // registry 里只存了 active 回调，按它反查（nw 应用基本都复用同一个 shortcut 对象）。
    for (var name in shortcutRegistry) {
      if (shortcutRegistry[name] !== shortcut.active) continue;
      tell('shortcut.unregister', { id: parseInt(name, 10), windowId: currentId });
      delete shortcutRegistry[name];
    }
  };

  // 同步通道没通的话，清单/argv 用异步补齐一遍，至少别让页面读到空清单。
  if (syncUnavailable) {
    rpcAsync('app.info', {}, function (error, value) {
      if (error || !value) return;
      App.fullArgv = value.argv || [];
      App.dataPath = value.dataPath;
      if (value.manifest) App.manifest = value.manifest;
      window.__nmbAppInfo = value;
    });
  }
