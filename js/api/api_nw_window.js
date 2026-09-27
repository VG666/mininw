// api_nw_window.js —— nw.Window / WindowState，含 nw.Window.open。
// 对应 SDK：src/resources/api_nw_window.js + api_nw_newwin.js。
// title / zoomLevel 在 SDK 里是**访问器**（读走 currentWindowInternal，写走 setXxxInternal），
// 不是普通字段——所以 sync() 只许写 __title 这种内部字段，别直接赋 this.title（那会变成设覆盖值）。

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
    // title / zoomLevel 是访问器（见下面 defineProperty），内部值只能写 __title/__zoom，
    // 构造期尤其不能写 this.title——那会走 setter 去发 win.setTitle（此时 __pending 都还没有）。
    this.__title = document.title;
    this.__zoom = 1;
    this.isFullscreen = false;
    this.isMaximized = false;
    this.isMinimized = false;
    this.menu = null;
    this.__pending = [];
    if (this.id !== null) windowById[this.id] = this;
  }
  WindowState.prototype = Object.create(Emitter.prototype);
  WindowState.prototype.constructor = WindowState;

  // title / zoomLevel 是**访问器**——SDK 里读走 currentWindowInternal.getTitleInternal/getZoom、
  // 写走 setTitleInternal/setZoom，不是普通字段（api_nw_window.js 用的是 __defineGetter__）。
  //   get —— 最近一次生效值：title 来自 native 的 __title 推送（标题栏上的就是它），
  //          zoom 来自 win.state 带回来的镜像。
  //   set —— 显式赋值 = 设**覆盖值**：此后页面再改 document.title 也不动标题栏，直到导航到
  //          新文档（nw.js 里这叫 title_override，native 侧对应 win.setTitle / win.setZoom）。
  Object.defineProperty(WindowState.prototype, 'title', {
    get: function () { return this.__title; },
    set: function (value) {
      value = String(value);
      this.__title = value;
      this.act('win.setTitle', { title: value });
    }
  });
  Object.defineProperty(WindowState.prototype, 'zoomLevel', {
    get: function () { return this.__zoom; },
    set: function (value) {
      value = Number(value);
      if (!isFinite(value) || value <= 0) throw new Error('zoomLevel 必须是正数');
      this.__zoom = value;
      this.act('win.setZoom', { factor: value });
    }
  });

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
    // 写 __title 而不是 this.title：后者是访问器，赋值会反过来再发一条 win.setTitle
    // （native 推来的状态又被当成本地改动送回去，变成回声）。
    if (state.title !== undefined && state.title !== this.__title) {
      this.__title = state.title;
      this.emit('title', state.title);
    }
    // zoom 同理，但**不发事件**：nw 没有 zoom 事件，而且这个值本来就来自 setZoom 自己。
    if (state.zoom !== undefined && state.zoom !== null && state.zoom !== this.__zoom) {
      this.__zoom = state.zoom;
    }
    if (state.url !== undefined && state.url !== this.url) {
      this.url = state.url;
      this.emit('url', state.url);
    }
  };

  // 窗口命令的三个口子——这层分法不是审美，是内核给的约束：
  //   read() —— 同步要结果。只有宿主放行在同步通道上跑的读命令能用（目前只有 win.state）。
  //   act()  —— 只求做到。窗口/菜单/托盘/热键都必须在 UI 线程上动，走异步 query 通道；
  //             nw 里这些接口本来也不靠返回值。
  // 具体走哪条通道、以及"命令能不能同步"的判断都在 api_nw_internal.js（对应 SDK 的
  // api_window_internal.js），这里只负责对象语义和 id 的排队补发。
  // id 还没定的窗口（Window.open 尚未结算）：命令先排队，id 到了再补发。
  WindowState.prototype.read = function (command, payload) {
    if (this.id === null) throw new Error('这个窗口还没拿到 id（win.create 尚未应答）');
    return internalRead(command, this.id, payload);
  };
  WindowState.prototype.readSafe = function (command, payload, fallback) {
    if (this.id === null) return fallback;
    return internalReadSafe(command, this.id, payload, fallback);
  };
  WindowState.prototype.act = function (command, payload) {
    if (this.id === null) {
      this.__pending.push({ command: command, payload: payload });
      return this;
    }
    internalTell(command, this.id, payload);
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
  // 原生拖动，在 mousedown 里调用。native 侧自己 SetCapture + SetWindowPos 跟随鼠标：
  // 本实现的无边框窗口是 WS_POPUP 且没有 caption，系统不给它走标题栏拖动。
  // 走异步通道下发，拖动全过程不会阻塞页面。
  WindowState.prototype.startDrag = function () { this.act('win.startDrag'); return this; };
  WindowState.prototype.resizeBy = function (width, height) { return this.resizeTo(this.width + width, this.height + height); };
  WindowState.prototype.setResizable = function (value) { this.act('win.setResizable', { value: !!value }); return this; };
  WindowState.prototype.setAlwaysOnTop = function (value) { this.act('win.setAlwaysOnTop', { value: !!value }); return this; };
  // kiosk = 全屏 + 置顶（native 侧 ApplyKiosk 按先置顶后吃边框的顺序处理，反了会闪一帧）。
  WindowState.prototype.enterKioskMode = function () { this.act('win.enterKiosk'); return this; };
  WindowState.prototype.leaveKioskMode = function () { this.act('win.leaveKiosk'); return this; };
  WindowState.prototype.toggleKioskMode = function () { this.act('win.toggleKiosk'); return this; };
  // 0 = 不限制该方向。native 在两处生效：WM_GETMINMAXINFO 拦拖拽、moveTo/resizeTo 编程钳制。
  WindowState.prototype.setMinimumSize = function (width, height) { this.act('win.setMinimumSize', { width: width, height: height }); return this; };
  WindowState.prototype.setMaximumSize = function (width, height) { this.act('win.setMaximumSize', { width: width, height: height }); return this; };
  // nw 0.12 遗留：'center' / 'mouse' 两个预置位（锚点都是窗口中心）。
  WindowState.prototype.setPosition = function (position) {
    if (position !== 'center' && position !== 'mouse') throw new Error("setPosition 只接受 'center' 或 'mouse'");
    return this.act('win.setPosition', { position: position });
  };
  // value 为 true 表示停止闪烁（FLASHW_STOP），缺省开始闪到窗口拿到焦点为止。
  WindowState.prototype.requestAttention = function (value) { this.act('win.requestAttention', { value: value === true }); return this; };
  // Windows 上"所有工作区可见"就是 TOPMOST（与 setAlwaysOnTop 共用同一位）；
  // canSet 在 Windows 恒真——它是给 Mac/Linux 判能力用的，这里静态回答。
  WindowState.prototype.setVisibleOnAllWorkspaces = function (value) { this.act('win.setVisibleOnAllWorkspaces', { value: value !== false }); return this; };
  WindowState.prototype.canSetVisibleOnAllWorkspaces = function () { return true; };
  // 鼠标穿透（WS_EX_TRANSPARENT）；常与 setTransparent 配合做悬浮层。
  WindowState.prototype.setIgnoreMouseEvents = function (value) { this.act('win.setIgnoreMouseEvents', { value: !!value }); return this; };
  // 内核视图透明开关（nw 已标注弃用，但 0.14 仍可用）。
  WindowState.prototype.setTransparent = function (value) { this.act('win.setTransparent', { value: !!value }); return this; };
  // kiosk = 全屏 + 置顶（native 侧 ApplyKiosk 按先置顶后吃边框的顺序处理，反了会闪一帧）。
  WindowState.prototype.enterKioskMode = function () { this.act('win.enterKiosk'); return this; };
  WindowState.prototype.leaveKioskMode = function () { this.act('win.leaveKiosk'); return this; };
  WindowState.prototype.toggleKioskMode = function () { this.act('win.toggleKiosk'); return this; };
  // 0 = 不限制该方向。native 在两处生效：WM_GETMINMAXINFO 拦拖拽、moveTo/resizeTo 编程钳制。
  WindowState.prototype.setMinimumSize = function (width, height) { this.act('win.setMinimumSize', { width: width, height: height }); return this; };
  WindowState.prototype.setMaximumSize = function (width, height) { this.act('win.setMaximumSize', { width: width, height: height }); return this; };
  // nw 0.12 遗留：'center' / 'mouse' 两个预置位（锚点都是窗口中心）。
  WindowState.prototype.setPosition = function (position) {
    if (position !== 'center' && position !== 'mouse') throw new Error("setPosition 只接受 'center' 或 'mouse'");
    return this.act('win.setPosition', { position: position });
  };
  // value 为 true 表示停止闪烁（FLASHW_STOP），缺省开始闪到窗口拿到焦点为止。
  WindowState.prototype.requestAttention = function (value) { this.act('win.requestAttention', { value: value === true }); return this; };
  // Windows 上"所有工作区可见"就是 TOPMOST（与 setAlwaysOnTop 共用同一位）；
  // canSet 在 Windows 恒真——它是给 Mac/Linux 判能力用的，这里静态回答。
  WindowState.prototype.setVisibleOnAllWorkspaces = function (value) { this.act('win.setVisibleOnAllWorkspaces', { value: value !== false }); return this; };
  WindowState.prototype.canSetVisibleOnAllWorkspaces = function () { return true; };
  // 鼠标穿透（WS_EX_TRANSPARENT）；常与 setTransparent 配合做悬浮层。
  WindowState.prototype.setIgnoreMouseEvents = function (value) { this.act('win.setIgnoreMouseEvents', { value: !!value }); return this; };
  // 内核视图透明开关（nw 已标注弃用，但 0.14 仍可用）。
  WindowState.prototype.setTransparent = function (value) { this.act('win.setTransparent', { value: !!value }); return this; };
  // setTitle / setZoom 是访问器的函数形态（nw 应用两种写法都有），别在这儿再发一遍命令。
  WindowState.prototype.setTitle = function (title) { this.title = title; return this; };
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
  WindowState.prototype.setZoom = function (factor) { this.zoomLevel = factor; return this; };
  // 任务栏按钮显隐（WS_EX_APPWINDOW/TOOLWINDOW）；native 侧会先藏再显让按钮立即重算。
  WindowState.prototype.setShowInTaskbar = function (show) { this.act('win.setShowInTaskbar', { value: show !== false }); return this; };
  // 任务栏进度条（ITaskbarList3）：<0 移除，>1 不确定态（跑马灯），0..1 百分比。
  WindowState.prototype.setProgressBar = function (value) {
    value = Number(value);
    if (!isFinite(value)) throw new Error('setProgressBar 需要数字');
    return this.act('win.setProgressBar', { value: value });
  };
  // 文档标注 Mac only：Windows 的 nw.js 同样是 no-op，如实对齐。
  WindowState.prototype.setBadgeLabel = function () { return this; };
  // 抓屏走异步通道（PrintWindow 在 UI 线程上跑，同步白名单没放行）。宿主回 RGBA 裸
  // 像素的 base64，这里组 canvas 转 dataURL——nw 的回调拿的就是 data:image/png;base64,...。
  // 签名 capturePage(callback [, type])；不给 callback 则返回 Promise（nw 0.13+ 的用法）。
  WindowState.prototype.capturePage = function (callback, type) {
    var wantType = (typeof callback === 'string' ? callback : (typeof type === 'string' ? type : 'png')).toLowerCase();
    if (wantType !== 'png' && wantType !== 'jpeg') throw new Error("capturePage 只支持 'png' / 'jpeg'");
    var self = this;
    function run(done) {
      if (self.id === null) { done(new Error('这个窗口还没拿到 id（win.create 尚未应答）')); return; }
      rpcAsync('win.capturePage', { id: self.id }, function (error, value) {
        if (error) { done(error); return; }
        var canvas = document.createElement('canvas');
        canvas.width = value.width;
        canvas.height = value.height;
        var context = canvas.getContext('2d');
        var image = context.createImageData(value.width, value.height);
        var bin = atob(value.data);
        for (var i = 0; i < bin.length && i < image.data.length; i++) image.data[i] = bin.charCodeAt(i);
        context.putImageData(image, 0, 0);
        done(null, canvas.toDataURL('image/' + wantType));
      });
    }
    if (typeof callback === 'function') {
      run(function (error, url) {
        if (error) console.error('[nw] capturePage 失败：' + error.message);
        else callback(url);
      });
      return this;
    }
    return new Promise(function (resolve, reject) {
      run(function (error, url) { return error ? reject(error) : resolve(url); });
    });
  };
  WindowState.prototype.get = function () { return this; };
  // 打印机枚举：宿主只回名字数组，这里补成 nw 的 printer 对象形状（{name: ...}）。
  // 签名 getPrinters(callback)（0.12 旧签名带 info 参数，兼容着接）。
  WindowState.prototype.getPrinters = function (info, callback) {
    var done = typeof info === 'function' ? info : callback;
    rpcAsync('win.getPrinters', { id: this.id }, function (error, list) {
      if (!done) return;
      if (error) { console.error('[nw] getPrinters 失败：' + error.message); done([]); return; }
      done((list || []).map(function (name) { return { name: name }; }));
    });
  };
  // 打印：内核无打印栈，native 会 Fail（原因记进 __nmbAsyncErrors），这里只如实转发。
  WindowState.prototype.print = function (options) { return this.act('win.print', { options: options || {} }); };
  // 停止加载当前文档。远程窗口句柄上没有对应的 stop 命令，只能作用于本页面。
  WindowState.prototype.stop = function () { if (this.window && this.window.stop) this.window.stop(); return this; };
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
