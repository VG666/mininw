# nw_json.h —— 极简 JSON（header-only）

- 源文件：[nw_json.h](../../src/nw_json.h)
- 角色：宿主侧所有 JSON 的解析与序列化。用途三处：读 package.json、解析页面发来的请求负载、拼应答信封。
- 为什么自带：nw.js 用 Chromium 的 `base::JSONReader/base::Value`；本模块没有 base，也不想为这点需求再拉一个 JSON 依赖。

## 能力边界

标准 JSON 子集：`null/bool/number/string/array/object`；支持 `\uXXXX`（含高/低代理对合并成一个码点再编码 UTF-8）、常见转义；object 用 `vector<pair>` 保存，**保持插入顺序**。

## 主要接口（class Json）

- 工厂：`object()/array()/fromText/fromNumber/fromBool`；类型判断 `isObject/isArray/...`；
- 写：`push`（数组）、`put(key, Json)`（同键覆盖）、`putString/putNumber/putBool`；
- 读：`find(key)` 指针；
  - `text(key, fallback)`：**容错取值**——字符串原样、数字转干净文本、bool 转 "true"/"false"；
  - `asText()` 是"对自身取值"版本（必须换名，否则 `text("c")` 与单参 fallback 版在 cl/clang 下 ambiguous）；
  - `boolean(key, fallback)`：bool/数字/字符串 "true"/"1" 都认；`number(key, fallback)`：数字/字符串(atof)/bool 都认。
  - 这种"类型不对就退回落值/再按字符串试一次"的宽容是刻意的：manifest 是用户手写文件，`"width": "800"` 比缺键更常见；
- `parse(text, out)`：递归下降解析（parseValue/Object/Array/String，strtod 解析数字）。**尾部多余内容忽略**（manifest 常带注释残渣/BOM 尾巴）；
- `dump()`/`write()`：序列化。字符串只转义 `"`、`\`、控制字符，中文按 UTF-8 原样写出（页面 JSON.parse 认，且 mbQuery 负载更短）。

## 两个细节

- `skipSpace` 额外放行 `//` 与 `/* */` 注释——真实项目 manifest 里经常带注释；
- `trimNumber(double)`：用 `%.6f` 再删尾随 0，避免窗口坐标写出 `100.00000000000001`；NaN/非有限值一律输出 `0`（不允许把 inf 写进 JSON）。
