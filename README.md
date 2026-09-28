# mininw —— 基于 miniblink 的轻量 nw.js 运行时

`mininw` 是一个**与 nw.js 接口兼容的桌面应用运行时**：它用 miniblink 当浏览器内核，
用一套自研的 C++ 宿主把"网页"和"系统能力（窗口 / 菜单 / 托盘 / 文件 / 剪贴板 / 进程…）"接起来，
并提供一个 CommonJS 风格、与 node 行为高度兼容的 `require` 环境，配合原生的 Node，共同实现了与 nw.js 接口兼容的桌面应用运行时。

它是一个**独立项目**：运行时既可以独立以 miniblink 内核运行，也可以在"桥模式"下把浏览器整体
（离屏合成 + 媒体接管）外包给 `NativeMediaBridge.dll`，从而复用其内置的 ffmpeg 媒体解码能力
——但这只是**可选的运行时依赖**。

> 第三方组件的引用署名、许可证与发行义务见 **[`开源声明.md`](开源声明.md)**（`reference\` 内的 nw.js MIT 源码快照、miniblink 内核、可选的 NativeMediaBridge.dll 等）。

## 许可证

本项目自身代码为 **LGPL-3.0-or-later**（全文见 [`LICENSE`](LICENSE)，LGPL-3.0 所补充的 GPL-3.0 见 [`COPYING`](COPYING)），
与 `NativeMediaBridge` 保持同一许可证。

- 用 nw 运行自己的网页应用时，**网页代码无需开源**；把 `nw.exe` 打包分发时，需随附 LGPL-3.0 + GPL-3.0 全文并提供源码获取方式。
- 修改了 `src\` / `js\` / `tools\` 后再发布二进制，修改部分须按 LGPL-3.0-or-later 提供源码。

---

## 一、起源与架构演进（来龙去脉）

> 这一段记录本项目的设计来源，便于后续维护者判断"某段代码是原始思路还是后期重写"。

最初的思路是：

1. **把 nw 的 node 运行子类拆出来作为主类** —— 让 node 侧（运行时入口）跑在前面，而不是
   沿用 nw.js 原本"Chromium 浏览器为主、node 内嵌进 renderer"的结构；
2. **再把浏览器层套入 miniblink** —— 用 miniblink 的 `mb*` C 接口充当 WebView 内核，替代
   庞大的 Chromium 内容层（`content::BrowserMainRunner` / `WebContents` 等）;
3. 依此目标，自己实现一套**轻量的 nw.js**：`window.nw` / `window.require` / `nw.Window`
   等接口、以及窗口/菜单/托盘/剪贴板等 native 能力。
4. 因为属于早期古法项目，在"拆分"过程中遇到了不少问题，后期ai横空出世，于是借助 AI 协助推进。**AI 改动较多，目前无法确切判定当前代码是<br />"沿原思路的小幅修补"还是"事实上的相对重写"**——但可以确定的是：**项目当前已经基本可用（css有部分涉及拖动等的窗口事件还没完成）**，能正常
加载应用、跑窗口、执行文件/进程/媒体等接口。后续若需要改动本目录，建议先对照 `reference/`
（原版 nw.js 参考）与 `archive/`（已退役的历史文件）定位"当前架构基准"，避免把后期引入的结构
误当成原始 nw 拆分意图。
5. 该项目原本想用比较新的108内核，但因为108内核的node问题，最终放弃了，改用miniblink。，最新的135版本，由于存在一些历史包袱不兼容不考虑

---

## 二、架构总览

与 nw.js 不同，本实现**没有 renderer 进程、也不把 V8 内嵌进渲染层**——一个 mininw 窗口 =
一个 Win32 框架窗口 + 一个 miniblink 视图 + 一份 `nw.Window` 状态，所有 native 行为都由宿主
（C++）直接处理。分层如下：

```
命令行 / nw.exe
   └─ nw_main.cpp          解析参数、nw init、定位应用目录、起宿主、进消息循环
        └─ nw_host.cpp     宿主层：窗口生命周期 + nw.* API 的 native 实现 + 注入脚本
             ├─ nw_kernel.cpp   浏览器内核绑定：动态加载 miniblink（mb* C API），无 Chromium
             ├─ nw_bridge.cpp   桥模式胶水：浏览器外包给 NativeMediaBridge.dll
             ├─ nw_rpc.cpp      loopback HTTP RPC：页面 → native 的"主用"同步通道
             ├─ nw_package.cpp  package.json 清单解析
             └─ 注入脚本:
                  js/api/*.js   页面侧 nw.* API 实现（App/Window/Menu/Tray/Clipboard/…）,属于最早的实现
                  js/nw_node.js node 兼容层：CommonJS + 内置模块（经 RPC 落到 native）
```

**关键设计取舍**

- **内核不是 Chromium，是 miniblink**：宿主按名字从 `miniblink_x64.dll` / `mb*_x64.dll` 动态
  取导出（刻意不 include 内核 SDK），因此本项目不必携带内核源码，也不与特定内核版本绑死。
- **node 是"桥"不是"真 node"**：`nw_node.js` 提供 `require` / `module` / `exports`、
  `process` / `Buffer` / `path` / `fs`(同步) / `os` / `url` / `events` / `util` / `assert` /
  `timers` / `querystring` 等，凡是必须碰系统的原语都经 RPC 回到 native。**不支持**原生扩展
  （`.node`）、`child_process`、`net/tls/dgram`、`http` 服务端、`worker_threads`。
- **两套 `require` 入口**：`window.require()` / `nw.require()` 是 RPC/CommonJS 兼容层（始终可用）；
  可选的 `nw.requireNative()` 在"内核暴露真实 node（`mbRequire` / `miniNodeRequire`）"时映射到
  原生模块，调用前必须做能力检测（见 `NW使用接口文档.md` 第 14 节）。
- **三条同步通道**（页面 → native 取数，按探测顺序）：① loopback HTTP（主用，随机端口 +
  CSPRNG 令牌 + 仅 bind 127.0.0.1，安全模型见 `nw_rpc.h`）；② `prompt` 退路；③
  `onLoadUrlBegin` 拦截退路。
- **桥模式**：`--nw-bridge=<dll>` 或自动查找 `NativeMediaBridge\bin`，把浏览器（离屏合成 +
  ffmpeg 媒体接管）整体交给桥；内核模块在同一进程内只 `mbInit` 一次，宿主与桥共享。

---

## 三、目录结构

```
nw/
├─ src/                  C++ 宿主源码（手写）
│  ├─ nw_main.cpp        入口：参数解析、nw init、消息循环
│  ├─ nw_host.cpp/.h     宿主层：窗口 + nw.* native 实现 + 脚本注入
│  ├─ nw_kernel.cpp/.h   miniblink 内核动态绑定（KernelApi 表）
│  ├─ nw_bridge.cpp/.h   桥模式胶水（NativeMediaBridge.dll）
│  ├─ nw_rpc.cpp/.h      loopback HTTP RPC 同步通道
│  ├─ nw_package.cpp/.h  package.json 解析
│  ├─ nw_json.h          轻量 JSON 助手
│  ├─ nw_script.h        nw_script_stub.cpp 注入脚本桩
│  └─ nw_script_stub.cpp 注入脚本回退桩
├─ js/
│  ├─ api/               nw.* API 的页面侧实现（12 个模块 + base.js/install.js）
│  └─ nw_node.js         node 兼容层（CommonJS + 内置模块）
├─ generated/            构建期生成的 C++（被 embed-js 烘焙进 exe 的脚本）
├─ reference/            原版 nw.js 参考（cc / idl / js），仅用于溯源对照
├─ archive/              已退役的历史文件（含自己的 README 说明）
├─ mb-docs/              运行/接口相关文档（43 篇，含本 README 引用的接口文档源）
├─ tests/                冒烟与集成测试（apps/ 示例应用 + *.bat/*.ps1）
├─ tools/                构建辅助（embed-js.ps1 等）
├─ build-nw.bat          用 zig 构建 bin/nw.exe
├─ run-video-test.bat    媒体相关测试启动
├─ NW使用接口文档.md     应用开发者视角的页面 JS 接口文档（权威）
└─ media-result.json     媒体测试产物
```

> 应用开发者如何使用页面接口（API 全貌、示例、限制），请直接读 **`mininw使用接口文档.md`**——
> 那是面向使用方的权威文档，本 README 只做工程/架构层面的总体描述。

---

## 四、构建

本机 MSVC CRT 损坏导致 `cl` 链接失败，因此改用 **zig 0.15.2** 交叉/本地编译（与上级项目一致）。

```powershell
# 在 nw\ 目录下
build-nw.bat
```

产出：

- `bin/nw.exe` —— 运行时主程序。JS（`js/api/*` + `js/nw_node.js`）通过 `tools/embed-js.ps1`
  烘焙进 exe（单文件分发）；若烘焙失败则回退为"exe 旁的脚本副本"。
- 内核搜索顺序：exe 旁 `miniblink_x64.dll` 优先，其次显式 `--nw-kernel=<path>`，再回退到
  `mb*_x64.dll` / `PATH`。

zig 查找路径：`build-nw.bat` 会依次探测 `..\.toolchain*` 与 `..\NativeMediaBridge\.toolchain*`，
找不到时回退到 `where zig`。

---

## 五、运行

```powershell
# 直接给应用目录（含 package.json / index.html）
.\bin\nw.exe ..\my-app

# 或显式指定
.\bin\nw.exe --nwapp=F:\work\my-app

# 脚手架一个新项目（名称直接采用，不追问）
.\bin\nw.exe init my-app
```

常用开关 / 环境变量：

| 形式 | 作用 |
|---|---|
| `--nwapp=<dir>` | 指定应用目录 |
| `--nw-kernel=<path>` | 强制内核 dll 路径 |
| `--nw-bridge=<dll>` | 指定桥 dll（启用桥模式） |
| `--nw-no-bridge` | 跳过桥模式，直接用旧内核路径 |
| `--nw-no-node` | 关闭 node 桥（等价于 `nodejs:false`） |
| `--nw-no-dialog` | 启动失败只写 stderr、不弹窗（CI/自动化必须） |
| `NMB_NWAPP` / `NMB_NW_NO_DIALOG` / `NMB_NO_NODEJS` | 等效环境变量（自动化用，避免真实命令行含参数导致窗口闪退） |

未给任何应用时，进入内置启动页（splash），不会报错退出。

---

## 六、测试

`tests/` 下是冒烟与集成测试。示例应用放在 `tests/apps/`，可直接用 `bin/nw.exe` 打开。

```powershell
# 基础接口验证（覆盖 nw / require / fs / path / Buffer / process / 窗口 / 屏幕 / 剪贴板 / DOM）
tests\run-smoke.bat

# 其它专项：窗口、标题、拖拽、文件系统、Chromium 切换等
tests\run-window.bat
tests\run-title.bat
tests\run-fsapp.bat
tests\run-drag.bat
tests\run-ccswitch.bat
```

---

## 七、参考与历史

- `reference/`：原版 nw.js 的 `cc` / `idl` / `js` 参考材料，仅用于对照"原 nw 思路"，不参与编译。
- `archive/`：拆分前的单体脚本 `nw_api.js` 等已退役文件，含独立的 `README.md` 说明其来由与
  替代关系（已被 `js/api/*` 取代）。
- `mb-docs/`：运行时/接口相关的设计文档集合。

---

## 八、许可证

本目录（`mininw`）是一个**独立项目**，采用自身的许可证（由项目持有者另行声明）。运行时动态依赖的
miniblink 内核、以及桥模式下可选的 `NativeMediaBridge.dll`、ffmpeg 媒体栈，各自遵循其原有许可证
与署名义务，请按对应项目要求履行——这部分第三方义务独立于 `mininw` 自身的许可证。
