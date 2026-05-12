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
    // true 表示这次 handle_next 收到了一个 UDP datagram。
    bool received = false;

    // true 表示服务端已经发送 Pong；非法包会 received=true 但 responded=false。
    bool responded = false;

    // 非法包被拒绝时的原因，方便测试和日志确认行为。
    PacketRejectReason reason = PacketRejectReason::None;

    // 如果包头能解析出来，这里保存收到的 sequence。
    std::optional<std::uint32_t> sequence;
};

// 客户端执行一次 Ping 后的结果。
struct PingResult {
    // true 表示收到合法且 sequence 匹配的 Pong。
    bool received = false;

    // 本次 Ping 使用的 sequence。
    std::uint32_t sequence = 0;

    // 从发送 Ping 到收到匹配 Pong 的耗时。
    std::chrono::milliseconds rtt{0};
};

// 当前 server 是最小同步模型：每次 handle_next 只收一个包，合法 Ping 就回 Pong。
// 这个版本刻意不做线程、连接状态和重传，方便先把协议包格式学清楚。
class PingServer {
public:
    // 创建并绑定 UDP server。port=0 时由操作系统自动分配端口，测试常用。
    explicit PingServer(std::uint16_t port);

    // 返回 server 实际绑定端口。port=0 自动分配时，调用者需要用它获取真实端口。
    std::uint16_t port() const;

    // 阻塞等待并处理一个 UDP datagram，直到收到包或 timeout 到期。
    // 合法 Ping 会回复 Pong；非法包会丢弃并返回拒绝原因。
    ServerStepResult handle_next(std::chrono::milliseconds timeout);

private:
    // 服务端 UDP socket，绑定到 127.0.0.1 的指定端口。
    UdpSocket socket_;
};

// 最小 Ping 客户端。每次 ping 发送一个 Ping 包，然后等待同 sequence 的 Pong。
class PingClient {
public:
    // 创建一个未绑定固定端口的 UDP socket，发送时由系统选择本地临时端口。
    PingClient();

    // 返回下一次 Ping 将使用的 sequence，主要给测试或调试查看。
    std::uint32_t next_sequence() const;

    // 发送一次 Ping，并在 timeout 内等待匹配 Pong。
    // 返回 received=false 表示超时、收到非法 Pong，或 Pong 的 sequence 不匹配。
    PingResult ping(const UdpEndpoint& server, std::chrono::milliseconds timeout);

private:
    // 客户端 UDP socket，用于发送 Ping 和接收 Pong。
    UdpSocket socket_;

    // 客户端每发一个 Ping 就递增一次。现在只用于匹配 Pong，后续会扩展到 ACK。
    std::uint32_t next_sequence_ = 1;
};

// 构造 Ping 包字节。当前没有 payload，只有 PacketHeader。
std::vector<std::uint8_t> make_ping_packet(std::uint32_t sequence);

// 构造 Pong 包字节。Pong 使用和 Ping 相同的 sequence。
std::vector<std::uint8_t> make_pong_packet(std::uint32_t sequence);

} // namespace mininet
