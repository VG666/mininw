// api_nw_internal.js —— native 绑定层。对应 SDK：src/resources/api_window_internal.js。
//
// SDK 里这层是"一句话一个 native 请求"（bindingUtil.sendRequestSync('nw.currentWindowInternal.getTitleInternal')），
// 对象语义全留给上层模块。本实现沿用同一条分界线，但底下换成了本桥的两条通道：
//   internalRead()     要结果，失败就抛。调用方决定要不要退化成兜底值。
//   internalReadSafe() 要结果，拿不到用兜底值，原因记进 window.__nmbRpcErrors。
//   internalTell()     只求做到，失败记进 window.__nmbAsyncErrors 并 warn。
//
// **哪条命令能同步由 native 说了算**（nw_host.cpp 的 SyncSafeCommand），页面不该自己猜，
// 所以这个判断收在本文件里：上层模块只表达"我要读"还是"我要改"，别再直接 rpc/tell。

  // 下面这份前缀清单**不是开关**，是 native 那份白名单的页面侧镜像，用途只有对账：
  // 某条命令没列在这儿却又能同步走通（或反过来），说明两边漂移了，记进
  // window.__nmbSyncSurprises 让自检能发现。权威永远是 native。
  var syncPrefixes = ['app.info', 'fs.', 'process.', 'clipboard.', 'screen.', 'shell.', 'win.state'];
  window.__nmbSyncPrefixes = syncPrefixes;

  function syncPrefixCovers(command) {
    for (var i = 0; i < syncPrefixes.length; i++) {
      if (command.indexOf(syncPrefixes[i]) === 0) return true;
    }
    return false;
  }

  function internalRead(command, id, payload) {
    payload = payload || {};
    if (id !== undefined && id !== null) payload.id = id;
    if (!syncPrefixCovers(command)) {
      (window.__nmbSyncSurprises = window.__nmbSyncSurprises || []).push(command);
    }
    return rpc(command, payload);
  }

  function internalReadSafe(command, id, payload, fallback) {
    payload = payload || {};
    if (id !== undefined && id !== null) payload.id = id;
    return rpcSafe(command, payload, fallback);
  }

  function internalTell(command, id, payload) {
    payload = payload || {};
    if (id !== undefined && id !== null) payload.id = id;
    tell(command, payload);
  }
