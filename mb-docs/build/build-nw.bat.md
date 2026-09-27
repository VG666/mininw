# build-nw.bat —— 构建 nw.exe

- 源文件：[build-nw.bat](../../build-nw.bat)
- 角色：将 `src\` 下宿主源文件与 `generated\` 内置脚本编成 `bin\nw.exe`。默认使用嵌入式页面脚本。

## 干了什么事（按序）

1. **找 zig**：本机 MSVC CRT 损坏、cl 链接必挂（文件头注释），所以用 zig 自带的 mingw 交叉工具链。
   候选目录 `..\.toolchain-fresh` → `..\.toolchain-fixed` → `..\.toolchain` 下的
   `zig-x86_64-windows-0.15.2\zig.exe`；都没有再 `where zig` 找 PATH。
2. **生成内置脚本**：调 [embed-js.ps1](embed-js.ps1.md)（入参 `js\api` / `js\nw_node.js` / `generated\nw_script_generated.cpp`）生成 [nw_script_generated.cpp](../src/nw_script_generated.cpp.md)；
   `errorlevel 1` 时回退 `SCRIPT_SRC=src\nw_script_stub.cpp`（空副本，此时 exe 旁必须有 JS）。
3. **编译**：
   `zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Isrc`
   `-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE`
   源文件：`src\nw_main src\nw_host src\nw_bridge src\nw_kernel src\nw_package src\nw_rpc` + 脚本副本（`-Isrc` 让 generated 的 cpp 能找到 `nw_script.h`）；
   链接库：`user32 gdi32 shell32 ole32 oleaut32 advapi32 kernel32 winspool ws2_32 m`
   （winspool=打印枚举、ws2_32=loopback RPC、shell32=托盘/ShellExecute 等）；
   产物 `bin\nw.exe`。
4. **清理旧副本**：生成脚本已编入 `nw.exe`，删除遗留的 `bin\api\`、`bin\nw_api.js`、`bin\nw_node.js`。修改 `js\nw_node.js` 后需要重新构建；只有脚本生成失败并使用空 stub 时才须在 exe 旁另行提供 JS。
5. **加载内核**：此脚本不复制 DLL。运行时在 exe 附近或 PATH 查找 `miniblink_x64.dll`，并可通过 `--nw-kernel=<路径>` 指定。

## 工程注意

- **全文件必须 ASCII**：cmd 按 OEM 码页（本机 936/GBK）逐字节读 .bat，UTF-8 中文会吞掉后续字符导致
  "The syntax of the command is incorrect"（文件头注释）。中文请写在 .md/.html 里。
- 在本机跑（PowerShell）需要给 zig 指定可写缓存，否则沙箱/中文路径可能报错：
  `$env:ZIG_GLOBAL_CACHE_DIR="f:\编程\MBPython-master\NativeMediaBridge\.zig-global-cache"`，
  并把 `TEMP/TMP` 指到同盘可写目录；然后在 `nw\` 下执行 `.\build-nw.bat`。
