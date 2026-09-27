// nw_node.js —— "内核跟 node 之间的桥梁"的 node 一侧。
//
// 先说清楚边界：nw.js 是真的把 V8 实例（node）塞进 renderer，所以页面里跑的是货真价实的
// node —— 能加载原生扩展、能跑 npm 包、net/dgram/child_process 全有。本实现没有 renderer，
// 也不打算为了这件事把 node 拖进来：这里提供的是 CommonJS 运行时 + 常用内置模块的
// 纯 JS 实现，凡是必须落到操作系统的原语（文件、环境、进程）都通过 __nmbRpc 回到 native。
//
// 所以这一层是"桥"，不是 node：
//   * 有：require（含 node_modules 解析）、module/exports、process、Buffer、path、
//         fs(同步 API)、os、url、querystring、events、util、assert、timers
//   * 没有：原生扩展（.node）、child_process、net/tls/dgram/http 服务端、worker_threads、
//         以及依赖它们的 npm 包
// 页面的 nw.require 就是这里的 require。要判断某个包能不能用，看它有没有碰上面"没有"那栏。

(function () {
  'use strict';
  if (window.__nmbNodeInstalled) return;
  window.__nmbNodeInstalled = true;

  var rpc = window.__nmbRpc;
  var rpcAsync = window.__nmbRpcAsync;
  if (typeof rpc !== 'function') {
    console.error('[nw] nw_node.js must be injected after the api bundle (js\\api\\*.js)');
    return;
  }

  // 这一条是整个 node 层的门：同步通道不通，readFileSync 之类就不可能真同步。
  // 但也不能让异常冒出去把 window.nw 一起带走，所以退化成一个空壳并把原因记下来。
  var bootProcess = window.__nmbBootProcessReply;
  var info = bootProcess && bootProcess.v ? bootProcess.v : null;
  if (!info) {
    try {
      info = rpc('process.info');
    } catch (error) {
      console.error('[nw] node 桥不可用（同步通道没通）：' + error.message);
      window.__nmbNodeError = String(error && error.message || error);
      info = { platform: 'win32', arch: 'x64', cwd: '', argv: [], env: '', versions: {}, pid: 0 };
    }
  }
  var isWindows = info.platform === 'win32';

  // ---------------------------------------------------------------------
  // Buffer：node 的 Buffer 是 Uint8Array 的子类，这里按同一形状做
  // ---------------------------------------------------------------------
  var BASE64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

  function decodeBase64(text) {
    var clean = String(text).replace(/[^A-Za-z0-9+/]/g, '');
    var out = new Uint8Array(Math.floor((clean.length * 3) / 4));
    var accumulator = 0;
    var bits = 0;
    var at = 0;
    for (var i = 0; i < clean.length; i++) {
      var value = BASE64.indexOf(clean[i]);
      if (value < 0) continue;
      accumulator = (accumulator << 6) | value;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out[at++] = (accumulator >> bits) & 0xff;
      }
    }
    return out.subarray(0, at);
  }

  function encodeBase64(bytes) {
    var out = '';
    var at = 0;
    while (at + 2 < bytes.length) {
      var chunk = (bytes[at] << 16) | (bytes[at + 1] << 8) | bytes[at + 2];
      out += BASE64[(chunk >> 18) & 63] + BASE64[(chunk >> 12) & 63] + BASE64[(chunk >> 6) & 63] + BASE64[chunk & 63];
      at += 3;
    }
    if (at < bytes.length) {
      var two = at + 1 < bytes.length;
      var last = (bytes[at] << 16) | (two ? bytes[at + 1] << 8 : 0);
      out += BASE64[(last >> 18) & 63] + BASE64[(last >> 12) & 63] + (two ? BASE64[(last >> 6) & 63] : '=') + '=';
    }
    return out;
  }

  function utf8Encode(text) {
    var out = [];
    for (var i = 0; i < text.length; i++) {
      var code = text.charCodeAt(i);
      if (code < 0x80) out.push(code);
      else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 63));
      else if (code >= 0xd800 && code <= 0xdbff) {
        var low = text.charCodeAt(++i);
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        out.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 63), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
      } else out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
    }
    return new Uint8Array(out);
  }

  function utf8Decode(bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; ) {
      var byte = bytes[i++];
      if (byte < 0x80) out += String.fromCharCode(byte);
      else if (byte < 0xe0) out += String.fromCharCode(((byte & 0x1f) << 6) | (bytes[i++] & 63));
      else if (byte < 0xf0) out += String.fromCharCode(((byte & 0x0f) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63));
      else {
        var code = ((byte & 0x07) << 18) | ((bytes[i++] & 63) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
        code -= 0x10000;
        out += String.fromCharCode(0xd800 + (code >> 10), 0xdc00 + (code & 1023));
      }
    }
    return out;
  }

  // node 里 Buffer 是 Uint8Array 的子类，toString 是"按编码解码"。裸 Uint8Array 的
  // toString() 是"字节值用逗号连起来"（Buffer.from('nw').toString() 会得到 "110,119"），
  // 站点按 node 语义用就会拿到一串数字。
  // 修法是在**实例**上装一个 own property 的 toString 把它盖住——不动
  // Uint8Array.prototype，免得把页面里别的 typed array 的语义一起改了。
  function bufferToString(bytes, encoding) {
    var name = String(encoding || 'utf8').toLowerCase();
    if (name === 'hex') {
      var hex = '';
      for (var i = 0; i < bytes.length; i++) hex += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
      return hex;
    }
    if (name === 'base64') return encodeBase64(bytes);
    if (name === 'binary' || name === 'latin1') {
      var raw = '';
      for (var j = 0; j < bytes.length; j++) raw += String.fromCharCode(bytes[j]);
      return raw;
    }
    // utf8 / ucs2 / 其它：按 UTF-8 解。ucs2 归到这里是本实现的已知近似。
    return utf8Decode(bytes);
  }

  function asBuffer(bytes) {
    var buffer = (bytes instanceof Uint8Array) ? bytes : new Uint8Array(bytes || 0);
    Object.defineProperty(buffer, 'toString', {
      value: function (encoding) { return bufferToString(buffer, encoding); },
      writable: true, enumerable: false, configurable: true
    });
    return buffer;
  }

  function BufferArg(value, encoding) {
    if (value instanceof Uint8Array) return asBuffer(value);
    return asBuffer(utf8Encode(String(value)));
  }

  BufferArg.isBuffer = function (value) { return value instanceof Uint8Array; };
  BufferArg.from = function (value, encoding) {
    if (typeof value === 'string' && String(encoding).toLowerCase() === 'base64') return asBuffer(decodeBase64(value));
    if (typeof value === 'string') return asBuffer(utf8Encode(value));
    if (value instanceof Uint8Array) return asBuffer(value.slice());
    if (Array.isArray(value)) return asBuffer(new Uint8Array(value));
    return asBuffer(new Uint8Array(0));
  };
  BufferArg.alloc = function (size) { return asBuffer(new Uint8Array(size)); };
  BufferArg.allocUnsafe = BufferArg.alloc;
  BufferArg.byteLength = function (text, encoding) { return BufferArg.from(text, encoding).length; };
  BufferArg.isEncoding = function (encoding) { return /^(utf-?8|base64|binary|latin1|hex)$/i.test(String(encoding)); };

  // ---------------------------------------------------------------------
  // path（nw.js 里这来自 node；这里手写）
  // ---------------------------------------------------------------------
  var path = {
    sep: isWindows ? '\\' : '/',
    delimiter: isWindows ? ';' : ':'
  };
  path.normalize = function (input) {
    var text = String(input).replace(/[\\/]+/g, path.sep);
    var drive = '';
    var match = /^([A-Za-z]:)/.exec(text);
    if (match) { drive = match[1]; text = text.slice(drive.length); }
    var rooted = text.charAt(0) === path.sep;
    var parts = text.split(path.sep);
    var stack = [];
    for (var i = 0; i < parts.length; i++) {
      var part = parts[i];
      if (!part || part === '.') continue;
      if (part === '..' && stack.length && stack[stack.length - 1] !== '..') stack.pop();
      else if (part === '..' && !rooted) stack.push('..');
      else if (part !== '..') stack.push(part);
    }
    var out = drive + (rooted ? path.sep : '') + stack.join(path.sep);
    return out || '.';
  };
  path.join = function () {
    var parts = Array.prototype.slice.call(arguments).filter(function (part) { return part && String(part).length; });
    return parts.length ? path.normalize(parts.join(path.sep)) : '.';
  };
  path.resolve = function () {
    var parts = Array.prototype.slice.call(arguments);
    var result = '';
    for (var i = parts.length - 1; i >= 0; i--) {
      if (!parts[i]) continue;
      result = result ? path.join(parts[i], result) : String(parts[i]);
      if (/^([A-Za-z]:[\\/]|[\\/])/.test(String(parts[i]))) break;
    }
    if (!/^([A-Za-z]:|[\\/])/.test(result)) result = path.join(process.cwd(), result);
    return path.normalize(result);
  };
  path.dirname = function (input) {
    var text = String(input).replace(/[\\/]+$/, '');
    var at = Math.max(text.lastIndexOf('\\'), text.lastIndexOf('/'));
    if (at < 0) return '.';
    if (at === 2 && text.charAt(1) === ':') return text.slice(0, 3);
    return text.slice(0, at) || path.sep;
  };
  path.basename = function (input, extension) {
    var text = String(input).replace(/[\\/]+$/, '');
    var at = Math.max(text.lastIndexOf('\\'), text.lastIndexOf('/'));
    var base = at < 0 ? text : text.slice(at + 1);
    if (extension && base.slice(-extension.length) === extension) base = base.slice(0, -extension.length);
    return base;
  };
  path.extname = function (input) {
    var base = path.basename(input);
    var at = base.lastIndexOf('.');
    return at <= 0 ? '' : base.slice(at);
  };
  path.isAbsolute = function (input) { return /^([A-Za-z]:[\\/]|[\\/])/.test(String(input)); };
  path.relative = function (from, to) {
    var a = path.resolve(from).split(path.sep);
    var b = path.resolve(to).split(path.sep);
    var i = 0;
    while (i < a.length && i < b.length && a[i].toLowerCase() === b[i].toLowerCase()) i++;
    var up = [];
    for (var j = i; j < a.length; j++) up.push('..');
    return up.concat(b.slice(i)).join(path.sep);
  };
  path.parse = function (input) {
    return {
      root: path.isAbsolute(input) ? path.dirname(input).split(path.sep)[0] + path.sep : '',
      dir: path.dirname(input),
      base: path.basename(input),
      ext: path.extname(input),
      name: path.basename(input, path.extname(input))
    };
  };
  path.format = function (object) {
    return (object.dir ? object.dir + path.sep : '') + (object.base || (object.name || '') + (object.ext || ''));
  };

  // ---------------------------------------------------------------------
  // process
  // ---------------------------------------------------------------------
  var env = {};
  info.env.split('\n').forEach(function (line) {
    var at = line.indexOf('=');
    if (at > 0) env[line.slice(0, at)] = line.slice(at + 1);
  });

  var process = {
    platform: info.platform,
    arch: info.arch,
    pid: info.pid,
    argv: [info.execPath].concat(info.argv ? info.argv.slice(1) : []),
    env: env,
    version: 'v' + info.nodeVersion,
    versions: { node: info.nodeVersion, nw: '0.0.0-bridge', kernel: 'miniblink' },
    execPath: info.execPath,
    cwd: function () { return info.cwd; },
    chdir: function (target) {
      rpc('process.chdir', { path: target });
      info.cwd = target;
    },
    exit: function (code) { rpc('process.exit', { code: code || 0 }); },
    nextTick: function (callback) {
      var args = Array.prototype.slice.call(arguments, 1);
      Promise.resolve().then(function () { callback.apply(null, args); });
    },
    hrtime: function (previous) {
      var now = performance.now() / 1000;
      var seconds = Math.floor(now);
      var nanos = Math.floor((now - seconds) * 1e9);
      if (previous) {
        seconds -= previous[0];
        nanos -= previous[1];
        if (nanos < 0) { seconds -= 1; nanos += 1e9; }
      }
      return [seconds, nanos];
    },
    memoryUsage: function () {
      return { rss: 0, heapTotal: 0, heapUsed: 0, external: 0 };
    },
    on: function () { return process; },
    emit: function () { return false; }
  };

  // ---------------------------------------------------------------------
  // fs：同步 API（nw 应用里同步读写极常见，异步版用 Promise 包一层即可）
  // ---------------------------------------------------------------------
  function makeError(code, message, target) {
    var error = new Error(code + ': ' + message + ', ' + target);
    error.code = code;
    error.path = target;
    return error;
  }

  var fs = {};
  fs.existsSync = function (target) { return rpc('fs.exists', { path: target }).exists; };
  fs.statSync = function (target) {
    var stat = rpc('fs.stat', { path: target });
    if (!stat.exists) throw makeError('ENOENT', 'no such file or directory', target);
    stat.isDirectory = function () { return stat.isDirectory; };
    stat.isFile = function () { return stat.isFile; };
    stat.mtime = new Date(stat.mtimeMs);
    return stat;
  };
  fs.lstatSync = fs.statSync;
  fs.readFileSync = function (target, options) {
    var encoding = typeof options === 'string' ? options : (options && options.encoding) || null;
    var name = encoding ? String(encoding).toLowerCase() : null;
    // 宿主只认 utf8 / base64 两种取法；hex 之类先按 base64 拿字节，本地再编码。
    var wire = (!name || name === 'utf8' || name === 'utf-8') ? 'utf8' : 'base64';
    var result = rpc('fs.readFile', { path: target, encoding: wire });
    // 不给 encoding 时 node 返回 Buffer；桥是以 utf8 文本回的，这里转回字节。
    if (!name) return asBuffer(utf8Encode(result.data));
    if (name === 'base64') return result.data;
    if (wire === 'base64') return bufferToString(decodeBase64(result.data), name);
    return result.data;
  };
  fs.writeFileSync = function (target, data, options) {
    var isBytes = data instanceof Uint8Array;
    rpc('fs.writeFile', {
      path: target,
      data: isBytes ? encodeBase64(data) : String(data),
      encoding: isBytes ? 'base64' : 'utf8'
    });
  };
  fs.appendFileSync = function (target, data) {
    var isBytes = data instanceof Uint8Array;
    rpc('fs.appendFile', {
      path: target,
      data: isBytes ? encodeBase64(data) : String(data),
      encoding: isBytes ? 'base64' : 'utf8'
    });
  };
  fs.readdirSync = function (target) { return rpc('fs.readdir', { path: target }); };
  fs.mkdirSync = function (target, options) {
    rpc('fs.mkdir', { path: target, recursive: !options || options.recursive !== false });
  };
  fs.rmdirSync = function (target) { rpc('fs.unlink', { path: target }); };
  fs.unlinkSync = function (target) { rpc('fs.unlink', { path: target }); };
  fs.renameSync = function (from, to) { rpc('fs.rename', { path: from, to: to }); };
  fs.copyFileSync = function (from, to) {
    rpc('fs.writeFile', { path: to, data: rpc('fs.readFile', { path: from, encoding: 'base64' }).data, encoding: 'base64' });
  };
  fs.realpathSync = function (target) { return rpc('fs.realpath', { path: target }).path; };
  fs.exists = function (target, callback) { callback(fs.existsSync(target)); };
  fs.readFile = function (target, options, callback) {
    var handler = typeof options === 'function' ? options : callback;
    try { handler(null, fs.readFileSync(target, options)); } catch (error) { handler(error); }
  };
  fs.writeFile = function (target, data, options, callback) {
    var handler = typeof options === 'function' ? options : callback;
    var isBytes = data instanceof Uint8Array;
    if (typeof rpcAsync !== 'function') {
      if (handler) handler(makeError('ENOSYS', '异步 RPC 不可用', target));
      return;
    }
    rpcAsync('fs.writeFile', {
      path: target,
      data: isBytes ? encodeBase64(data) : String(data),
      encoding: isBytes ? 'base64' : 'utf8'
    }, function (error) { if (handler) handler(error || null); });
  };
  // 真·异步：走 mbQuery（请求方向可靠，无需应答回程）。mb108 没有同步通道，
  // 之前包 appendFileSync 的假异步在 mb108 上必然抛错，测试报告就永远写不出来。
  fs.appendFile = function (target, data, options, callback) {
    var handler = typeof options === 'function' ? options : callback;
    var isBytes = data instanceof Uint8Array;
    rpcAsync('fs.appendFile', {
      path: target,
      data: isBytes ? encodeBase64(data) : String(data),
      encoding: isBytes ? 'base64' : 'utf8'
    }, function (error) { if (handler) handler(error || null); });
  };
  fs.readdir = function (target, callback) {
    try { callback(null, fs.readdirSync(target)); } catch (error) { callback(error); }
  };
  fs.stat = function (target, callback) {
    try { callback(null, fs.statSync(target)); } catch (error) { callback(error); }
  };
  fs.promises = {
    readFile: function (target, options) { return Promise.resolve().then(function () { return fs.readFileSync(target, options); }); },
    writeFile: function (target, data, options) {
      return new Promise(function (resolve, reject) {
        fs.writeFile(target, data, options, function (error) { return error ? reject(error) : resolve(); });
      });
    },
    readdir: function (target) { return Promise.resolve().then(function () { return fs.readdirSync(target); }); },
    stat: function (target) { return Promise.resolve().then(function () { return fs.statSync(target); }); },
    mkdir: function (target, options) { return Promise.resolve().then(function () { return fs.mkdirSync(target, options); }); },
    unlink: function (target) { return Promise.resolve().then(function () { return fs.unlinkSync(target); }); }
  };
  fs.constants = { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1 };

  // ---------------------------------------------------------------------
  // 其余内置模块（纯 JS 版本）
  // ---------------------------------------------------------------------
  var EventEmitter = window.nw && window.nw.__Emitter;
  var events = { EventEmitter: EventEmitter };
  var os = {
    platform: function () { return process.platform; },
    arch: function () { return process.arch; },
    type: function () { return 'Windows_NT'; },
    homedir: function () { return process.env.USERPROFILE || process.env.HOME || process.cwd(); },
    tmpdir: function () { return process.env.TEMP || process.env.TMP || process.cwd(); },
    hostname: function () { return process.env.COMPUTERNAME || 'localhost'; },
    cpus: function () { return []; },
    totalmem: function () { return 0; },
    freemem: function () { return 0; },
    EOL: isWindows ? '\r\n' : '\n'
  };
  var url = {
    parse: function (input) {
      var match = /^([a-z][a-z0-9+.-]*):\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(String(input));
      if (!match) return { href: String(input), pathname: String(input), query: null, hash: null };
      var authority = match[2] || '';
      var at = authority.indexOf('@');
      var auth = at >= 0 ? authority.slice(0, at) : '';
      var host = at >= 0 ? authority.slice(at + 1) : authority;
      var colon = host.lastIndexOf(':');
      return {
        protocol: match[1].toLowerCase() + ':',
        auth: auth,
        host: host,
        hostname: colon > 0 ? host.slice(0, colon) : host,
        port: colon > 0 ? host.slice(colon + 1) : null,
        pathname: match[3] || '/',
        search: match[4] || '',
        query: (match[4] || '').replace(/^\?/, ''),
        hash: match[5] || '',
        href: String(input)
      };
    },
    format: function (object) {
      var auth = object.auth ? object.auth + '@' : '';
      return (object.protocol || '') + '//' + auth + (object.host || object.hostname || '') + (object.pathname || '');
    },
    resolve: function (from, to) {
      try { return new URL(to, from).href; } catch (error) { return to; }
    }
  };
  var querystring = {
    parse: function (text) {
      var out = {};
      String(text).replace(/^\?/, '').split('&').forEach(function (pair) {
        if (!pair) return;
        var at = pair.indexOf('=');
        var key = at < 0 ? pair : pair.slice(0, at);
        var value = at < 0 ? '' : pair.slice(at + 1);
        out[decodeURIComponent(key)] = decodeURIComponent(value.replace(/\+/g, ' '));
      });
      return out;
    },
    stringify: function (object) {
      return Object.keys(object || {}).map(function (key) {
        return encodeURIComponent(key) + '=' + encodeURIComponent(object[key]);
      }).join('&');
    },
    escape: encodeURIComponent,
    unescape: decodeURIComponent
  };
  var util = {
    format: function () {
      var parts = Array.prototype.slice.call(arguments);
      var head = parts.shift();
      if (typeof head !== 'string') return parts.length ? head + ' ' + parts.join(' ') : String(head);
      return head.replace(/%[sdifjoO%]/g, function (token) {
        if (token === '%%') return '%';
        var value = parts.shift();
        if (token === '%j') { try { return JSON.stringify(value); } catch (error) { return '[Circular]'; } }
        return String(value);
      }) + (parts.length ? ' ' + parts.join(' ') : '');
    },
    inspect: function (value) { try { return JSON.stringify(value, null, 2); } catch (error) { return String(value); } },
    inherits: function (child, parent) {
      child.super_ = parent;
      child.prototype = Object.create(parent.prototype, { constructor: { value: child, enumerable: false } });
    },
    promisify: function (fn) {
      return function () {
        var args = Array.prototype.slice.call(arguments);
        var self = this;
        return new Promise(function (resolve, reject) {
          args.push(function (error, value) { return error ? reject(error) : resolve(value); });
          fn.apply(self, args);
        });
      };
    },
    deprecate: function (fn) { return fn; },
    types: { isDate: function (value) { return value instanceof Date; } }
  };
  var assert = function (value, message) {
    if (!value) throw new Error(message || 'Assertion failed');
  };
  assert.equal = function (a, b, message) { if (a != b) throw new Error(message || (a + ' != ' + b)); };
  assert.strictEqual = function (a, b, message) { if (a !== b) throw new Error(message || (a + ' !== ' + b)); };
  assert.ok = assert;
  assert.deepStrictEqual = function (a, b, message) {
    if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(message || 'deepStrictEqual failed');
  };
  var timers = {
    setTimeout: function (fn, delay) { return window.setTimeout(fn, delay); },
    setInterval: function (fn, delay) { return window.setInterval(fn, delay); },
    setImmediate: function (fn) { return window.setTimeout(fn, 0); },
    clearTimeout: window.clearTimeout,
    clearInterval: window.clearInterval,
    clearImmediate: window.clearTimeout
  };

  var builtins = {
    buffer: { Buffer: BufferArg },
    events: events,
    fs: fs,
    os: os,
    path: path,
    process: process,
    querystring: querystring,
    timers: timers,
    url: url,
    util: util,
    assert: assert
  };

  // ---------------------------------------------------------------------
  // require：node_modules 逐级上溯 + package.json 的 main
  // ---------------------------------------------------------------------
  var moduleCache = {};

  function fileExists(target) {
    try { return rpc('fs.exists', { path: target }).exists; } catch (error) { return false; }
  }

  function resolveAsFile(target) {
    var candidates = [target, target + '.js', target + '.json'];
    for (var i = 0; i < candidates.length; i++) {
      if (fileExists(candidates[i])) return candidates[i];
    }
    return null;
  }

  function resolveModule(fromDirectory, request) {
    if (request.charAt(0) === '.' || path.isAbsolute(request)) {
      var base = path.resolve(fromDirectory, request);
      var direct = resolveAsFile(base);
      if (direct) return direct;
      var index = resolveAsFile(path.join(base, 'index'));
      return index;
    }
    // 裸模块名：从当前目录一路往上找 node_modules。
    var current = path.resolve(fromDirectory);
    for (;;) {
      var candidate = path.join(current, 'node_modules', request);
      var found = resolveAsFile(candidate) || resolveAsFile(path.join(candidate, 'index'));
      if (found) return found;
      var packageFile = path.join(candidate, 'package.json');
      if (fileExists(packageFile)) {
        try {
          var manifest = JSON.parse(fs.readFileSync(packageFile, 'utf8'));
          var main = manifest.main || 'index.js';
          var entry = resolveAsFile(path.join(candidate, main)) || resolveAsFile(path.join(candidate, 'index'));
          if (entry) return entry;
        } catch (error) { /* manifest 坏了就继续往上找 */ }
      }
      var parent = path.dirname(current);
      if (parent === current) return null;
      current = parent;
    }
  }

  function makeRequire(fromDirectory) {
    function require(request) {
      var builtin = builtins[String(request).replace(/^node:/, '')];
      if (builtin) return builtin;

      var resolved = resolveModule(fromDirectory, String(request));
      if (!resolved) {
        throw new Error("Cannot find module '" + request + "'（本桥只提供内置模块与纯 JS 包，原生扩展 .node 不支持）");
      }
      if (moduleCache[resolved]) return moduleCache[resolved].exports;

      var module = { id: resolved, filename: resolved, exports: {}, loaded: false };
      moduleCache[resolved] = module;

      if (/\.json$/i.test(resolved)) {
        module.exports = JSON.parse(fs.readFileSync(resolved, 'utf8'));
      } else {
        var source = fs.readFileSync(resolved, 'utf8');
        var wrapper = new Function('exports', 'require', 'module', '__filename', '__dirname', source);
        wrapper(module.exports, makeRequire(path.dirname(resolved)), module, resolved, path.dirname(resolved));
      }
      module.loaded = true;
      return module.exports;
    }
    require.resolve = function (request) { return resolveModule(fromDirectory, String(request)); };
    require.cache = moduleCache;
    require.main = { filename: appInfo_entry() };
    require.builtins = builtins;
    return require;
  }

  function appInfo_entry() {
    return path.join(info.appPath, '__nw_entry__.js');
  }

  var appRequire = makeRequire(info.appPath);

  // The bundled kernel exposes miniNodeRequire; other kernels may expose mbRequire.
  // Keep the RPC/CommonJS require independent of either native entry point.
  var nativeEntry = typeof window.miniNodeRequire === 'function' ? 'miniNodeRequire' :
    (typeof window.mbRequire === 'function' ? 'mbRequire' : null);
  var nativeRequire = nativeEntry ? function (request) {
    return window[nativeEntry].call(window, request);
  } : null;
  window.__nmbNativeNode = { require: nativeRequire, enabled: !!nativeRequire, entry: nativeEntry };
  window.__nmbNativeRequire = nativeRequire;
  if (window.nw) window.nw.requireNative = nativeRequire;

  window.require = appRequire;
  window.module = { exports: {}, id: '.', filename: appInfo_entry() };
  window.exports = window.module.exports;
  window.global = window;
  window.Buffer = BufferArg;
  window.process = process;
  window.__nmbRequire = appRequire;
  if (window.nw) window.nw.require = appRequire;

  // nw.js 的 process 是 App 级共享对象；这里同进程单窗口，直接给同一个引用。
  window.__nmbNode = {
    Buffer: BufferArg,
    process: process,
    require: appRequire,
    modules: builtins,
    // 原生扩展必需的东西没有，明确说清楚，省得用户去翻为什么 .node 加载失败。
    nativeAddons: false
  };
})();
