# run-window.bat —— 窗口绑定回归（mb108 窗口类型 ABI）

- 源文件：[tests/run-window.bat](../../tests/run-window.bat)
- 探针：[window_probe.ps1](window_probe.ps1.md)；复用 [title 应用](app-title.md)开窗（只需要它有个窗口）。

## 验证什么

修复 mb108 窗口类型 ABI 混用（旧代码把控件按 TRANSPARENT 窗口建，导致内核子窗口跑到屏幕 (0,0)）后，
确认 miniblink 控件窗口：

1. 作为框架窗口的**真实子窗口**存在；
2. 不是画在屏幕原点 (0,0)；
3. 落在框架窗口客户区内。

即使页面的同步 fs 报告路径不可用，这套也能测——证据全部来自 Win32 API（GetWindow/GetParent/
GetWindowRect）。

## 干了什么事

1. 先 `taskkill` 清掉残留 nw.exe（残留进程会干扰 Get-Process 找主窗口）；
2. 启动 `bin\nw.exe --nwapp=tests\apps\title --nw-no-dialog`；
3. **等主窗口句柄**（最多 10 秒）：每秒用 PowerShell
   `Get-Process nw | MainWindowHandle` 取非零句柄，写进 `%TEMP%\nw_hwnd.txt` 再 `set /p` 读入。
   注释说明为什么写临时文件而不是 for /f：PS 命令里的括号在 for /f 的 `in('...')` 解析里不安全。
   10 秒仍无句柄 → `RC=2`；
4. 多等 1 秒让内核把控件子窗口建完，调
   `powershell -File window_probe.ps1 <hwnd>`，取 `%errorlevel%`；
5. taskkill 收尾，按 RC 打印 PASS / "no main window after 10s" / FAIL 并以 RC 退出
   （0 通过、1 断言失败、2 无窗口）。
