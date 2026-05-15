#pragma once

#include "mininet/ack_tracker.hpp"
#include "mininet/packet.hpp"
#include "mininet/reliable_message.hpp"
#include "mininet/udp_socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mininet {

// UDP 本身没有连接状态，这个枚举表示 MiniNet 在应用层维护出来的“虚拟连接”状态。
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    TimedOut,
};

// 连接层的时间配置。使用 steady_clock duration，便于测试传入虚拟 now 精确推进状态机。
struct ConnectionConfig {
    // 连接建立后，如果超过这个间隔没有发送任何连接内包，就主动发送 Heartbeat。
    std::chrono::milliseconds heartbeat_interval{1000};

    // 连接建立后，如果超过这个时间没有收到合法连接内包，就认为对端超时。
    std::chrono::milliseconds timeout{5000};
};

// 服务端收到的一条可靠无序消息。服务端可能同时维护多个 session，所以要带上来源 session_id。
struct ReceivedReliableMessage {
    // 发送这条消息的客户端 session id，用于上层区分来源。
    std::uint32_t session_id = 0;

    // 上层应用传入的原始 payload 字节；连接层不解释内容。
    std::vector<std::uint8_t> payload;
};

// 服务端保存的单个客户端会话。endpoint 和 session_id 共同标识一条逻辑连接。
struct ServerSession {
    // 客户端 UDP 地址和端口；重复 ConnectRequest 会按 endpoint 复用已有 session。
    UdpEndpoint endpoint;

    // 服务端分配的非 0 会话 id；客户端后续连接内包必须携带它。
    std::uint32_t session_id = 0;

    // 最近一次收到该会话合法连接内包的时间，用于 timeout 判断。
    std::chrono::steady_clock::time_point last_recv_time{};

    // 最近一次向该会话发包的时间，用于心跳节流。
    std::chrono::steady_clock::time_point last_send_time{};

    // 该会话独立的 sequence/ACK 状态；不同客户端之间不能共享确认窗口。
    AckTracker ack_tracker;

    // 该会话独立的可靠消息发送队列；等待 packet ACK 后清理。
    ReliableSender reliable_sender;

    // 该会话独立的可靠消息接收去重器；避免重传包导致上层重复处理。
    ReliableReceiver reliable_receiver;
};

// 客户端 update 的结果，用于测试和日志观察本轮状态机做了什么。
struct ConnectionClientUpdateResult {
    // true 表示本轮 update 收到了一个 UDP datagram。
    bool received = false;

    // true 表示本轮 update 发送了 ConnectRequest、Heartbeat、Disconnect 或 ReliableData。
    bool sent = false;

    // true 表示连接状态在本轮发生变化，例如 Connecting -> Connected。
    bool state_changed = false;

    // 如果收到包但被拒绝，这里保存拒绝原因。
    PacketRejectReason reason = PacketRejectReason::None;

    // 如果包头可解析，这里保存本轮收到的 header。
    std::optional<PacketHeader> header;

    // 本轮第一次处理的可靠无序消息 payload；重复 message_id 会被去重，不会放进这里。
    std::vector<std::vector<std::uint8_t>> received_reliable_messages;
};

// 服务端 update 的结果，用于测试和日志观察本轮状态机做了什么。
struct ConnectionServerUpdateResult {
    // true 表示本轮 update 收到了一个 UDP datagram。
    bool received = false;

    // true 表示本轮 update 至少发送了一个响应、心跳或 ReliableData。
    bool sent = false;

    // true 表示本轮接收了一个新的 ConnectRequest 并创建 session。
    bool accepted_connection = false;

    // true 表示本轮收到匹配 endpoint 和 session_id 的 Disconnect 并移除 session。
    bool disconnected_session = false;

    // 如果收到包但被拒绝，这里保存拒绝原因。
    PacketRejectReason reason = PacketRejectReason::None;

    // 如果包头可解析，这里保存本轮收到的 header。
    std::optional<PacketHeader> header;

    // 本轮因超时被移除的 session_id 列表。
    std::vector<std::uint32_t> timed_out_sessions;

    // 本轮第一次处理的可靠无序消息；服务端额外记录来源 session_id。
    std::vector<ReceivedReliableMessage> received_reliable_messages;
};

// 构造只有 header 的连接控制包。握手、心跳、断开都走这条路径；ReliableData 使用独立构造函数。
std::vector<std::uint8_t> make_connection_packet(PacketType type,
                                                 std::uint32_t sequence,
                                                 std::uint32_t session_id,
                                                 std::uint32_t ack = 0,
                                                 std::uint32_t ack_bits = 0);

// UDP 虚拟连接客户端。调用方显式调用 connect/update/disconnect 推进状态机。
class ConnectionClient {
public:
    // 创建客户端 UDP socket，并记录要连接的 server endpoint。
    explicit ConnectionClient(const UdpEndpoint& server, ConnectionConfig config = {});

    // 返回客户端当前逻辑连接状态。
    ConnectionState state() const;

    // 返回当前会话 id；未连接或尚未收到 ConnectAccept 时为 0。
    std::uint32_t session_id() const;

    // 返回客户端本地 UDP 端口，便于测试或日志观察。
    std::uint16_t local_port() const;

    // 返回客户端已发送连接内包快照，便于测试和调试观察 ACK 状态。
    const std::vector<SentPacketRecord>& sent_packets() const;

    // 返回仍在等待 packet ACK 的可靠消息数量，主要用于测试确认 pending 是否被清理。
    std::size_t reliable_pending_count() const;

    // 发送 ConnectRequest 并进入 Connecting；ConnectRequest 的 session_id 必须为 0。
    ConnectionClientUpdateResult connect(std::chrono::steady_clock::time_point now);

    // 推进客户端状态机：最多收一个包、处理可靠消息/心跳、发送到期数据、检查 timeout。
    ConnectionClientUpdateResult update(std::chrono::steady_clock::time_point now,
                                        std::chrono::milliseconds receive_timeout = std::chrono::milliseconds(0));

    // 如果当前已连接，发送 Disconnect 并回到 Disconnected。
    ConnectionClientUpdateResult disconnect(std::chrono::steady_clock::time_point now);

    // 在已连接状态下排队一条可靠无序消息；真正发包发生在后续 update 中。
    bool send_reliable(const std::vector<std::uint8_t>& payload, std::chrono::steady_clock::time_point now);

private:
    // 客户端 UDP socket，用于发送控制包、可靠消息包并接收服务端响应。
    UdpSocket socket_;

    // 服务端 endpoint；客户端只接受来自这个 endpoint 的连接包。
    UdpEndpoint server_;

    // 心跳和 timeout 配置。
    ConnectionConfig config_;

    // 当前逻辑连接状态。
    ConnectionState state_ = ConnectionState::Disconnected;

    // 服务端分配的 session id；未连接时为 0。
    std::uint32_t session_id_ = 0;

    // 客户端连接内 sequence/ACK 状态；ConnectRequest 不纳入可靠确认。
    AckTracker ack_tracker_;

    // 最近一次收到合法连接内包的时间，用于 timeout。
    std::chrono::steady_clock::time_point last_recv_time_{};

    // 最近一次向服务端发包的时间，用于心跳节流。
    std::chrono::steady_clock::time_point last_send_time_{};

    // 客户端可靠消息发送队列；消息会反复装入新 packet，直到某个携带它的 packet 被 ACK。
    ReliableSender reliable_sender_;

    // 客户端可靠消息接收去重器；避免服务端重传导致同一 message_id 重复交给上层。
    ReliableReceiver reliable_receiver_;
};

// UDP 虚拟连接服务端。调用方显式调用 update 推进会话维护。
class ConnectionServer {
public:
    // 创建并绑定服务端 UDP socket。port=0 时由系统分配空闲端口。
    explicit ConnectionServer(std::uint16_t port, ConnectionConfig config = {});

    // 返回服务端实际监听端口。
    std::uint16_t port() const;

    // 返回当前活跃 session 数量。
    std::size_t session_count() const;

    // 返回当前 session 快照，便于测试检查服务端状态。
    const std::vector<ServerSession>& sessions() const;

    // 按 endpoint 查找 session；重复 ConnectRequest 需要复用已有 session_id。
    const ServerSession* find_session(const UdpEndpoint& endpoint) const;

    // 推进服务端状态机：最多收一个包、处理握手/断开/可靠消息、发送到期数据、清理 timeout。
    ConnectionServerUpdateResult update(std::chrono::steady_clock::time_point now,
                                        std::chrono::milliseconds receive_timeout = std::chrono::milliseconds(0));

    // 给指定 session 排队一条可靠无序消息；真正发包发生在后续 update 中。
    bool send_reliable(std::uint32_t session_id,
                       const std::vector<std::uint8_t>& payload,
                       std::chrono::steady_clock::time_point now);

private:
    // 服务端 UDP socket，绑定到指定端口。
    UdpSocket socket_;

    // 心跳和 timeout 配置。
    ConnectionConfig config_;

    // 活跃 session 列表；学习项目先用 vector，避免引入更复杂的索引结构。
    std::vector<ServerSession> sessions_;

    // 下一个可分配 session id。0 保留给未连接/ConnectRequest。
    std::uint32_t next_session_id_ = 1;

    // 服务端握手包发包序号；连接内包使用各 session 自己的 AckTracker。
    std::uint32_t next_sequence_ = 1;
};

} // namespace mininet
