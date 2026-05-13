#include "mininet/connection.hpp"

#include <algorithm>

namespace mininet {

namespace {

bool same_endpoint(const UdpEndpoint& left, const UdpEndpoint& right)
{
    return left.address == right.address && left.port == right.port;
}

std::vector<ServerSession>::iterator find_session_by_endpoint(std::vector<ServerSession>& sessions,
                                                              const UdpEndpoint& endpoint)
{
    return std::find_if(sessions.begin(), sessions.end(), [&](const ServerSession& session) {
        return same_endpoint(session.endpoint, endpoint);
    });
}

std::vector<ServerSession>::iterator find_session_by_endpoint_and_id(std::vector<ServerSession>& sessions,
                                                                     const UdpEndpoint& endpoint,
                                                                     std::uint32_t session_id)
{
    return std::find_if(sessions.begin(), sessions.end(), [&](const ServerSession& session) {
        return session.session_id == session_id && same_endpoint(session.endpoint, endpoint);
    });
}

bool is_session_control_packet(PacketType type)
{
    return type == PacketType::Disconnect ||
           type == PacketType::Heartbeat;
}

PacketValidationResult validate_any_packet(ByteView bytes)
{
    // connection 层需要先知道包的真实 type，再决定走握手、断开或心跳分支。
    // 这里使用一个已知类型做基础校验入口；如果 type 不匹配，仍可从 header 读取真实 type。
    auto validation = validate_packet_type(bytes, PacketType::Heartbeat);
    if (validation.accepted || validation.reason != PacketRejectReason::UnexpectedType) {
        return validation;
    }

    validation.accepted = true;
    validation.reason = PacketRejectReason::None;
    return validation;
}

} // namespace

std::vector<std::uint8_t> make_connection_packet(PacketType type, std::uint32_t sequence, std::uint32_t session_id)
{
    // 连接控制包当前只有 header，没有 payload。
    // 这样握手、心跳、断开都复用同一条编码路径，减少协议格式分叉。
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = type;
    header.sequence = sequence;
    header.session_id = session_id;

    const auto encoded = encode_packet_header(header);
    return {encoded.begin(), encoded.end()};
}

ConnectionClient::ConnectionClient(const UdpEndpoint& server, ConnectionConfig config)
    : socket_(UdpSocket::open())
    , server_(server)
    , config_(config)
{
}

ConnectionState ConnectionClient::state() const
{
    return state_;
}

std::uint32_t ConnectionClient::session_id() const
{
    return session_id_;
}

std::uint16_t ConnectionClient::local_port() const
{
    return socket_.local_port();
}

ConnectionClientUpdateResult ConnectionClient::connect(std::chrono::steady_clock::time_point now)
{
    // connect 只发起一次握手请求，不阻塞等待接受包。
    // 调用方继续通过 update 驱动收包，这样测试可以精确控制时间推进。
    ConnectionClientUpdateResult result;
    if (state_ == ConnectionState::Connected || state_ == ConnectionState::Connecting) {
        return result;
    }

    const auto request = make_connection_packet(PacketType::ConnectRequest, next_sequence_++, 0);
    socket_.send_to(request, server_);

    state_ = ConnectionState::Connecting;
    session_id_ = 0;
    last_recv_time_ = now;
    last_send_time_ = now;

    result.sent = true;
    result.state_changed = true;
    return result;
}

ConnectionClientUpdateResult ConnectionClient::update(std::chrono::steady_clock::time_point now,
                                                      std::chrono::milliseconds receive_timeout)
{
    // 每次 update 最多收一个 datagram，避免隐藏的循环阻塞调用方。
    // 收到合法 ConnectAccept 后进入 Connected；连接内任意合法包都会刷新 last_recv_time。
    ConnectionClientUpdateResult result;
    const auto datagram = socket_.receive_from(receive_timeout);
    if (datagram.has_value()) {
        result.received = true;

        if (!same_endpoint(datagram->sender, server_)) {
            result.reason = PacketRejectReason::UnexpectedType;
        } else {
            auto validation = validate_any_packet(datagram->bytes);
            result.reason = validation.reason;
            result.header = validation.header;
            if (validation.accepted) {
                const auto& header = *validation.header;
                if (state_ == ConnectionState::Connecting && header.type == PacketType::ConnectAccept) {
                    validation = validate_connect_accept_packet(datagram->bytes);
                    result.reason = validation.reason;
                    if (validation.accepted) {
                        session_id_ = header.session_id;
                        state_ = ConnectionState::Connected;
                        last_recv_time_ = now;
                        result.state_changed = true;
                    }
                } else if (state_ == ConnectionState::Connected && is_session_control_packet(header.type) &&
                           header.session_id == session_id_) {
                    last_recv_time_ = now;

                    if (header.type == PacketType::Disconnect) {
                        state_ = ConnectionState::Disconnected;
                        session_id_ = 0;
                        result.state_changed = true;
                    }
                }
            }
        }
    }

    if (state_ == ConnectionState::Connecting && now - last_recv_time_ > config_.timeout) {
        state_ = ConnectionState::TimedOut;
        session_id_ = 0;
        result.state_changed = true;
        return result;
    }

    if (state_ == ConnectionState::Connected && now - last_recv_time_ > config_.timeout) {
        state_ = ConnectionState::TimedOut;
        session_id_ = 0;
        result.state_changed = true;
        return result;
    }

    if (state_ == ConnectionState::Connected && now - last_send_time_ >= config_.heartbeat_interval) {
        const auto heartbeat = make_connection_packet(PacketType::Heartbeat, next_sequence_++, session_id_);
        socket_.send_to(heartbeat, server_);
        last_send_time_ = now;
        result.sent = true;
    }

    return result;
}

ConnectionClientUpdateResult ConnectionClient::disconnect(std::chrono::steady_clock::time_point now)
{
    // Disconnect 是显式的礼貌断开；如果包丢了，服务端仍可依靠 timeout 清理 session。
    ConnectionClientUpdateResult result;
    if (state_ != ConnectionState::Connected) {
        return result;
    }

    const auto packet = make_connection_packet(PacketType::Disconnect, next_sequence_++, session_id_);
    socket_.send_to(packet, server_);

    state_ = ConnectionState::Disconnected;
    session_id_ = 0;
    last_send_time_ = now;

    result.sent = true;
    result.state_changed = true;
    return result;
}

ConnectionServer::ConnectionServer(std::uint16_t port, ConnectionConfig config)
    : socket_(UdpSocket::bind(port))
    , config_(config)
{
}

std::uint16_t ConnectionServer::port() const
{
    return socket_.local_port();
}

std::size_t ConnectionServer::session_count() const
{
    return sessions_.size();
}

const std::vector<ServerSession>& ConnectionServer::sessions() const
{
    return sessions_;
}

const ServerSession* ConnectionServer::find_session(const UdpEndpoint& endpoint) const
{
    const auto found = std::find_if(sessions_.begin(), sessions_.end(), [&](const ServerSession& session) {
        return same_endpoint(session.endpoint, endpoint);
    });
    if (found == sessions_.end()) {
        return nullptr;
    }
    return &*found;
}

ConnectionServerUpdateResult ConnectionServer::update(std::chrono::steady_clock::time_point now,
                                                      std::chrono::milliseconds receive_timeout)
{
    // 服务端 update 分三步：先处理一个入站包，再发送需要的心跳，最后清理超时 session。
    // 重复 ConnectRequest 只复用已有 session_id 回 ConnectAccept，避免同 endpoint 产生多条连接。
    ConnectionServerUpdateResult result;
    const auto datagram = socket_.receive_from(receive_timeout);
    if (datagram.has_value()) {
        result.received = true;

        auto validation = validate_any_packet(datagram->bytes);
        result.reason = validation.reason;
        result.header = validation.header;
        if (validation.accepted) {
            const auto& header = *validation.header;
            if (header.type == PacketType::ConnectRequest) {
                validation = validate_connect_request_packet(datagram->bytes);
                result.reason = validation.reason;
                if (validation.accepted) {
                    auto found = find_session_by_endpoint(sessions_, datagram->sender);
                    if (found == sessions_.end()) {
                        ServerSession session;
                        session.endpoint = datagram->sender;
                        session.session_id = next_session_id_++;
                        session.last_recv_time = now;
                        session.last_send_time = now;
                        sessions_.push_back(session);
                        found = sessions_.end() - 1;
                        result.accepted_connection = true;
                    } else {
                        found->last_recv_time = now;
                    }

                    const auto accept = make_connection_packet(PacketType::ConnectAccept, next_sequence_++, found->session_id);
                    socket_.send_to(accept, datagram->sender);
                    found->last_send_time = now;
                    result.sent = true;
                }
            } else if (is_session_control_packet(header.type)) {
                auto found = find_session_by_endpoint_and_id(sessions_, datagram->sender, header.session_id);
                if (found != sessions_.end()) {
                    found->last_recv_time = now;

                    if (header.type == PacketType::Disconnect) {
                        sessions_.erase(found);
                        result.disconnected_session = true;
                    } else if (header.type == PacketType::Heartbeat) {
                        const auto heartbeat = make_connection_packet(PacketType::Heartbeat, next_sequence_++, found->session_id);
                        socket_.send_to(heartbeat, datagram->sender);
                        found->last_send_time = now;
                        result.sent = true;
                    }
                }
            }
        }
    }

    auto write = sessions_.begin();
    for (auto read = sessions_.begin(); read != sessions_.end(); ++read) {
        if (now - read->last_recv_time > config_.timeout) {
            result.timed_out_sessions.push_back(read->session_id);
            continue;
        }

        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    sessions_.erase(write, sessions_.end());

    for (auto& session : sessions_) {
        if (now - session.last_send_time >= config_.heartbeat_interval) {
            const auto heartbeat = make_connection_packet(PacketType::Heartbeat, next_sequence_++, session.session_id);
            socket_.send_to(heartbeat, session.endpoint);
            session.last_send_time = now;
            result.sent = true;
        }
    }

    return result;
}

} // namespace mininet
