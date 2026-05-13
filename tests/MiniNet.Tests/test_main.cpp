#include "mininet/connection.hpp"
#include "mininet/packet.hpp"
#include "mininet/ping.hpp"
#include "mininet/udp_socket.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace mininet;
using Clock = std::chrono::steady_clock;

int failures = 0;

void require(bool condition, const std::string& message)
{
    // 简单测试框架：记录所有失败点，便于一次看到多个验收项。
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

std::vector<std::uint8_t> make_packet(PacketType type, std::uint32_t sequence, std::uint32_t session_id)
{
    return make_connection_packet(type, sequence, session_id);
}

PacketHeader received_header(const UdpSocket& socket, const std::string& message)
{
    const auto datagram = socket.receive_from(std::chrono::milliseconds(200));
    require(datagram.has_value(), message + " received");

    const auto decoded = datagram.has_value() ? decode_packet_header(datagram->bytes) : std::nullopt;
    require(decoded.has_value(), message + " decodes");
    return decoded.value_or(PacketHeader{});
}

void send_from_server(UdpSocket& server, const UdpEndpoint& client, PacketType type, std::uint32_t session_id)
{
    const auto packet = make_packet(type, 100, session_id);
    server.send_to(packet, client);
}

void test_packet_header_round_trip()
{
    // header 必须固定为 14 字节，并携带 session_id，避免连接层协议退化。
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = PacketType::Heartbeat;
    header.sequence = 42;
    header.session_id = 0xAABBCCDD;

    const auto encoded = encode_packet_header(header);
    const auto decoded = decode_packet_header(encoded);

    require(kPacketHeaderSize == 14, "PacketHeader size is 14 bytes");
    require(encoded.size() == 14, "encoded PacketHeader size is 14 bytes");
    require(decoded.has_value(), "PacketHeader decodes");
    require(decoded->magic == kPacketMagic, "magic round-trips");
    require(decoded->version == kProtocolVersion, "version round-trips");
    require(decoded->type == PacketType::Heartbeat, "type round-trips");
    require(decoded->sequence == 42, "sequence round-trips");
    require(decoded->session_id == 0xAABBCCDD, "session_id round-trips");
}

void test_connection_packet_validation()
{
    // ConnectRequest 只能使用 session_id=0，ConnectAccept 必须携带非 0 session_id。
    require(validate_connect_request_packet(make_packet(PacketType::ConnectRequest, 1, 0)).accepted,
            "ConnectRequest session_id=0 is accepted");
    require(validate_connect_request_packet(make_packet(PacketType::ConnectRequest, 1, 7)).reason ==
                PacketRejectReason::BadSessionId,
            "ConnectRequest session_id!=0 is rejected");
    require(validate_connect_accept_packet(make_packet(PacketType::ConnectAccept, 1, 7)).accepted,
            "ConnectAccept session_id!=0 is accepted");
    require(validate_connect_accept_packet(make_packet(PacketType::ConnectAccept, 1, 0)).reason ==
                PacketRejectReason::BadSessionId,
            "ConnectAccept session_id=0 is rejected");
}

void test_basic_packet_validation_rejections()
{
    // 保留基础协议校验回归：非法 header 不应进入上层业务逻辑。
    auto valid = make_ping_packet(1);
    require(validate_ping_packet(valid).accepted, "valid Ping is accepted");

    auto bad_magic = valid;
    bad_magic[0] = 0;
    require(validate_ping_packet(bad_magic).reason == PacketRejectReason::BadMagic, "bad magic is rejected");

    auto bad_version = valid;
    bad_version[4] = 99;
    require(validate_ping_packet(bad_version).reason == PacketRejectReason::BadVersion, "bad version is rejected");

    auto unknown_type = valid;
    unknown_type[5] = 99;
    require(validate_ping_packet(unknown_type).reason == PacketRejectReason::UnknownType, "unknown type is rejected");

    std::vector<std::uint8_t> too_short(kPacketHeaderSize - 1, 0);
    require(validate_ping_packet(too_short).reason == PacketRejectReason::TooShort, "short packet is rejected");
}

void test_client_connect_accept_and_timeout()
{
    const auto t0 = Clock::now();
    UdpSocket fake_server = UdpSocket::bind(0);
    const UdpEndpoint server_endpoint{"127.0.0.1", fake_server.local_port()};
    ConnectionClient client(server_endpoint);

    require(client.state() == ConnectionState::Disconnected, "client starts Disconnected");

    const auto connect_result = client.connect(t0);
    require(connect_result.sent, "client sends ConnectRequest");
    require(client.state() == ConnectionState::Connecting, "client enters Connecting after connect");

    const auto request = fake_server.receive_from(std::chrono::milliseconds(200));
    require(request.has_value(), "fake server receives ConnectRequest");
    require(validate_connect_request_packet(request->bytes).accepted, "client ConnectRequest is valid");

    // 错误 type 不能让 Connecting 客户端进入 Connected。
    send_from_server(fake_server, request->sender, PacketType::Heartbeat, 77);
    client.update(t0 + std::chrono::milliseconds(100));
    require(client.state() == ConnectionState::Connecting, "wrong type does not connect client");

    // session_id=0 的 ConnectAccept 非法，客户端不能接受。
    send_from_server(fake_server, request->sender, PacketType::ConnectAccept, 0);
    client.update(t0 + std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connecting, "bad ConnectAccept session_id does not connect client");

    send_from_server(fake_server, request->sender, PacketType::ConnectAccept, 77);
    const auto accept_result = client.update(t0 + std::chrono::milliseconds(300));
    require(accept_result.state_changed, "valid ConnectAccept changes client state");
    require(client.state() == ConnectionState::Connected, "valid ConnectAccept connects client");
    require(client.session_id() == 77, "client stores accepted session_id");

    // 错误 session_id 的 Heartbeat 不能刷新 last_recv_time，5 秒后应进入 TimedOut。
    send_from_server(fake_server, request->sender, PacketType::Heartbeat, 78);
    client.update(t0 + std::chrono::milliseconds(4300));
    require(client.state() == ConnectionState::Connected, "wrong session_id does not immediately disconnect client");

    client.update(t0 + std::chrono::milliseconds(5301));
    require(client.state() == ConnectionState::TimedOut, "client becomes TimedOut after 5 seconds without valid packet");
    require(client.session_id() == 0, "client clears session_id after timeout");
}

void test_client_heartbeat_interval()
{
    const auto t0 = Clock::now();
    UdpSocket fake_server = UdpSocket::bind(0);
    const UdpEndpoint server_endpoint{"127.0.0.1", fake_server.local_port()};
    ConnectionClient client(server_endpoint);

    client.connect(t0);
    const auto request = fake_server.receive_from(std::chrono::milliseconds(200));
    send_from_server(fake_server, request->sender, PacketType::ConnectAccept, 9);
    client.update(t0);

    client.update(t0 + std::chrono::milliseconds(999));
    require(!fake_server.receive_from(std::chrono::milliseconds(50)).has_value(),
            "client does not send Heartbeat before 1 second");

    client.update(t0 + std::chrono::milliseconds(1000));
    const auto heartbeat = received_header(fake_server, "client Heartbeat after 1 second");
    require(heartbeat.type == PacketType::Heartbeat, "client sends Heartbeat while Connected");
    require(heartbeat.session_id == 9, "client Heartbeat carries session_id");

    ConnectionClient disconnected_client(server_endpoint);
    disconnected_client.update(t0 + std::chrono::milliseconds(2000));
    require(!fake_server.receive_from(std::chrono::milliseconds(50)).has_value(),
            "Disconnected client does not send Heartbeat");
}

void test_server_connect_request_and_duplicate()
{
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    UdpSocket client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    client.send_to(make_packet(PacketType::ConnectRequest, 1, 0), server_endpoint);
    const auto first_result = server.update(t0, std::chrono::milliseconds(200));
    require(first_result.accepted_connection, "server creates session for valid ConnectRequest");
    require(first_result.sent, "server replies to ConnectRequest");
    require(server.session_count() == 1, "server has one session after first ConnectRequest");

    const auto first_accept = received_header(client, "first ConnectAccept");
    require(first_accept.type == PacketType::ConnectAccept, "server replies with ConnectAccept");
    require(first_accept.session_id != 0, "server ConnectAccept has non-zero session_id");

    client.send_to(make_packet(PacketType::ConnectRequest, 2, 0), server_endpoint);
    const auto duplicate_result = server.update(t0 + std::chrono::milliseconds(100), std::chrono::milliseconds(200));
    require(!duplicate_result.accepted_connection, "duplicate ConnectRequest does not create new session");
    require(server.session_count() == 1, "server still has one session after duplicate ConnectRequest");

    const auto duplicate_accept = received_header(client, "duplicate ConnectAccept");
    require(duplicate_accept.session_id == first_accept.session_id,
            "duplicate ConnectRequest returns the same session_id");
}

void test_server_heartbeat_and_disconnect_rules()
{
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    UdpSocket client = UdpSocket::open();
    UdpSocket unknown_client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    client.send_to(make_packet(PacketType::ConnectRequest, 1, 0), server_endpoint);
    server.update(t0, std::chrono::milliseconds(200));
    const auto accept = received_header(client, "ConnectAccept for heartbeat test");
    const auto session_id = accept.session_id;
    require(server.session_count() == 1, "server has session before heartbeat checks");

    // 合法 Heartbeat 应刷新 last_recv_time，并立即回 Heartbeat。
    client.send_to(make_packet(PacketType::Heartbeat, 2, session_id), server_endpoint);
    const auto heartbeat_result = server.update(t0 + std::chrono::milliseconds(1000), std::chrono::milliseconds(200));
    require(heartbeat_result.sent, "server replies to valid Heartbeat");
    const auto heartbeat_reply = received_header(client, "server Heartbeat reply");
    require(heartbeat_reply.type == PacketType::Heartbeat, "server replies with Heartbeat");
    require(heartbeat_reply.session_id == session_id, "server Heartbeat reply carries session_id");

    // 错误 session_id 与未知 endpoint 都不能刷新 session，超过 5 秒后会被清理。
    client.send_to(make_packet(PacketType::Heartbeat, 3, session_id + 1), server_endpoint);
    server.update(t0 + std::chrono::milliseconds(5100), std::chrono::milliseconds(200));
    require(server.session_count() == 1, "wrong session_id Heartbeat does not remove session immediately");
    // 这一轮服务端可能按 1 秒间隔主动发 Heartbeat，先排空，避免后续误当成 ConnectAccept。
    (void)client.receive_from(std::chrono::milliseconds(50));

    unknown_client.send_to(make_packet(PacketType::Heartbeat, 4, session_id), server_endpoint);
    const auto timeout_result = server.update(t0 + std::chrono::milliseconds(6101), std::chrono::milliseconds(200));
    require(!timeout_result.timed_out_sessions.empty(), "unknown endpoint Heartbeat does not refresh session");
    require(server.session_count() == 0, "server removes session after 5 seconds without valid packet");

    client.send_to(make_packet(PacketType::ConnectRequest, 5, 0), server_endpoint);
    server.update(t0 + std::chrono::milliseconds(7000), std::chrono::milliseconds(200));
    const auto reconnect_accept = received_header(client, "Reconnect ConnectAccept");
    const auto reconnect_session = reconnect_accept.session_id;

    client.send_to(make_packet(PacketType::Disconnect, 6, reconnect_session + 1), server_endpoint);
    server.update(t0 + std::chrono::milliseconds(7100), std::chrono::milliseconds(200));
    require(server.session_count() == 1, "wrong session_id Disconnect does not remove session");

    unknown_client.send_to(make_packet(PacketType::Disconnect, 7, reconnect_session), server_endpoint);
    server.update(t0 + std::chrono::milliseconds(7200), std::chrono::milliseconds(200));
    require(server.session_count() == 1, "unknown endpoint Disconnect does not remove session");

    client.send_to(make_packet(PacketType::Disconnect, 8, reconnect_session), server_endpoint);
    const auto disconnect_result = server.update(t0 + std::chrono::milliseconds(7300), std::chrono::milliseconds(200));
    require(disconnect_result.disconnected_session, "valid Disconnect removes session");
    require(server.session_count() == 0, "server has no session after valid Disconnect");
}

void test_integration_connect_heartbeat_and_timeout()
{
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    ConnectionClient client({"127.0.0.1", server.port()});

    client.connect(t0);
    server.update(t0, std::chrono::milliseconds(200));
    client.update(t0, std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "integration connects client");
    require(server.session_count() == 1, "integration creates server session");

    // 客户端 1 秒心跳一次，服务端收到后刷新，连接应保持。
    client.update(t0 + std::chrono::milliseconds(1000));
    server.update(t0 + std::chrono::milliseconds(1000), std::chrono::milliseconds(200));
    client.update(t0 + std::chrono::milliseconds(1000), std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "heartbeat keeps client connected");
    require(server.session_count() == 1, "heartbeat keeps server session alive");

    // 不再驱动客户端发送心跳，只推进服务端时间，服务端应在 5 秒后清理 session。
    const auto timeout_result = server.update(t0 + std::chrono::milliseconds(6101));
    require(!timeout_result.timed_out_sessions.empty(), "server reports timed-out session");
    require(server.session_count() == 0, "server cleans session after heartbeat stops");
}

void test_ping_pong_integration()
{
    // 保留前序 issue 的 Ping/Pong 回归测试，确保连接层改动不破坏基础 UDP 包。
    PingServer server(0);
    UdpSocket client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    for (std::uint32_t sequence = 1; sequence <= 5; ++sequence) {
        const auto ping = make_ping_packet(sequence);
        client.send_to(ping, server_endpoint);

        const auto server_result = server.handle_next(std::chrono::milliseconds(500));
        require(server_result.received, "server receives Ping");
        require(server_result.responded, "server responds with Pong");
        require(server_result.sequence.has_value() && *server_result.sequence == sequence, "server keeps sequence");

        const auto response = client.receive_from(std::chrono::milliseconds(500));
        require(response.has_value(), "client receives Pong");

        const auto pong = validate_pong_packet(response->bytes);
        require(pong.accepted, "Pong is valid");
        require(pong.header->sequence == sequence, "Pong sequence matches Ping");
    }
}

void test_server_drops_invalid_packets()
{
    // 服务端收到非法包时应丢弃，不应该回 Pong。
    PingServer server(0);
    UdpSocket client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    auto bad_magic = make_ping_packet(7);
    bad_magic[0] = 0;
    client.send_to(bad_magic, server_endpoint);

    const auto server_result = server.handle_next(std::chrono::milliseconds(500));
    require(server_result.received, "server receives invalid packet");
    require(!server_result.responded, "server does not respond to invalid packet");
    require(server_result.reason == PacketRejectReason::BadMagic, "server reports bad magic");

    const auto response = client.receive_from(std::chrono::milliseconds(100));
    require(!response.has_value(), "client receives no Pong for invalid packet");
}

} // namespace

int main()
{
    // 不引入第三方测试框架，避免学习项目早期增加依赖和网络下载成本。
    test_packet_header_round_trip();
    test_connection_packet_validation();
    test_basic_packet_validation_rejections();
    test_client_connect_accept_and_timeout();
    test_client_heartbeat_interval();
    test_server_connect_request_and_duplicate();
    test_server_heartbeat_and_disconnect_rules();
    test_integration_connect_heartbeat_and_timeout();
    test_ping_pong_integration();
    test_server_drops_invalid_packets();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All MiniNet tests passed\n";
    return EXIT_SUCCESS;
}
