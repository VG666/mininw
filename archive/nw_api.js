// nw_api.js —— 在页面里重建 nw.* 的那一层。
//
// 与 nw.js 的关系：文件名/对象形状/事件名照抄 nw.js（这样原有 nw 应用不用改代码），
// 但底下的实现全部换成本桥的通道，而不是 nw.js 里 renderer 侧那套 V8 binding。
// 渲染/排版/网络一概不在这层，全部由 miniblink 内核负责。
//
// ---------------------------------------------------------------------------
// 两条通道，别混用（内核的限制，不是设计偏好）
// ---------------------------------------------------------------------------
// 内核里 **没有任何"同步绑定 JS 函数"的导出**（mb132 把 wkeJsBindFunction 那套删了），
// 唯一的页面→native 回调通道是 mbQuery，而它的应答是内核在**后续任务**里调
// __onMbQuery__ 投递的——也就是说 mbQuery 返回时结果还没到，同步读必然是 undefined。
// 内核自己注入的垫片长这样（从 mb132_x64.dll 里挖出来的，见 f:\ffbuild\dll_strings.py）：
//
//     __globalThis__.mbQuery = function (customMsg, request, cb) {
//         var id = -1;
//         if ('function' == typeof cb) { id = ++__g_callbackMapIdGen__; __g_callbackMap__[id] = cb; }
//         func(customMsg, request, id);          // 进 native
//     };
//     __globalThis__.__onMbQuery__ = function (id, customMsg, response) {
//         var cb = __g_callbackMap__[id];
//         if (cb) { cb(customMsg, response); delete __g_callbackMap__[id]; }
//     };
//
// 所以：
//   rpc()      —— 同步取数。走**同步 XHR** 打 nmb-rpc://，宿主在 onLoadUrlBegin 里当场作答。
//                 这条路跑在内核的网络线程上，宿主那边只放行纯数据命令（fs/clipboard/screen/...）。
//   rpcAsync() —— 动手。走 mbQuery（异步，返回值拿不到），碰窗口/菜单/托盘/热键这类必须
//                 在 UI 线程上做的事。nw 里这些接口本来也不靠返回值。
(function () {
  'use strict';
  if (window.__nmbInstalled) return;
  window.__nmbInstalled = true;

  // ---------------------------------------------------------------------
  // 通道 1：同步取数 —— 同步 XHR，宿主在 onLoadUrlBegin 里当场作答
  // ---------------------------------------------------------------------
  // mbQuery 是内核往页面里注入的，注入时机不由本脚本决定：别在顶部捕一份可能还是
  // undefined 的快照，到用的时候再取。
  function queryChannel() {
    return typeof window.mbQuery === 'function' ? window.mbQuery : null;
  }

  var syncBase = null;      // 探测成功后的基址；null = 不可用
  var SYNC_CANDIDATES = ['nmb-rpc://rpc/', 'http://nmb-rpc/rpc/'];

  // 载荷走 base64：请求要拼进 URL，裸 JSON 里的非 ASCII（中文标题、路径）会被
  // encodeURIComponent 膨胀三倍，而 base64 只有 4/3。内核没有 btoa 之外的选择，
  // 中文得先自己编成 UTF-8 字节串。
  function utf8Binary(text) {
    if (typeof TextEncoder === 'function') {
      var bytes = new TextEncoder().encode(text);
      var out = '';
      for (var i = 0; i < bytes.length; i++) out += String.fromCharCode(bytes[i]);
      return out;
    }
    return unescape(encodeURIComponent(text));   // 内核里一定有 encodeURIComponent
  }

  function encodePayload(text) {
    return encodeURIComponent(btoa(utf8Binary(text)));
  }

  function sendSync(base, request) {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', base + encodePayload(request), false);   // false = 同步，就靠这个
    xhr.send();
    // 自定义 scheme 的响应没有真正的状态码（内核不给设），所以 0 也算成功，
    // 判成功看的是"拿到了正文能解析出信封"。失败时同步 XHR 会直接抛异常。
    return xhr.responseText || '';
  }

  function probeSyncChannel() {
    // 用一条真实命令探测：探通了就顺手把结果留下，省一次往返。
    for (var i = 0; i < SYNC_CANDIDATES.length; i++) {
      try {
        var text = sendSync(SYNC_CANDIDATES[i], '{"c":"app.info"}');
        var parsed = JSON.parse(text);
        if (parsed && parsed.v) {
          syncBase = SYNC_CANDIDATES[i];
          window.__nmbSyncChannel = syncBase;
          return parsed.v;
        }
      } catch (error) {
        window.__nmbSyncProbeError = String(error && error.message || error);
      }
    }
    return null;
  }

  function rpc(command, payload) {
    if (!syncBase) throw new Error('同步通道不可用（无法同步拿到 ' + command + ' 的结果）');
    payload = payload || {};
    payload.c = command;
    var parsed = JSON.parse(sendSync(syncBase, JSON.stringify(payload)));
    if (parsed.s === 'err') throw new Error(parsed.m);
    return parsed.v;
  }
  window.__nmbRpc = rpc;

  // "能同步就同步、不能就用兜底值"：给那些有更好、没有也能活的读命令用。
  // 宿主拒绝（比如命令碰了 UI 线程）时把原因记下来，页面自检时能看到。
  function rpcSafe(command, payload, fallback) {
    try {
      return rpc(command, payload);
    } catch (error) {
      (window.__nmbRpcErrors = window.__nmbRpcErrors || []).push(
        command + ': ' + String((error && error.message) || error));
      return fallback;
    }
  }

  // ---------------------------------------------------------------------
  // 通道 2：动手用 —— mbQuery，异步，返回值拿不到
  // ---------------------------------------------------------------------
  var nextQueryId = 10000;

  function rpcAsync(command, payload, done) {
    var mbq = queryChannel();
    if (!mbq) {
      if (done) done(new Error('内核没有提供 mbQuery 通道'));
      return;
    }
    payload = payload || {};
    payload.c = command;
    mbq(nextQueryId++, JSON.stringify(payload), function (customMsg, response) {
      if (!done) return;
      var parsed = null;
      try { parsed = JSON.parse(response); } catch (error) { parsed = null; }
      if (!parsed || parsed.s === 'err') done(new Error(parsed ? parsed.m : '应答不是 JSON'));
      else done(null, parsed.v);
    });
  }
  window.__nmbRpcAsync = rpcAsync;

  // 只求做到、不关心结果的那类调用（窗口/菜单/托盘/热键）。失败会把原因留在
  // window.__nmbAsyncErrors 里，而不是静静咽掉。
  function tell(command, payload) {
    rpcAsync(command, payload, function (error) {
      if (!error) return;
      (window.__nmbAsyncErrors = window.__nmbAsyncErrors || []).push(command + ': ' + error.message);
      if (window.console && console.warn) console.warn('[nw] ' + command + ' 失败：' + error.message);
    });
  }
  window.__nmbTell = tell;

  // ---------------------------------------------------------------------
  // 极简事件发射器：nw 的 on/once/emit 语义
  // ---------------------------------------------------------------------
  function Emitter() { this.__handlers = {}; }
  Emitter.prototype.on = function (name, callback) {
    (this.__handlers[name] = this.__handlers[name] || []).push(callback);
    return this;
  };
  Emitter.prototype.once = function (name, callback) {
    var self = this;
    function wrapper() {
      self.removeListener(name, wrapper);
      callback.apply(null, arguments);
    }
    wrapper.__once = callback;
    return this.on(name, wrapper);
  };
  Emitter.prototype.removeListener = function (name, callback) {
    var list = this.__handlers[name];
    if (!list) return this;
    for (var i = list.length - 1; i >= 0; i--) {
      if (list[i] === callback || list[i].__once === callback) list.splice(i, 1);
    }
    return this;
  };
  Emitter.prototype.removeAllListeners = function (name) {
    if (name) delete this.__handlers[name];
    else this.__handlers = {};
    return this;
  };
  Emitter.prototype.emit = function (name) {
    var args = Array.prototype.slice.call(arguments, 1);
    var list = (this.__handlers[name] || []).slice();
    for (var i = 0; i < list.length; i++) {
      try {
        list[i].apply(this, args);
      } catch (error) {
        console.error('[nw] 事件 ' + name + ' 的处理器抛错：', error);
      }
    }
    return this;
  };

  // ---------------------------------------------------------------------
  // nw.App
  // ---------------------------------------------------------------------
  // 先把同步通道探通：这一下既拿到了 app.info，也决定了本文件后半段所有读命令能不能用。
  // 探不通也**不能**抛出去——那样 window.nw 根本建不出来，页面连自报故障的机会都没有。
  var appInfo = probeSyncChannel();
  var syncUnavailable = !appInfo;
  if (!appInfo) appInfo = { manifest: {}, argv: [], dataPath: '' };

  // 线程拓扑：宿主实测出来报给页面的（真值，不是推断）。这决定 UI 命令能不能走同步通道，
  // 自检脚本应该把它打出来。
  var threads = (appInfo.threads) || null;
  window.__nmbChannel = {
    syncBase: syncBase,
    threads: threads,
    syncSharesUi: !!(threads && threads.syncSharesUi),
    syncProbeError: window.__nmbSyncProbeError || null
  };
  // 原始 app.info 留在页面上：路径字段（appPath / startupUrl / manifestPath）不出现在
  // nw 的公开 API 里，但自检要能看见它们——路径是最容易悄悄坏掉的一类值。
  window.__nmbAppInfo = appInfo;

  var manifest = appInfo.manifest || { name: appInfo.name, version: appInfo.version, main: appInfo.startupUrl };

  var App = new Emitter();
  App.argv = appInfo.argv || [];
  App.fullArgv = App.argv;
  App.dataPath = appInfo.dataPath;
  App.manifest = manifest;
  App.filter = new Emitter();
  // 这三个动的是消息循环/进程，走异步通道（nw 里它们也不返回任何东西）。
  App.quit = function () { tell('app.quit'); };
  App.exit = function (code) { tell('app.exit', { code: code || 0 }); };
  App.restart = function () { tell('app.restart'); };
  App.clearCache = function () { /* 缓存由内核管理，无对外开关 */ };

  // 同步通道没通的话，清单/argv 用异步补齐一遍，至少别让页面读到空清单。
  if (syncUnavailable) {
    rpcAsync('app.info', {}, function (error, value) {
      if (error || !value) return;
      App.argv = value.argv || [];
      App.fullArgv = App.argv;
      App.dataPath = value.dataPath;
      if (value.manifest) App.manifest = value.manifest;
      window.__nmbAppInfo = value;
    });
  }

  // ---------------------------------------------------------------------
  // nw.Window
  // ---------------------------------------------------------------------
  var windowById = {};
  var currentId = window.__nwWindowId || 0;

  // id 可能是 null：nw.Window.open 的应答是异步回来的（win.create 要在 UI 线程上建视图），
  // 所以先把对象交出去、命令挂在 __pending 里，等 native 报来真 id 再补发（见 __resolve）。
  function WindowState(id) {
    Emitter.call(this);
    this.id = (id === undefined) ? null : id;
    this.window = window;
    this.x = 0;
    this.y = 0;
    this.width = 0;
    this.height = 0;
    this.title = document.title;
    this.isFullscreen = false;
    this.isMaximized = false;
    this.isMinimized = false;
    this.menu = null;
    this.__pending = [];
    if (this.id !== null) windowById[this.id] = this;
  }
  WindowState.prototype = Object.create(Emitter.prototype);
  WindowState.prototype.constructor = WindowState;

  // 把 native 报过来的状态合并进对象，顺便把变化转成事件。
  WindowState.prototype.sync = function (state) {
    var self = this;
    // 位置/尺寸：native 报的是 GetWindowRect 的实测值，填进对象（nw 里 width/height/x/y
    // 就是可读属性）。这里**不**发 resize/move 事件——resizeTo/moveTo 自己已经发过，
    // 同步再发一遍就成了回声（同一个动作被报两次）。
    ['x', 'y', 'width', 'height'].forEach(function (key) {
      if (state[key] !== undefined && state[key] !== null) self[key] = state[key];
    });
    ['maximized', 'minimized', 'fullscreen'].forEach(function (key) {
      var field = 'is' + key.charAt(0).toUpperCase() + key.slice(1);
      if (state[key] !== undefined && state[key] !== self[field]) {
        self[field] = state[key];
        self.emit(state[key] ? key : 'leave-' + key);
      }
    });
    if (state.title !== undefined && state.title !== this.title) {
      this.title = state.title;
      this.emit('title', state.title);
    }
    if (state.url !== undefined && state.url !== this.url) {
      this.url = state.url;
      this.emit('url', state.url);
    }
  };

  // 窗口命令的两个口子——这层分法不是审美，是内核给的约束：
  //   read() —— 同步要结果。只有宿主放行在同步通道上跑的读命令能用（目前只有 win.state）。
  //   act()  —— 只求做到。窗口/菜单/托盘/热键都必须在 UI 线程上动，走异步 query 通道；
  //             nw 里这些接口本来也不靠返回值。
  // id 还没定的窗口（Window.open 尚未结算）：命令先排队，id 到了再补发。
  WindowState.prototype.read = function (command, payload) {
    if (this.id === null) throw new Error('这个窗口还没拿到 id（win.create 尚未应答）');
    payload = payload || {};
    payload.id = this.id;
    return rpc(command, payload);
  };
  WindowState.prototype.readSafe = function (command, payload, fallback) {
    if (this.id === null) return fallback;
    payload = payload || {};
    payload.id = this.id;
    return rpcSafe(command, payload, fallback);
  };
  WindowState.prototype.act = function (command, payload) {
    payload = payload || {};
    if (this.id === null) {
      this.__pending.push({ command: command, payload: payload });
      return this;
    }
    payload.id = this.id;
    tell(command, payload);
    return this;
  };
  // native 报来真 id：登记进窗口表，把排队中的命令补发出去。
  WindowState.prototype.__resolve = function (id) {
    if (this.id === id) return this;
    this.id = id;
    windowById[id] = this;
    var queue = this.__pending;
    this.__pending = [];
    for (var i = 0; i < queue.length; i++) this.act(queue[i].command, queue[i].payload);
    return this;
  };

  WindowState.prototype.show = function (show) { this.act(show === false ? 'win.hide' : 'win.show'); return this.emit('show'); };
  WindowState.prototype.hide = function () { this.act('win.hide'); return this.emit('hide'); };
  WindowState.prototype.focus = function () { this.act('win.focus'); return this; };
  WindowState.prototype.blur = function () { this.act('win.blur'); return this; };
  WindowState.prototype.close = function (force) {
    if (!force && !this.emit('close')) return;
    // 关闭**不能**排队：窗口还没结算就关，语义是"别开了"，native 那边没有对应操作。
    if (this.id !== null) this.act('win.close');
  };
  WindowState.prototype.destroy = function () { this.close(true); };
  WindowState.prototype.maximize = function () { this.act('win.maximize'); return this; };
  WindowState.prototype.unmaximize = function () { this.act('win.unmaximize'); return this; };
  WindowState.prototype.minimize = function () { this.act('win.minimize'); return this; };
  WindowState.prototype.restore = function () { this.act('win.restore'); return this; };
  WindowState.prototype.enterFullscreen = function () { this.act('win.fullscreen'); return this.emit('fullscreen'); };
  WindowState.prototype.leaveFullscreen = function () { this.act('win.leaveFullscreen'); return this.emit('leave-fullscreen'); };
  WindowState.prototype.toggleFullscreen = function () {
    return this.isFullscreen ? this.leaveFullscreen() : this.enterFullscreen();
  };
  WindowState.prototype.moveTo = function (x, y) { this.act('win.moveTo', { x: x, y: y }); this.x = x; this.y = y; return this; };
  WindowState.prototype.resizeTo = function (width, height) {
    if (width < 0 || height < 0) throw new Error('resizeTo 不接受负数');
    this.act('win.resizeTo', { width: width, height: height });
    this.width = width;
    this.height = height;
    return this.emit('resize', width, height);
  };
  WindowState.prototype.moveBy = function (x, y) { return this.moveTo(this.x + x, this.y + y); };
  WindowState.prototype.resizeBy = function (width, height) { return this.resizeTo(this.width + width, this.height + height); };
  WindowState.prototype.setResizable = function (value) { this.act('win.setResizable', { value: !!value }); return this; };
  WindowState.prototype.setAlwaysOnTop = function (value) { this.act('win.setAlwaysOnTop', { value: !!value }); return this; };
  WindowState.prototype.setTitle = function (title) { this.act('win.setTitle', { title: title }); this.title = title; return this; };
  WindowState.prototype.reload = function () { this.act('win.reload'); return this; };
  WindowState.prototype.reloadIgnoringCache = WindowState.prototype.reload;
  WindowState.prototype.back = function () { this.act('win.back'); return this; };
  WindowState.prototype.forward = function () { this.act('win.forward'); return this; };
  // 历史栈在 Blink 里，同步通道拿不到（重入红线）。宿主放行时给真值，否则老实回 false，
  // 并把原因留在 window.__nmbRpcErrors 里——别编一个看起来像真话的值。
  WindowState.prototype.canGoBack = function () { return this.readSafe('win.canGoBack', {}, false).value === true; };
  WindowState.prototype.canGoForward = function () { return this.readSafe('win.canGoForward', {}, false).value === true; };
  WindowState.prototype.load = WindowState.prototype.reload;
  WindowState.prototype.navigate = function (url) { this.act('win.loadURL', { url: url }); return this; };
  WindowState.prototype.eval = function (code) {
    // 内核的 mbRunJs 会把脚本最后一条表达式的值回给我们，所以原样送过去即可
    // （别自作聪明包一层函数，那样语句形式的 code 就跑不了了）。
    // 这条是同步通道的 Blink 重入红线之一，宿主会明确拒绝；nw 应用里用它取值的写法
    // 需要改成事件/回调驱动，或者把逻辑放进页面自己执行。
    return this.read('win.eval', { code: String(code) }).value;
  };
  WindowState.prototype.evalNWBin = function () { throw new Error('nw 的二进制脚本（v8 snapshot）本实现不支持'); };
  WindowState.prototype.showDevTools = function (show) { this.act('win.devtools', { show: show !== false }); return this; };
  WindowState.prototype.closeDevTools = function () { this.act('win.devtools', { show: false }); return this; };
  WindowState.prototype.setMenu = function (menu) {
    // nw.Menu 的 popup/attach 都走这里：把菜单树序列化给 native 建 HMENU。
    this.menu = menu;
    if (!menu) { this.act('menu.clear'); return this; }
    if (this.id === null) {
      // 窗口还没结算：菜单树先排队，等 id 到了由 __resolve 补发（menu.set 带 id）。
      this.__pending.push({ command: 'menu.set', payload: { items: serializeMenu(menu.items), type: menu.type } });
      return this;
    }
    menu.attach(this.id);
    return this;
  };
  WindowState.prototype.setZoom = function (factor) { this.act('win.setZoom', { factor: factor }); return this; };
  WindowState.prototype.setShowInTaskbar = function () { return this; };
  WindowState.prototype.setProgressBar = function () { return this; };
  WindowState.prototype.capturePage = function () {
    throw new Error('capturePage 需要内核的离屏抓帧接口，当前桥未暴露');
  };
  WindowState.prototype.get = function () { return this; };
  WindowState.prototype.syncNow = function () {
    var state = this.readSafe('win.state', {}, null);
    if (state) this.sync(state);
    return this;
  };

  var Window = {
    get: function (id) {
      if (id === undefined || id === null) id = currentId;
      if (windowById[id]) return windowById[id];
      var state = new WindowState(id);
      // win.state 只读 native 的窗口字段，宿主放行它在同步通道上跑；万一被拒也退化成
      // "只知道 id 的对象"，setTitle / on('close') 这些照样能用。
      var detail = state.readSafe('win.state', {}, null);
      if (detail) state.sync(detail);
      return state;
    },
    open: function (url, options) {
      var config = {};
      for (var key in (options || {})) config[key] = options[key];
      config.url = url;
      // win.create 要在 UI 线程上建视图，只能走异步。nw 的 Window.open 是同步返回句柄的，
      // 所以先给一个 id 待定的对象——它上面的命令会排队，等 native 报了 id 再补发。
      var state = new WindowState(null);
      rpcAsync('win.create', { config: JSON.stringify(config) }, function (error, value) {
        if (error || !value) {
          state.emit('error', error || new Error('win.create 没有给出窗口号'));
          return;
        }
        state.__resolve(value.id);
        var detail = state.readSafe('win.state', {}, null);
        if (detail) state.sync(detail);
        state.emit('loaded');
      });
      return state;
    },
    getAll: function () {
      // 窗口表由 UI 线程持有，同步通道只读它不安全，所以没进同步白名单。
      // 拿不到就退化成"至少把当前窗口给出去"，而不是把调用方炸掉。
      var list = rpcSafe('win.list', {}, null);
      if (!list) return [Window.getCurrent()];
      return list.map(function (entry) { return Window.get(entry.id); });
    },
    getCurrent: function () { return Window.get(currentId); }
  };

  // ---------------------------------------------------------------------
  // nw.Menu / nw.MenuItem
  // ---------------------------------------------------------------------
  var menuClickRegistry = {};
  var menuClickSeq = 1001;

  // native 建 HMENU 时用的 id 必须和这里的注册表一一对应，否则点菜单找不到回调。
  // 所以 id 由 JS 分配并通过 "id" 字段交给 native（native 只在缺 id 时自己编号）。
  function serializeMenu(items) {
    return items.map(function (item) {
      if (!item) return { type: 'separator' };
      var plain = {
        type: item.type || 'normal',
        label: item.label || '',
        enabled: item.enabled !== false,
        checked: !!item.checked
      };
      if (plain.type === 'separator') return plain;
      if (typeof item.click === 'function') {
        plain.id = menuClickSeq++;
        menuClickRegistry[plain.id] = item.click;
      }
      if (item.submenu && item.submenu.items) plain.submenu = serializeMenu(item.submenu.items);
      return plain;
    });
  }

  function MenuItem(options) {
    options = options || {};
    this.type = options.type || 'normal';
    this.label = options.label || '';
    this.enabled = options.enabled !== false;
    this.checked = !!options.checked;
    this.click = options.click || null;
    this.submenu = options.submenu || null;
  }

  function Menu(options) {
    options = options || {};
    this.type = options.type || 'menubar';
    this.items = [];
  }
  Menu.prototype.append = function (item) { this.items.push(item); return this; };
  Menu.prototype.insert = function (item, at) { this.items.splice(at, 0, item); return this; };
  Menu.prototype.remove = function (item) {
    var at = this.items.indexOf(item);
    if (at >= 0) this.items.splice(at, 1);
    return this;
  };
  Menu.prototype.removeAt = function (at) { this.items.splice(at, 1); return this; };
  Menu.prototype.popup = function (x, y) {
    tell('menu.popup', { x: x || 0, y: y || 0, items: serializeMenu(this.items) });
    return this;
  };
  Menu.prototype.attach = function (windowId) {
    if (windowId === null || windowId === undefined) return this;   // 窗口还没结算，见 setMenu 的排队分支
    tell('menu.set', { id: windowId, items: serializeMenu(this.items), type: this.type });
    return this;
  };

  // native 只会回一个菜单 id，这里把 id 映射回函数。
  window.__nmbMenuClick = function (id) {
    var callback = menuClickRegistry[id];
    if (typeof callback === 'function') callback();
  };

  // ---------------------------------------------------------------------
  // nw.Tray
  // ---------------------------------------------------------------------
  function Tray(options) {
    Emitter.call(this);
    options = options || {};
    this.title = options.title || '';
    this.tooltip = options.tooltip || '';
    this.id = null;
    if (options.icon !== undefined) this.__create(options);
  }
  Tray.prototype = Object.create(Emitter.prototype);
  Tray.prototype.constructor = Tray;
  // 建托盘图标要在 UI 线程上做（Shell_NotifyIcon 的宿主窗口在那边），只能异步拿 id。
  // 还没结算就 remove() 的：这里只清本地登记，native 那边图标建好后会一直挂着——
  // 这是本实现已知的粗糙点，写出来免得被当成意外。
  Tray.prototype.__create = function (options) {
    var self = this;
    rpcAsync('tray.create', {
      tooltip: this.tooltip || this.title || document.title,
      icon: typeof options.icon === 'string' ? options.icon : ''
    }, function (error, value) {
      if (error || !value) return;      // 托盘建不起来就静默降级，不该拖垮页面
      self.id = value.id;
      trayRegistry[self.id] = self;
    });
  };
  Tray.prototype.remove = function () {
    if (this.id !== null) tell('tray.remove', { id: this.id });
    delete trayRegistry[this.id];
    this.id = null;
  };
  Tray.prototype.setTitle = function (title) { return this.setTooltip(title); };
  Tray.prototype.setTooltip = function (tooltip) {
    this.tooltip = tooltip;
    if (this.id !== null) tell('tray.setTooltip', { id: this.id, tooltip: tooltip });
    return this;
  };
  Tray.prototype.setIcon = function () { return this; };
  Tray.prototype.setMenu = function (menu) { this.menu = menu; return this; };

  var trayRegistry = {};
  window.__nmbTrayEvent = function (id, event) {
    var tray = trayRegistry[id];
    if (!tray) return;
    if (event === 'right-click' && tray.menu) return tray.menu.popup();
    tray.emit(event);
  };

  // ---------------------------------------------------------------------
  // nw.Clipboard / nw.Shell / nw.Screen / nw.Shortcut
  // ---------------------------------------------------------------------
  function Clipboard() {}
  Clipboard.prototype.get = function (type) { return rpc('clipboard.read', { type: type || 'text' }).data; };
  Clipboard.prototype.set = function (data, type) {
    return rpc('clipboard.write', { data: String(data), type: type || 'text' });
  };
  Clipboard.prototype.clear = function () { return rpc('clipboard.clear'); };
  Clipboard.prototype.readAvailableTypes = function () { return ['text']; };

  function Shell() {}
  Shell.prototype.openExternal = function (uri) { return rpc('shell.openExternal', { target: uri }); };
  Shell.prototype.openItem = function (path) { return rpc('shell.openItem', { target: path }); };
  Shell.prototype.showItemInFolder = function (path) { return rpc('shell.showItemInFolder', { target: path }); };
  Shell.prototype.beep = function () { return rpc('shell.beep'); };

  function Screen() {}
  Screen.prototype.Init = function () {};
  Screen.prototype.screens = function () { return rpc('screen.screens'); };

  // nw.js 里这三个是"命名空间式"的用法：不 new，直接在构造器上取。
  //   nw.Shell.openExternal(uri) / nw.Screen.screens() / nw.Clipboard.get()
  // 但剪贴板的**实例**方法也叫 get()（那个是"读文本"），和静态 get() 不是一回事，
  // 所以它排除在搬运之外，静态 get 单独装（返回单例）。
  function exposeStatics(ctor, skip) {
    for (var name in ctor.prototype) {
      if (skip && skip.indexOf(name) >= 0) continue;
      if (typeof ctor.prototype[name] === 'function') ctor[name] = ctor.prototype[name];
    }
  }
  exposeStatics(Shell);
  exposeStatics(Screen);
  exposeStatics(Clipboard, ['get']);

  var clipboardSingleton = null;
  Clipboard.get = function () {
    if (!clipboardSingleton) clipboardSingleton = new Clipboard();
    return clipboardSingleton;
  };

  function Shortcut() { this.__ids = {}; }
  Shortcut.prototype.register = function (accelerator, callback) {
    var id = (Object.keys(this.__ids).length + 1) * 1000;
    this.__ids[id] = callback;
    shortcutRegistry[id] = callback;
    // callback 传的是注册表里的 key 字符串：native 触发时只会把这段字符串原样回给页面。
    // RegisterHotKey 绑在窗口所属线程上，所以这条只能走异步通道。
    tell('shortcut.register', { id: id, key: accelerator, callback: String(id), windowId: currentId });
    return true;
  };
  Shortcut.prototype.unregister = function (accelerator) {
    // nw 的 unregister 收的是快捷键串而不是 id，这里反查一次。
    for (var id in this.__ids) tell('shortcut.unregister', { id: parseInt(id, 10) });
    this.__ids = {};
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

  // ---------------------------------------------------------------------
  // 内核/桥推上来的事件
  // ---------------------------------------------------------------------
  window.__nmbEvent = function (name, value) {
    var state = windowById[currentId];
    if (!state) return;
    if (name === 'title') state.sync({ title: value });
    if (name === 'url') state.sync({ url: value });
  };

  window.__nmbOpenWindow = function (url) {
    return Window.open(url, {});
  };

  // 关窗前问一句页面：nw 应用常见的"有未保存内容就拦一下"。
  window.__nmbCloseVerdict = function () {
    var state = windowById[currentId];
    if (!state) return 'close';
    var vetoed = false;
    var list = state.__handlers.close || [];
    for (var i = 0; i < list.length; i++) {
      var result = list[i]();
      if (result === false) vetoed = true;
    }
    return vetoed ? 'cancel' : 'close';
  };

  // window.open → nw.Window.open（nw.js 里也是这么接管的）
  var nativeOpen = window.open;
  window.open = function (url, name, features) {
    try {
      var options = {};
      if (typeof features === 'string') {
        features.split(',').forEach(function (part) {
          var pieces = part.split('=');
          if (pieces.length === 2) options[pieces[0].trim()] = isNaN(pieces[1]) ? pieces[1] : parseInt(pieces[1], 10);
        });
      }
      return Window.open(url, options);
    } catch (error) {
      return nativeOpen ? nativeOpen.apply(window, arguments) : null;
    }
  };

  // ---------------------------------------------------------------------
  // 组装 nw 对象
  // ---------------------------------------------------------------------
  window.nw = {
    App: App,
    Window: Window,
    Menu: Menu,
    MenuItem: MenuItem,
    Tray: Tray,
    Clipboard: Clipboard,
    Shell: Shell,
    Screen: Screen,
    Shortcut: Shortcut,
    Object: function () {},
    require: window.__nmbRequire || undefined
  };
  // nw_node.js 要复用它来造自己的事件对象（node 的 events 模块）。
  window.nw.__Emitter = Emitter;

  // 当前窗口对象先建出来并登记，页面脚本可以直接 nw.Window.get()。
  Window.get(currentId);
})();
