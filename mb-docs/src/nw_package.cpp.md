# nw_package.cpp —— 清单解析、应用定位、路径与编码工具（实现）

- 源文件：[nw_package.cpp](../../src/nw_package.cpp)
- 角色：[nw_package.h](nw_package.h.md) 的实现。宿主启动早期被调用，决定"跑哪个应用、打开哪个 URL、开不开 node"。

## 实现的工具函数（被全项目复用）

- `WideToUtf8 / Utf8ToWide`：`WideCharToMultiByte/MultiByteToWideChar(CP_UTF8)`，两遍分配；
- `NormalizePath`：斜杠统一为反斜杠 + 就地折叠 `.`/`..`。**三种根分开处理**（盘 `C:`、UNC `\\srv`、单反斜杠根），注释记录了踩坑：若按"结尾是冒号就不加分隔符"拼接，会把 `C:\Users\x` 错拼成盘相对路径 `C:Users\x`，落到 C: 盘当前目录；
- `PercentEncodePath`：file:// 路径里的非安全字符按 UTF-8 百分号编码。mbLoadURL 只吃 UTF-8，应用目录带空格/中文（如 `F:\编程\...`）时不编码会加载失败；
- `JoinPath`：叶子是盘绝对/UNC 时直接覆盖；否则拼 base + `\` 后统一 Normalize；
- `PathExists/IsDirectory`（GetFileAttributesW）、`ReadTextFile`（CreateFile 读，限 64MB，**剥除 UTF-8 BOM**——不剥会让 JSON 解析在第一个字符就失败）；
- `ExecutablePath/ExecutableDirectory`（GetModuleFileNameW 倍增缓冲）；
- `ToFileUrl`：相对路径先相对当前目录转绝对，再 Normalize，UNC 用 `file:`，其余 `file:///` + 百分号编码；
- `HasScheme/ResolveMain`：判断 main 是否完整 URL；相对 main 拼成应用目录下的 file://；
- `Tokenize`：按空格/引号拆 chromium-args（nw.js 用 base::StringTokenizer + 单引号 quote）；
- `CollectChromiumArgs`：`NW_PRE_ARGS` 在前、manifest `chromium-args` 在后，只采纳 `-x/--x` 开关形式，裸参数丢弃。

## `LoadManifest(path, out)` 干的事

1. path 是目录 → 找其中 `package.json`（没有再试 `manifest.json`）；是文件 → 直接用（本实现不支持 .nw 解包）；不存在报错；
2. `ReadTextFile` + `Json::parse`，必须是 object，否则失败；原文存 `source`；
3. 取字段：`name(缺省 nwjs)/version/main/nodejs(缺省 true)/single-instance/disable-dev-tools/js-flags/user-agent/chromium-args`；
4. `window` 段不存在则给空对象并补 `position:"center"`（与 nw.js 一致）；
5. `--url=<...>` 开关优先于 manifest main（无 scheme 时补 `http://`），否则 `ResolveMain(appPath, main)` 得 startupUrl。

## `AutoLocateApp(args, appPath)` 顺序

1. exe 所在目录本身含 package.json/manifest.json（nw.js GetSelfPath 语义）；
2. exe 旁边的 `package.nw` 目录；
3. `--nwapp=<dir>`；
4. 第一个不带 `-`/`/` 开关前缀的参数。
