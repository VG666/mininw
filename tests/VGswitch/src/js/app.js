'use strict';

/**
 * VG Switch —— nw 运行时示例应用
 * 页面结构：标题栏（品牌 / 状态 / 项目 / 主题 / 窗口动作 / 新增）+ 供应商卡片列表。
 * 数据落在 nw.App.dataPath 下的 providers.json，纯同步 fs 接口。
 */
(function () {

  var MINUTE = 60 * 1000;
  var DAY = 24 * 60 * MINUTE;

  var hasNw = typeof window.nw !== 'undefined' && !!window.nw.Window;
  var win = hasNw ? window.nw.Window.get() : null;
  var fs = null;
  var path = null;
  var httpMod = null;
  var httpsMod = null;
  var urlMod = null;
  var storageModulesLoaded = false;
  var proxyModulesLoaded = false;
  var nativeRequire = null;     // 内核原生 node 入口（才有 http/https）
  var nodeBuffer = null;        // 原生 node 的 Buffer，和 http 模块同源
  var proxyLoadError = '';      // 取不到 http 模块时的原因，直接给用户看

  function loadStorageModules() {
    if (storageModulesLoaded) return !!fs && !!path;
    storageModulesLoaded = true;
    try {
      fs = window.require('fs');
      path = window.require('path');
    } catch (err) {
      fs = null;
      path = null;
    }
    return !!fs && !!path;
  }

  // 本机运行时有**两套** require：
  //   1) window.require / nw.require —— RPC 兼容层，只有 fs/path/os/url 等纯 JS 模块，
  //      **没有 http/net/tls**，所以监听端口不能走它（实测 require('http') 直接抛
  //      "Cannot find module 'http'"）。
  //   2) 内核原生 node 入口 —— 才有真 http/https；形态随版本不同：nw.requireNative 可能是
  //      函数，也可能是 { require } 对象，mbRequire / miniNodeRequire 是内核内部名。
  // 这里逐个探测并缓存，拿不到就明确报错而不是假装可用。
  function pickNativeRequire() {
    if (nativeRequire) return nativeRequire;
    function bind(fn) { return function (name) { return fn.call(window, name); }; }
    if (hasNw) {
      if (typeof nw.requireNative === 'function') nativeRequire = bind(nw.requireNative);
      else if (nw.requireNative && typeof nw.requireNative.require === 'function') nativeRequire = bind(nw.requireNative.require);
      else if (typeof window.mbRequire === 'function') nativeRequire = bind(window.mbRequire);
      else if (typeof window.miniNodeRequire === 'function') nativeRequire = bind(window.miniNodeRequire);
    }
    return nativeRequire;
  }

  function loadProxyModules() {
    if (proxyModulesLoaded) return !!httpMod;
    proxyModulesLoaded = true;
    var req = pickNativeRequire();
    if (!req) {
      proxyLoadError = '内核没有可用的原生 Node 入口，拿不到 http 模块';
      return false;
    }
    try {
      httpMod = req('http');
      httpsMod = req('https');
      urlMod = req('url');
      nodeBuffer = req('buffer') ? req('buffer').Buffer : window.Buffer;
    } catch (err) {
      proxyLoadError = (err && err.message) || String(err);
      httpMod = null;
      httpsMod = null;
      urlMod = null;
    }
    return !!httpMod;
  }

  /** 请求体拼接/长度要跟 http 模块同源（原生 node 的 Buffer 与兼容层的 Uint8Array 不是一回事）。 */
  function concatChunks(chunks) {
    return (nodeBuffer || window.Buffer).concat(chunks);
  }

  function byteLength(text) {
    return (nodeBuffer || window.Buffer).byteLength(text);
  }

  // ---------------------------------------------------------------- 数据

  /** 项（组内一个具体模型/上游）字段默认值：新增字段统一在这里补，避免老数据缺项。 */
  function newProvider(fields) {
    var item = {
      id: 'p' + Date.now().toString(36) + Math.floor(Math.random() * 1e4).toString(36),
      name: '',            // 上送给上游的具体模型名（如 gpt-4o）
      baseUrl: '',         // 上游 base，如 https://api.openai.com/v1
      apiKey: '',          // 上游密钥
      enabled: true,       // 是否启用（关闭则自动选择不会选用）
      color: '#7c5cff',
      headroom: null       // 单独设置的 Headroom：{ enabled:true, url:'' }；null 表示用全局
    };
    for (var key in fields) {
      if (Object.prototype.hasOwnProperty.call(fields, key)) item[key] = fields[key];
    }
    return item;
  }

  /** 组内“项”调色板，用于头像配色。 */
  var ITEM_PALETTE = ['#7c5cff', '#2f9e5f', '#2f6fed', '#e0603a', '#d94f8a', '#12a5a5', '#c9932f'];

  function seedGroups() {
    return [
      {
        id: 'g-default', name: 'gpt-4o', autoSelect: false, activeItemId: null,
        items: [
          newProvider({
            id: 'p-openai', name: 'gpt-4o', color: ITEM_PALETTE[1],
            baseUrl: 'https://api.openai.com/v1', apiKey: ''
          }),
          newProvider({
            id: 'p-azure', name: 'gpt-4o', color: ITEM_PALETTE[2],
            baseUrl: 'https://my-resource.openai.azure.com', apiKey: ''
          })
        ]
      }
    ];
  }

  var state = {
    proxy: { enabled: false, port: 18080, key: '' },
    headroom: { enabled: false, url: '' },
    minimizeToTray: true,   // 关闭窗口时最小化到托盘（托盘不可用时自动按退出处理）
    groups: seedGroups(),
    activeGroupId: null
  };

  function dataFile() {
    if (!loadStorageModules()) return '';
    var base = '';
    try {
      base = (hasNw && nw.App && nw.App.dataPath) ? nw.App.dataPath : process.cwd();
    } catch (err) {
      base = '';
    }
    return path && base ? path.join(base, 'providers.json') : '';
  }

  function load() {
    var file = dataFile();
    if (!fs || !file || !fs.existsSync(file)) return;
    try {
      var raw = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (!raw) return;
      state.minimizeToTray = raw.minimizeToTray !== false;
      state.proxy = {
        enabled: !!(raw.proxy && raw.proxy.enabled),
        port: Number(raw.proxy && raw.proxy.port) || 18080,
        key: (raw.proxy && raw.proxy.key) || ''
      };
      state.headroom = {
        enabled: !!(raw.headroom && raw.headroom.enabled),
        url: (raw.headroom && raw.headroom.url) || ''
      };
      if (raw.groups && raw.groups.length) {
        var groups = [];
        for (var g = 0; g < raw.groups.length; g++) {
          var rg = raw.groups[g] || {};
          var rawItems = rg.items || [];
          var items = [];
          for (var i = 0; i < rawItems.length; i++) items.push(migrateItem(rawItems[i]));
          groups.push({
            id: rg.id || ('g' + Date.now().toString(36) + g),
            name: rg.name || '未命名组',
            items: items,
            autoSelect: !!rg.autoSelect,
            activeItemId: rg.activeItemId || (items[0] ? items[0].id : null)
          });
        }
        state.groups = groups;
      } else if (raw.providers && raw.providers.length) {
        var migrated = [];
        for (var j = 0; j < raw.providers.length; j++) migrated.push(migrateItem(raw.providers[j]));
        state.groups = [{ id: 'default', name: '默认组', items: migrated, autoSelect: false, activeItemId: migrated[0] ? migrated[0].id : null }];
      }
      state.activeGroupId = raw.activeGroupId || null;
      if (state.activeGroupId && !findGroup(state.activeGroupId)) state.activeGroupId = null;
    } catch (err) {
      /* 数据损坏就用默认数据继续启动 */
    }
  }

  /** 兼容旧数据：把任意形状的项归一化为新模型。 */
  function migrateItem(raw) {
    var item = newProvider({});
    if (!raw) return item;
    item.id = raw.id || item.id;
    item.name = raw.name || raw.model || '';
    item.baseUrl = raw.baseUrl || raw.url || '';
    item.apiKey = raw.apiKey || '';
    item.enabled = raw.enabled !== false;
    item.color = raw.color || item.color;
    if (raw.headroom && raw.headroom.enabled) item.headroom = { enabled: true, url: raw.headroom.url || '' };
    return item;
  }

  function save() {
    var file = dataFile();
    if (!fs || !file) return;
    try {
      fs.writeFileSync(file, JSON.stringify(state, null, 2), 'utf8');
    } catch (err) {
      /* 写入失败不影响界面 */
    }
  }

  // ---------------------------------------------------------------- 接口规格
  //
  // 用量与模型都按"请求 + 提取器"描述，模板变量 {{baseUrl}} / {{apiKey}}
  // 在发起请求前替换成供应商自己的地址和密钥。

  var MODELS_SPEC = {
    extractor: function (response) {
      var list = (response && (response.data || response.models)) || [];
      var ids = [];
      for (var i = 0; i < list.length; i++) {
        var item = list[i];
        var id = typeof item === 'string' ? item : (item && (item.id || item.name));
        if (id) ids.push(id);
      }
      return { models: ids };
    }
  };

  /**
   * 可选用的用量接口。每个预设 = 默认路径 + 提取器；
   * 供应商可在编辑弹窗里切换预设并改写路径。
   */
  var USAGE_PRESETS = [
    {
      id: 'generic-usage',
      name: '通用剩余额度 /v1/usage',
      path: '/v1/usage',
      extractor: function (response) {
        var r = response || {};
        var quota = r.quota || {};
        return {
          isValid: pick(r.is_active, r.isValid, true) !== false,
          remaining: pick(r.remaining, quota.remaining, r.balance),
          unit: pick(r.unit, quota.unit, 'USD'),
          // TrueSOTA 的 /v1/usage 自带 model_stats，顺带把模型取回来。
          models: modelsFromStats(r.model_stats),
          summary: summaryFrom(r)
        };
      }
    },
    {
      id: 'openai-billing',
      name: 'OpenAI 计费 /v1/dashboard/billing/subscription',
      path: '/v1/dashboard/billing/subscription',
      extractor: function (response) {
        var r = response || {};
        return {
          isValid: pick(r.has_payment_method, r.isValid, true) !== false,
          remaining: pick(r.total_available, r.remaining, r.balance, r.hard_limit_usd),
          unit: pick(r.unit, 'USD'),
          models: [],
          summary: summaryFrom(r)
        };
      }
    },
    {
      id: 'credit-grants',
      name: '额度列表 /v1/dashboard/billing/credit_grants',
      path: '/v1/dashboard/billing/credit_grants',
      extractor: function (response) {
        var r = response || {};
        return {
          isValid: pick(r.isValid, true) !== false,
          remaining: pick(r.total_available, r.total_granted, r.remaining),
          unit: pick(r.unit, 'USD'),
          models: [],
          summary: summaryFrom(r)
        };
      }
    },
    {
      id: 'custom-path',
      name: '自定义路径',
      path: '/v1/usage',
      extractor: function (response) {
        var r = response || {};
        var quota = r.quota || {};
        return {
          isValid: pick(r.is_active, r.isValid, true) !== false,
          remaining: pick(r.remaining, quota.remaining, r.balance, r.credit),
          unit: pick(r.unit, quota.unit, 'USD'),
          models: modelsFromStats(r.model_stats),
          summary: summaryFrom(r)
        };
      }
    }
  ];

  function presetOf(provider) {
    for (var i = 0; i < USAGE_PRESETS.length; i++) {
      if (USAGE_PRESETS[i].id === (provider.usageSpec || 'generic-usage')) return USAGE_PRESETS[i];
    }
    return USAGE_PRESETS[0];
  }

  function usageSpecOf(provider) {
    var preset = presetOf(provider);
    var path = provider.usagePath || preset.path;
    return {
      request: {
        url: '{{baseUrl}}' + normalizePath(path),
        method: 'GET',
        headers: { 'Authorization': 'Bearer {{apiKey}}' }
      },
      extractor: preset.extractor
    };
  }

  function modelsSpecOf(provider) {
    return {
      request: {
        url: '{{baseUrl}}' + normalizePath(provider.modelsPath || '/v1/models'),
        method: 'GET',
        headers: { 'Authorization': 'Bearer {{apiKey}}' }
      },
      extractor: MODELS_SPEC.extractor
    };
  }

  function normalizePath(value) {
    var text = String(value == null ? '' : value).trim();
    if (!text) return '/v1/usage';
    if (text.charAt(0) !== '/') text = '/' + text;
    return text.replace(/\/+$/, '') || '/v1/usage';
  }

  function modelsFromStats(stats) {
    var ids = [];
    if (!stats || !stats.length) return ids;
    for (var i = 0; i < stats.length; i++) {
      var id = stats[i] && (stats[i].model || stats[i].name);
      if (id) ids.push(id);
    }
    return ids;
  }

  function summaryFrom(response) {
    var total = (response && response.usage && response.usage.total) || null;
    if (!total && response && response.daily_usage && response.daily_usage.length) {
      total = response.daily_usage[response.daily_usage.length - 1];
    }
    if (!total) return null;
    return {
      requests: pick(total.requests, 0),
      tokens: pick(total.total_tokens, 0),
      cost: pick(total.cost, total.actual_cost, 0)
    };
  }

  function formatAmount(value) {
    if (typeof value === 'number' && isFinite(value)) return value.toFixed(2);
    return String(value);
  }

  function formatTokens(value) {
    var num = Number(value) || 0;
    if (num >= 1000000) return (num / 1000000).toFixed(2) + 'M';
    if (num >= 1000) return (num / 1000).toFixed(1) + 'K';
    return String(num);
  }

  function fillTemplate(text, provider) {
    return String(text == null ? '' : text)
      .replace(/\{\{\s*baseUrl\s*\}\}/g, String(provider.baseUrl || '').replace(/\/+$/, ''))
      .replace(/\{\{\s*apiKey\s*\}\}/g, String(provider.apiKey || ''));
  }

  /** 页面内只能用 XHR：nw 运行时没有 http 模块。 */
  function httpJson(spec, provider, done) {
    var url = fillTemplate(spec.request.url, provider);
    var settled = false;
    var xhr = new XMLHttpRequest();

    function finish(err, data) {
      if (settled) return;
      settled = true;
      done(err, data);
    }

    try {
      xhr.open(spec.request.method || 'GET', url, true);
    } catch (err) {
      finish(new Error('地址无效：' + url));
      return;
    }

    var headers = {};
    var base = spec.request.headers || {};
    for (var key in base) {
      if (Object.prototype.hasOwnProperty.call(base, key)) headers[key] = base[key];
    }
    if (provider.userAgent) headers['User-Agent'] = provider.userAgent;
    var extra = parseJsonObject(provider.headerOverride);
    for (var extraKey in extra) {
      if (Object.prototype.hasOwnProperty.call(extra, extraKey)) headers[extraKey] = extra[extraKey];
    }
    for (var name in headers) {
      if (!Object.prototype.hasOwnProperty.call(headers, name)) continue;
      try {
        xhr.setRequestHeader(name, fillTemplate(headers[name], provider));
      } catch (err) {
        /* 忽略内核拒绝的请求头（如 User-Agent） */
      }
    }

    xhr.timeout = 8000;
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      if (xhr.status >= 200 && xhr.status < 300) {
        var data = null;
        try {
          data = JSON.parse(xhr.responseText);
        } catch (err) {
          finish(new Error('响应不是 JSON'));
          return;
        }
        finish(null, data);
      } else {
        finish(new Error('HTTP ' + xhr.status));
      }
    };
    xhr.ontimeout = function () {
      finish(new Error('请求超时'));
    };
    xhr.onerror = function () {
      finish(new Error('请求失败'));
    };

    try {
      xhr.send(null);
    } catch (err) {
      finish(err);
    }
  }

  function parseJsonObject(text) {
    if (!text) return {};
    try {
      var value = JSON.parse(text);
      return (value && typeof value === 'object') ? value : {};
    } catch (err) {
      return {};
    }
  }

  function runSpec(spec, provider, done) {
    httpJson(spec, provider, function (err, data) {
      if (err) {
        done(err);
        return;
      }
      var result = null;
      try {
        result = spec.extractor(data);
      } catch (err2) {
        done(new Error('解析响应失败'));
        return;
      }
      done(null, result || {});
    });
  }

  // ---------------------------------------------------------------- 工具

  function pick() {
    for (var i = 0; i < arguments.length; i++) {
      var value = arguments[i];
      if (value !== undefined && value !== null && value !== '') return value;
    }
    return undefined;
  }

  function esc(text) {
    return String(text == null ? '' : text)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function timeAgo(ts) {
    if (!ts) return '未查询';
    var diff = Date.now() - ts;
    if (diff < MINUTE) return '刚刚';
    var m = Math.floor(diff / MINUTE);
    if (m < 60) return m + ' 分钟前';
    var h = Math.floor(m / 60);
    if (h < 24) return h + ' 小时前';
    return Math.floor(h / 24) + ' 天前';
  }

  function icon(name, cls, size) {
    var style = size ? ' style="width:' + size + 'px;height:' + size + 'px"' : '';
    return '<svg class="icon' + (cls ? ' ' + cls : '') + '"' + style + '>' +
      '<use href="#' + name + '" xlink:href="#' + name + '"/></svg>';
  }

  function find(id) {
    for (var g = 0; g < state.groups.length; g++) {
      var items = state.groups[g].items;
      for (var i = 0; i < items.length; i++) {
        if (items[i].id === id) return items[i];
      }
    }
    return null;
  }

  function findGroup(id) {
    for (var g = 0; g < state.groups.length; g++) {
      if (state.groups[g].id === id) return state.groups[g];
    }
    return null;
  }

  function groupOfItem(id) {
    for (var g = 0; g < state.groups.length; g++) {
      var items = state.groups[g].items;
      for (var i = 0; i < items.length; i++) {
        if (items[i].id === id) return state.groups[g];
      }
    }
    return null;
  }

  function allItems() {
    var out = [];
    for (var g = 0; g < state.groups.length; g++) out = out.concat(state.groups[g].items);
    return out;
  }

  function firstItemId() {
    for (var g = 0; g < state.groups.length; g++) {
      if (state.groups[g].items.length) return state.groups[g].items[0].id;
    }
    return null;
  }

  var toastTimer = null;
  var toastEl = document.getElementById('toast');

  function toast(text) {
    toastEl.textContent = text;
    toastEl.classList.add('show');
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      toastEl.classList.remove('show');
    }, 1800);
  }

  // ---------------------------------------------------------------- 列表渲染

  var listEl = document.getElementById('list');

  function metaHtml(item) {
    var status = item.enabled === false
      ? '<span class="err">已禁用</span>'
      : '<span class="ok">已启用</span>';
    var extra = item.headroom ? ' · <span class="hint">Headroom</span>' : '';
    var err = item.lastError
      ? ' <span class="err" title="' + esc(item.lastError) + '">' + icon('i-warn', '', 13) + ' 上次转发失败</span>'
      : '';
    return '<div class="meta-row">' + status + extra + err + '</div>';
  }

  function cardActions(item) {
    function btn(act, label, iconName, extraClass) {
      return '<button class="icon-btn sm' + (extraClass ? ' ' + extraClass : '') +
        '" data-act="' + act + '" data-id="' + item.id + '" title="' + label + '">' +
        icon(iconName) + '</button>';
    }
    return '<div class="card-actions">' +
      btn('edit', '编辑项', 'i-edit') +
      '<label class="switch sm" title="启用 / 禁用此模型">' +
      '<input type="checkbox" data-act="toggle-enabled" data-id="' + item.id + '"' +
      (item.enabled !== false ? ' checked' : '') + '><span class="slider"></span></label>' +
      btn('remove', '删除项', 'i-trash', 'danger') +
      '</div>';
  }

  function render() {
    if (!state.activeGroupId) {
      renderGroups();
    } else {
      var group = findGroup(state.activeGroupId);
      if (!group) {
        state.activeGroupId = null;
        renderGroups();
      } else {
        renderItems(group);
      }
    }
    renderProxyStatus();
    renderCrumb();
  }

  /** 标题栏代理状态条：状态灯 + 文案 + 开关同步（三态：停止 / 启动中 / 运行中）。 */
  function renderProxyStatus() {
    var port = state.proxy.port || 18080;
    var want = !!state.proxy.enabled;
    var running = !!(want && proxyServer && proxyListening);
    var starting = !!(want && proxyServer && !proxyListening);
    document.getElementById('usageText').textContent = !want
      ? '代理已停止'
      : (running ? ('代理运行中 · :' + port) : '代理启动中…');
    document.getElementById('usageToggle').checked = want;
    var dot = document.getElementById('proxyDot');
    dot.classList.toggle('running', running);
    dot.classList.toggle('starting', starting);
    dot.classList.toggle('stopped', !running && !starting);
    if (typeof updateTray === 'function') updateTray();
  }

  /** 单个项（组内一个具体模型/上游）的卡片。 */
  function itemCardHtml(group, item) {
    var active = item.id === group.activeItemId;
    var tags = '';
    if (active) tags += '<span class="tag active-tag">当前项</span>';
    tags += (item.enabled === false)
      ? '<span class="tag off-tag">已禁用</span>'
      : '<span class="tag on-tag">已启用</span>';
    return '<article class="card' + (active ? ' active' : '') + '" data-type="item" ' +
      'data-id="' + item.id + '" title="点击设为该组当前项">' +
      (active ? '<span class="accent"></span>' : '') +
      '<div class="avatar" style="background:' + esc(item.color || '#555') + '">' +
      esc(String(item.name || '?').charAt(0).toUpperCase()) + '</div>' +
      '<div class="card-main">' +
      '<div class="card-title"><span class="name">' + esc(item.name || '(未命名模型)') + '</span>' + tags + '</div>' +
      '<div class="card-sub">' + esc(item.baseUrl) + '</div>' +
      '</div>' +
      '<div class="card-meta">' + metaHtml(item) + '</div>' +
      cardActions(item) + '</article>';
  }

  function renderItems(group) {
    var html = autoCardHtml(group);
    if (!group.items.length) {
      html += '<div class="empty">这个组里还没有项，点击标题栏 + 添加一个</div>';
    } else {
      for (var i = 0; i < group.items.length; i++) html += itemCardHtml(group, group.items[i]);
    }
    listEl.innerHTML = html;
  }

  /** 组顶层“自动选择”选项卡片：开启后代理在组内已启用项之间轮流挑选。 */
  function autoCardHtml(group) {
    var on = !!group.autoSelect;
    return '<article class="card auto-card' + (on ? ' on' : '') + '" data-type="auto" ' +
      'data-id="' + esc(group.id) + '" title="点击切换自动选择">' +
      '<svg class="icon auto-ico"><use href="#i-magic" xlink:href="#i-magic"/></svg>' +
      '<div class="card-main">' +
      '<div class="card-title"><span class="name">自动选择</span>' +
      (on ? '<span class="tag auto-tag">已开启</span>' : '') + '</div>' +
      '<div class="card-sub">开启后代理在组内“已启用”的项之间轮流挑选；关闭则固定使用当前项。</div>' +
      '</div>' +
      '<label class="switch auto-switch"><input type="checkbox" ' + (on ? 'checked' : '') +
      ' tabindex="-1"><span class="slider"></span></label>' +
      '</article>';
  }

  function toggleAuto(id) {
    var group = findGroup(id);
    if (!group) return;
    group.autoSelect = !group.autoSelect;
    render();
    save();
  }

  function toggleItemEnabled(id) {
    var item = find(id);
    if (!item) return;
    item.enabled = item.enabled === false ? true : false;
    var group = groupOfItem(id);
    if (!group) { render(); save(); return; }
    if (!item.enabled && group.activeItemId === id) group.activeItemId = null;
    if (item.enabled && !group.activeItemId) group.activeItemId = id;
    render();
    save();
  }

  function groupActionBtn(act, label, iconName, extraClass) {
    return '<button class="icon-btn sm' + (extraClass ? ' ' + extraClass : '') +
      '" data-act="' + act + '" title="' + label + '">' + icon(iconName) + '</button>';
  }

  function groupCardHtml(group) {
    return '<article class="card group-card" data-type="group" data-id="' + esc(group.id) + '" ' +
      'title="进入该组">' +
      '<svg class="icon group-ico"><use href="#i-folder" xlink:href="#i-folder"/></svg>' +
      '<div class="card-main">' +
      '<div class="card-title"><span class="name">' + esc(group.name) + '</span>' +
      (group.autoSelect ? '<span class="tag auto-tag">自动</span>' : '') + '</div>' +
      '<div class="card-sub">' + group.items.length + ' 个项 · 点击进入</div>' +
      '</div>' +
      '<div class="card-actions">' +
      groupActionBtn('group-edit', '重命名组', 'i-edit') +
      groupActionBtn('group-remove', '删除组', 'i-trash', 'danger') +
      '</div></article>';
  }

  function renderGroups() {
    var html = '';
    for (var i = 0; i < state.groups.length; i++) html += groupCardHtml(state.groups[i]);
    if (!state.groups.length) {
      html = '<div class="empty">还没有任何组，点击标题栏 + 新建一个组</div>';
    }
    listEl.innerHTML = html;
  }

  function renderCrumb() {
    var node = document.getElementById('crumb');
    if (!node) return;
    if (!state.activeGroupId) {
      node.innerHTML = '<span class="crumb-item current">全部组</span>';
      return;
    }
    var group = findGroup(state.activeGroupId);
    node.innerHTML = '<span class="crumb-item" id="crumbHome">全部组</span>' +
      '<span class="crumb-sep">›</span>' +
      '<span class="crumb-item current">' + esc(group ? group.name : '') + '</span>';
  }

  // ---------------------------------------------------------------- 业务动作

  function activate(id) {
    var group = groupOfItem(id);
    if (!group) return;
    group.activeItemId = id;
    var item = find(id);
    render();
    save();
    if (item) toast('已设为当前项：' + item.name);
  }

  /** 按该供应商选用的用量接口取余额/剩余额度。 */
  function refresh(id) {
    var item = find(id);
    if (!item || item.status === 'querying') return;
    item.status = 'querying';
    render();
    runSpec(usageSpecOf(item), item, function (err, result) {
      var target = find(id);
      if (!target) return;
      target.queriedAt = Date.now();
      if (err) {
        target.status = 'error';
        target.balance = '';
        target.lastError = err.message;
        render();
        save();
        toast(target.name + ' 查询失败：' + err.message);
        return;
      }
      target.status = 'ok';
      target.valid = result.isValid !== false;
      target.lastError = '';
      var remaining = result.remaining;
      target.balance = (remaining === undefined || remaining === null)
        ? '未知'
        : (formatAmount(remaining) + ' ' + (result.unit || ''));
      if (result.summary) target.summary = result.summary;
      if (result.models && result.models.length) {
        target.models = result.models;
        target.modelsAt = Date.now();
      }
      render();
      save();
      toast(target.name + ' 余额/剩余：' + target.balance);
    });
  }

  /** 测速：量一次用量接口的往返耗时（结果同时当刷新用）。 */
  function ping(id) {
    var item = find(id);
    if (!item) return;
    toast('正在测速 ' + item.name + ' …');
    var started = Date.now();
    runSpec(usageSpecOf(item), item, function (err, result) {
      var cost = Date.now() - started;
      var target = find(id);
      if (!target) return;
      target.latency = cost;
      if (err) {
        target.status = 'error';
        target.lastError = err.message;
        target.balance = '';
        render();
        save();
        toast(target.name + '：' + cost + ' ms，' + err.message);
        return;
      }
      target.status = 'ok';
      target.valid = result.isValid !== false;
      target.lastError = '';
      target.queriedAt = Date.now();
      var remaining = result.remaining;
      target.balance = (remaining === undefined || remaining === null)
        ? '未知'
        : (formatAmount(remaining) + ' ' + (result.unit || ''));
      if (result.summary) target.summary = result.summary;
      if (result.models && result.models.length) {
        target.models = result.models;
        target.modelsAt = Date.now();
      }
      render();
      save();
      toast(target.name + '：延迟 ' + cost + ' ms，余额 ' + target.balance);
    });
  }

  /** 用量统计：展示上一次用量接口返回的汇总。 */
  function showUsage(id) {
    var item = find(id);
    if (!item) return;
    if (!item.summary) {
      toast(item.name + ' 还没有用量数据，先刷新一次');
      return;
    }
    toast(item.name + '｜请求 ' + item.summary.requests + ' · 令牌 ' +
      formatTokens(item.summary.tokens) + ' · 成本 $' + formatAmount(item.summary.cost));
  }

  /** 按该供应商的模型接口取模型列表。 */
  function refreshModels(id) {
    var item = find(id);
    if (!item) return;
    toast('正在获取 ' + item.name + ' 的模型…');
    runSpec(modelsSpecOf(item), item, function (err, result) {
      var target = find(id);
      if (!target) return;
      if (err) {
        toast(target.name + ' 获取模型失败：' + err.message);
        return;
      }
      target.models = result.models || [];
      target.modelsAt = Date.now();
      render();
      save();
      toast(target.name + '：' + target.models.length + ' 个模型');
    });
  }

  function refreshAll() {
    var items = allItems();
    for (var i = 0; i < items.length; i++) refresh(items[i].id);
    toast('已刷新全部用量');
  }

  function refreshAllModels() {
    var items = allItems();
    for (var i = 0; i < items.length; i++) refreshModels(items[i].id);
  }

  function addProvider(name, baseUrl, apiKey) {
    var id = 'p' + Date.now().toString(36);
    var item = newProvider({
      id: id,
      name: name,
      color: ITEM_PALETTE[Date.now() % ITEM_PALETTE.length],
      baseUrl: baseUrl,
      apiKey: apiKey || ''
    });
    var group = findGroup(state.activeGroupId) || state.groups[0] || null;
    if (!group) {
      group = { id: 'g' + Date.now().toString(36), name: '默认组', autoSelect: false, activeItemId: null, items: [] };
      state.groups.push(group);
    }
    group.items.push(item);
    if (!group.activeItemId) group.activeItemId = item.id;
    render();
    save();
    toast('已添加 ' + name);
    return id;
  }

  var confirmAction = null;

  function askConfirm(title, text, onOk) {
    document.getElementById('confirmTitle').textContent = title;
    document.getElementById('confirmText').textContent = text;
    confirmAction = onOk;
    document.getElementById('confirmMask').classList.add('show');
  }

  document.getElementById('btnConfirmCancel').addEventListener('click', function () {
    confirmAction = null;
    document.getElementById('confirmMask').classList.remove('show');
  });

  document.getElementById('btnConfirmOk').addEventListener('click', function () {
    var action = confirmAction;
    confirmAction = null;
    document.getElementById('confirmMask').classList.remove('show');
    if (action) action();
  });

  function removeProvider(id) {
    for (var g = 0; g < state.groups.length; g++) {
      var items = state.groups[g].items;
      for (var i = 0; i < items.length; i++) {
        if (items[i].id === id) {
          var name = items[i].name;
          items.splice(i, 1);
          if (state.groups[g].activeItemId === id) state.groups[g].activeItemId = null;
          render();
          save();
          toast('已删除 ' + name);
          return;
        }
      }
    }
  }

  // ---------------------------------------------------------------- 菜单

  var floating = null;

  function closeMenus() {
    if (floating) floating.classList.remove('show');
  }

  function buildMenu(items) {
    if (!floating) {
      floating = document.createElement('div');
      floating.className = 'menu';
      document.body.appendChild(floating);
    }
    floating.innerHTML = '';
    for (var i = 0; i < items.length; i++) {
      (function (item) {
        var el = document.createElement('div');
        el.className = 'menu-item';
        el.innerHTML = (item.icon ? icon(item.icon, '', 15) : '') + '<span>' + esc(item.label) + '</span>';
        el.addEventListener('click', function () {
          closeMenus();
          item.run();
        });
        floating.appendChild(el);
      })(items[i]);
    }
    return floating;
  }

  function showMenuAt(anchor, items, alignLeft) {
    var menu = buildMenu(items);
    var rect = anchor.getBoundingClientRect();
    menu.style.position = 'fixed';
    menu.style.right = 'auto';
    menu.style.top = (rect.bottom + 6) + 'px';
    menu.style.left = alignLeft ? rect.left + 'px' : '';
    if (!alignLeft) {
      menu.style.left = (rect.right - 180) + 'px';
    }
    menu.classList.add('show');
  }

  /** 在鼠标位置弹出菜单（卡片右键菜单）。 */
  function showMenuXY(x, y, items) {
    var menu = buildMenu(items);
    menu.style.position = 'fixed';
    menu.style.right = 'auto';
    menu.style.top = y + 'px';
    menu.style.left = x + 'px';
    menu.classList.add('show');
  }

  document.getElementById('btnSettings').addEventListener('click', function (event) {
    event.stopPropagation();
    showMenuAt(this, [
      { label: '打开数据目录', icon: 'i-folder', run: openDataDir },
      { label: '备份数据', icon: 'i-copy', run: backupData },
      { label: '代理与 Headroom 设置', icon: 'i-globe', run: openProxySettings },
      { label: '应用设置', icon: 'i-gear', run: openAppSettings },
      { label: '关于 VG Switch', icon: 'i-info', run: showAbout },
      { label: '退出', icon: 'i-quit', run: quitApp }
    ], true);
  });

  document.getElementById('btnMore').addEventListener('click', function (event) {
    event.stopPropagation();
    showMenuAt(this, [
      { label: '复制代理地址', icon: 'i-copy', run: copyActiveUrl },
      { label: '代理与 Headroom 设置', icon: 'i-globe', run: openProxySettings },
      { label: '打开数据目录', icon: 'i-folder', run: openDataDir },
      { label: '退出', icon: 'i-quit', run: quitApp }
    ], false);
  });

  // ---------------------------------------------------------------- 代理与 Headroom 设置

  function openProxySettings() {
    el('proxyEnabled').checked = !!state.proxy.enabled;
    el('proxyPort').value = state.proxy.port || 18080;
    el('proxyKey').value = state.proxy.key || '';
    el('hrEnabledGlobal').checked = !!state.headroom.enabled;
    el('hrUrlGlobal').value = state.headroom.url || '';
    el('proxyMask').classList.add('show');
    el('proxyPort').focus();
  }

  document.getElementById('btnProxyCancel').addEventListener('click', function () {
    el('proxyMask').classList.remove('show');
  });

  document.getElementById('btnGenKey').addEventListener('click', function () {
    el('proxyKey').value = randomKey();
    toast('已生成客户端密钥');
  });

  document.getElementById('btnProxySave').addEventListener('click', function () {
    state.proxy = {
      enabled: el('proxyEnabled').checked,
      port: Number(el('proxyPort').value) || 18080,
      key: el('proxyKey').value.trim()
    };
    state.headroom = {
      enabled: el('hrEnabledGlobal').checked,
      url: el('hrUrlGlobal').value.trim()
    };
    save();
    stopProxy();
    if (state.proxy.enabled) startProxy();
    el('proxyMask').classList.remove('show');
    renderProxyStatus();
    toast(state.proxy.enabled ? '已保存并（重新）启动反向代理' : '已保存（代理已停止）');
  });

  // ---------------------------------------------------------------- 应用设置（窗口行为）

  function openAppSettings() {
    el('optMinimizeToTray').checked = !!state.minimizeToTray;
    el('settingsMask').classList.add('show');
  }

  document.getElementById('btnSettingsCancel').addEventListener('click', function () {
    el('settingsMask').classList.remove('show');
  });

  document.getElementById('btnSettingsSave').addEventListener('click', function () {
    state.minimizeToTray = el('optMinimizeToTray').checked;
    save();
    el('settingsMask').classList.remove('show');
    renderProxyStatus(); // 顺带刷新托盘菜单（提示语会跟着开关变化）
    toast(state.minimizeToTray ? '关闭窗口时将最小化到托盘' : '关闭窗口时将直接退出');
  });

  // ---------------------------------------------------------------- 反向代理服务

  var proxyServer = null;
  var proxyListening = false; // listen 回调到了才算真的在跑
  var rrCounters = {};
  var proxySockets = {};   // 存活的客户端连接，停服时要主动断开，否则端口不会真正释放
  var proxyInflight = {};  // 正在转发的上游请求，客户端断开时中止

  function proxyBaseUrl() {
    return 'http://127.0.0.1:' + (Number(state.proxy.port) || 18080) + '/v1';
  }

  function randomKey() {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    var s = '';
    for (var i = 0; i < 32; i++) s += chars.charAt(Math.floor(Math.random() * chars.length));
    return 'vg-' + s;
  }

  function startProxy() {
    if (!state.proxy.enabled) return;
    if (!loadProxyModules()) {
      state.proxy.enabled = false;
      save();
      renderProxyStatus();
      toast('无法启动代理：' + (proxyLoadError || '缺少 http 模块'));
      return;
    }
    var port = Number(state.proxy.port) || 18080;
    stopProxy();
    proxySockets = {};
    proxyInflight = {};

    var server = httpMod.createServer(function (req, res) { handleProxy(req, res); });
    proxyServer = server;

    // 记账所有连接：keep-alive 长连接不断开的话，server.close() 不会真正释放端口，
    // 改端口保存后重启就会撞上一个“关不掉”的旧监听。
    server.on('connection', function (socket) {
      var sid = 's' + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
      proxySockets[sid] = socket;
      socket.on('close', function () { delete proxySockets[sid]; });
    });
    server.keepAliveTimeout = 15000;
    server.headersTimeout = 20000;

    server.on('clientError', function (err, socket) {
      try { socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n'); } catch (e) {}
    });

    server.on('error', function (err) {
      proxyServer = null;
      var why = (err && err.code === 'EADDRINUSE') ? ('端口 ' + port + ' 已被占用') : (err && err.message);
      state.proxy.enabled = false; // 起不来就把开关复位，避免界面一直显示“运行中”
      save();
      renderProxyStatus();
      toast('代理启动失败：' + why);
    });

    // 这个内核里 node 的事件循环起得很慢（实测 listen 回调可能十几秒才到），
    // 所以在真正 listening 之前状态条只显示"启动中"，不能提前报"运行中"。
    server.listen(port, '127.0.0.1', function () {
      proxyListening = true;
      renderProxyStatus();
      toast('反向代理已启动：' + proxyBaseUrl());
    });
  }

  function stopProxy() {
    var server = proxyServer;
    proxyServer = null;
    proxyListening = false;

    var inflight = proxyInflight;
    proxyInflight = {};
    for (var rid in inflight) {
      if (!Object.prototype.hasOwnProperty.call(inflight, rid)) continue;
      try { inflight[rid].abort(); } catch (e) {}
    }

    var sockets = proxySockets;
    proxySockets = {};
    for (var sid in sockets) {
      if (!Object.prototype.hasOwnProperty.call(sockets, sid)) continue;
      try { sockets[sid].destroy(); } catch (e) {}
    }

    if (!server) return;
    try {
      if (typeof server.closeAllConnections === 'function') server.closeAllConnections();
      if (typeof server.closeIdleConnections === 'function') server.closeIdleConnections();
      server.close();
    } catch (e) {}
  }

  function sendJson(res, code, obj) {
    var text = JSON.stringify(obj);
    res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': byteLength(text) });
    res.end(text);
  }

  /** 解析出该项实际要转发到的上游 base（优先用单独的 Headroom，否则全局 Headroom，否则直接用 baseUrl）。 */
  function resolveHeadroom(item) {
    if (item.headroom && item.headroom.enabled && item.headroom.url) return item.headroom.url;
    if (state.headroom.enabled && state.headroom.url) return state.headroom.url;
    return null;
  }

  /** 在组内挑选要使用的项：自动选择→轮流；否则用当前项，缺失则取第一个启用的。 */
  function selectItem(group) {
    var enabled = [];
    for (var i = 0; i < group.items.length; i++) if (group.items[i].enabled !== false) enabled.push(group.items[i]);
    if (!enabled.length) return null;
    if (group.autoSelect) {
      var n = rrCounters[group.id] || 0;
      rrCounters[group.id] = (n + 1) % enabled.length;
      return enabled[n % enabled.length];
    }
    var active = null;
    for (var j = 0; j < group.items.length; j++) if (group.items[j].id === group.activeItemId) active = group.items[j];
    if (active && active.enabled !== false) return active;
    return enabled[0];
  }

  function buildTarget(base, pathname) {
    var trimmed = String(base || '').replace(/\/+$/, '');
    if (!trimmed) return '';
    if (/\/v1$/.test(trimmed)) return trimmed + pathname.replace(/^\/v1/, '');
    return trimmed + pathname;
  }

  function handleProxy(req, res) {
    var parsed = urlMod.parse(req.url, true);
    var pathname = parsed.pathname || '/';

    // 健康检查：不挑模型也不需要密钥，方便本地自检代理是否在跑
    if (req.method === 'GET' && (pathname === '/' || pathname === '/health')) {
      return sendJson(res, 200, {
        ok: true,
        service: 'vg-switch',
        running: true,
        baseUrl: proxyBaseUrl(),
        authRequired: !!state.proxy.key,
        headroom: !!(state.headroom.enabled && state.headroom.url),
        groups: state.groups.map(function (g) {
          return { name: g.name, items: g.items.length, autoSelect: !!g.autoSelect };
        })
      });
    }

    if (state.proxy.key) {
      var provided = req.headers['x-api-key'] || '';
      var auth = req.headers['authorization'] || '';
      var bearer = auth.replace(/^Bearer\s+/i, '');
      if (provided !== state.proxy.key && bearer !== state.proxy.key) {
        return sendJson(res, 401, { error: { message: '未授权的客户端密钥' } });
      }
    }
    if (req.method === 'GET' && pathname === '/v1/models') {
      var data = state.groups.map(function (g) {
        return { id: g.name, object: 'model', owned_by: 'vg-switch', created: 0, permission: ['read', 'write'] };
      });
      return sendJson(res, 200, { object: 'list', data: data });
    }
    if (pathname.indexOf('/v1/') !== 0) {
      return sendJson(res, 404, { error: { message: '仅支持 /v1/* 路径' } });
    }
    var chunks = [];
    var tooBig = false;
    req.on('data', function (c) {
      chunks.push(c);
      if (!tooBig && concatChunks(chunks).length > 16 * 1024 * 1024) { tooBig = true; req.destroy(); }
    });
    req.on('end', function () {
      if (tooBig) return sendJson(res, 413, { error: { message: '请求体过大' } });
      var bodyStr = chunks.length ? concatChunks(chunks).toString('utf8') : '';
      var body = null;
      if (bodyStr) {
        try { body = JSON.parse(bodyStr); } catch (e) { return sendJson(res, 400, { error: { message: '请求体不是合法 JSON' } }); }
      }
      var modelName = (body && body.model) || parsed.query.model;
      if (!modelName) return sendJson(res, 400, { error: { message: '缺少 model 字段，无法路由到组' } });
      var group = null;
      for (var g = 0; g < state.groups.length; g++) if (state.groups[g].name === modelName) { group = state.groups[g]; break; }
      if (!group) return sendJson(res, 404, { error: { message: '没有名为「' + modelName + '」的组/模型' } });
      var item = selectItem(group);
      if (!item) return sendJson(res, 503, { error: { message: '该组没有可用的启用项' } });
      var target = buildTarget(resolveHeadroom(item) || item.baseUrl, pathname);
      if (!target) return sendJson(res, 502, { error: { message: '项未配置上游地址' } });
      if (body && typeof body === 'object') body.model = item.name;
      forwardToUpstream(req, res, target, body, item.apiKey);
    });
    req.on('error', function () { try { res.end(); } catch (e) {} });
  }

  function forwardToUpstream(req, res, target, body, apiKey) {
    var options = urlMod.parse(target);
    options.method = req.method;
    options.headers = {};
    var src = req.headers || {};
    for (var key in src) {
      if (!Object.prototype.hasOwnProperty.call(src, key)) continue;
      var lower = key.toLowerCase();
      if (lower === 'host' || lower === 'content-length' || lower === 'connection' || lower === 'transfer-encoding') continue;
      options.headers[key] = src[key];
    }
    if (apiKey) options.headers['authorization'] = 'Bearer ' + apiKey;
    var mod = options.protocol === 'https:' ? httpsMod : httpMod;
    if (!mod) return sendJson(res, 500, { error: { message: '运行时不支持该协议' } });

    var upstream = mod.request(options, function (upRes) {
      if (!res.headersSent) res.writeHead(upRes.statusCode, upRes.headers);
      upRes.pipe(res);
    });

    // 记在途请求：停代理 / 客户端中途断开（关掉聊天流式响应）时把上游也掐掉。
    var rid = 'r' + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
    proxyInflight[rid] = upstream;
    function done() { delete proxyInflight[rid]; }
    upstream.on('close', done);
    upstream.on('error', function (err) {
      done();
      if (!res.headersSent) sendJson(res, 502, { error: { message: '上游连接失败：' + err.message } });
      else { try { res.end(); } catch (e) {} }
    });
    res.on('close', function () {
      if (proxyInflight[rid]) {
        try { upstream.abort(); } catch (e) {}
        done();
      }
    });

    if (body) upstream.write(JSON.stringify(body));
    upstream.end();
  }

  // ---------------------------------------------------------------- 组管理

  var groupEditId = null;

  function openGroupDialog(id) {
    groupEditId = id || null;
    var group = id ? findGroup(id) : null;
    el('groupTitle').textContent = group ? '重命名组' : '新建组';
    el('gName').value = group ? group.name : '';
    el('addGroupMask').classList.add('show');
    el('gName').focus();
  }

  document.getElementById('btnGroupCancel').addEventListener('click', function () {
    el('addGroupMask').classList.remove('show');
  });

  document.getElementById('btnGroupOk').addEventListener('click', function () {
    var name = el('gName').value.trim();
    if (!name) {
      toast('请填写组名称');
      return;
    }
    if (groupEditId) {
      var group = findGroup(groupEditId);
      if (group) {
        group.name = name;
        toast('已重命名为 ' + name);
      }
    } else {
      state.groups.push({ id: 'g' + Date.now().toString(36), name: name, items: [] });
      toast('已新建组 ' + name);
    }
    el('addGroupMask').classList.remove('show');
    render();
    save();
  });

  function askRemoveGroup(id) {
    var group = findGroup(id);
    if (!group) return;
    askConfirm('删除组', '将删除「' + group.name + '」及其下 ' + group.items.length + ' 个项，删除后无法恢复。', function () {
      var index = -1;
      for (var g = 0; g < state.groups.length; g++) {
        if (state.groups[g].id === id) index = g;
      }
      if (index < 0) return;
      state.groups.splice(index, 1);
      if (state.activeGroupId === id) state.activeGroupId = null;
      render();
      save();
      toast('已删除组 ' + group.name);
    });
  }

  function enterGroup(id) {
    state.activeGroupId = id;
    render();
  }

  document.getElementById('crumb').addEventListener('click', function (event) {
    if (event.target && event.target.id === 'crumbHome') {
      state.activeGroupId = null;
      render();
    }
  });

  document.addEventListener('mousedown', function (event) {
    var el = event.target;
    while (el && el !== document) {
      if (el.classList && (el.classList.contains('menu') || el.classList.contains('combo'))) return;
      if (el.id === 'btnSettings' || el.id === 'btnMore') return;
      el = el.parentNode;
    }
    closeMenus();
  });

  // ---------------------------------------------------------------- 标题栏

  document.getElementById('btnCheckUpdate').addEventListener('click', function () {
    toast('已是最新版本');
  });

  document.getElementById('btnUsage').addEventListener('click', function () {
    toast((proxyServer ? '代理地址：' + proxyBaseUrl() : '代理未运行，启用后地址为：' + proxyBaseUrl()) + '（模型名即组名）');
  });

  document.getElementById('usageToggle').addEventListener('change', function () {
    setProxyEnabled(this.checked);
  });

  document.getElementById('btnAdd').addEventListener('click', function () {
    if (state.activeGroupId) {
      document.getElementById('addMask').classList.add('show');
      document.getElementById('fName').focus();
    } else {
      openGroupDialog(null);
    }
  });

  document.getElementById('btnAddCancel').addEventListener('click', function () {
    document.getElementById('addMask').classList.remove('show');
  });

  document.getElementById('btnAddOk').addEventListener('click', function () {
    var name = document.getElementById('fName').value.trim();
    var base = document.getElementById('fUrl').value.trim();
    if (!name) {
      toast('请填写具体模型名称');
      return;
    }
    if (!base) {
      toast('请填写上游地址');
      return;
    }
    var newId = addProvider(name, base, document.getElementById('fKey').value.trim());
    document.getElementById('fName').value = '';
    document.getElementById('fUrl').value = '';
    document.getElementById('fKey').value = '';
    document.getElementById('addMask').classList.remove('show');
    openEdit(newId);
  });

  // ---------------------------------------------------------------- 编辑供应商

  var editId = null;

  function el(id) {
    return document.getElementById(id);
  }

  function openEdit(id) {
    var item = find(id);
    if (!item) return;
    editId = id;
    el('editTitle').textContent = '编辑项 · ' + (item.name || '未命名');
    el('eAvatar').textContent = String(item.name || '?').charAt(0).toUpperCase();
    el('eAvatar').style.background = item.color || '#555';
    el('eName').value = item.name || '';
    el('eBase').value = item.baseUrl || '';
    el('eKey').value = item.apiKey || '';
    el('eKey').type = 'password';
    el('eEnabled').checked = item.enabled !== false;
    var hr = item.headroom || {};
    el('eHrEnabled').checked = !!hr.enabled;
    el('eHrUrl').value = hr.url || '';
    el('editMask').classList.add('show');
  }

  function closeEdit() {
    editId = null;
    el('editMask').classList.remove('show');
  }

  el('btnEditBack').addEventListener('click', closeEdit);
  el('btnEditCancel').addEventListener('click', closeEdit);

  el('advHead').addEventListener('click', function () {
    el('advBox').classList.toggle('open');
  });

  el('btnToggleKey').addEventListener('click', function () {
    var input = el('eKey');
    input.type = input.type === 'password' ? 'text' : 'password';
  });

  el('btnEditSave').addEventListener('click', function () {
    var item = find(editId);
    if (!item) { closeEdit(); return; }
    var name = el('eName').value.trim();
    var base = el('eBase').value.trim();
    if (!name) { toast('请填写具体模型名称'); return; }
    if (!base) { toast('请填写上游地址'); return; }
    item.name = name;
    item.baseUrl = base;
    item.apiKey = el('eKey').value.trim();
    item.enabled = el('eEnabled').checked;
    item.headroom = el('eHrEnabled').checked
      ? { enabled: true, url: el('eHrUrl').value.trim() }
      : null;
    var group = groupOfItem(item.id);
    if (group && !group.activeItemId) group.activeItemId = item.id;
    closeEdit();
    render();
    save();
    toast('已保存 ' + item.name);
  });

  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape') {
      el('addMask').classList.remove('show');
      el('editMask').classList.remove('show');
      el('confirmMask').classList.remove('show');
      el('proxyMask').classList.remove('show');
      el('addGroupMask').classList.remove('show');
      closeMenus();
    }
  });

  // ---------------------------------------------------------------- 窗口动作

  var pinned = false;

  document.getElementById('btnPin').addEventListener('click', function () {
    pinned = !pinned;
    this.classList.toggle('on', pinned);
    if (win) win.setAlwaysOnTop(pinned);
    toast(pinned ? '窗口已置顶' : '已取消置顶');
  });

  document.getElementById('btnCopy').addEventListener('click', copyActiveUrl);

  // 最大化 / 向下还原共用一个按钮，按当前状态切换图标与提示。
  var btnMax = document.getElementById('btnRestore');
  var maxUse = btnMax.querySelector('use');

  function setMaxIcon(icon, tip) {
    if (maxUse) {
      maxUse.setAttribute('href', '#' + icon);
      maxUse.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', '#' + icon);
    }
    btnMax.title = tip;
    btnMax.setAttribute('aria-label', tip);
  }

  function syncMaxIcon() {
    var maxed = !!(win && win.isMaximized);
    setMaxIcon(maxed ? 'i-restore' : 'i-max', maxed ? '向下还原' : '最大化');
  }

  btnMax.addEventListener('click', function () {
    if (!win) return;
    if (win.isMaximized) {
      win.unmaximize();
    } else {
      win.maximize();
    }
    syncMaxIcon();
  });

  document.getElementById('btnMinimize').addEventListener('click', function () {
    if (win) win.minimize();
  });

  document.getElementById('btnClose').addEventListener('click', function () {
    // 注意：本运行时的 Window.close() 发完 'close' 事件就照关，事件里无法否决
    // （看 WindowState.prototype.close：!force && !this.emit('close') 恒为假），
    // 所以能不能转托盘必须在这里就决定，不能直接调 win.close()。
    if (canHideToTray()) hideToTray();
    else quitApp();
  });

  // Alt+F4 / 系统菜单关闭走的是宿主 WM_CLOSE，那里读 window.__nmbCloseVerdict：
  // 返回 'cancel' 就不关窗口（nw_host.cpp 的 WM_CLOSE 分支）。
  window.__nmbCloseVerdict = function () {
    if (canHideToTray()) {
      hideToTray();
      return 'cancel';
    }
    return 'close';
  };

  if (win) {
    // 系统菜单、任务栏双击标题栏等外部路径改变最大化状态时也要同步图标。
    win.on('maximize', syncMaxIcon);
    win.on('unmaximize', syncMaxIcon);
    win.on('restore', syncMaxIcon);
    syncMaxIcon();
  }

  // ---------------------------------------------------------------- 系统托盘

  var tray = null;
  var trayProxyItem = null;
  var trayWinItem = null;
  var trayIconPath = '';
  var windowHidden = false;
  var quitting = false;

  /** 托盘图标必须是 **.ico**：宿主侧是 LoadImage(IMAGE_ICON, LR_LOADFROMFILE)，只有 .ico
      （含 DIB）能被接受，PNG 会加载失败并被悄悄换成系统默认图标。仓库里没有图片资源，
      所以这里用 canvas 画好后按 ICO 容器（ICONDIR + BITMAPINFOHEADER + BGRA 行，自底向上）
      组包写盘。字节布局是实测通过过的版本，改之前先看 probe 的结论。 */
  function ensureTrayIcon() {
    if (trayIconPath && fs && fs.existsSync(trayIconPath)) return trayIconPath;
    if (!loadStorageModules() || !fs || !path) return '';
    try {
      var canvas = document.createElement('canvas');
      canvas.width = 32;
      canvas.height = 32;
      var ctx = canvas.getContext('2d');
      var grad = ctx.createLinearGradient(0, 0, 32, 32);
      grad.addColorStop(0, '#3ff0ab');
      grad.addColorStop(1, '#13b274');
      ctx.fillStyle = grad;
      var r = 7;
      ctx.beginPath();
      ctx.moveTo(r, 0);
      ctx.lineTo(32 - r, 0);
      ctx.quadraticCurveTo(32, 0, 32, r);
      ctx.lineTo(32, 32 - r);
      ctx.quadraticCurveTo(32, 32, 32 - r, 32);
      ctx.lineTo(r, 32);
      ctx.quadraticCurveTo(0, 32, 0, 32 - r);
      ctx.lineTo(0, r);
      ctx.quadraticCurveTo(0, 0, r, 0);
      ctx.closePath();
      ctx.fill();
      // 中间的深色 V
      ctx.strokeStyle = '#0d2b20';
      ctx.lineWidth = 3.4;
      ctx.lineCap = 'round';
      ctx.lineJoin = 'round';
      ctx.beginPath();
      ctx.moveTo(9.5, 10);
      ctx.lineTo(16, 23);
      ctx.lineTo(22.5, 10);
      ctx.stroke();
      // BGRA，行序自底向上
      var n = 32;
      var pixels = ctx.getImageData(0, 0, n, n).data;
      var xor = new Uint8Array(n * n * 4);
      for (var y = 0; y < n; y++) {
        for (var x = 0; x < n; x++) {
          var s = (((n - 1 - y) * n) + x) * 4;
          var d = ((y * n) + x) * 4;
          xor[d] = pixels[s + 2];
          xor[d + 1] = pixels[s + 1];
          xor[d + 2] = pixels[s];
          xor[d + 3] = pixels[s + 3];
        }
      }
      var dib = new Uint8Array(40);
      var dv = new DataView(dib.buffer);
      dv.setUint32(0, 40, true);
      dv.setInt32(4, n, true);
      dv.setInt32(8, n * 2, true);     // 位图高度含 AND 掩膜
      dv.setUint16(12, 1, true);
      dv.setUint16(14, 32, true);
      dv.setUint32(20, xor.length, true);

      var image = new Uint8Array(dib.length + xor.length);
      image.set(dib, 0);
      image.set(xor, dib.length);

      var dir = new Uint8Array(22);
      var dv2 = new DataView(dir.buffer);
      dv2.setUint16(0, 0, true);
      dv2.setUint16(2, 1, true);       // type 1 = icon
      dv2.setUint16(4, 1, true);       // 图像数
      dir[6] = n;
      dir[7] = n;
      dv2.setUint16(10, 1, true);
      dv2.setUint16(12, 32, true);
      dv2.setUint32(14, image.length, true);
      dv2.setUint32(18, dir.length, true);

      var ico = new Uint8Array(dir.length + image.length);
      ico.set(dir, 0);
      ico.set(image, dir.length);

      var file = path.join(path.dirname(dataFile()), 'tray-icon.ico');
      fs.writeFileSync(file, ico);
      trayIconPath = file;
      return file;
    } catch (err) {
      return '';
    }
  }

  function showWindow() {
    if (!win) return;
    try {
      win.show();
      if (win.isMinimized) win.restore();
      win.focus();
      windowHidden = false;
    } catch (e) {}
  }

  function hideToTray() {
    if (!win) return;
    try {
      win.hide();
      windowHidden = true;
      updateTray();
    } catch (e) {}
  }

  /** 能不能走“关闭→托盘”：开关要开、托盘对象要真的在宿主那边结算出 id。 */
  function canHideToTray() {
    return !!(state.minimizeToTray && win && tray && tray.id !== null);
  }

  function setProxyEnabled(on) {
    state.proxy.enabled = !!on;
    save();
    if (state.proxy.enabled) startProxy();
    else stopProxy();
    renderProxyStatus();
    toast(state.proxy.enabled ? '反向代理已开启' : '反向代理已停止');
  }

  function buildTrayMenu() {
    var MenuItem = nw.MenuItem;
    var menu = new nw.Menu();
    tray.setMenu(menu);

    trayWinItem = new MenuItem({ label: '显示窗口', click: function () { showWindow(); } });
    menu.append(trayWinItem);
    menu.append(new MenuItem({ label: '隐藏到托盘', click: function () { hideToTray(); } }));
    menu.append(new MenuItem({ type: 'separator' }));

    trayProxyItem = new MenuItem({
      label: '启动反向代理',
      click: function () { setProxyEnabled(!state.proxy.enabled); }
    });
    menu.append(trayProxyItem);
    menu.append(new MenuItem({
      label: '复制代理地址',
      click: function () { copyActiveUrl(); }
    }));
    menu.append(new MenuItem({
      label: '代理设置…',
      click: function () { showWindow(); openProxySettings(); }
    }));
    menu.append(new MenuItem({
      label: '应用设置…',
      click: function () { showWindow(); openAppSettings(); }
    }));
    menu.append(new MenuItem({ type: 'separator' }));
    menu.append(new MenuItem({ label: '退出', click: function () { quitApp(); } }));
  }

  function updateTray() {
    if (!tray) return;
    var running = !!proxyServer;
    tray.tooltip = 'VG Switch · ' + (running
      ? ('代理运行中 ' + proxyBaseUrl())
      : '代理已停止');
    if (trayProxyItem) trayProxyItem.label = state.proxy.enabled ? '停止反向代理' : '启动反向代理';
    if (trayWinItem) trayWinItem.label = windowHidden ? '显示窗口' : '聚焦窗口';
  }

  function initTray() {
    if (!hasNw || !win || !nw.Tray || !nw.Menu) return;
    var icon = ensureTrayIcon();
    if (!icon) {
      toast('托盘不可用：图标文件没能写到数据目录');
      return;
    }
    try {
      tray = new nw.Tray({ title: 'VG Switch', tooltip: 'VG Switch', icon: icon });
    } catch (err) {
      tray = null;
      return;
    }
    try {
      buildTrayMenu();  // 菜单挂不上也要保留"点击唤回"这条基本能力
    } catch (e) {}
    // 本实现发的事件名是 click / double-click / right-click（右键内部会自己弹 menu），
    // 不是 nw.js 的 dblclick。
    tray.on('click', function () { showWindow(); });
    tray.on('double-click', function () { showWindow(); });
    updateTray();
    // 创建走的是异步 RPC，id 由宿主结算；没结算出来说明 Shell_NotifyIcon 那边没建成。
    setTimeout(function () {
      if (tray && tray.id === null) toast('托盘图标没创建成功，关闭窗口将无法转入托盘');
      updateTray();
    }, 1500);
  }

  function removeTray() {
    if (!tray) return;
    try { tray.remove(); } catch (e) {}
    tray = null;
  }

  // 标题栏拖动交给宿主的原生拖动（nw 的 win.startDrag：native 侧 SetCapture + SetWindowPos）。
  // 另两条路都不通：CSS 的 -webkit-app-region 拖动区在这套"离屏内核 + 合成分窗口"的实现里
  // 落不到可见窗口上；自己用 win.moveBy 攒位移又会因为转发给页面的 screenX/screenY 是内核
  // 绑定窗的坐标（恒偏 -32000），叠上 hover polyfill 合成出来的 mousemove 算出巨大的 dx/dy。
  var titlebar = document.getElementById('titlebar');

  titlebar.addEventListener('mousedown', function (event) {
    if (event.button !== 0 || !win || typeof win.startDrag !== 'function') return;
    // 按钮/输入框以及显式标了 .no-drag 的区域不参与拖动，否则按下去就成了拖动起始点。
    for (var el = event.target; el && el !== titlebar; el = el.parentNode) {
      var tag = el.tagName;
      if (tag === 'BUTTON' || tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || tag === 'A') return;
      if (el.classList && el.classList.contains('no-drag')) return;
    }
    win.startDrag();
  });

  titlebar.addEventListener('dblclick', function (event) {
    if (!win) return;
    // 图标常常是按钮里的 svg/use，target 不一定是 BUTTON，往上找到标题栏为止。
    for (var el = event.target; el && el !== titlebar; el = el.parentNode) {
      if (el.tagName === 'BUTTON') return;
      if (el.classList && el.classList.contains('no-drag')) return;
    }
    if (win.isMaximized) {
      win.unmaximize();
    } else {
      win.maximize();
    }
    syncMaxIcon();
  });

  // ---------------------------------------------------------------- 杂项动作

  function copyUrl(id) {
    var item = find(id);
    if (!item) return;
    var text = item.baseUrl || '';
    if (hasNw && nw.Clipboard) nw.Clipboard.get().set(text, 'text');
    toast('已复制上游地址：' + text);
  }

  function copyActiveUrl() {
    var url = 'http://127.0.0.1:' + (state.proxy.port || 18080) + '/v1';
    if (hasNw && nw.Clipboard) nw.Clipboard.get().set(url, 'text');
    toast('已复制代理地址：' + url);
  }

  function openDataDir() {
    var file = dataFile();
    if (!file) {
      toast('当前环境没有文件系统接口');
      return;
    }
    if (hasNw && nw.Shell && fs.existsSync(file)) {
      nw.Shell.showItemInFolder(file);
    } else {
      toast(file);
    }
  }

  function backupData() {
    var file = dataFile();
    if (!fs || !file || !fs.existsSync(file)) {
      toast('还没有可备份的数据');
      return;
    }
    var backup = file.replace(/\.json$/, '') + '-backup.json';
    fs.writeFileSync(backup, fs.readFileSync(file), 'utf8');
    toast('已备份到 ' + backup);
  }

  function showAbout() {
    toast('VG Switch · nw 运行时示例 v1.0.0');
  }

  function quitApp() {
    if (quitting) return;
    quitting = true;
    // 先收尾（停代理、落盘、撤托盘），再关窗口；close 事件里同一套逻辑会再走一遍。
    stopProxy();
    save();
    removeTray();
    if (win) {
      try { win.close(true); } catch (e) {}
    } else if (hasNw) {
      try { nw.App.quit(); } catch (e) {}
    }
  }

  // ---------------------------------------------------------------- 列表交互

  function askRemove(id) {
    var item = find(id);
    if (!item) return;
    askConfirm('删除供应商', '将删除「' + item.name + '」，删除后无法恢复。', function () {
      removeProvider(id);
    });
  }

  listEl.addEventListener('click', function (event) {
    var actionBtn = null;
    var el = event.target;
    while (el && el !== listEl) {
      if (el.getAttribute && el.getAttribute('data-act')) actionBtn = el;
      if (el.classList && el.classList.contains('card')) break;
      el = el.parentNode;
    }
    if (!el || !el.getAttribute) return;
    var type = el.getAttribute('data-type');
    var id = el.getAttribute('data-id');
    if (actionBtn) {
      event.stopPropagation();
      var act = actionBtn.getAttribute('data-act');
      if (type === 'group') {
        if (act === 'group-edit') openGroupDialog(id);
        else if (act === 'group-remove') askRemoveGroup(id);
        return;
      }
      var targetId = actionBtn.getAttribute('data-id') || id;
      if (act === 'edit') openEdit(targetId);
      else if (act === 'toggle-enabled') toggleItemEnabled(targetId);
      else if (act === 'remove') askRemove(targetId);
      return;
    }
    if (type === 'auto') toggleAuto(id);
    else if (type === 'group') enterGroup(id);
    else activate(id);
  });

  listEl.addEventListener('contextmenu', function (event) {
    var el = event.target;
    while (el && el !== listEl && !(el.classList && el.classList.contains('card'))) {
      el = el.parentNode;
    }
    if (!el || !el.getAttribute) return;
    var type = el.getAttribute('data-type');
    var id = el.getAttribute('data-id');
    if (type === 'group') {
      var group = findGroup(id);
      if (!group) return;
      event.preventDefault();
      showMenuXY(event.clientX, event.clientY, [
        { label: '重命名组', icon: 'i-edit', run: function () { openGroupDialog(id); } },
        { label: '删除组', icon: 'i-trash', run: function () { askRemoveGroup(id); } }
      ]);
      return;
    }
    var item = find(id);
    if (!item) return;
    event.preventDefault();
    showMenuXY(event.clientX, event.clientY, [
      { label: '编辑项', icon: 'i-edit', run: function () { openEdit(id); } },
      { label: (item.enabled === false ? '启用此模型' : '禁用此模型'), icon: 'i-eye', run: function () { toggleItemEnabled(id); } },
      { label: '删除项', icon: 'i-trash', run: function () { askRemove(id); } }
    ]);
  });

  // ---------------------------------------------------------------- 启动

  render();
  // 首屏先显示内存默认数据；持久化配置在首帧后读取，避免同步 fs 阻塞页面绘制。
  setTimeout(function () {
    load();
    render();
    initTray();
    if (state.proxy.enabled) startProxy();
  }, 0);
  if (win) {
    win.setTitle('VG Switch');
    win.on('close', function () {
      // 这个分支只在"真的要关"时才会走到（本运行时 close() 发完事件就关，否决不了），
      // 所以这里只做收尾：停代理、存盘、撤托盘。
      stopProxy();
      save();
      removeTray();
      this.close(true);
    });
  }

})();
