# run-fsapp.bat —— fs 改文件测试（手工版启动器）

- 源文件：[tests/run-fsapp.bat](../../tests/run-fsapp.bat)
- 被测应用：[tests/apps/fsapp](app-fsapp.md)

## 干了什么事

1. 检查 `bin\nw.exe`（没有则退出码 1）；
2. `start bin\nw.exe --nwapp=tests\apps\fsapp --nw-no-dialog` 启动，**窗口保持打开**（手工测试）；
3. 提示看两处结论：窗口里的 verdict 横幅，以及"硬证据" `tests\apps\fsapp\test-file.txt`。

## 测试动作

页面加载时会自动跑一次主脚本栈同步 fs，并在 600ms 后自动点一次 appendFile；也可手工点页面按钮。
每次 `require('fs').appendFile` 向 test-file.txt 追加一行时间戳，再 `readFileSync` 回读，
回读到则页面判 PASS。是否真改了磁盘以 test-file.txt 内容为准。
自动化（无窗口停留）版本见 [run-fsapp-auto.bat](run-fsapp-auto.bat.md)。
