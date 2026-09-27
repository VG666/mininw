# run-smoke.bat —— 21 项端到端冒烟的驱动脚本

- 源文件：[tests/run-smoke.bat](../../tests/run-smoke.bat)
- 被测应用：[tests/apps/smoke](app-smoke.md)

## 干了什么事

1. 检查 `bin\nw.exe`，没有则退出码 1；
2. 删掉旧的 `tests\.out\smoke-result.json`，启动
   `bin\nw.exe --nwapp=tests\apps\smoke --out=<smoke-result.json 全路径>`（`start` 后台启动）；
3. **轮询等报告**：每约 0.5~1 秒（`ping -n 2` 计时）看报告文件是否出现，最多 ~40 秒（80 次）；
   超时 → taskkill 并退出码 **2**；
4. **两阶段收割**：页面先写一版报告（同步断言，几乎立刻落盘），约 1.4 秒后再追加一版
   （异步观测：mbQuery 应答、Window.open 的 id 结算）。所以看到文件后还多等 ~5 秒、
   让应用自己 quit，再读文件——否则第二阶段丢失（脚本注释明确）；
5. `findstr "\"ok\": false"`：报告里**只要有一条 false 就判 FAIL（退出码 3）**，不接受部分通过；
   没有 false → 打印 PASS，退出码 **0**。

## 取证产物

- `tests\.out\smoke-result.json`：含 `passed/total/ok`、`channel`（syncBase、线程拓扑 main/ui/sync、
  syncSharesUi、rpcErrors、asyncErrors、nodeError）与每条 `results`（name/ok/detail）和 `late` 数组。

## 为什么这样设计

页面在真实进程里把整条桥（nw.* + node 桥 + native fs/clipboard/screen RPC）都驱动一遍，
并把结果用被测的 fs 通道自己落盘——"落盘"这一步本身也是 fs 往返用例。驱动脚本只负责
"起进程、等文件、判 false"，不碰页面内部。
