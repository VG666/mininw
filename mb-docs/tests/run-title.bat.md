# run-title.bat —— 窗口标题链路回归（外部 tasklist 取证）

- 源文件：[tests/run-title.bat](../../tests/run-title.bat)
- 被测应用：[tests/apps/title](app-title.md)

## 为什么需要"窗口外的证人"

页面**读不到自己的标题栏文字**（JS 没有这个 API）。所以证据分两路：
- 页面每步 `console.log`，经宿主 `OnConsole` 写进 **stderr**（本脚本重定向到 `.out\title-err.txt`）；
- 脚本用 **`tasklist /v` 的 "Window Title" 字段从进程外部每秒采样真实标题栏**，
  时间线同时打到屏幕和 `.out\title-caption.txt`。

## 干了什么事

1. 清理 `.out\` 下的 `title-err.txt / title-out.txt / title-caption.txt` 与 `title-result.json`；
2. `start bin\nw.exe --nwapp=tests\apps\title --out=<result>`，stdout/stderr 分别重定向；
3. 采样循环（最多 40 次 ≈ mb108 冷启动 ~9s + 页面日程 ~13s 留足余量；进程退出则提前结束）：
   `tasklist /v /fi "imagename eq nw.exe" /fo list | findstr /b "Window Title:"`
   取标题（/fo list 每行一个 "标签: 值"，for /f 按冒号切出值），打 `Ns <标题>` 并追加 caption 日志；
4. 收割：taskkill，打印 stderr 页面日志与 `.out\title-result.json`（JSONL 断言，页面侧 mb108 上
   可能是 UNVERIFIED）；
5. **以标题栏本身做权威判定**（页面 state 在 mb108 上读不到，不采信）：
   - caption 里必须出现过 `OVERRIDE`（setTitle 覆盖值确实上了标题栏），否则退出码 **3**；
   - 最后一帧非 `N/A` 标题必须是 `AFTER-TITLE`（导航清掉覆盖值、新文档 title 拿回标题栏），
     否则退出码 **4**；
   - 两者满足 → PASS，退出码 **0**；根本没报告 → 退出码 **2**。

## 脚本里的 cmd 坑（注释有记录）

- 裸 `|` 出现在 echo 文本里会被当管道；
- for 块里嵌套 if 会被 cmd 解析器搞坏，所以判定放到顶层；
- `findstr /v "N/A"` 排除采不到标题的帧，再取最后一帧。
