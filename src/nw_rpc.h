#pragma once
// 本机 loopback HTTP RPC —— mb108 上**主用**的页面 → native 同步通道。
//
// 为什么需要它：mb108 的 prompt 回程存在内核级竞态——native 每次都生成了正确的
// JSON，但内核把 mbCreateString 句柄带回 JS 的那一步会概率性丢成 null/脏串，
// 页面侧无论重试多少次都只是赌命中率。同步 XHR + onLoadUrlBegin 在 mb108 上又不
// 回调。唯一与内核状态机无关的可靠 IPC，就是本机 TCP：页面发同步 XHR 阻塞在
// send() 上，宿主在自己的 accept 线程里当场作答，回程只经过标准 socket 字节流。
//
// 安全模型（随机令牌、防跨进程调用）：
//   * 只 bind 127.0.0.1：物理上不接受任何非本机连接，对端地址再校验一遍；
//   * 端口由 OS 在 bind(port=0) 时随机分配，每次启动都不同；
//   * 每次启动用 CSPRNG 生成 256bit 令牌，随注入脚本只交给本进程的页面
//     （不落盘、不进命令行），任何请求都必须在 query 上带对令牌，否则 403。
//     本机别的进程 / 浏览器里的恶意页面都拿不到它；
//   * 仅接受 POST，载荷大小封顶，socket 读写均带超时。
//
// 协议（刻意保持为 CORS “简单请求”，同步 XHR 不能发预检）：
//   请求：POST http://127.0.0.1:<port>/w<windowId>/?t=<token>
//         Content-Type: text/plain;charset=UTF-8
//         body: encodeURIComponent(base64(UTF-8(JSON 请求)))   与其它通道同编码
//   应答：HTTP/1.1 200，text/plain; charset=utf-8，body 即 JSON 信封，
//         恒带 Access-Control-Allow-Origin: *（file:// 源跨到 http 源要用）。
//
// 线程：accept 跑在一个专用线程上（select 200ms 轮询 running_，保证 Stop 能快速
// 退出）；每条连接一个一次性 worker 线程，跑完即收，由 accept 循环顺手收尸。
// Stop 时先断 accept，再 shutdown 所有在飞连接的 socket 打断 recv，逐条 join——
// 进程退出那一刻不允许还有 worker 在碰 Host 单例。worker 调 handler 时不在
// UI 线程上，所以 handler 只允许做数据类命令（Host::Dispatch 的同步白名单兜底）。

#include <cstdint>
#include <functional>
#include <string>

namespace nmb {
namespace nw {

// windowId 由页面在路径里带上（window.__nwWindowId，注入脚本预给）；
// payload 是 percent-encoded base64 文本，解码由宿主侧复用既有函数完成。
using RpcHandler = std::function<std::string(int windowId, const std::string& payload)>;

class RpcServer {
public:
    static RpcServer& Instance();

    // 幂等：重复 Start 返回已在跑的实例。失败（WSAStartup/bind 失败）返回 false
    // 并写 error；此时页面会自动退回 prompt 退路，宿主不必因此整体启动失败。
    bool Start(RpcHandler handler, std::string& error);
    void Stop();

    bool running() const { return listen_ != kInvalidSocket; }
    int port() const { return port_; }          // 0 = 未监听
    const std::string& token() const { return token_; }

private:
    RpcServer() = default;
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    static constexpr uintptr_t kInvalidSocket = ~static_cast<uintptr_t>(0);

    void AcceptLoop();
    void HandleConnection(uintptr_t socket);
    void ReapFinished();                         // 收掉已结束 worker 的句柄/socket

    RpcHandler handler_;
    volatile uintptr_t listen_ = kInvalidSocket;
    volatile bool running_ = false;
    void* acceptThread_ = nullptr;               // HANDLE
    void* connMutex_ = nullptr;                  // CRITICAL_SECTION*
    void* conns_ = nullptr;                      // std::vector<ActiveConn>*（实现藏在 cpp）
    int port_ = 0;
    std::string token_;
};

} // namespace nw
} // namespace nmb
