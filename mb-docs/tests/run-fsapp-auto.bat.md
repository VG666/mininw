# run-fsapp-auto.bat —— fs 改文件测试（自动取证版）

- 源文件：[tests/run-fsapp-auto.bat](../../tests/run-fsapp-auto.bat)
- 被测应用：[tests/apps/fsapp](app-fsapp.md)

## 干了什么事

1. 先删掉旧的 `tests\apps\fsapp\test-file.txt`（保证这次跑出来的文件是新证据）；
2. `start bin\nw.exe --nwapp=tests\apps\fsapp --nw-no-dialog`；
3. `ping -n 9` 等约 8 秒：页面加载即做主脚本栈 `appendFileSync + readFileSync`，
   600ms 后又自动做一次异步 `appendFile`，这段时间足够两次落盘；
4. 打印三段取证：
   - `---title---`：`tasklist /v /fo csv | find "fsapp"` 抓窗口标题——页面把结论
     （`fsapp:PASS:<通道>` 或 `fsapp:FAIL:...|probe=...|node=...|main=...`）写进标题，
     不用读 DOM 就能从外部定位断点；
   - `---file---`：test-file.txt 存在就 `type` 出真实内容，不存在打印 `NO-FILE`；
   - `---end---`；
5. taskkill 收尾，退出码恒 0（结论靠打印内容人工/上层判断，不在此脚本里做布尔判定）。

## 与手工版的区别

[run-fsapp.bat](run-fsapp.bat.md) 留窗等人点按钮；本脚本无人值守，用"删文件→跑→直接看磁盘文件
+标题"证明 fs 真的穿过桥改了文件系统。
