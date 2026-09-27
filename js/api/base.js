// base.js —— 页面侧 nw.* 层的地基：两条通道（同步取数 / 异步动手）、事件发射器、公共小工具。
//
// 与 SDK 的对应：src/resources/base.js 那类基础设施，加上内核 bindingUtil 的角色。
// 各模块共用同一个函数作用域——宿主把 api\*.js **拼成一个脚本**（顺序由同目录
// modules.txt 显式指定，不再看文件名字典序），外面统一包
// 一层 IIFE（含 window.__nwApiInstalled 幂等判断；不能叫 __nmbInstalled，桥的注入脚本占它）。所以：不要给单个模块再套自己的 IIFE，
// 也不要再写 'use strict'（已经在外层了）。本文件必须在 modules.txt 里排第一。

// 本层（nw\api\*.js）—— 在页面里重建 nw.*，按 SDK 的模块边界拆成多个文件。
//
// 与 nw.js 的关系：文件名/对象形状/事件名照抄 nw.js（这样原有 nw 应用不用改代码），
// 但底下的实现全部换成本桥的通道，而不是 nw.js 里 renderer 侧那套 V8 binding。
// 渲染/排版/网络一概不在这层，全部由 miniblink 内核负责。
//
// ---------------------------------------------------------------------------
// 三条同步候选通道 + 一条异步通道，别混用（内核的限制，不是设计偏好）
// ---------------------------------------------------------------------------
// 内核里 **没有任何"同步绑定 JS 函数"的导出**（mb132 把 wkeJsBindFunction 那套删了），
// 页面→native 的回调通道 mbQuery 的应答是内核在**后续任务**里调 __onMbQuery__ 投递的
// ——也就是说 mbQuery 返回时结果还没到，同步读必然是 undefined。
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
// 同步取数（rpc）按下面顺序探测，探通谁用谁，一次会话内不变：
//   1) loopback HTTP（mb108 主通道）——同步 XHR POST 到宿主只监听 127.0.0.1 的随机端口，
//      路径带窗口号、query 带每进程随机令牌。回程是标准 TCP 字节流，不经过内核的
//      prompt 返回值路径（那条路有内核级竞态，会概率性丢成 null/脏串）。
//   2) window.prompt（兼容退路）——内核支持 onPromptBox 时可用，但 mb108 回程有竞态，
//      只留少量重试，不再作为主通道。
//   3) 同步 XHR 打 nmb-rpc:// / 同源 file 路径（mb132 路径）——宿主在 onLoadUrlBegin
//      里当场作答，跑在内核网络线程上，只放行纯数据命令。
//
//   rpcAsync() —— 动手。走 mbQuery（异步，返回值拿不到），碰窗口/菜单/托盘/热键这类必须
//                 在 UI 线程上做的事。nw 里这些接口本来也不靠返回值。
  // ---------------------------------------------------------------------
  // 同步取数：loopback HTTP（主）/ prompt（退路）/ 同步 XHR 拦截（旧内核）
  // ---------------------------------------------------------------------
  // mbQuery 是内核往页面里注入的，注入时机不由本脚本决定：别在顶部捕一份可能还是
  // undefined 的快照，到用的时候再取。
  function queryChannel() {
    return typeof window.mbQuery === 'function' ? window.mbQuery : null;
  }

  var syncBase = null;        // 探测成功后的通道标记（XHR 通道时是 URL 基址）；null = 不可用
  var useLoopbackChannel = false;
  var usePromptChannel = false;

  // mb108 会在网络 hook 之前拒绝跨源 HTTP 和未知 scheme，但同源 file:// XHR 正常。
  // 用相对路径生成同源 file URL，native 在真正访问磁盘前按固定路径段接管。
  var SYNC_CANDIDATES = ['./__nmb_rpc__/', 'http://nmb-rpc/rpc/', 'nmb-rpc://rpc/'];
  // prompt 同步通道（兼容退路）：window.prompt 是同步模态——页面 JS 阻塞在 prompt()
  // 上，native 在 mbOnPromptBox 里当场作答，返回值就是应答 JSON。mb108 的回程有内核
  // 级竞态，所以它只作退路，且只留少量重试。载荷编码复用 base64（见 encodePayload）。
  var PROMPT_MARKER = '__nmb_rpc__:';
  var PROMPT_MAX_ATTEMPTS = 3;

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

  // ---- 通道 1a：loopback HTTP（mb108 主通道）----
  // 端口/令牌由 native 随注入脚本预给（window.__nmbRpcPort/__nmbRpcToken），每个进程
  // 都不同。窗口号走路径（window.__nwWindowId 同样是预注入的），令牌走 query。
  function loopbackEndpoint() {
    var port = window.__nmbRpcPort;
    var token = window.__nmbRpcToken;
    if (!port || !token) return null;
    if (window.__nwWindowId === undefined || window.__nwWindowId === null) return null;
    return 'http://127.0.0.1:' + port + '/w' + window.__nwWindowId +
           '/?t=' + encodeURIComponent(token);
  }

  function loopbackOnce(request) {
    var url = loopbackEndpoint();
    if (!url) throw new Error('loopback 通道未配置（端口或令牌缺失）');
    // 连接级失败（对端 RST/瞬断，send() 直接抛）换一条新连接再试一次；HTTP 状态码
    // 错误（403/404…）不重试——那是协议/令牌问题，重试不会变。同步通道的命令都是
    // 读或整文件覆盖/剪贴板写这类可重放操作，多打一遍不会产生不同结果。
    var lastError = null;
    for (var attempt = 0; attempt < 2; attempt++) {
      var xhr = new XMLHttpRequest();
      xhr.open('POST', url, false);   // false = 同步，就靠这个
      // 同步 XHR 不能触发 CORS 预检（预检是 OPTIONS，async=false 不允许），所以请求必须
      // 保持"简单请求"：方法 POST + 以下三种安全 Content-Type 之一，且不加任何自定义头。
      // 令牌放在 URL query 里而不是 X-NMB-Token 头里，正是为了这条。
      xhr.setRequestHeader('Content-Type', 'text/plain;charset=UTF-8');
      try {
        xhr.send(encodePayload(request));
      } catch (error) {
        lastError = error;
        continue;
      }
      if (xhr.status !== 200) {
        throw new Error('loopback HTTP ' + xhr.status +
                        (xhr.responseText ? ' ' + String(xhr.responseText).slice(0, 80) : ''));
      }
      return xhr.responseText || '';
    }
    throw lastError || new Error('loopback 连接失败');
  }

  // ---- 通道 1b：prompt（兼容退路）----
  // mb108 把 mbCreateString 的句柄带回 JS 那一步有内核级竞态：native 应答正确，
  // 页面仍可能拿到 null/脏串。这里只做少量同步重试兜底，应答合法性以 JSON.parse 为准。
  function promptOnce(request) {
    var msg = PROMPT_MARKER + encodePayload(request);
    for (var attempt = 0; attempt < PROMPT_MAX_ATTEMPTS; attempt++) {
      var reply = null;
      try { reply = window.prompt(msg, ''); } catch (error) { reply = null; }
      // mb108 偶尔在有效 JSON 前后带 NUL；截取 JSON 信封后再解析，避免丢掉有效应答。
      if (typeof reply === 'string' && reply) {
        var start = reply.indexOf('{');
        var end = reply.lastIndexOf('}');
        var candidate = start >= 0 && end >= start ? reply.slice(start, end + 1) : reply;
        try { JSON.parse(candidate); return candidate; } catch (error) { /* 竞态脏应答：重试 */ }
      }
    }
    throw new Error('prompt 同步通道应答无效（重试 ' + PROMPT_MAX_ATTEMPTS + ' 次仍失败）');
  }

  // ---- 通道 1c：同步 XHR 拦截（mb132 的 onLoadUrlBegin 路径）----
  function xhrOnce(base, request) {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', base + encodePayload(request), false);   // false = 同步，就靠这个
    xhr.send();
    // 自定义 scheme 的响应没有真正的状态码（内核不给设），所以 0 也算成功，
    // 判成功看的是"拿到了正文能解析出信封"。失败时同步 XHR 会直接抛异常。
    return xhr.responseText || '';
  }

  function sendSync(base, request) {
    if (useLoopbackChannel) return loopbackOnce(request);
    if (usePromptChannel) return promptOnce(request);
    return xhrOnce(base, request);
  }

  function probeSyncChannel() {
    // 1) loopback HTTP：一条真实 app.info 既完成探测又留下结果，省一次往返。
    //    这是 mb108 的主通道——回程走标准 TCP，不经过有竞态的 prompt 返回值路径。
    if (loopbackEndpoint()) {
      try {
        var httpParsed = JSON.parse(loopbackOnce('{"c":"app.info"}'));
        if (httpParsed && httpParsed.v) {
          useLoopbackChannel = true;
          syncBase = 'http-loopback';
          window.__nmbSyncChannel = 'http-loopback';
          return httpParsed.v;
        }
      } catch (error) {
        window.__nmbSyncProbeError = 'loopback: ' + String(error && error.message || error);
      }
    }
    // 2) prompt：内核支持时的兼容退路，探测本身也吃过竞态，给两轮机会。
    if (window.__nmbPromptAvailable) {
      for (var attempt = 0; attempt < 2; attempt++) {
        try {
          var parsed = JSON.parse(promptOnce('{"c":"app.info"}'));
          if (parsed && parsed.v) {
            usePromptChannel = true;
            syncBase = 'prompt';   // 占位：rpc() 只要求"探测已通过"，XHR 基址不再使用
            window.__nmbSyncChannel = 'prompt';
            return parsed.v;
          }
        } catch (error) {
          window.__nmbSyncProbeError =
            (window.__nmbSyncProbeError ? window.__nmbSyncProbeError + '; ' : '') +
            'prompt: ' + String(error && error.message || error);
        }
      }
    }
    // 3) 同步 XHR 候选（mb132 的网络回调路径）。
    for (var i = 0; i < SYNC_CANDIDATES.length; i++) {
      try {
        var xhrParsed = JSON.parse(xhrOnce(SYNC_CANDIDATES[i], '{"c":"app.info"}'));
        if (xhrParsed && xhrParsed.v) {
          syncBase = SYNC_CANDIDATES[i];
          window.__nmbSyncChannel = syncBase;
          return xhrParsed.v;
        }
      } catch (error) {
        window.__nmbSyncProbeError =
          (window.__nmbSyncProbeError ? window.__nmbSyncProbeError + '; ' : '') +
          String(error && error.message || error);
      }
    }
    return null;
  }

  function rpc(command, payload) {
    // mb108 不允许在脚本上下文创建回调内发同步 XHR；初始化数据由 native 预注入，
    // 页面开始运行后第一次真正的同步操作再探测，此时网络栈已经可重入。
    if (!syncBase) probeSyncChannel();
    if (!syncBase) throw new Error('同步通道不可用（无法同步拿到 ' + command + ' 的结果）');
    payload = payload || {};
    payload.c = command;
    var text = sendSync(syncBase, JSON.stringify(payload));
    var parsed;
    try {
      parsed = JSON.parse(text);
    } catch (error) {
      // 应答不是 JSON：把原文带出来，title 取证能直接看到内核到底回了什么。
      throw new Error(command + ' 应答不是 JSON：[' + String(text).slice(0, 40) + ']');
    }
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
  function exposeStatics(ctor, skip) {
    for (var name in ctor.prototype) {
      if (skip && skip.indexOf(name) >= 0) continue;
      if (typeof ctor.prototype[name] === 'function') ctor[name] = ctor.prototype[name];
    }
  }
