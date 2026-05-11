#include "mininet/ping.hpp"

#include <chrono>
#include <vector>

namespace mininet {

namespace {

std::vector<std::uint8_t> make_packet(PacketType type, std::uint32_t sequence)
{
    // Ping 和 Pong 目前只有 header，没有 payload。
    // 后续可靠消息或状态同步会在 header 后面追加 payload。
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = type;
    header.sequence = sequence;

    const auto encoded = encode_packet_header(header);

    return {encoded.begin(), encoded.end()};
}

} // namespace

PingServer::PingServer(std::uint16_t port)
    : socket_(UdpSocket::bind(port))
{
}

std::uint16_t PingServer::port() const
{
    return socket_.local_port();
}

ServerStepResult PingServer::handle_next(std::chrono::milliseconds timeout)
{
    // 同步模型：一次只处理一个 datagram。
    // 这让测试更确定，也方便观察“收到 -> 校验 -> 回复/丢弃”的完整流程。
    const auto datagram = socket_.receive_from(timeout);
    if (!datagram.has_value()) {
        return {};
    }

    const auto validation = validate_ping_packet(datagram->bytes);
    if (!validation.accepted) {
        // 非法包只记录原因，不回复。真实游戏网络里也常见这种防御性处理。
        std::optional<std::uint32_t> sequence;
        if (validation.header.has_value()) {
            sequence = validation.header->sequence;
        }

        ServerStepResult result;
        result.received = true;
        result.responded = false;
        result.reason = validation.reason;
        result.sequence = sequence;
        return result;
    }

    const auto sequence = validation.header->sequence;
    // Pong 保留 Ping 的 sequence，客户端用它确认这次回复对应哪次请求。
    const auto pong = make_pong_packet(sequence);
    socket_.send_to(pong, datagram->sender);

    ServerStepResult result;
    result.received = true;
    result.responded = true;
    result.reason = PacketRejectReason::None;
    result.sequence = sequence;
    return result;
}

PingClient::PingClient()
    : socket_(UdpSocket::open())
{
}

std::uint32_t PingClient::next_sequence() const
{
    return next_sequence_;
}

PingResult PingClient::ping(const UdpEndpoint& server, std::chrono::milliseconds timeout)
{
    // 先取出本次 sequence，再递增，为下一次 Ping 做准备。
    const auto sequence = next_sequence_++;
    const auto ping = make_ping_packet(sequence);

    const auto start = std::chrono::steady_clock::now();
    socket_.send_to(ping, server);

    const auto datagram = socket_.receive_from(timeout);
    if (!datagram.has_value()) {
        return {};
    }

    const auto validation = validate_pong_packet(datagram->bytes);
    if (!validation.accepted || validation.header->sequence != sequence) {
        // 收到格式错误或 sequence 不匹配的 Pong，都视为本次 Ping 没成功。
        return {};
    }

    const auto end = std::chrono::steady_clock::now();
    PingResult result;
    result.received = true;
    result.sequence = sequence;
    result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    return result;
}

std::vector<std::uint8_t> make_ping_packet(std::uint32_t sequence)
{
    return make_packet(PacketType::Ping, sequence);
}

std::vector<std::uint8_t> make_pong_packet(std::uint32_t sequence)
{
    return make_packet(PacketType::Pong, sequence);
}

} // namespace mininet
