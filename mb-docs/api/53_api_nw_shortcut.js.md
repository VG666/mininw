# 53_api_nw_shortcut.js —— nw.Shortcut（全局热键）

- 源文件：[js/api/53_api_nw_shortcut.js](../../js/api/53_api_nw_shortcut.js)
- 对应 SDK：`api_nw_shortcut.js`。

## 干了什么事

- `Shortcut(option)`：option `{key, active, failed}`。`key` 形如 `"Ctrl+Shift+P"`；active 是热键按下回调，failed 是注册失败回调。继承 Emitter（active/failed）；
- **id 段规则**：原型注册用的数字 id = `(keys 数量 + 1) * 1000` 起，保证"相同组合键的多个 Shortcut 实例"落在同一区间，native 侧同一组合只保留最后一次注册（SDK 语义）；[10 号 App](10_api_nw_app.js.md) 的转发入口则用 1..999 段，互不干扰；
- `shortcutRegistry`：全局表，key → 最新 Shortcut；
- `register()/unregister()`：异步 `shortcut.register/unregister`（payload 带组合键与回调名），native RegisterHotKey；`Shortcut.registerGlobalHotKey/unregisterGlobalHotKey` 是同名转发（App 层再转发到这里）；
- 派发口 `window.__nmbHotkey(key)`：WM_HOTKEY 经 EvalInWindow 调这里 → 查 registry → emit active；
- native 注册失败时页面通过异步回执 emit failed。实现见 [nw_host.cpp](../../src/nw_host.cpp) 的 `ApiShortcut`。
