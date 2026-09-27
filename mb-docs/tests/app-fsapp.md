# tests/apps/fsapp —— fs 修改磁盘文件可用性应用

- 页面：[tests/apps/fsapp/index.html](../../tests/apps/fsapp/index.html)；清单：[package.json](../../tests/apps/fsapp/package.json)
  （name `fsapp`，`nodejs:true`，760x600，title "nw SDK fs test"）
- 驱动：[run-fsapp.bat](run-fsapp.bat.md)（手工）/ [run-fsapp-auto.bat](run-fsapp-auto.bat.md)（自动取证）

## 要验证的事

nw.js 标准写法 `require('fs').appendFile(...)` 能否真的穿过桥改到应用目录下的
`test-file.txt`，并 `readFileSync` 回读；另外专门验证**首屏主脚本栈**上同步 fs 立即可用。

## 页面干了什么事

1. **环境徽章**：探测 nw / require / process / fs 是否存在，打到顶部；
2. **目标文件定位** `resolveTarget()`：依次取 `nw.__dirname` → `__nmbAppInfo.appPath`
   → `process.cwd()`，拼出应用目录下的 `test-file.txt`；
3. **主脚本栈同步 fs（页面加载即做，关键项）**：
   `appendFileSync(file, 'main-stack @ <ISO>\n')` 后立刻 `readFileSync` 回读、数行数；
   结果写 `window.__mainStackFs`（`ok:<行数>` / `FAIL:<原因>`）并显示。
   这条验的是同步 RPC 通道在**首屏主脚本栈**上就能用，而不是要等定时器；
4. **600ms 后自动跑一次** `doModify()`（也绑定按钮手工触发）：
   `fs.appendFile(file, 'click @ <ISO>\n', cb)`（nw.js 标准异步写法，本兼容层里是同步实现的包装），
   回调里 `verify()` → `readFileSync` 回读末行；3 秒回调没回来判超时；
5. **结论外显，方便窗口外取证**：`setVerdict` 除了页面横幅，还把
   `fsapp:PASS:<通道>` 或 `fsapp:FAIL:...|probe=<探测错误>|node=<node错误>|main=<主栈结果>`
   写进 `document.title`（auto runner 用 tasklist 读标题即可定位断点），并 console.log（宿主转 stderr）。

## 硬证据

磁盘上 `tests/apps/fsapp/test-file.txt`：每次成功追加一行时间戳。runner 删旧文件后启动，
结束直接 type 该文件——有新内容即证明 fs 真正落了盘。
