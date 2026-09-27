// api_nw_menuitem.js —— nw.MenuItem 与"菜单树 → 可 JSON 化的纯对象"序列化。
// 对应 SDK：src/resources/api_nw_menuitem.js。
// 菜单回调的 id 由这里分配：native 建 HMENU 时原样用回去，点击才能找回函数（见 api_nw_menu.js 的注册表）。

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
