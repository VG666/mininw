#include "nw_rpc.h"

// winsock2 必须在 windows.h 之前，否则后者会拖进 winsock v1 造成重定义。
// WIN32_LEAN_AND_MEAN 由构建命令行统一定义。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace nmb {
namespace nw {

namespace {

constexpr int kMaxHeaderBytes = 64 * 1024;        // 请求头（请求行+头字段）封顶
constexpr int kMaxBodyBytes = 32 * 1024 * 1024;   // fs 写文件可能带较大内容
constexpr int kRecvTimeoutMs = 10000;             // 单条连接读超时
constexpr int kSendTimeoutMs = 10000;

// 在飞连接：句柄/socket 都由 accept 线程统一收（worker 自己不 close，
// 避免 close 与 shutdown 撞在 fd 复用窗口上）。
struct ActiveConn {
    SOCKET sock{INVALID_SOCKET};
    HANDLE thread{nullptr};
};

using ConnList = std::vector<ActiveConn>;

// RtlGenRandom 在 advapi32 里实际导出名是 SystemFunction036（XP 起全版本可用）。
// 不静态链接它，免得工具链导入库差异再添一个构建故障点；拿不到就退化到
// tick/pid/线程id/性能计数器多源播种（理论分支，Windows 上取不到的概率为零）。
bool CryptRandom(void* buffer, size_t size) {
    HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
    if (!advapi) advapi = LoadLibraryW(L"advapi32.dll");
    if (advapi) {
        using SystemFunction036Fn = BOOLEAN (WINAPI*)(PVOID, ULONG);
        auto fn = reinterpret_cast<SystemFunction036Fn>(
            GetProcAddress(advapi, "SystemFunction036"));
        if (fn && fn(buffer, static_cast<ULONG>(size))) return true;
    }
    unsigned char* out = static_cast<unsigned char*>(buffer);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    unsigned int seed = static_cast<unsigned int>(
        GetTickCount() ^ GetCurrentProcessId() ^ GetCurrentThreadId() ^
        static_cast<unsigned long>(counter.QuadPart));
    for (size_t i = 0; i < size; ++i) {
        seed = seed * 1103515245u + 12345u;
        out[i] = static_cast<unsigned char>(seed >> 16);
    }
    return false;
}

std::string HexToken(size_t bytes) {
    std::vector<unsigned char> raw(bytes);
    CryptRandom(raw.data(), raw.size());
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        out[i * 2] = kHex[raw[i] >> 4];
        out[i * 2 + 1] = kHex[raw[i] & 0x0F];
    }
    return out;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    return text.substr(begin, end - begin);
}

// 拼一份恒带 CORS 头的应答（错误状态码也得带，否则跨源 XHR 连错误正文都读不到）。
void SendResponse(SOCKET sock, int status, const char* reason, const std::string& body) {
    char header[512];
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason, body.size());
    if (headerLen > 0) send(sock, header, headerLen, 0);
    if (!body.empty()) send(sock, body.data(), static_cast<int>(body.size()), 0);
}

// 尽力读满 length 字节（recv 可能一次只回一部分）。超时/对端关闭即失败。
bool RecvExact(SOCKET sock, char* buffer, size_t length) {
    size_t got = 0;
    while (got < length) {
        int n = recv(sock, buffer + got, static_cast<int>(length - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

RpcServer& RpcServer::Instance() {
    static RpcServer server;
    return server;
}

bool RpcServer::Start(RpcHandler handler, std::string& error) {
    if (running_) return true;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }

    auto* lock = new CRITICAL_SECTION();
    InitializeCriticalSection(lock);
    connMutex_ = lock;
    conns_ = new ConnList();

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        error = "failed to create the RPC listen socket";
        goto fail;
    }

    // 只绑 loopback：物理上拒绝一切外部网卡进来的连接。端口填 0 让 OS 随机挑一个
    // 临时端口，随后 getsockname 取回真值注入页面。
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            error = "failed to bind RPC to a 127.0.0.1 random port";
            closesocket(listenSocket);
            goto fail;
        }
    }
    if (listen(listenSocket, SOMAXCONN) != 0) {
        error = "RPC listen failed";
        closesocket(listenSocket);
        goto fail;
    }
    {
        sockaddr_in bound{};
        int boundLen = sizeof(bound);
        if (getsockname(listenSocket, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
            error = "failed to get the actual RPC port";
            closesocket(listenSocket);
            goto fail;
        }
        port_ = ntohs(bound.sin_port);
    }

    // 非阻塞 + select 轮询：Stop 时不必靠“跨线程 closesocket”硬打断 accept
    // （那在 Windows 上是未定义行为），置 running_=false 后 200ms 内自然退出。
    {
        u_long nonblocking = 1;
        ioctlsocket(listenSocket, FIONBIO, &nonblocking);
    }

    handler_ = std::move(handler);
    token_ = HexToken(32);                          // 256bit
    listen_ = reinterpret_cast<uintptr_t>(listenSocket);
    running_ = true;
    acceptThread_ = CreateThread(nullptr, 0,
        [](LPVOID param) -> DWORD {
            static_cast<RpcServer*>(param)->AcceptLoop();
            return 0;
        }, this, 0, nullptr);
    if (!acceptThread_) {
        error = "failed to create the RPC accept thread";
        listen_ = kInvalidSocket;
        running_ = false;
        closesocket(listenSocket);
        goto fail;
    }
    return true;

fail:
    DeleteCriticalSection(lock);
    delete lock;
    connMutex_ = nullptr;
    delete static_cast<ConnList*>(conns_);
    conns_ = nullptr;
    WSACleanup();
    return false;
}

void RpcServer::Stop() {
    if (!running_ && !acceptThread_) return;
    running_ = false;
    if (acceptThread_) {
        // accept 线程每 200ms 看一次 running_，很快就会退出，并顺手收完最后一轮尸。
        WaitForSingleObject(static_cast<HANDLE>(acceptThread_), INFINITE);
        CloseHandle(static_cast<HANDLE>(acceptThread_));
        acceptThread_ = nullptr;
    }
    if (listen_ != kInvalidSocket) {
        closesocket(static_cast<SOCKET>(listen_));
        listen_ = kInvalidSocket;
    }

    // accept 线程已死，不会再有新 fd。把仍在 recv 上等着的连接全部 shutdown 打断，
    // 然后逐条 join 并回收：进程退出那一刻不允许还有 worker 碰 Host 单例，
    // 也不允许 socket/句柄漏给操作系统。
    auto* lock = static_cast<CRITICAL_SECTION*>(connMutex_);
    ConnList remaining;
    if (lock && conns_) {
        EnterCriticalSection(lock);
        remaining.swap(*static_cast<ConnList*>(conns_));
        LeaveCriticalSection(lock);
    }
    for (ActiveConn& conn : remaining) {
        shutdown(conn.sock, SD_BOTH);
        WaitForSingleObject(conn.thread, INFINITE);
        closesocket(conn.sock);
        CloseHandle(conn.thread);
    }

    if (lock) {
        DeleteCriticalSection(lock);
        delete lock;
        connMutex_ = nullptr;
    }
    delete static_cast<ConnList*>(conns_);
    conns_ = nullptr;
    WSACleanup();
    port_ = 0;
    token_.clear();
    handler_ = nullptr;
}

void RpcServer::ReapFinished() {
    auto* lock = static_cast<CRITICAL_SECTION*>(connMutex_);
    auto* list = static_cast<ConnList*>(conns_);
    if (!lock || !list) return;
    ConnList dead;
    EnterCriticalSection(lock);
    for (auto it = list->begin(); it != list->end();) {
        // 线程句柄还在手里：零超时探一下，已结束的就收掉（socket 由这里关，
        // worker 自己不关，杜绝“shutdown 撞上 fd 已被复用”的窗口）。
        if (WaitForSingleObject(it->thread, 0) == WAIT_OBJECT_0) {
            dead.push_back(*it);
            it = list->erase(it);
        } else {
            ++it;
        }
    }
    LeaveCriticalSection(lock);
    for (ActiveConn& conn : dead) {
        closesocket(conn.sock);
        CloseHandle(conn.thread);
    }
}

void RpcServer::AcceptLoop() {
    SOCKET listenSocket = static_cast<SOCKET>(listen_);
    while (running_) {
        ReapFinished();

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSocket, &readable);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;
        int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;                    // 超时：回头看 running_
        sockaddr_in client{};
        int clientLen = sizeof(client);
        SOCKET conn = accept(listenSocket, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (conn == INVALID_SOCKET) continue;
        // 双保险：对端必须真是 loopback（只绑了 LOOPBACK，理论上不可能不是）。
        if (ntohl(client.sin_addr.s_addr) != INADDR_LOOPBACK) {
            closesocket(conn);
            continue;
        }

        // 关键：Windows 上 accept 出来的 socket **继承监听 socket 的阻塞模式**，
        // 而监听 socket 为了 select 轮询被设成了非阻塞。不复位的话 worker 的 recv
        // 在数据未到时立刻拿到 WSAEWOULDBLOCK（而不是按 SO_RCVTIMEO 等待），我们会
        // 误判成连接失败且不回任何应答——表现为页面随机一条 XHR "Failed to load"。
        u_long blocking = 0;
        ioctlsocket(conn, FIONBIO, &blocking);

        DWORD timeoutMs = kRecvTimeoutMs;
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        timeoutMs = kSendTimeoutMs;
        setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        struct WorkerParam { RpcServer* server; SOCKET sock; };
        auto* param = new WorkerParam{this, conn};
        HANDLE thread = CreateThread(nullptr, 0,
            [](LPVOID raw) -> DWORD {
                auto* p = static_cast<WorkerParam*>(raw);
                p->server->HandleConnection(reinterpret_cast<uintptr_t>(p->sock));
                delete p;
                return 0;
            }, param, 0, nullptr);
        if (!thread) {
            closesocket(conn);
            delete param;
            continue;
        }

        auto* lock = static_cast<CRITICAL_SECTION*>(connMutex_);
        auto* list = static_cast<ConnList*>(conns_);
        EnterCriticalSection(lock);
        list->push_back(ActiveConn{conn, thread});
        LeaveCriticalSection(lock);
    }
    ReapFinished();
}

void RpcServer::HandleConnection(uintptr_t socketValue) {
    SOCKET sock = static_cast<SOCKET>(socketValue);

    // ---- 读请求头直到 \r\n\r\n ----
    std::string raw;
    raw.reserve(2048);
    char chunk[4096];
    size_t headerEnd = std::string::npos;
    while (true) {
        int n = recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) return;                          // 超时/对端关：静默断开
        raw.append(chunk, static_cast<size_t>(n));
        headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (raw.size() > kMaxHeaderBytes) {
            SendResponse(sock, 431, "Request Header Fields Too Large", "header too large");
            return;
        }
    }

    // ---- 请求行：METHOD SP TARGET SP HTTP/1.1 ----
    size_t firstLineEnd = raw.find("\r\n");
    std::string firstLine = raw.substr(0, firstLineEnd);
    size_t sp1 = firstLine.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : firstLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        SendResponse(sock, 400, "Bad Request", "malformed request line");
        return;
    }
    std::string method = firstLine.substr(0, sp1);
    std::string target = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);

    // OPTIONS 只可能来自跨源预检：同步 XHR 不会发预检（刻意只用简单请求），
    // 但如实答一份宽松 CORS，方便排查时用 fetch 手工打。
    if (method == "OPTIONS") {
        static const char kPreflight[] =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(sock, kPreflight, static_cast<int>(sizeof(kPreflight) - 1), 0);
        return;
    }
    if (method != "POST") {
        SendResponse(sock, 405, "Method Not Allowed", "POST only");
        return;
    }

    // ---- 头字段：只需要 Content-Length ----
    long long contentLength = 0;
    size_t cursor = firstLineEnd + 2;
    while (cursor < headerEnd) {
        size_t lineEnd = raw.find("\r\n", cursor);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) break;
        std::string line = raw.substr(cursor, lineEnd - cursor);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = Trim(line.substr(0, colon));
            std::string value = Trim(line.substr(colon + 1));
            if (_stricmp(name.c_str(), "Content-Length") == 0) contentLength = std::atoll(value.c_str());
        }
        cursor = lineEnd + 2;
    }
    if (contentLength <= 0) {
        SendResponse(sock, 400, "Bad Request", "empty body");
        return;
    }
    if (contentLength > kMaxBodyBytes) {
        SendResponse(sock, 413, "Payload Too Large", "body too large");
        return;
    }

    // ---- 路径与令牌：/w<id>[/...]?t=<token> ----
    std::string path = target;
    std::string query;
    size_t question = target.find('?');
    if (question != std::string::npos) {
        path = target.substr(0, question);
        query = target.substr(question + 1);
    }
    int windowId = 0;
    bool pathOk = false;
    if (path.compare(0, 2, "/w") == 0) {
        size_t digits = 2;
        long long parsed = 0;
        bool any = false;
        while (digits < path.size() && path[digits] >= '0' && path[digits] <= '9') {
            parsed = parsed * 10 + (path[digits] - '0');
            any = true;
            ++digits;
        }
        if (any && (digits == path.size() || path[digits] == '/')) {
            windowId = static_cast<int>(parsed);
            pathOk = true;
        }
    }
    if (!pathOk) {
        SendResponse(sock, 404, "Not Found", "unknown path");
        return;
    }

    std::string offeredToken;
    size_t queryPos = 0;
    while (queryPos < query.size()) {
        size_t amp = query.find('&', queryPos);
        std::string pair = query.substr(queryPos,
            amp == std::string::npos ? std::string::npos : amp - queryPos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == "t") {
            offeredToken = pair.substr(eq + 1);
            break;
        }
        if (amp == std::string::npos) break;
        queryPos = amp + 1;
    }
    // 随机令牌是唯一的跨进程防线，拿不到/不对一律 403，不回其它细节。
    if (token_.empty() || offeredToken != token_) {
        SendResponse(sock, 403, "Forbidden", "bad token");
        return;
    }

    // ---- body：头后已收到的部分 + 继续读到 Content-Length ----
    std::string body = raw.substr(headerEnd + 4);
    if (static_cast<long long>(body.size()) < contentLength) {
        size_t oldSize = body.size();
        size_t need = static_cast<size_t>(contentLength) - oldSize;
        body.resize(static_cast<size_t>(contentLength));
        if (!RecvExact(sock, &body[oldSize], need)) return;
    } else if (static_cast<long long>(body.size()) > contentLength) {
        body.resize(static_cast<size_t>(contentLength));  // Connection: close，多余字节丢弃
    }

    std::string reply;
    if (handler_) {
        reply = handler_(windowId, body);
    } else {
        reply = "{\"s\":\"err\",\"m\":\"RPC 服务未就绪\"}";
    }
    SendResponse(sock, 200, "OK", reply);
}

} // namespace nw
} // namespace nmb
