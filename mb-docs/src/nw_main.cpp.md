# nw_main.cpp —— 进程入口

- 源文件：[nw_main.cpp](../../src/nw_main.cpp)
- 角色：**nw.exe 的 `main()`**。整个进程只做三件事：解析命令行、起宿主、进消息循环。
- 对应 nw.js 原版：`content::BrowserMainRunner` + `nw::NWAppMainDelegate`（几百行拉 Chromium）；本项目把浏览器外包给 miniblink，所以入口只剩几十行。

## 干了什么事（按执行顺序）

1. `SetProcessDPIAware()`：声明系统 DPI 感知，高分屏不被系统拉伸发虚。刻意用老接口而非 `SetProcessDpiAwarenessContext`（后者要 Win10 头文件，此处无需）。
2. `SplitCommandLine()`：`GetCommandLineW` + `CommandLineToArgvW` 拆宽字符参数，跳过 argv[0]（exe 自己）。
3. 遍历参数填 `nmb::nw::HostOptions`：
   - `--nwapp=<目录>` → `options.appPath`
   - `--nw-kernel=<路径或目录>` → `options.kernelPath`（交给 LoadKernel 决定是 dll 还是目录）
   - `--nw-no-node` → `options.disableNode = true`（等价 manifest `nodejs:false`）
   - `--nw-no-dialog` → 本文件静态变量 `g_noDialog`：启动失败只写 stderr，**不弹模态框**——自动化测试/CI 必须有它，否则进程挂死等点击。
   - 若没给 `--nwapp`，第一个不以 `-` 开头的参数也当应用目录（与 `nw.exe path\to\app` 用法一致）。
4. `Host::Instance().Start(GetModuleHandleW(nullptr), options, error)`；失败调 `ShowError()`（OutputDebugString + stderr，非 no-dialog 时 MessageBox），返回 1。
5. `Host::OpenWindow(std::string(), nullptr)` 开主窗（空 URL = 用 manifest 的 main）；失败返回 2。
6. `int exitCode = host.Run()`（Win32 消息循环）；**`host.Shutdown()`** 在返回前同步停掉 loopback RPC（join 全部 accept/worker 线程），最后 `return exitCode`。

## 为什么用普通 main 而不是 wmain

源码注释明确：wmain 需要 `-municode` 换入口点，而参数本就从 `GetCommandLineW` 取，没必要多担一个编译开关（zig/mingw 兼容考虑）。

## 协作关系

- 只依赖 [nw_host.h](../../src/nw_host.h)（`Host`、`HostOptions`）；
- 宽字符转 UTF-8 用的是 `nmb::nw::WideToUtf8`（实现在 [nw_package.cpp](../../src/nw_package.cpp)）。
