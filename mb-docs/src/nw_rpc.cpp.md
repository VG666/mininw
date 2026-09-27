# nw_rpc.cpp —— loopback HTTP RPC 服务（实现）

- 源文件：[nw_rpc.cpp](../../src/nw_rpc.cpp)
- 角色：[nw_rpc.h](nw_rpc.h.md) 的完整 WinSock2 实现。被 [nw_host.cpp](nw_host.cpp.md) 的 `Host::Start()` 启动、`Host::Shutdown()` 停止；被页面 [00_base.js](../api/00_base.js.md) 的 `loopbackOnce` 以同步 XHR 访问。

## 常量与数据

- `kMaxHeaderBytes = 64KB`、`kMaxBodyBytes = 32MB`（fs 写可能带较大内容）、收发超时各 `10000ms`；
- `struct ActiveConn { SOCKET sock; HANDLE thread; }`：在飞连接登记表。**socket 与线程句柄统一由 accept 线程收**（worker 自己不 close），杜绝 shutdown 撞上 fd 复用窗口。

## 各部分干的事

### 令牌：`CryptRandom` + `HexToken`
- 优先动态 `GetProcAddress(advapi32, "SystemFunction036")`（即 RtlGenRandom，XP 起可用）。刻意不静态链接，避免导入库差异成为构建故障点；
- 取不到才退化到 tick/pid/线程id/性能计数器多源播种的 LCG（Windows 上走不到的理论分支）；
- `HexToken(32)` → 32 字节随机数的 64 字符十六进制串 = 256bit 令牌。

### 应答：`SendResponse`
恒带 `Content-Type: text/plain; charset=utf-8`、`Access-Control-Allow-Origin: *`、`Cache-Control: no-store`、`Connection: close`。**错误状态码也带 CORS**，否则跨源 XHR 连错误正文都读不到。`RecvExact` 负责收满 Content-Length 字节。

### `Start()`
WSAStartup → 建 CRITICAL_SECTION 与连接表 → `socket(AF_INET, SOCK_STREAM)` → **bind `INADDR_LOOPBACK` 端口 0** → listen → `getsockname` 取回实际端口 → 监听 socket 置非阻塞（供 select 轮询）→ 生成令牌、置 running、建 accept 线程。任一步失败走 `fail:` 清理并返回 false（调用方不因此中止启动）。

### accept 循环：`AcceptLoop()`
1. 每轮先 `ReapFinished()` 收掉已结束的 worker；
2. `select` 200ms 超时等监听 socket 可读，超时就回头看 `running_`（Stop 最迟 200ms 退出，无需跨线程 closesocket 硬打断 accept——那在 Windows 上是未定义行为）；
3. accept 后校验 `ntohl(client.sin_addr.s_addr) == INADDR_LOOPBACK`，不是立刻关；
4. **关键修复（曾导致随机单条 XHR 失败）**：Windows 上 accept 出的 socket **继承监听 socket 的非阻塞模式**；不复位则 worker 的 recv 在数据未到时立刻拿到 `WSAEWOULDBLOCK`，被误判失败且不回应答。这里必须 `ioctlsocket(conn, FIONBIO, 0)` 改回阻塞，再设 `SO_RCVTIMEO/SO_SNDTIMEO`；
5. 建一次性 worker 线程跑 `HandleConnection`，句柄登记进表（临界区保护）。

### 连接处理：`HandleConnection()`（在 worker 线程）
- 读到 `\r\n\r\n` 为止（超 64KB 回 431）；解析请求行 METHOD/TARGET；
- `OPTIONS` 回 204 + 宽松 CORS（手工 fetch 排查用；同步 XHR 不会发预检）；非 POST 回 405；
- 只取 `Content-Length`；无 body 回 400、超 32MB 回 413；
- 路径必须形如 `/w<数字窗口id>(/...)`，否则 404；从 query 解析 `t`，与令牌不符（或令牌为空）一律 **403** 且不回额外细节；
- 收齐 body（头后已含的部分 + RecvExact 补齐，多余丢弃），调 `handler_(windowId, body)`，回 200 + JSON 信封。handler 为空时回 `{"s":"err","m":"RPC 服务未就绪"}`。

### 收尸：`ReapFinished()`
零超时 `WaitForSingleObject` 探测登记表里的 worker；已结束的摘出表、由本线程统一 `closesocket` + `CloseHandle`。

### `Stop()`
置 `running_=false` → join accept 线程（并关监听 socket）→ 临界区内 swap 出全部在飞连接 → 逐个 `shutdown(SD_BOTH)` 打断阻塞中的 recv → `WaitForSingleObject` join → closesocket/CloseHandle → 删临界区、删表、`WSACleanup`、清空端口/令牌/handler。
**必须在进程退出前同步执行**（Host::Shutdown），否则 worker 可能在 CRT/单例析构后还回调 Dispatch 踩已释放内存。
