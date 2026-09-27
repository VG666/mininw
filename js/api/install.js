// install.js —— 装配层：把上述模块缝成 window.nw，接内核/桥推上来的事件入口，接管 window.open。
// 对应 SDK 里 nw.require('nw') 装出来的那一层（不含模块实现本身）。
// 它必须最后加载：前面所有模块都在这里被引用（在 modules.txt 里排最后）。

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
    // nw.Object 在 SDK 里是"JS 对象 ↔ native 句柄"的桥（api_nw_object.js：allocateId /
    // create / destroy / callObjectMethod），本实现没有 native 对象后端，所以它只是个占位。
    // 留着是因为自检要求这几个构造器齐备，而且 nw 应用的特性探测大多只看"有没有"。
    Object: function () {},
    require: window.__nmbRequire || undefined
  };
  // nw_node.js 要复用它来造自己的事件对象（node 的 events 模块）。
  window.nw.__Emitter = Emitter;

  // 当前窗口对象先建出来并登记，页面脚本可以直接 nw.Window.get()。
  Window.get(currentId);
