#pragma once

#include "mininet/packet.hpp"
#include "mininet/udp_socket.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace mininet {

// 服务端处理一个 UDP 包后的结果，主要给测试和日志使用。
// 后续增加连接、ACK、重传时，也会继续沿用这种“处理一步，返回结果”的思路。
struct ServerStepResult {
    bool received = false;
    bool responded = false;
    PacketRejectReason reason = PacketRejectReason::None;
    std::optional<std::uint32_t> sequence;
};

struct PingResult {
    bool received = false;
    std::uint32_t sequence = 0;
    std::chrono::milliseconds rtt{0};
};

// 当前 server 是最小同步模型：每次 handle_next 只收一个包，合法 Ping 就回 Pong。
// 这个版本刻意不做线程、连接状态和重传，方便先把协议包格式学清楚。
class PingServer {
public:
    explicit PingServer(std::uint16_t port);

    std::uint16_t port() const;

    ServerStepResult handle_next(std::chrono::milliseconds timeout);

private:
    UdpSocket socket_;
};

class PingClient {
public:
    PingClient();

    std::uint32_t next_sequence() const;

    PingResult ping(const UdpEndpoint& server, std::chrono::milliseconds timeout);

private:
    UdpSocket socket_;

    // 客户端每发一个 Ping 就递增一次。现在只用于匹配 Pong，后续会扩展到 ACK。
    std::uint32_t next_sequence_ = 1;
};

std::vector<std::uint8_t> make_ping_packet(std::uint32_t sequence);

std::vector<std::uint8_t> make_pong_packet(std::uint32_t sequence);

} // namespace mininet
