# window_probe.ps1 —— Win32 窗口绑定探针

- 源文件：[tests/window_probe.ps1](../../tests/window_probe.ps1)
- 被 [run-window.bat](run-window.bat.md) 以框架窗口句柄 `$h` 为参数调用。

## 干了什么事

1. `Add-Type` 内联 C# 声明三个 user32 P/Invoke：
   `GetWindow(hwnd, uCmd)`、`GetParent(hwnd)`、`GetWindowRect(hwnd, out RECT)`；
2. 参数无效（空或 0）→ 打印并退出码 **2**；
3. `$child = GetWindow($main, 5)`（5 = **GW_CHILD**，取框架窗口的第一个子窗口，即内核控件窗口）；
   `$parent = GetParent($child)`；分别取主窗口与子窗口的屏幕坐标矩形；
4. 打印 main/child 句柄、child 的父、两者矩形，便于肉眼核对；
5. 四条断言（`Check` 任一失败置 fail=1）：
   - **child-exists**：GW_CHILD 取到非零句柄；
   - **parent-is-host**：`GetParent(child) == 框架 hwnd`（不是孤儿顶层窗口）；
   - **not-screen-origin**：子窗口左上角不是屏幕 (0,0)（旧 TRANSPARENT/CONTROL 混用的症状）；
   - **child-inside-host**：子矩形四条边都在主矩形内（容差 ±2px，边框取整）；
6. 全过退出码 **0**，任一失败退出码 **1**。输出全 ASCII（控制台码页安全）。
