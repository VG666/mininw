# tests/apps/smoke —— 端到端冒烟应用（被测对象本身）

- 页面：[tests/apps/smoke/index.html](../../tests/apps/smoke/index.html)；清单：[package.json](../../tests/apps/smoke/package.json)
  （name `nw-bridge-smoke`，760x560 居中，title "nw bridge smoke"）
- 驱动：[run-smoke.bat](run-smoke.bat.md)。**不需要构建：页面本身就是被测对象。**

## 页面干了什么事

`check(name, body)` 包含兼容层和原生 Node 可选入口断言，每个捕获异常转成 `{name, ok, detail}`；
结果既渲染到页面 `<pre>`，也经 node 桥 `fs.writeFile` 落盘（落盘本身即 fs 用例），
最后 `nw.App.quit()` 自退。

### 第一阶段：同步断言（页面加载即跑）

1. `window.nw` 已注入；2. `window.mbQuery` 是函数；3. `App.manifest.name` 正确；
4. **App.dataPath 是盘绝对路径**（正则 `^[A-Za-z]:\\`——专盯 NormalizePath 曾把
   `C:\Users\x` 折成 `C:Users\x` 的盘相对路径 bug）；
5. argv 里能找到 `--out=`；
6. node 桥 `require('fs'/'path')` 与 `nw.require` 一致；原生入口 `nw.requireNative` 按内核能力探测，存在时试加载 `path`，不存在时记录 unsupported；7. Buffer hex 往返（`'nw'→'6e77'`）；
8. `process.platform==='win32'`；9. `path.join` win32 反斜杠；10. `os.EOL===CRLF`；
11. **fs 落盘读回**：writeFileSync 写含中文的探针文件 → readFileSync 比对 → existsSync → unlinkSync；
12. `Screen.screens`（属性不是方法，至少 1 块屏，bounds.width>0）；
13. `Window.get()` 有 title/setTitle（顺手 setTitle 验证）；
14. **Clipboard 往返并还原原内容**（静态 get() 取单例、实例 get() 读文本的 nw 语义）；
15. Menu/MenuItem/Tray/Shell/Shortcut 构造器齐备；
16. DOM/canvas 2D 可用；17. file:// 下同步 XHR 能读本文件（验证 async=false 可用）；
18. **同步通道 nmb-rpc 拦截即时作答**：读 `__nmbChannel`（syncBase 必须非空，打出
   threads main/ui/sync 与 syncSharesUi）；
19. **win.state 走同步通道**：当前窗口 id 非空、width/height>0、x/y 读得到；
20. **Window.open 先给句柄、id 异步结算**：立即返回的对象 setTitle 应进 pending（id 此刻为 null）；
21. mbQuery 异步应答已发出（此阶段读 __asyncProbe 应仍为 0，等第二阶段）。

### 第二阶段：1200ms 后异步观测（追加 `late[]`，重写报告后 quit）

mbQuery 回调确实到达（`__asyncProbe=1`）、`Window.open` 的 id 已结算且 pending 清空、
同步通道仍可用；任一不满足在 `late` 里写 `asyncCheck=FAIL: ...`。

## 取证产物

`smoke-result.json`：summary、channel（含实测线程拓扑、rpcErrors/asyncErrors/nodeError 与 nativeNode 能力）、
results、late。runner 用 findstr 扫 `"ok": false` 判定。
