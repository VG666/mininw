# nw 使用接口文档

版本：2026-09-24  
适用运行目录：`F:\编程\nw_mb\nw\bin`  
适用平台：Windows x64

本文只说明应用开发者如何使用当前 `nw` 运行时提供的页面 JavaScript 接口，不涉及 DLL 导出函数、miniblink ABI 或 native 内部实现。

## 1. 一个 nw 应用如何启动

一个最小应用目录如下：

```text
my-app/
├─ package.json
├─ index.html
└─ app.js
```

### 1.1 `package.json` 是应用入口配置

```json
{
  "name": "my-nw-app",
  "version": "1.0.0",
  "main": "index.html",
  "window": {
    "title": "我的 nw 应用",
    "width": 960,
    "height": 640,
    "position": "center"
  }
}
```

`main` 是启动入口：

- `main: "index.html"`：启动后加载 HTML；
- `main: "app.js"`：启动后加载 JavaScript；
- 也可以填写 URL 或 `file://` 地址；
- 相对路径相对于应用目录解析。

### 1.2 HTML 页面入口

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>我的 nw 应用</title>
</head>
<body>
  <button id="open">打开窗口</button>
  <script src="app.js"></script>
</body>
</html>
```

页面加载后，运行时会注入：

```javascript
window.nw
window.require
window.Buffer
window.process
```

### 1.3 JavaScript 入口

```javascript
'use strict';

const fs = window.require('fs');
const path = window.require('path');
const win = nw.Window.get();

win.title = '应用已启动';
console.log(process.platform, process.pid);
```

`window.require()` 是推荐入口；`nw.require()` 是同一个模块加载器的正式别名：

```javascript
const fs1 = window.require('fs');
const fs2 = nw.require('fs');
console.assert(fs1 === fs2);
```

## 2. 接口分类：入口、读取、修改出口

应用代码建议按以下规范组织：

```text
package.json.main
    ↓
index.html / app.js                 页面入口
    ↓
window.require(...) / nw.require(...)   模块入口
    ↓
nw.App / nw.Window / nw.Menu ...    数据读取与状态对象
    ↓
setTitle、resizeTo、load、set、append、register ... 修改出口
```

### 2.1 入口函数

| 入口 | 用途 |
|---|---|
| `package.json.main` | 指定应用启动页面或脚本 |
| `window.require(name)` | 加载内置模块或本地 JavaScript 模块 |
| `nw.require(name)` | `window.require` 的正式别名 |
| `nw.Window.get()` | 取得当前窗口对象 |
| `nw.App` | 取得应用级信息和生命周期接口 |
| `nw.Screen.Init()` | 初始化并读取显示器信息 |

### 2.2 读取接口

读取接口只获取当前数据，不改变应用状态，例如：

```javascript
nw.App.argv;
nw.App.manifest;
nw.App.dataPath;
nw.Window.get().title;
nw.Window.get().x;
nw.Window.get().width;
nw.Screen.screens;
nw.Clipboard.get().get();
```

### 2.3 修改出口

修改出口是应用真正改变窗口、文件、菜单、剪贴板、托盘或进程状态的函数。业务代码应通过这些公开方法修改，不应直接调用内部 `mbQuery`。

| 对象 | 修改出口示例 |
|---|---|
| 当前窗口 | `setTitle`、`show`、`hide`、`focus`、`resizeTo`、`moveTo`、`loadURL`、`reload`、`close` |
| 应用进程 | `nw.App.quit()`、`nw.App.exit(code)`、`nw.App.restart()` |
| 文件 | `fs.writeFileSync`、`fs.appendFileSync`、`fs.renameSync`、`fs.unlinkSync` |
| 菜单 | `menu.append`、`menu.insert`、`menu.remove`、`menu.popup`、`win.setMenu` |
| 剪贴板 | `clipboard.set`、`clipboard.clear` |
| 托盘 | `tray.setIcon`、`tray.setTooltip`、`tray.remove` |
| 全局热键 | `shortcut.register`、`shortcut.unregister` |
| 外部资源 | `nw.Shell.openExternal`、`nw.Shell.openItem` |
| 媒体 | HTML 媒体元素的 `load`、`play`、`pause`、`currentTime`、`volume` |

## 3. 内置模块接口

当前 `require` 支持以下模块：

```text
buffer
assert
events
fs
os
path
process
querystring
timers
url
util
```

也支持 `node:` 前缀：

```javascript
const fs = require('node:fs');
const path = require('node:path');
```

### 3.1 `path`

Windows 路径处理：

```javascript
const path = require('path');

const file = path.join('data', 'config', 'app.json');
const dir = path.dirname(file);
const name = path.basename(file);
const ext = path.extname(file);
```

常用函数：

```text
join
resolve
normalize
dirname
basename
extname
relative
isAbsolute
parse
format
```

### 3.2 `fs`

文件接口是同步接口：

```javascript
const fs = require('fs');
const path = require('path');

const file = path.join(process.cwd(), 'data.txt');

fs.writeFileSync(file, '第一行\r\n', 'utf8');
fs.appendFileSync(file, '第二行\r\n', 'utf8');

const text = fs.readFileSync(file, 'utf8');
console.log(text);

if (fs.existsSync(file)) {
  fs.unlinkSync(file);
}
```

常用函数：

```text
existsSync
statSync
lstatSync
readFileSync
writeFileSync
appendFileSync
readdirSync
mkdirSync
unlinkSync
renameSync
realpathSync
```

目录创建示例：

```javascript
const outputDir = path.join(process.cwd(), 'output');
if (!fs.existsSync(outputDir)) {
  fs.mkdirSync(outputDir, { recursive: true });
}
```

当前实现支持文本和 Buffer 数据：

```javascript
const bytes = Buffer.from('中文内容', 'utf8');
fs.writeFileSync('data.bin', bytes);
const back = fs.readFileSync('data.bin');
console.log(back.toString('utf8'));
```

### 3.3 `process`

```javascript
console.log(process.platform); // win32
console.log(process.arch);
console.log(process.pid);
console.log(process.cwd());
console.log(process.argv);
console.log(process.env);
```

进程修改出口：

```javascript
process.chdir('D:\\work');
process.exit(0);
```

正常关闭 nw 应用时，优先使用：

```javascript
nw.App.quit();
```

### 3.4 `os`

```javascript
const os = require('os');

console.log(os.platform());
console.log(os.arch());
console.log(os.EOL);       // Windows 为 \r\n
console.log(os.homedir());
console.log(os.tmpdir());
console.log(os.cpus());
```

### 3.5 `Buffer`

```javascript
const data = Buffer.from('nw', 'utf8');
console.log(data.toString('hex')); // 6e77
console.log(data.toString('base64'));
```

## 4. `nw.App`

### 4.1 应用信息

```javascript
console.log(nw.App.manifest);
console.log(nw.App.manifest.name);
console.log(nw.App.manifest.version);
console.log(nw.App.argv);
console.log(nw.App.dataPath);
```

`nw.App.argv` 是传给应用的参数数组。例如启动命令带有：

```text
--out=C:\\temp\\result.json
```

页面中读取：

```javascript
const out = nw.App.argv.find(value => value.indexOf('--out=') === 0);
const outPath = out ? out.slice('--out='.length) : '';
```

### 4.2 应用生命周期修改出口

```javascript
nw.App.quit();
nw.App.exit(0);
nw.App.restart();
```

说明：

- `quit()`：正常退出应用；
- `exit(code)`：使用指定退出码退出；
- `restart()`：重新启动应用；
- 这些操作是异步通知型操作，调用后不要继续依赖窗口对象完成后续业务。

## 5. `nw.Window`

### 5.1 获取当前窗口

```javascript
const win = nw.Window.get();
```

### 5.2 窗口状态读取

```javascript
console.log(win.id);
console.log(win.x, win.y);
console.log(win.width, win.height);
console.log(win.title);
console.log(win.zoomLevel);
console.log(win.isMaximized);
console.log(win.isMinimized);
console.log(win.isFullscreen);
```

### 5.3 窗口修改出口

```javascript
win.setTitle('新的窗口标题');
win.title = '新的窗口标题';

win.moveTo(100, 80);
win.resizeTo(1200, 800);
win.moveBy(20, 10);
win.resizeBy(100, 50);

win.show();
win.hide();
win.focus();
win.blur();
win.minimize();
win.restore();
win.maximize();
win.unmaximize();
```

其他常用修改接口：

```javascript
win.setResizable(true);
win.setAlwaysOnTop(true);
win.setShowInTaskbar(true);
win.setPosition('center');
win.setMinimumSize(400, 300);
win.setMaximumSize(1920, 1080);
win.setZoom(1.25);
win.enterFullscreen();
win.leaveFullscreen();
win.toggleFullscreen();
win.reload();
win.reloadIgnoringCache();
win.loadURL('https://example.com/');
win.stop();
```

### 5.4 窗口关闭事件

```javascript
win.on('close', function () {
  // 做清理工作
  this.close(true);
});
```

直接关闭：

```javascript
win.close(true);
```

### 5.5 打开新窗口

```javascript
const child = nw.Window.open('child.html', {
  width: 640,
  height: 480,
  position: 'center',
  title: '子窗口'
});

child.on('loaded', function () {
  console.log('子窗口已加载，id=', child.id);
});
```

注意：`Window.open()` 会立即返回窗口对象，但新窗口的真实 `id` 可能稍后才结算。不要在调用返回的瞬间假定 `child.id` 已经可用。

### 5.6 窗口事件

```javascript
win.on('loaded', handler);
win.on('move', handler);
win.on('resize', handler);
win.on('focus', handler);
win.on('blur', handler);
win.on('maximize', handler);
win.on('unmaximize', handler);
win.on('minimize', handler);
win.on('restore', handler);
win.on('enter-fullscreen', handler);
win.on('leave-fullscreen', handler);
win.on('close', handler);
```

## 6. `nw.Menu` 与 `nw.MenuItem`

### 6.1 创建菜单

```javascript
const menu = new nw.Menu({ type: 'menubar' });

const fileItem = new nw.MenuItem({
  label: '文件',
  click: function () {
    console.log('点击文件');
  }
});

menu.append(fileItem);
nw.Window.get().setMenu(menu);
```

### 6.2 菜单项类型

```javascript
new nw.MenuItem({ label: '普通菜单' });
new nw.MenuItem({ label: '复选菜单', type: 'checkbox', checked: true });
new nw.MenuItem({ type: 'separator' });
```

### 6.3 子菜单

```javascript
const recent = new nw.Menu({ type: 'contextmenu' });
recent.append(new nw.MenuItem({ label: '项目一' }));
recent.append(new nw.MenuItem({ label: '项目二' }));

const menu = new nw.Menu({ type: 'menubar' });
menu.append(new nw.MenuItem({
  label: '最近打开',
  submenu: recent
}));
```

### 6.4 右键菜单

```javascript
const context = new nw.Menu({ type: 'contextmenu' });
context.append(new nw.MenuItem({
  label: '刷新',
  click: function () {
    nw.Window.get().reload();
  }
}));

document.addEventListener('contextmenu', function (event) {
  event.preventDefault();
  context.popup(event.screenX, event.screenY);
});
```

## 7. `nw.Clipboard`

```javascript
const clipboard = nw.Clipboard.get();

const oldText = clipboard.get();
clipboard.set('复制到剪贴板');
console.log(clipboard.get());
clipboard.clear();

// 恢复原内容
if (oldText) clipboard.set(oldText);
```

支持的类型：

```javascript
clipboard.set('普通文本', 'text');
clipboard.set('<b>HTML</b>', 'html');
const text = clipboard.get('text');
const html = clipboard.get('html');
```

可用方法：

```text
nw.Clipboard.get()
clipboard.get(type)
clipboard.set(data, type)
clipboard.clear()
clipboard.readAvailableTypes()
```

## 8. `nw.Shell`

```javascript
nw.Shell.openExternal('https://example.com/');
nw.Shell.openItem('C:\\work\\report.pdf');
nw.Shell.showItemInFolder('C:\\work\\report.pdf');
nw.Shell.beep();
```

接口说明：

| 接口 | 用途 |
|---|---|
| `openExternal(uri)` | 使用系统默认程序打开 URL |
| `openItem(path)` | 使用文件关联程序打开文件 |
| `showItemInFolder(path)` | 在资源管理器中定位文件 |
| `beep()` | 播放系统提示音 |

## 9. `nw.Screen`

```javascript
nw.Screen.Init();

for (const screen of nw.Screen.screens) {
  console.log(screen.id, screen.bounds);
}
```

屏幕对象通常包含：

```javascript
{
  id,
  bounds: { x, y, width, height },
  work_area,
  scaleFactor,
  isPrimary
}
```

监听显示器布局变化：

```javascript
nw.Screen.on('displayBoundsChanged', function () {
  nw.Screen.Init();
  console.log(nw.Screen.screens);
});
```

当前实现只保证 `displayBoundsChanged`，不依赖 `displayAdded` 和 `displayRemoved`。

## 10. `nw.Shortcut`

```javascript
const shortcut = new nw.Shortcut({
  key: 'Ctrl+Shift+P',
  active: function () {
    console.log('热键触发');
  },
  failed: function (message) {
    console.error('热键注册失败', message);
  }
});

shortcut.register();
```

注销：

```javascript
shortcut.unregister();
```

应用退出前建议注销自己注册的全局热键。

## 11. `nw.Tray`

```javascript
const tray = new nw.Tray({
  icon: 'assets/app.ico',
  tooltip: '我的 nw 应用'
});

tray.on('click', function () {
  const win = nw.Window.get();
  win.show();
  win.focus();
});

tray.setTooltip('应用运行中');
tray.setIcon('assets/app-active.ico');
```

删除托盘：

```javascript
tray.remove();
```

托盘菜单：

```javascript
const trayMenu = new nw.Menu({ type: 'contextmenu' });
trayMenu.append(new nw.MenuItem({
  label: '退出',
  click: function () {
    nw.App.quit();
  }
}));

const tray = new nw.Tray({
  icon: 'assets/app.ico',
  menu: trayMenu
});
```

## 12. 页面媒体接口

普通页面优先使用标准 HTML 媒体 API：

```html
<video id="player" controls width="800"></video>
```

```javascript
const video = document.getElementById('player');
video.src = 'https://example.com/video.mp4';
video.load();
video.play();
video.pause();
video.currentTime = 30;
video.volume = 0.8;
video.muted = false;
```

桥会对页面中的 `audio` / `video` 元素同步加载、播放、暂停、进度、音量、静音、控件和显示区域。

当页面已经获得可直接访问的媒体地址，但原始地址是 `blob:` 或内核无法直接解码时，可使用：

```javascript
const video = document.querySelector('video');
window.__nmbServeVideo(video, directUrl);
```

其中 `directUrl` 必须是桥的 FFmpeg 可以直接访问的地址。

`window.__nmbServeMse()` 和 `window.__nmbMedia()` 属于高级兼容/诊断接口，不建议普通业务直接调用。

## 13. 本地模块与 `node_modules`

### 13.1 加载本地模块

`app.js`：

```javascript
const tools = require('./tools');
console.log(tools.add(1, 2));
```

`tools.js`：

```javascript
exports.add = function (a, b) {
  return a + b;
};
```

### 13.2 加载 JSON

```javascript
const config = require('./config.json');
console.log(config.name);
```

### 13.3 加载纯 JavaScript npm 包

运行时支持从当前目录向上查找 `node_modules`，并读取包的 `package.json.main`。

只建议使用纯 JavaScript 包。依赖以下 Node 原生能力的包不能使用：

```text
.node 原生扩展
child_process
net
tls
dgram
http 服务端
worker_threads
```

## 14. 可选原生 Node 入口与使用限制

`window.require()` 和 `nw.require()` 始终是现有 RPC/CommonJS 兼容层，仅支持上述已列出的模块。需要当前内核提供的原生模块时，先检测 `nw.requireNative`：

```javascript
const fs = nw.require('fs'); // 兼容层
if (typeof nw.requireNative === 'function') {
  const http = nw.requireNative('http'); // 当前内核原生模块；仍须处理加载异常
}
```

内核若暴露 `miniNodeRequire`（本项目内核脚本）或 `mbRequire`，页面会将其映射到 `nw.requireNative`；否则为 `null`。该能力取决于实际加载的内核和 Node 构建/启用状态，不能仅凭 `package.json` 的 `nodejs:true` 推断模块可用。`http`、`child_process`、原生扩展等必须分别验证，不能因为入口存在就认为全部可用。`nodejs:false` 时不会注入此入口。

1. 兼容层入口是 `window.require()` 或 `nw.require()`；可选原生入口是 `nw.requireNative()`，调用前必须做能力检测。
2. 不要在应用中直接依赖内核内部函数 `mbRequire()`、`miniNodeRequire()` 或 `__nmbNativeRequire`。
3. `window.mbQuery()` 是底层通信通道，不是普通业务 API，不要自行拼接内部协议。
4. `window.open()` 已接管为 nw 窗口创建流程，窗口真实 ID 可能异步产生。
5. `nw.Window.eval()` 属于受限制的高级接口，不应作为业务通信方式；业务页面应使用事件、模块或回调。
6. 当前实现是 Windows 运行时，Mac/Linux 专属字段可能只是保留接口形状或不生效。
7. 文件系统接口当前以同步方法为主；不要在大文件或高频循环中阻塞页面。
8. 媒体能否播放取决于桥使用的 FFmpeg、网络地址、Cookie、Referer、防盗链和媒体格式。
9. 修改窗口、菜单、托盘、热键等 native 状态的接口大多是异步操作；需要结果时使用事件或回调，不要只依赖函数返回值。

## 15. 最小完整示例

### `package.json`

```json
{
  "name": "nw-example",
  "version": "1.0.0",
  "main": "index.html",
  "window": {
    "title": "nw 示例",
    "width": 800,
    "height": 500,
    "position": "center"
  }
}
```

### `index.html`

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>nw 示例</title>
</head>
<body>
  <button id="save">写入文件</button>
  <button id="title">修改标题</button>
  <button id="quit">退出</button>
  <pre id="output"></pre>
  <script src="app.js"></script>
</body>
</html>
```

### `app.js`

```javascript
'use strict';

const fs = window.require('fs');
const path = window.require('path');
const win = nw.Window.get();
const output = document.getElementById('output');

function log(value) {
  output.textContent += String(value) + '\n';
}

document.getElementById('save').addEventListener('click', function () {
  const file = path.join(nw.App.dataPath, 'example.txt');
  fs.writeFileSync(file, '由 nw 应用写入\r\n', 'utf8');
  log('已写入：' + file);
});

document.getElementById('title').addEventListener('click', function () {
  win.setTitle('标题已修改');
  log('窗口标题已修改');
});

document.getElementById('quit').addEventListener('click', function () {
  nw.App.quit();
});

log('应用：' + nw.App.manifest.name);
log('平台：' + process.platform);
log('窗口：' + win.width + ' x ' + win.height);
```

## 16. 运行方式

在包含 `nw.exe` 的运行目录中启动应用：

```powershell
.\nw.exe ..\my-app
```

也可以直接指定 `package.json` 所在目录：

```powershell
.\nw.exe F:\\work\\my-app
```

当前项目的测试应用可以作为实际参考：

```text
nw\tests\apps\smoke\package.json
nw\tests\apps\smoke\index.html
```

基础接口验证：

```powershell
cd F:\\编程\\nw_mb\\nw
call tests\\run-smoke.bat
```

该测试覆盖 `window.nw`、`window.require`、`nw.require`、`fs`、`path`、`Buffer`、`process`、窗口、屏幕、剪贴板、DOM 和异步窗口操作。
