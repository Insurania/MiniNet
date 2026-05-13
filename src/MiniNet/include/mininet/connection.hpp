#pragma once

#include "mininet/ack_tracker.hpp"
#include "mininet/packet.hpp"
#include "mininet/udp_socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mininet {

// 逻辑连接状态。UDP 本身没有连接，这里表达 MiniNet 应用层维护的状态。
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    TimedOut,
};

// 连接层可配置时间。单位使用 steady_clock duration，便于测试传入虚拟 now。
struct ConnectionConfig {
    // 心跳发送间隔；连接建立后超过这个时间没有发包，就发送 Heartbeat。
    std::chrono::milliseconds heartbeat_interval{1000};

    // 接收超时时间；超过这个时间没有收到合法连接内包，就认为连接断开。
    std::chrono::milliseconds timeout{5000};
};

// 服务端保存的单个客户端会话。endpoint 和 session_id 共同标识一条逻辑连接。
struct ServerSession {
    // 客户端 UDP 地址和端口；重复 ConnectRequest 按它去重。
    UdpEndpoint endpoint;

    // 服务端分配的非 0 会话 id，客户端后续包必须携带它。
    std::uint32_t session_id = 0;

    // 最近一次收到该会话合法包的时间，用于 timeout 判断。
    std::chrono::steady_clock::time_point last_recv_time{};

    // 最近一次向该会话发包的时间，用于 heartbeat 节流。
    std::chrono::steady_clock::time_point last_send_time{};

    // 该会话独立的 sequence/ACK 状态；不同客户端之间不能共享确认窗口。
    AckTracker ack_tracker;
};

// 客户端 update 的结果，用于测试和日志观察本轮状态机做了什么。
struct ConnectionClientUpdateResult {
    // true 表示本轮 update 收到了一个 UDP datagram。
    bool received = false;

    // true 表示本轮 update 发送了 ConnectRequest、Heartbeat 或 Disconnect。
    bool sent = false;

    // true 表示连接状态在本轮发生变化。
    bool state_changed = false;

    // 如果收到包但被拒绝，这里保存拒绝原因。
    PacketRejectReason reason = PacketRejectReason::None;

    // 如果包头可解析，这里保存本轮收到的 header。
    std::optional<PacketHeader> header;
};

// 服务端 update 的结果，用于测试和日志观察本轮状态机做了什么。
struct ConnectionServerUpdateResult {
    // true 表示本轮 update 收到了一个 UDP datagram。
    bool received = false;

    // true 表示本轮 update 至少发送了一个响应或 Heartbeat。
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
};

// 构造只有 header 的连接控制包。连接层当前不定义 payload。
std::vector<std::uint8_t> make_connection_packet(PacketType type,
                                                 std::uint32_t sequence,
                                                 std::uint32_t session_id,
                                                 std::uint32_t ack = 0,
                                                 std::uint32_t ack_bits = 0);

// UDP 虚拟连接客户端。调用方显式调用 connect/update/disconnect 推进状态机。
class ConnectionClient {
public:
    // 创建客户端 socket，并记录要连接的 server endpoint。
    explicit ConnectionClient(const UdpEndpoint& server, ConnectionConfig config = {});

    // 返回客户端当前逻辑连接状态。
    ConnectionState state() const;

    // 返回当前会话 id；未连接或尚未接受时为 0。
    std::uint32_t session_id() const;

    // 返回客户端本地 UDP 端口，便于测试或日志观察。
    std::uint16_t local_port() const;

    // 返回客户端已发送连接内包快照，便于测试和调试观察 ACK 状态。
    const std::vector<SentPacketRecord>& sent_packets() const;

    // 发送 ConnectRequest 并进入 Connecting；ConnectRequest 的 session_id 必须为 0。
    ConnectionClientUpdateResult connect(std::chrono::steady_clock::time_point now);

    // 推进客户端状态机：收一个包、处理握手/心跳、检查 timeout。
    ConnectionClientUpdateResult update(std::chrono::steady_clock::time_point now,
                                        std::chrono::milliseconds receive_timeout = std::chrono::milliseconds(0));

    // 如果当前已连接，发送 Disconnect 并回到 Disconnected。
    ConnectionClientUpdateResult disconnect(std::chrono::steady_clock::time_point now);

private:
    // 客户端 UDP socket，用于发送控制包和接收服务端响应。
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

    // 最近一次向服务端发包的时间，用于 heartbeat 节流。
    std::chrono::steady_clock::time_point last_send_time_{};
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

    // 推进服务端状态机：收一个包、处理握手/断开、发送心跳、清理 timeout。
    ConnectionServerUpdateResult update(std::chrono::steady_clock::time_point now,
                                        std::chrono::milliseconds receive_timeout = std::chrono::milliseconds(0));

private:
    // 服务端 UDP socket，绑定到 127.0.0.1 的指定端口。
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
