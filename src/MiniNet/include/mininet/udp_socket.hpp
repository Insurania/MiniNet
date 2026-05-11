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
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
};

// UDP 每次 receive 都是一整个 datagram，同时能拿到发送方地址。
// 因为 UDP 无连接，服务端回复时必须使用 sender 里的地址和端口。
struct UdpDatagram {
    std::vector<std::uint8_t> bytes;
    UdpEndpoint sender;
};

// 一个很薄的 UDP socket 封装。
// 目标是把平台差异集中在这里，避免协议代码直接碰 winsock / POSIX socket。
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    static UdpSocket open();

    // port=0 表示让操作系统自动分配一个可用端口，测试里很常用。
    static UdpSocket bind(std::uint16_t port);

    std::uint16_t local_port() const;

    void send_to(ByteView bytes, const UdpEndpoint& endpoint) const;

    // UDP 没有“连接断开”事件；当前用 timeout 表达“这段时间内没有收到包”。
    std::optional<UdpDatagram> receive_from(std::chrono::milliseconds timeout) const;

private:
#ifdef _WIN32
    using NativeSocket = SOCKET;
    static constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
    using NativeSocket = int;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    explicit UdpSocket(NativeSocket socket);

    NativeSocket socket_ = kInvalidSocket;
};

} // namespace mininet
