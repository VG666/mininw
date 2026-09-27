# nw_api.js（根目录）—— 历史旧版单体页面脚本，当前不参与运行

- 源文件：[nw_api.js](../../archive/nw_api.js)（约 32KB）
- 状态标注：**这是拆分前的旧单体版本，保留作历史参考；当前构建与运行都不再读取它。**

## 来龙去脉

- 早期页面侧 nw.* 全部代码都在这一个文件里（约 700 行）：通道层 + App/Window/Menu/MenuItem/Clipboard/Shell/Screen/Shortcut/Tray + 装配层；
- 后来用一次性脚本 [archive/split_api.py](../build/split_api.py.md) 按 nw.js SDK 的模块边界把它拆成了
  [js/api/](../../js/api/) 目录下的 12 个模块（拆分时还做了"反向拼回逐行比对"的自检，证明搬家没丢语句）；
- 拆分后：
  - 运行时 `Host::LoadScripts()` 只枚举 exe 旁边的 `api\*.js`（`AssembleApiModules`），**不再读本文件**；
  - 内置副本由 [embed-js.ps1](../build/embed-js.ps1.md) 从 `api\` 目录生成，也不含本文件；
  - [build-nw.bat](../build/build-nw.bat.md) 还会主动删除遗留的 `bin\nw_api.js`，避免出现"第二份错误事实源"。

## 为什么文档里仍收它

- 它记录了通道设计的演进痕迹（文件头还在描述"同步 XHR 打 nmb-rpc:// 是唯一同步路径"的更早阶段，与现在
  "loopback 为主、prompt/XHR 为退路"的架构已不一致——阅读时以 [js/api/00_base.js](../api/00_base.js.md) 与
  [00-架构总览](../00-架构总览.md) 为准）；
- 查某段逻辑的演变历史时可以对照它与拆分后模块。

## 想改页面行为该改哪里

改 [js/api/](../../js/api/) 下对应模块（索引见 [mb.md](../mb.md) 第二节），**不要改本文件**，改了也不会生效。
