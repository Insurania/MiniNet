#include "mininet/ack_tracker.hpp"
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

std::vector<std::uint8_t> make_packet(PacketType type,
                                      std::uint32_t sequence,
                                      std::uint32_t session_id,
                                      std::uint32_t ack = 0,
                                      std::uint32_t ack_bits = 0)
{
    return make_connection_packet(type, sequence, session_id, ack, ack_bits);
}

PacketHeader received_header(const UdpSocket& socket, const std::string& message)
{
    const auto datagram = socket.receive_from(std::chrono::milliseconds(200));
    require(datagram.has_value(), message + " received");

    const auto decoded = datagram.has_value() ? decode_packet_header(datagram->bytes) : std::nullopt;
    require(decoded.has_value(), message + " decodes");
    return decoded.value_or(PacketHeader{});
}

bool sent_packet_acked(const AckTracker& tracker, std::uint32_t sequence)
{
    for (const auto& record : tracker.sent_packets()) {
        if (record.sequence == sequence) {
            return record.acked;
        }
    }
    return false;
}

bool sent_packet_acked(const std::vector<SentPacketRecord>& sent_packets, std::uint32_t sequence)
{
    for (const auto& record : sent_packets) {
        if (record.sequence == sequence) {
            return record.acked;
        }
    }
    return false;
}

void send_from_server(UdpSocket& server, const UdpEndpoint& client, PacketType type, std::uint32_t session_id)
{
    const auto packet = make_packet(type, 100, session_id);
    server.send_to(packet, client);
}

void test_packet_header_round_trip_and_byte_order()
{
    // Header 固定顺序：magic/version/type/sequence/session_id/ack/ack_bits。
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = PacketType::Heartbeat;
    header.sequence = 0x01020304;
    header.session_id = 0xAABBCCDD;
    header.ack = 0x11223344;
    header.ack_bits = 0x55667788;

    const auto encoded = encode_packet_header(header);
    const auto decoded = decode_packet_header(encoded);

    require(kPacketHeaderSize == 22, "PacketHeader size is 22 bytes");
    require(encoded.size() == 22, "encoded PacketHeader size is 22 bytes");
    require(encoded[0] == 0x4D && encoded[1] == 0x4E && encoded[2] == 0x45 && encoded[3] == 0x54,
            "magic uses network byte order");
    require(encoded[4] == kProtocolVersion, "version follows magic");
    require(encoded[5] == static_cast<std::uint8_t>(PacketType::Heartbeat), "type follows version");
    require(encoded[6] == 0x01 && encoded[7] == 0x02 && encoded[8] == 0x03 && encoded[9] == 0x04,
            "sequence uses network byte order");
    require(encoded[10] == 0xAA && encoded[11] == 0xBB && encoded[12] == 0xCC && encoded[13] == 0xDD,
            "session_id uses network byte order");
    require(encoded[14] == 0x11 && encoded[15] == 0x22 && encoded[16] == 0x33 && encoded[17] == 0x44,
            "ack uses network byte order");
    require(encoded[18] == 0x55 && encoded[19] == 0x66 && encoded[20] == 0x77 && encoded[21] == 0x88,
            "ack_bits uses network byte order");

    require(decoded.has_value(), "PacketHeader decodes");
    require(decoded->magic == kPacketMagic, "magic round-trips");
    require(decoded->version == kProtocolVersion, "version round-trips");
    require(decoded->type == PacketType::Heartbeat, "type round-trips");
    require(decoded->sequence == 0x01020304, "sequence round-trips");
    require(decoded->session_id == 0xAABBCCDD, "session_id round-trips");
    require(decoded->ack == 0x11223344, "ack round-trips");
    require(decoded->ack_bits == 0x55667788, "ack_bits round-trips");
}

void test_ack_tracker_sequence_allocation_and_comparison()
{
    // 出站 sequence 从 1 开始连续分配，并支持 uint32 回绕比较。
    AckTracker tracker;
    require(tracker.allocate_send_sequence() == 1, "first send sequence is 1");
    require(tracker.allocate_send_sequence() == 2, "second send sequence is 2");
    require(tracker.allocate_send_sequence() == 3, "third send sequence is 3");

    require(sequence_greater_than(1, 0), "1 is greater than 0");
    require(sequence_greater_than(0, UINT32_MAX), "0 is greater than UINT32_MAX after wrap");
    require(!sequence_greater_than(UINT32_MAX, 0), "UINT32_MAX is not greater than 0 after wrap");
    require(!sequence_greater_than(10, 10), "equal sequence is not greater");
}

void test_ack_tracker_empty_state_and_initial_ack()
{
    // 没有接收历史时，ack=0/ack_bits=0 不能误确认已发送包。
    AckTracker tracker;
    const auto state = tracker.make_ack_state();
    require(!state.has_received_sequence, "empty AckState has no received sequence");
    require(state.ack == 0, "empty AckState ack is 0");
    require(state.ack_bits == 0, "empty AckState ack_bits is 0");

    tracker.record_sent(1, Clock::now());
    tracker.process_ack(0, 0);
    require(!sent_packet_acked(tracker, 1), "initial empty ACK does not ack sent packet");
}

void test_ack_tracker_record_received_ordering()
{
    // record_received 应维护最新 ack，并用 ack_bits 表达乱序历史。
    AckTracker single;
    single.record_received(10);
    auto state = single.make_ack_state();
    require(state.has_received_sequence, "single receive enables AckState");
    require(state.ack == 10, "10 -> ack 10");
    require(state.ack_bits == 0, "single receive has no ack_bits");

    AckTracker ordered;
    ordered.record_received(10);
    ordered.record_received(11);
    ordered.record_received(12);
    state = ordered.make_ack_state();
    require(state.ack == 12, "10/11/12 -> ack 12");
    require((state.ack_bits & 0x1u) != 0, "ack_bits expresses 11");
    require((state.ack_bits & 0x2u) != 0, "ack_bits expresses 10");

    AckTracker out_of_order;
    out_of_order.record_received(10);
    out_of_order.record_received(12);
    out_of_order.record_received(11);
    state = out_of_order.make_ack_state();
    require(state.ack == 12, "10/12/11 -> ack 12");
    require((state.ack_bits & 0x1u) != 0, "out-of-order ack_bits expresses 11");
    require((state.ack_bits & 0x2u) != 0, "out-of-order ack_bits expresses 10");
    out_of_order.record_received(10);
    state = out_of_order.make_ack_state();
    require(state.ack == 12, "duplicate 10 does not move ack backward");
    require((state.ack_bits & 0x3u) == 0x3u, "duplicate 10 does not break ack_bits");
}

void test_ack_tracker_ack_bits_window()
{
    // bit0 表示 ack-1，bit31 表示 ack-32，窗口外不表达。
    AckTracker adjacent;
    adjacent.record_received(100);
    adjacent.record_received(99);
    adjacent.record_received(98);
    auto state = adjacent.make_ack_state();
    require(state.ack == 100, "100/99/98 -> ack 100");
    require((state.ack_bits & 0x1u) != 0, "bit0 expresses 99");
    require((state.ack_bits & 0x2u) != 0, "bit1 expresses 98");

    AckTracker gaps;
    gaps.record_received(100);
    gaps.record_received(98);
    gaps.record_received(96);
    state = gaps.make_ack_state();
    require(state.ack == 100, "100/98/96 -> ack 100");
    require((state.ack_bits & 0x1u) == 0, "bit0 remains clear for missing 99");
    require((state.ack_bits & 0x2u) != 0, "bit1 expresses 98");
    require((state.ack_bits & 0x4u) == 0, "bit2 remains clear for missing 97");
    require((state.ack_bits & 0x8u) != 0, "bit3 expresses 96");

    AckTracker edge;
    edge.record_received(100);
    edge.record_received(68);
    edge.record_received(67);
    state = edge.make_ack_state();
    require((state.ack_bits & (std::uint32_t{1} << 31)) != 0, "bit31 expresses ack-32");
    require((state.ack_bits & (std::uint32_t{1} << 30)) == 0, "sequence outside 32-window is not expressed");
}

void test_ack_tracker_process_ack()
{
    // process_ack 标记被 ack 或 ack_bits 覆盖的包，未覆盖包保持 unacked。
    AckTracker tracker;
    const auto now = Clock::now();
    for (std::uint32_t sequence = 7; sequence <= 11; ++sequence) {
        tracker.record_sent(sequence, now);
    }

    tracker.process_ack(10, 0);
    require(sent_packet_acked(tracker, 10), "ack=10 marks sent 10");
    require(!sent_packet_acked(tracker, 9), "ack=10 alone does not mark 9");

    tracker.process_ack(10, 0x1u);
    require(sent_packet_acked(tracker, 9), "ack=10 bit0 marks 9");
    require(!sent_packet_acked(tracker, 8), "missing bit1 keeps 8 unacked");

    tracker.process_ack(10, 0x4u);
    require(sent_packet_acked(tracker, 7), "ack=10 bit2 marks 7");
    require(!sent_packet_acked(tracker, 8), "unmentioned sent packet remains unacked");

    tracker.process_ack(10, 0x5u);
    require(sent_packet_acked(tracker, 10), "duplicate ACK keeps 10 acked");
    require(sent_packet_acked(tracker, 9), "duplicate ACK keeps 9 acked");
    require(sent_packet_acked(tracker, 7), "duplicate ACK keeps 7 acked");

    tracker.process_ack(1234, 0);
    require(!sent_packet_acked(tracker, 8), "unknown sent sequence does not crash or ack unrelated packet");
    require(!sent_packet_acked(tracker, 11), "unacked packet does not trigger retransmit side effect");
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
    // 非法 header 不应进入上层业务逻辑。
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

void test_invalid_packet_does_not_update_ack_state()
{
    // 非法连接包不能刷新接收历史，也不能污染后续 ACK。
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    UdpSocket client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    client.send_to(make_packet(PacketType::ConnectRequest, 1, 0), server_endpoint);
    server.update(t0, std::chrono::milliseconds(200));
    const auto accept = received_header(client, "ConnectAccept before invalid packet");
    const auto session_id = accept.session_id;

    auto bad_heartbeat = make_packet(PacketType::Heartbeat, 55, session_id);
    bad_heartbeat[0] = 0;
    client.send_to(bad_heartbeat, server_endpoint);
    const auto invalid_result = server.update(t0 + std::chrono::milliseconds(100), std::chrono::milliseconds(200));
    require(invalid_result.reason == PacketRejectReason::BadMagic, "invalid Heartbeat is rejected");
    require(server.sessions().front().ack_tracker.make_ack_state().ack == 0, "invalid packet does not update ack");

    client.send_to(make_packet(PacketType::Heartbeat, 2, session_id), server_endpoint);
    server.update(t0 + std::chrono::milliseconds(200), std::chrono::milliseconds(200));
    const auto reply = received_header(client, "Heartbeat reply after invalid packet");
    require(reply.ack == 2, "first valid Heartbeat determines ack");
    require(reply.ack_bits == 0, "invalid sequence is absent from ack_bits");
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
    require(heartbeat.sequence == 1, "first client Heartbeat sequence is 1");
    require(heartbeat.ack == 0, "client Heartbeat has empty ack before receive history");
    require(heartbeat.ack_bits == 0, "client Heartbeat has empty ack_bits before receive history");

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
    require(heartbeat_reply.ack == 2, "server Heartbeat reply acknowledges client Heartbeat");

    // 错误 session_id 与未知 endpoint 都不能刷新 session，超过 5 秒后会被清理。
    client.send_to(make_packet(PacketType::Heartbeat, 3, session_id + 1), server_endpoint);
    server.update(t0 + std::chrono::milliseconds(5100), std::chrono::milliseconds(200));
    require(server.session_count() == 1, "wrong session_id Heartbeat does not remove session immediately");
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

void test_integration_acknowledged_heartbeats()
{
    // 握手后多轮 Heartbeat 应携带 ACK，并标记双方已发送包为 acked。
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    ConnectionClient client({"127.0.0.1", server.port()});

    client.connect(t0);
    server.update(t0, std::chrono::milliseconds(200));
    client.update(t0, std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "integration connects client");
    require(server.session_count() == 1, "integration creates server session");

    client.update(t0 + std::chrono::milliseconds(1000));
    server.update(t0 + std::chrono::milliseconds(1000), std::chrono::milliseconds(200));
    client.update(t0 + std::chrono::milliseconds(1000), std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "heartbeat keeps client connected");
    require(server.session_count() == 1, "heartbeat keeps server session alive");
    require(sent_packet_acked(client.sent_packets(), 1), "client marks first Heartbeat acked from server ACK");

    client.update(t0 + std::chrono::milliseconds(2000));
    server.update(t0 + std::chrono::milliseconds(2000), std::chrono::milliseconds(200));
    require(server.sessions().size() == 1, "server still has one session after second Heartbeat");
    require(sent_packet_acked(server.sessions().front().ack_tracker, 1),
            "server marks first Heartbeat acked from client ACK");

    client.update(t0 + std::chrono::milliseconds(2000), std::chrono::milliseconds(200));
    require(sent_packet_acked(client.sent_packets(), 2), "client marks second Heartbeat acked from server ACK");

    const auto timeout_result = server.update(t0 + std::chrono::milliseconds(7101));
    require(!timeout_result.timed_out_sessions.empty(), "server reports timed-out session");
    require(server.session_count() == 0, "server cleans session after heartbeat stops");
}

void test_ping_pong_integration()
{
    // 保留前序 issue 的 Ping/Pong 回归测试，确认 header 扩展不破坏基础 UDP 包。
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
    // PingServer 收到非法包时应丢弃，不应该回 Pong。
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
    test_packet_header_round_trip_and_byte_order();
    test_ack_tracker_sequence_allocation_and_comparison();
    test_ack_tracker_empty_state_and_initial_ack();
    test_ack_tracker_record_received_ordering();
    test_ack_tracker_ack_bits_window();
    test_ack_tracker_process_ack();
    test_connection_packet_validation();
    test_basic_packet_validation_rejections();
    test_invalid_packet_does_not_update_ack_state();
    test_client_connect_accept_and_timeout();
    test_client_heartbeat_interval();
    test_server_connect_request_and_duplicate();
    test_server_heartbeat_and_disconnect_rules();
    test_integration_acknowledged_heartbeats();
    test_ping_pong_integration();
    test_server_drops_invalid_packets();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All MiniNet tests passed\n";
    return EXIT_SUCCESS;
}
