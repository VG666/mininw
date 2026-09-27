// api_nw_tray.js —— nw.Tray。对应 SDK：src/resources/api_nw_tray.js。

  function Tray(options) {
    Emitter.call(this);
    if (typeof options === 'string') options = { icon: options };   // new Tray('icon.png') 的便捷形态
    options = options || {};
    this.title = options.title || '';
    this.tooltip = options.tooltip || '';
    this.icon = typeof options.icon === 'string' ? options.icon : '';
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
      // 创建结算前就调过 setIcon 的：这里补发一次，别把图标留在队列里。
      if (self.__iconQueued) tell('tray.setIcon', { id: self.id, icon: self.__iconQueued });
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
  // 图标路径相对应用根（native 侧解析加载）。id 未结算先记账，创建应答到了再补发。
  Tray.prototype.setIcon = function (icon) {
    this.__iconQueued = icon;
    if (this.id !== null) tell('tray.setIcon', { id: this.id, icon: icon });
    return this;
  };
  Tray.prototype.setMenu = function (menu) { this.menu = menu; return this; };

  var trayRegistry = {};
  window.__nmbTrayEvent = function (id, event) {
    var tray = trayRegistry[id];
    if (!tray) return;
    if (event === 'right-click' && tray.menu) return tray.menu.popup();
    tray.emit(event);
  };
