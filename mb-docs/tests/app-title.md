# tests/apps/title —— 窗口标题（title_override）语义探针

- 页面：[index.html](../../tests/apps/title/index.html)、导航目标 [after.html](../../tests/apps/title/after.html)；
  清单：[package.json](../../tests/apps/title/package.json)（name `nw-bridge-title-probe`，420x200，初始 title `MANIFEST-TITLE`）
- 驱动：[run-title.bat](run-title.bat.md)（外部 tasklist 采样为权威证据）

## 要验证的语义（nw.js title_override）

1. 窗口建好后，HTML 的 `<title>` 决定标题栏；
2. 页面显式 `win.setTitle(x)` = 设**覆盖值**，标题栏立即变 x；
3. 覆盖值生效期间再改 `document.title`，**不得**改标题栏；
4. 导航到新文档（loadURL/reload/back/forward 同理）后覆盖值清除，新页面 `<title>` 重新说了算。

## 页面日程（全部标题用 ASCII，避开管道/控制台编码问题）

- 0.5s：`0.html-title`，期望 `HTML-TITLE`（什么都不做，只核对）；
- 3.0s：`1.setTitle`，调 `setTitle('OVERRIDE')`；
- 6.0s：`2.document.title`，改 `document.title='JS-TITLE-2'`，标题栏仍应是 `OVERRIDE`；
- 9.0s：`3.navigate`，`location.href='after.html'`；after.html 的 `<title>` 是 `AFTER-TITLE`，
  它在 800ms 后 `syncNow()` 核对一次（第 4 步 `4.after-load`），3 秒后 `nw.App.quit()` 自退。

## 记录方式（两路人马）

- 每步动作后留 300ms 再 `win.syncNow()` 从 native **同步读权威值** `win.__title`
  （那是宿主 SetWindowTextW 用的同一字段），与 want 比对；为避免与待处理异步命令竞速才留 300ms；
- 每条结果 `{name, ok, actual, want}` 用 `fs.appendFile` 以 **JSONL** 追加到 `--out` 文件
  （mb108 上 writeFileSync 可能抛，改走不依赖应答回程的异步 appendFile）；
- 同时每步 console.log，宿主写 stderr；mb108 若同步读不可用，页面记录标 UNVERIFIED/THREW，
  **最终语义判定交给 runner 对真实标题栏的时间线**（出现过 OVERRIDE 且末帧 AFTER-TITLE）。
