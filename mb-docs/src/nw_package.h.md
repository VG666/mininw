# nw_package.h —— package.json 清单与路径工具（声明）

- 源文件：[nw_package.h](../../src/nw_package.h)
- 角色：声明应用清单结构 `Manifest`、清单加载/应用目录定位两个入口，以及一组 Win32 路径与编码工具。实现见 [nw_package.cpp](nw_package.cpp.md)。移植自 nw.js 的 `src/nw_package.{h,cc}`（原件在 [reference/cc](../reference/00-extracted总览.md)），保留字段名与判定顺序，去掉 Chromium 依赖。

## `struct Manifest` 主要字段

- `ok/error`、`appPath`（应用根目录）、`manifestPath`（package.json 全路径）；
- `name`（缺省 "nwjs"，比 nw.js 宽松）、`version`、`main`（缺省 `nw:blank`）、`startupUrl`（main 解析后的 file:// 或远程 URL）；
- `chromiumArgs`（环境 `NW_PRE_ARGS` + manifest `chromium-args`，只留开关形式）、`jsFlags`、`userAgent`；
- `useNode`（manifest `nodejs`，缺省 true）、`singleInstance`、`disableDevTools`；
- `root`（原始 JSON，供 `nw.App.manifest`）、`window`（window 段，缺省补 `{"position":"center"}`）、`source`（package.json 原文透传页面）。

## 顶层函数

- `LoadManifest(path, out)`：读并解析应用目录下的 package.json；path 给目录或 manifest 文件均可；
- `AutoLocateApp(args, appPath)`：按 nw.js 顺序自动找应用（exe 目录 → exe\package.nw → `--nwapp=` → 第一个非开关参数）；
- 路径/编码工具：`WideToUtf8/Utf8ToWide`、`JoinPath`、`PathExists/IsDirectory/ReadTextFile`、`ExecutablePath/ExecutableDirectory`、`ToFileUrl`（带 scheme 的输入原样返回）。

## 与原版的差异（头注释明确）

- `base::Value/JSONFileValueDeserializer` → 自带 [nw_json.h](nw_json.h.md)；`base::FilePath/PathService` → Win32 自拼；
- 不做 zip `.nw` 解包，只认目录形式应用；
- `CommandLine::AppendSwitch` 简化为把 chromium-args 收进字符串字段。
