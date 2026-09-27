// api_nw_clipboard.js —— nw.Clipboard。对应 SDK：src/resources/api_nw_clipboard.js。
// 静态 get() 取单例，实例上的 get() 才是"读数据"——两个 get 不是一回事，别合并。
//
// 类型对应（native 侧 ApiClipboard）：text/raw → CF_UNICODETEXT，html → CF_HTML
// （头部回填/剥离在宿主做，页面拿到的就是片段），png/rtf → 注册格式。
// JSON 通道装不下裸字节：png/rtf 与宿主约定用 base64 交换；rtf 本质是文本，
// set 端在这里自动编 base64，get 端自动解回文本；png 保持 base64 进出。

  function bytesToBase64(bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; i += 0x8000) {
      out += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(out);
  }

  function base64ToText(text) {
    var bin = atob(text);
    var bytes = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new TextDecoder('utf-8').decode(bytes);
  }

  function Clipboard() {}
  Clipboard.prototype.get = function (type) {
    var kind = type || 'text';
    var data = rpc('clipboard.read', { type: kind }).data;
    return kind === 'rtf' && data ? base64ToText(data) : data;
  };
  Clipboard.prototype.set = function (data, type) {
    var kind = type || 'text';
    var payload = data;
    if (kind === 'rtf') payload = btoa(utf8Binary(String(data)));
    else if (kind === 'png' && data instanceof Uint8Array) payload = bytesToBase64(data);
    return rpc('clipboard.write', { data: String(payload), type: kind });
  };
  Clipboard.prototype.clear = function () { return rpc('clipboard.clear'); };
  // 宿主 EnumClipboardFormats 真实枚举并映射成 nw 类型名；通道没通时退到 ['text']。
  Clipboard.prototype.readAvailableTypes = function () {
    return rpcSafe('clipboard.availableTypes', {}, ['text']);
  };
  exposeStatics(Clipboard, ['get']);
  var clipboardSingleton = null;
  Clipboard.get = function () {
    if (!clipboardSingleton) clipboardSingleton = new Clipboard();
    return clipboardSingleton;
  };
