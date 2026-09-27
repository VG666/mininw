# nw_node.js —— 页面侧 node 桥（纯 JS CommonJS 运行时）

- 源文件：[nw_node.js](../../js/nw_node.js)（约 28KB）
- 角色：在页面里提供 nw 应用常用的 node 能力。**它是"桥"不是真 node**：nw.js 真的把 V8(node) 塞进 renderer，能加载原生扩展、跑 npm 原生包；本实现没有 renderer 也不拖 node 进来，这里是 CommonJS 运行时 + 常用内置模块的纯 JS 实现，凡要落到操作系统的原语（文件/环境/进程）都通过同步通道（`window.__nmbRpc`）回到 native 的 `ApiFs/ApiProcess`。

## 边界（文件头明示）

- 有：`require`（含 node_modules 逐级解析、package.json main、.json 加载、缓存）、`module/exports`、`process`、`Buffer`、`path`、`fs`（同步 API）、`os`、`url`、`querystring`、`events`、`util`、`assert`、`timers`；
- 没有：原生扩展（.node）、child_process、net/tls/dgram/http 服务端、worker_threads，以及依赖它们的 npm 包；
- 页面的 `nw.require` / 全局 `require` 就是这里的 require。

## 干了什么事（按段落）

1. **启动信息**：优先用 native 预注入的 `window.__nmbBootProcessReply`（mb108 在脚本上下文回调内不能发同步 XHR）；没有再 `rpc('process.info')`；同步通道不通则退化成空壳并把原因记进 `window.__nmbNodeError`，不拖垮 window.nw。
2. **Buffer**：手写 base64 编解码；Buffer 按"Uint8Array 子类同形状"实现；`toString` 按编码解码，并显式处理裸 Uint8Array 的 toString 差异（只改 BufferArg 的原型链，不改全局 Uint8Array）。
3. **path**：node path 的 win32 语义（join/resolve/dirname/basename/extname/normalize 等），手写。
4. **process**：platform/arch/cwd/argv/env/versions/pid 来自 process.info；含 cwd/exit 等桥接口。
5. **fs（同步 API）**：`existsSync/statSync/lstatSync/readFileSync/writeFileSync/appendFileSync/readdirSync/mkdirSync/unlinkSync/renameSync/realpathSync` 等，每个都是一条同步 RPC：
   - 线格式只走 utf8/base64：无 encoding 返回 Buffer（桥以 utf8 回，再转字节）；hex 等编码先按 base64 取字节再本地编；写字节时 base64 上行；
   - stat 返回带 `isDirectory()/isFile()` 与由 mtimeMs 构造的 Date；错误伪造 node 形状（`code`/`path`，如 ENOENT）。
6. **其余内置**：os、url、querystring、events（复用 api 层 Emitter 思路）、util、assert、timers（setTimeout 等直接借 window）。
7. **require 机制**：
   - `builtins` 表（支持 `node:` 前缀）；
   - `resolveModule`：相对/绝对路径先找文件（x / x.js / x.json）再找 index；裸模块名从当前目录一路向上找 `node_modules/<name>`，读其 package.json 的 main；
   - `makeRequire`：moduleCache 缓存；.json 直接 parse；.js 用 `new Function('exports','require','module','__filename','__dirname', source)` 包一层执行，子 require 以文件所在目录为根；
   - 暴露 `require.resolve/cache/main/builtins`。
8. **挂全局**：`window.require/module/exports/global/Buffer/process/__nmbRequire`，并回填 `window.nw.require`；`window.__nmbNode` 给出兼容层能力自述（modules、`nativeAddons:false`）。原生入口单独映射为 `nw.requireNative`：优先使用内核 `miniNodeRequire`，其次 `mbRequire`，均不存在则为 `null`。它不覆盖兼容层；其模块可用性取决于实际内核，须逐项测试。

## 与谁协作

- 必须在 api 脚本之后注入（用 `window.__nmbRpc/__nmbRpcAsync`，缺失会报错并放弃）；
- native 后端命令见 [nw_host.cpp](nw_host.cpp.md) 的 `ApiFs`（`fs.*`）与 `ApiProcess`（`process.*`），二者都在同步白名单内。
