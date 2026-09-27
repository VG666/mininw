// api_nw_menu.js —— nw.Menu（menubar / popup）与菜单点击回调的入口。
// 对应 SDK：src/resources/api_nw_menu.js。

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
