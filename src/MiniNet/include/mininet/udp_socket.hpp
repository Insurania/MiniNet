#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mininet/packet.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace mininet {

struct UdpEndpoint {
    // IPv4 字符串地址。当前只支持类似 127.0.0.1 的 IPv4 地址。
    std::string address = "127.0.0.1";

    // UDP 端口号。0 通常表示让操作系统自动分配端口。
    std::uint16_t port = 0;
};

// UDP 每次 receive 都是一整个 datagram，同时能拿到发送方地址。
// 因为 UDP 无连接，服务端回复时必须使用 sender 里的地址和端口。
struct UdpDatagram {
    // UDP 包里的原始字节。
    std::vector<std::uint8_t> bytes;

    // 发送方地址。服务端回复时必须把 Pong 发回这里。
    UdpEndpoint sender;
};

// 一个很薄的 UDP socket 封装。
// 目标是把平台差异集中在这里，避免协议代码直接碰 winsock / POSIX socket。
class UdpSocket {
public:
    // 创建一个空 socket 包装对象。主要用于移动赋值前的默认状态。
    UdpSocket();

    // 析构时关闭底层系统 socket。
    ~UdpSocket();

    // socket 是系统资源，不能随意复制；复制会导致两个对象关闭同一个 socket。
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // 允许移动，把 socket 所有权从一个 UdpSocket 转移到另一个。
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    // 创建一个未绑定固定端口的 UDP socket，适合客户端发送请求。
    static UdpSocket open();

    // port=0 表示让操作系统自动分配一个可用端口，测试里很常用。
    static UdpSocket bind(std::uint16_t port);

    // 查询当前 socket 的本地端口。自动分配端口后需要用它告诉客户端发到哪里。
    std::uint16_t local_port() const;

    // 把一段字节作为 UDP datagram 发送到指定 endpoint。
    void send_to(ByteView bytes, const UdpEndpoint& endpoint) const;

    // UDP 没有“连接断开”事件；当前用 timeout 表达“这段时间内没有收到包”。
    std::optional<UdpDatagram> receive_from(std::chrono::milliseconds timeout) const;

private:
#ifdef _WIN32
    // Windows socket 句柄类型。
    using NativeSocket = SOCKET;

    // Windows 表示无效 socket 的特殊值。
    static constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
    // POSIX 系统上 socket 是 int 文件描述符。
    using NativeSocket = int;

    // POSIX 表示无效 socket 的常见值。
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    // 用已有系统 socket 句柄创建包装对象，只在 open/bind 内部使用。
    explicit UdpSocket(NativeSocket socket);

    // 当前对象拥有的底层系统 socket。
    NativeSocket socket_ = kInvalidSocket;
};

} // namespace mininet
