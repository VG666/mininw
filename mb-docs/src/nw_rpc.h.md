# nw_rpc.h —— loopback HTTP 同步通道（声明）

- 源文件：[nw_rpc.h](../../src/nw_rpc.h)
- 角色：声明 `nmb::nw::RpcServer` 单例——本项目在 mb108 上**主用**的页面→native 同步通道。完整实现见 [nw_rpc.cpp](nw_rpc.cpp.md)。

## 为什么需要它（背景）

mb108 的 prompt 回程存在**内核级竞态**：native 每次都生成正确 JSON、`mbCreateString` 也返回非空句柄，
内核把句柄带回 JS 的那一步却概率性丢成 null/脏串，页面重试再多也只是赌命中率；
而同步 XHR + onLoadUrlBegin 在 mb108 上对同步 XHR 不回调。唯一与内核状态机无关的可靠 IPC 就是本机 TCP：
页面同步 XHR 阻塞在 send()，宿主在自己的 accept 线程当场作答，回程只走标准 socket 字节流。

## 接口

```cpp
using RpcHandler = std::function<std::string(int windowId, const std::string& payload)>;

class RpcServer {
  static RpcServer& Instance();
  bool Start(RpcHandler handler, std::string& error);  // 幂等；失败不致命（页面退回 prompt）
  void Stop();                                         // join 全部线程、WSACleanup
  bool running() const;
  int  port() const;                // 0 = 未监听；OS 随机分配
  const std::string& token() const; // 每进程 256bit 随机令牌
};
```

## 头注释里写定的安全模型与协议

- 只 `bind 127.0.0.1`：物理上不接受非本机连接，accept 后对端地址再校验一遍；
- 端口 `bind(port=0)` 由 OS 随机分配，每次启动都不同；
- 每进程 CSPRNG 生成 256bit 令牌，随注入脚本只交给本进程页面（**不落盘、不进命令行**）；任何请求必须在 query 带对 `t=<token>`，否则 403；
- 仅 POST，载荷大小封顶，收发各带超时；
- 协议刻意保持 CORS"简单请求"（同步 XHR 不能发预检）：
  `POST http://127.0.0.1:<port>/w<windowId>/?t=<token>`，`Content-Type: text/plain;charset=UTF-8`，
  body 是与其它通道相同的 `encodeURIComponent(base64(UTF-8(JSON)))`；应答 body 即 JSON 信封，恒带 `Access-Control-Allow-Origin: *`。

## 线程模型

accept 专用线程（非阻塞监听 socket + select 200ms 轮询 `running_`，保证 Stop 快速退出）；每条连接一个一次性 worker 线程，跑完由 accept 循环收尸。worker 不在 UI 线程，handler 只允许数据类命令（native 侧在 handler 里显式标 `SyncTransport::Loopback`，同步白名单兜底）。

## 实现隐藏手法

`listen_/running_` 用 `uintptr_t/volatile`，线程句柄与连接表用 `void*` 持有——把 `<windows.h>` 的 CRITICAL_SECTION、SOCKET、HANDLE 等全部藏进 .cpp，头文件保持轻依赖（仅 cstdint/functional/string）。
