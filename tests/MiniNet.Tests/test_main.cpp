#include "mininet/ack_tracker.hpp"
#include "mininet/connection.hpp"
#include "mininet/network_simulator.hpp"
#include "mininet/packet.hpp"
#include "mininet/ping.hpp"
#include "mininet/reliable_message.hpp"
#include "mininet/snapshot.hpp"
#include "mininet/udp_socket.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace mininet;
using Clock = std::chrono::steady_clock;

int failures = 0;
std::string current_test;

void require(bool condition, const std::string& message)
{
    // 简单测试框架：记录所有失败点，便于一次看到多个验收项。
    if (!condition) {
        ++failures;
        std::cerr << "[FAIL] " << current_test << ": " << message << '\n';
    }
}

void log_value(const std::string& message)
{
    std::cout << "  " << message << '\n';
}

void run_test(const std::string& name, void (*test)())
{
    current_test = name;
    const auto before = failures;
    std::cout << "[RUN] " << name << '\n';
    test();
    if (failures == before) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cout << "[FAIL] " << name << " added " << (failures - before) << " failure(s)\n";
    }
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values)
{
    return {values.begin(), values.end()};
}

std::string byte_list(const std::vector<std::uint8_t>& values)
{
    std::string text;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            text += ",";
        }
        text += std::to_string(values[index]);
    }
    return text;
}

std::string byte_list(std::initializer_list<std::uint8_t> values)
{
    return byte_list(std::vector<std::uint8_t>{values});
}

std::int64_t delay_ms(Clock::time_point start, Clock::time_point delivery_time)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(delivery_time - start).count();
}

UdpEndpoint endpoint(const std::string& address, std::uint16_t port)
{
    return UdpEndpoint{address, port};
}

const char* to_string(NetworkSimulatorConfigRejectReason reason)
{
    switch (reason) {
    case NetworkSimulatorConfigRejectReason::None:
        return "None";
    case NetworkSimulatorConfigRejectReason::InvalidLossRate:
        return "InvalidLossRate";
    case NetworkSimulatorConfigRejectReason::InvalidDuplicateRate:
        return "InvalidDuplicateRate";
    case NetworkSimulatorConfigRejectReason::InvalidLatencyRange:
        return "InvalidLatencyRange";
    }

    return "Unknown";
}

NetworkSimulatorConfig network_simulator_config(double loss_rate,
                                                double duplicate_rate,
                                                std::chrono::milliseconds min_latency,
                                                std::chrono::milliseconds max_latency,
                                                std::uint32_t random_seed = 1)
{
    NetworkSimulatorConfig config;
    config.loss_rate = loss_rate;
    config.duplicate_rate = duplicate_rate;
    config.min_latency = min_latency;
    config.max_latency = max_latency;
    config.random_seed = random_seed;
    return config;
}

std::string packet_order(const std::vector<QueuedPacket>& packets)
{
    std::string text;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        if (index != 0) {
            text += ",";
        }
        text += byte_list(packets[index].bytes);
    }
    return text;
}

const char* to_string(SnapshotInsertStatus status)
{
    switch (status) {
    case SnapshotInsertStatus::Inserted:
        return "Inserted";
    case SnapshotInsertStatus::Duplicate:
        return "Duplicate";
    case SnapshotInsertStatus::TooOld:
        return "TooOld";
    case SnapshotInsertStatus::InsertedAndEvictedOldest:
        return "InsertedAndEvictedOldest";
    }

    return "Unknown";
}

Snapshot make_test_snapshot(SnapshotId snapshot_id)
{
    Snapshot snapshot;
    snapshot.snapshot_id = snapshot_id;
    snapshot.server_tick = snapshot_id * 10;
    snapshot.server_time_ms = 1000 + snapshot_id * 50;
    snapshot.entities.push_back(EntityState{101, Vec2f{1.5f + static_cast<float>(snapshot_id), -2.0f},
                                            Vec2f{0.25f, 0.5f}});
    snapshot.entities.push_back(EntityState{202, Vec2f{-4.0f, 8.0f + static_cast<float>(snapshot_id)},
                                            Vec2f{-0.5f, 1.25f}});
    return snapshot;
}

Snapshot make_snapshot_with_entity_count(std::size_t count)
{
    Snapshot snapshot;
    snapshot.snapshot_id = 900;
    snapshot.server_tick = 901;
    snapshot.server_time_ms = 902;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = static_cast<float>(index);
        snapshot.entities.push_back(
            EntityState{static_cast<EntityId>(1000 + index), Vec2f{value, value + 1.0f}, Vec2f{0.0f, 0.0f}});
    }
    return snapshot;
}

bool nearly_equal(float left, float right)
{
    return std::fabs(left - right) < 0.0001f;
}

bool snapshots_equal(const Snapshot& left, const Snapshot& right)
{
    if (left.snapshot_id != right.snapshot_id || left.server_tick != right.server_tick ||
        left.server_time_ms != right.server_time_ms || left.entities.size() != right.entities.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.entities.size(); ++index) {
        const auto& a = left.entities[index];
        const auto& b = right.entities[index];
        if (a.entity_id != b.entity_id || !nearly_equal(a.position.x, b.position.x) ||
            !nearly_equal(a.position.y, b.position.y) || !nearly_equal(a.velocity.x, b.velocity.x) ||
            !nearly_equal(a.velocity.y, b.velocity.y)) {
            return false;
        }
    }

    return true;
}

std::string snapshot_ids(const SnapshotBuffer& buffer)
{
    std::string text;
    for (std::size_t index = 0; index < buffer.snapshots().size(); ++index) {
        if (index != 0) {
            text += ",";
        }
        text += std::to_string(buffer.snapshots()[index].snapshot_id);
    }
    return text;
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

void test_reliable_sender_message_ids_are_independent()
{
    const auto t0 = Clock::now();
    ReliableSender first;
    ReliableSender second;

    require(first.enqueue(bytes({1}), t0), "first sender queues message 1");
    require(first.enqueue(bytes({2}), t0), "first sender queues message 2");
    require(second.enqueue(bytes({9}), t0), "second sender queues message 1 independently");

    const auto first_messages = first.select_messages_for_packet(t0, kMaxDatagramSize - kPacketHeaderSize);
    const auto second_messages = second.select_messages_for_packet(t0, kMaxDatagramSize - kPacketHeaderSize);

    log_value("first_sender ids=" + std::to_string(first_messages[0].message_id) + "," +
              std::to_string(first_messages[1].message_id));
    log_value("second_sender first_id=" + std::to_string(second_messages[0].message_id));
    require(first_messages.size() == 2, "first sender selects two messages");
    require(first_messages[0].message_id == 1, "first sender starts message_id at 1");
    require(first_messages[1].message_id == 2, "first sender increments message_id");
    require(second_messages.size() == 1, "second sender selects one message");
    require(second_messages[0].message_id == 1, "second sender has independent message_id sequence");
}

void test_reliable_sender_pending_delivery_and_packet_selection()
{
    const auto t0 = Clock::now();
    ReliableSender sender;
    require(sender.enqueue(bytes({10}), t0), "queues first reliable message");
    require(sender.enqueue(bytes({20}), t0), "queues second reliable message");
    require(sender.pending_count() == 2, "new reliable messages are pending");

    const auto selected = sender.select_messages_for_packet(t0, kMaxDatagramSize - kPacketHeaderSize);
    sender.mark_packet_sent(10, selected, t0);
    log_value("packet_seq=10 message_ids=" + std::to_string(selected[0].message_id) + "," +
              std::to_string(selected[1].message_id) + " pending_count=" + std::to_string(sender.pending_count()));

    sender.process_acked_packets({SentPacketRecord{10, t0, false}});
    require(sender.pending_count() == 2, "unacked packet keeps messages pending");
    require(sender.select_messages_for_packet(t0 + std::chrono::milliseconds(249), kMaxDatagramSize - kPacketHeaderSize).empty(),
            "sent but undelivered messages do not resend before interval");

    sender.process_acked_packets({SentPacketRecord{10, t0, true}});
    log_value("after_ack_10 pending_count=" + std::to_string(sender.pending_count()));
    require(sender.pending_count() == 0, "acked packet removes delivered messages from pending queue");
    require(sender.select_messages_for_packet(t0 + std::chrono::milliseconds(500), kMaxDatagramSize - kPacketHeaderSize).empty(),
            "delivered messages are not selected for later packets");
}

void test_reliable_data_encoding_decoding_round_trip()
{
    const ReliableMessage one{7, bytes({1, 2, 3, 255})};
    const auto one_payload = encode_reliable_data_payload({one});
    const auto one_decoded = decode_reliable_data_payload(one_payload);
    log_value("single encoded_size=" + std::to_string(one_payload.size()) + " payload=" +
              byte_list(one_decoded.messages.empty() ? std::vector<std::uint8_t>{} : one_decoded.messages[0].payload));
    require(one_decoded.ok, "single reliable message decodes");
    require(one_decoded.messages.size() == 1, "single reliable message count round-trips");
    require(one_decoded.messages[0].message_id == 7, "single message_id round-trips");
    require(one_decoded.messages[0].payload == one.payload, "single payload bytes round-trip");

    const std::vector<ReliableMessage> many{{1, bytes({42})}, {2, bytes({0, 1, 2, 3})}, {3, {}}};
    const auto many_payload = encode_reliable_data_payload(many);
    const auto many_decoded = decode_reliable_data_payload(many_payload);
    log_value("multi encoded_size=" + std::to_string(many_payload.size()) + " message_count=" +
              std::to_string(many_decoded.messages.size()));
    require(many_decoded.ok, "multiple reliable messages decode");
    require(many_decoded.messages.size() == many.size(), "multiple message count round-trips");
    for (std::size_t index = 0; index < many.size() && index < many_decoded.messages.size(); ++index) {
        require(many_decoded.messages[index].message_id == many[index].message_id, "multi message_id round-trips");
        require(many_decoded.messages[index].payload == many[index].payload, "multi payload bytes round-trip");
    }
}

void test_reliable_data_decode_rejects_malformed_payloads()
{
    const auto payload_too_long = bytes({1, 0, 0, 0, 5, 0, 4, 9, 9});
    const auto count_mismatch = bytes({2, 0, 0, 0, 1, 0, 1, 7});
    const auto truncated_header = bytes({1, 0, 0, 0});
    const auto trailing_bytes = bytes({0, 123});

    const auto too_long = decode_reliable_data_payload(payload_too_long);
    const auto mismatch = decode_reliable_data_payload(count_mismatch);
    const auto truncated = decode_reliable_data_payload(truncated_header);
    const auto trailing = decode_reliable_data_payload(trailing_bytes);

    log_value("malformed payload_size_too_long ok=" + std::to_string(too_long.ok));
    log_value("malformed count_mismatch ok=" + std::to_string(mismatch.ok));
    require(!too_long.ok && too_long.messages.empty(), "payload_size beyond bytes fails without messages");
    require(!mismatch.ok && mismatch.messages.empty(), "message_count mismatch fails without crash");
    require(!truncated.ok && truncated.messages.empty(), "truncated message header fails without crash");
    require(!trailing.ok && trailing.messages.empty(), "extra trailing bytes fail without crash");
}

void test_reliable_sender_max_datagram_and_available_space()
{
    const auto t0 = Clock::now();
    ReliableSender sender;
    const auto max_payload_size = kMaxDatagramSize - kPacketHeaderSize - 1 - kReliableMessageOverhead;
    require(sender.enqueue(std::vector<std::uint8_t>(max_payload_size, 1), t0), "max fitting payload queues");
    require(!sender.enqueue(std::vector<std::uint8_t>(max_payload_size + 1, 2), t0), "oversized payload is rejected");

    const auto exact = sender.select_messages_for_packet(t0, kMaxDatagramSize - kPacketHeaderSize);
    log_value("max_datagram_size=" + std::to_string(kMaxDatagramSize) + " max_payload_size=" +
              std::to_string(max_payload_size) + " selected_count=" + std::to_string(exact.size()));
    require(exact.size() == 1, "max fitting payload is packable");

    ReliableSender tight_sender;
    require(tight_sender.enqueue(std::vector<std::uint8_t>(100, 3), t0), "queues payload for tight space test");
    const auto too_tight = tight_sender.select_messages_for_packet(t0, 1 + kReliableMessageOverhead + 99);
    const auto enough = tight_sender.select_messages_for_packet(t0, 1 + kReliableMessageOverhead + 100);
    log_value("available_too_tight_selected=" + std::to_string(too_tight.size()) +
              " available_exact_selected=" + std::to_string(enough.size()));
    require(too_tight.empty(), "message exceeding available space is not packed this round");
    require(enough.size() == 1, "message is packed when available space is sufficient");
}

void test_reliable_packet_mapping_and_ack_delivery()
{
    const auto t0 = Clock::now();
    ReliableSender unknown_ack_sender;
    require(unknown_ack_sender.enqueue(bytes({1}), t0), "queues message before unknown ack");
    unknown_ack_sender.process_acked_packets({SentPacketRecord{999, t0, true}});
    log_value("unknown_ack_seq=999 pending_count=" + std::to_string(unknown_ack_sender.pending_count()));
    require(unknown_ack_sender.pending_count() == 1, "ACK for unknown packet mapping does not crash or deliver");

    ReliableSender sender;
    require(sender.enqueue(bytes({1}), t0), "queues mapped message 1");
    require(sender.enqueue(bytes({2}), t0), "queues mapped message 2");
    const auto selected = sender.select_messages_for_packet(t0, kMaxDatagramSize - kPacketHeaderSize);
    sender.mark_packet_sent(10, selected, t0);
    log_value("packet_seq=10 carries message_ids=" + std::to_string(selected[0].message_id) + "," +
              std::to_string(selected[1].message_id));

    sender.process_acked_packets({SentPacketRecord{10, t0, true}});
    require(!sender.is_pending(1), "ACK 10 delivers message 1");
    require(!sender.is_pending(2), "ACK 10 delivers message 2");
    require(sender.pending_count() == 0, "mapping cleanup leaves no pending messages");

    sender.process_acked_packets({SentPacketRecord{10, t0, true}});
    require(sender.pending_count() == 0, "duplicate ACK after mapping cleanup is harmless");
}

void test_reliable_resend_timing_and_delivery()
{
    const auto t0 = Clock::now();
    ReliableSender sender;
    require(sender.enqueue(bytes({8}), t0), "queues message for resend");
    const auto first = sender.select_messages_for_packet(t0, kMaxDatagramSize - kPacketHeaderSize);
    sender.mark_packet_sent(1, first, t0);
    require(first.size() == 1, "first send selects message");

    const auto before_interval = sender.select_messages_for_packet(t0 + std::chrono::milliseconds(249),
                                                                   kMaxDatagramSize - kPacketHeaderSize);
    const auto after_interval = sender.select_messages_for_packet(t0 + std::chrono::milliseconds(250),
                                                                  kMaxDatagramSize - kPacketHeaderSize);
    log_value("first_seq=1 resend_before_250ms=" + std::to_string(before_interval.size()) +
              " resend_at_250ms=" + std::to_string(after_interval.size()));
    require(before_interval.empty(), "message does not resend before 250ms");
    require(after_interval.size() == 1 && after_interval[0].message_id == first[0].message_id,
            "message can resend after 250ms");

    sender.mark_packet_sent(2, after_interval, t0 + std::chrono::milliseconds(250));
    sender.process_acked_packets({SentPacketRecord{2, t0 + std::chrono::milliseconds(250), true}});
    require(sender.pending_count() == 0, "ACK for resend packet delivers message");
    require(sender.select_messages_for_packet(t0 + std::chrono::milliseconds(1000), kMaxDatagramSize - kPacketHeaderSize).empty(),
            "delivered message does not resend");
}

void test_reliable_receiver_deduplicates_per_instance()
{
    ReliableReceiver receiver;
    const auto first = receiver.receive(ReliableMessage{5, bytes({1})});
    const auto duplicate = receiver.receive(ReliableMessage{5, bytes({2})});
    const auto different = receiver.receive(ReliableMessage{6, bytes({3})});

    ReliableReceiver other_receiver;
    const auto same_id_other_receiver = other_receiver.receive(ReliableMessage{5, bytes({4})});

    log_value("id=5 first_should_process=" + std::to_string(first.should_process) +
              " duplicate_should_process=" + std::to_string(duplicate.should_process));
    log_value("id=6 should_process=" + std::to_string(different.should_process) +
              " other_receiver_id5_should_process=" + std::to_string(same_id_other_receiver.should_process));
    require(first.should_process, "first message_id 5 should process");
    require(!duplicate.should_process, "duplicate message_id 5 should not process");
    require(different.should_process, "different message id should process");
    require(same_id_other_receiver.should_process, "different receiver/session same id should process independently");
}

void test_integration_client_to_server_reliable_resend_and_cleanup()
{
    const auto t0 = Clock::now();
    UdpSocket fake_server = UdpSocket::bind(0);
    const UdpEndpoint server_endpoint{"127.0.0.1", fake_server.local_port()};
    ConnectionClient client(server_endpoint);

    client.connect(t0);
    const auto request = fake_server.receive_from(std::chrono::milliseconds(200));
    require(request.has_value(), "fake server receives ConnectRequest");
    const auto accept = make_connection_packet(PacketType::ConnectAccept, 100, 77);
    fake_server.send_to(accept, request->sender);
    client.update(t0 + std::chrono::milliseconds(1), std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "client connects to fake server");

    const auto app_payload = bytes({9, 8, 7});
    require(client.send_reliable(app_payload, t0 + std::chrono::milliseconds(10)), "client queues reliable payload");
    require(client.reliable_pending_count() == 1, "client pending count is 1 after queue");

    client.update(t0 + std::chrono::milliseconds(10));
    const auto first_packet = fake_server.receive_from(std::chrono::milliseconds(200));
    require(first_packet.has_value(), "fake server receives first ReliableData");
    const auto first_header = decode_packet_header(first_packet->bytes);
    require(first_header.has_value(), "first ReliableData header decodes");
    const auto first_decoded = decode_reliable_data_payload(
        ByteView(first_packet->bytes.data() + kPacketHeaderSize, first_packet->bytes.size() - kPacketHeaderSize));
    ReliableReceiver server_receiver;
    const auto first_received = server_receiver.receive(first_decoded.messages[0]);
    log_value("client_to_server first_seq=" + std::to_string(first_header->sequence) +
              " message_id=" + std::to_string(first_received.message_id) +
              " should_process=" + std::to_string(first_received.should_process) +
              " pending_count=" + std::to_string(client.reliable_pending_count()));
    require(first_decoded.ok, "server decodes first client ReliableData");
    require(first_received.should_process, "server processes first client reliable message");

    client.update(t0 + std::chrono::milliseconds(249));
    require(!fake_server.receive_from(std::chrono::milliseconds(50)).has_value(), "client does not resend before 250ms");

    client.update(t0 + std::chrono::milliseconds(260));
    const auto resend_packet = fake_server.receive_from(std::chrono::milliseconds(200));
    require(resend_packet.has_value(), "fake server receives client resend after missing ACK");
    const auto resend_header = decode_packet_header(resend_packet->bytes);
    const auto resend_decoded = decode_reliable_data_payload(
        ByteView(resend_packet->bytes.data() + kPacketHeaderSize, resend_packet->bytes.size() - kPacketHeaderSize));
    const auto duplicate_received = server_receiver.receive(resend_decoded.messages[0]);
    log_value("client_to_server resend_seq=" + std::to_string(resend_header->sequence) +
              " duplicate_should_process=" + std::to_string(duplicate_received.should_process));
    require(resend_header->sequence != first_header->sequence, "resend uses a new packet sequence");
    require(!duplicate_received.should_process, "server deduplicates resent client reliable message");

    const auto ack = make_connection_packet(PacketType::Heartbeat, 200, 77, resend_header->sequence, 0);
    fake_server.send_to(ack, resend_packet->sender);
    client.update(t0 + std::chrono::milliseconds(270), std::chrono::milliseconds(200));
    log_value("client_to_server after_ack pending_count=" + std::to_string(client.reliable_pending_count()));
    require(client.reliable_pending_count() == 0, "client clears pending after ACK for resend packet");
}

void test_integration_server_to_client_reliable_resend_and_cleanup()
{
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    UdpSocket fake_client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    fake_client.send_to(make_connection_packet(PacketType::ConnectRequest, 1, 0), server_endpoint);
    server.update(t0, std::chrono::milliseconds(200));
    const auto accept = fake_client.receive_from(std::chrono::milliseconds(200));
    require(accept.has_value(), "fake client receives ConnectAccept");
    const auto accept_header = decode_packet_header(accept->bytes);
    require(accept_header.has_value(), "ConnectAccept header decodes");
    const auto session_id = accept_header->session_id;

    const auto app_payload = bytes({4, 5, 6});
    require(server.send_reliable(session_id, app_payload, t0 + std::chrono::milliseconds(10)),
            "server queues reliable payload");
    require(server.sessions().front().reliable_sender.pending_count() == 1, "server pending count is 1 after queue");

    server.update(t0 + std::chrono::milliseconds(10));
    const auto first_packet = fake_client.receive_from(std::chrono::milliseconds(200));
    require(first_packet.has_value(), "fake client receives first server ReliableData");
    const auto first_header = decode_packet_header(first_packet->bytes);
    const auto first_decoded = decode_reliable_data_payload(
        ByteView(first_packet->bytes.data() + kPacketHeaderSize, first_packet->bytes.size() - kPacketHeaderSize));
    ReliableReceiver client_receiver;
    const auto first_received = client_receiver.receive(first_decoded.messages[0]);
    log_value("server_to_client first_seq=" + std::to_string(first_header->sequence) +
              " message_id=" + std::to_string(first_received.message_id) +
              " should_process=" + std::to_string(first_received.should_process) +
              " pending_count=" + std::to_string(server.sessions().front().reliable_sender.pending_count()));
    require(first_decoded.ok, "client decodes first server ReliableData");
    require(first_received.should_process, "client processes first server reliable message");

    server.update(t0 + std::chrono::milliseconds(249));
    require(!fake_client.receive_from(std::chrono::milliseconds(50)).has_value(), "server does not resend before 250ms");

    server.update(t0 + std::chrono::milliseconds(260));
    const auto resend_packet = fake_client.receive_from(std::chrono::milliseconds(200));
    require(resend_packet.has_value(), "fake client receives server resend after missing ACK");
    const auto resend_header = decode_packet_header(resend_packet->bytes);
    const auto resend_decoded = decode_reliable_data_payload(
        ByteView(resend_packet->bytes.data() + kPacketHeaderSize, resend_packet->bytes.size() - kPacketHeaderSize));
    const auto duplicate_received = client_receiver.receive(resend_decoded.messages[0]);
    log_value("server_to_client resend_seq=" + std::to_string(resend_header->sequence) +
              " duplicate_should_process=" + std::to_string(duplicate_received.should_process));
    require(resend_header->sequence != first_header->sequence, "server resend uses a new packet sequence");
    require(!duplicate_received.should_process, "client deduplicates resent server reliable message");

    fake_client.send_to(make_connection_packet(PacketType::Heartbeat, 2, session_id, resend_header->sequence, 0),
                        server_endpoint);
    server.update(t0 + std::chrono::milliseconds(270), std::chrono::milliseconds(200));
    log_value("server_to_client after_ack pending_count=" +
              std::to_string(server.sessions().front().reliable_sender.pending_count()));
    require(server.sessions().front().reliable_sender.pending_count() == 0,
            "server clears pending after ACK for resend packet");
}

void test_snapshot_payload_round_trip()
{
    const auto snapshot = make_test_snapshot(42);
    const auto encoded = encode_snapshot_payload(snapshot);
    require(encoded.has_value(), "snapshot payload encodes");

    const auto decoded = encoded.has_value() ? decode_snapshot_payload(*encoded) : SnapshotDecodeResult{};
    log_value("snapshot_id=" + std::to_string(snapshot.snapshot_id) +
              " entity_count=" + std::to_string(snapshot.entities.size()) +
              " encoded_size=" + std::to_string(encoded.has_value() ? encoded->size() : 0));
    require(decoded.ok, "encoded snapshot decodes");
    require(decoded.ok && snapshots_equal(decoded.snapshot, snapshot), "decoded snapshot equals source snapshot");
}

void test_snapshot_payload_decode_rejects_malformed_inputs()
{
    const auto snapshot = make_test_snapshot(7);
    const auto encoded = encode_snapshot_payload(snapshot);
    require(encoded.has_value(), "valid snapshot encodes before malformed cases");

    const std::vector<std::uint8_t> short_payload(19, 0);
    auto too_many_entities = std::vector<std::uint8_t>(20, 0);
    too_many_entities[19] = static_cast<std::uint8_t>(kMaxEntitiesPerSnapshot + 1);

    auto missing_bytes = encoded.value_or(std::vector<std::uint8_t>{});
    if (!missing_bytes.empty()) {
        missing_bytes.pop_back();
    }

    auto extra_bytes = encoded.value_or(std::vector<std::uint8_t>{});
    extra_bytes.push_back(0xAA);

    const auto short_result = decode_snapshot_payload(short_payload);
    const auto too_many_result = decode_snapshot_payload(too_many_entities);
    const auto missing_result = decode_snapshot_payload(missing_bytes);
    const auto extra_result = decode_snapshot_payload(extra_bytes);

    log_value("short_payload_ok=" + std::to_string(short_result.ok) +
              " bytes=" + std::to_string(short_payload.size()));
    log_value("too_many_entities_ok=" + std::to_string(too_many_result.ok) +
              " entity_count=" + std::to_string(kMaxEntitiesPerSnapshot + 1));
    log_value("missing_bytes_ok=" + std::to_string(missing_result.ok) +
              " bytes=" + std::to_string(missing_bytes.size()));
    log_value("extra_bytes_ok=" + std::to_string(extra_result.ok) +
              " bytes=" + std::to_string(extra_bytes.size()));

    require(!short_result.ok, "short snapshot payload fails");
    require(!too_many_result.ok, "entity count over max fails");
    require(!missing_result.ok, "insufficient entity bytes fail");
    require(!extra_result.ok, "extra trailing bytes fail");
}

void test_snapshot_encode_rejects_too_many_entities()
{
    const auto too_large = make_snapshot_with_entity_count(kMaxEntitiesPerSnapshot + 1);
    const auto payload = encode_snapshot_payload(too_large);
    const auto packet = make_snapshot_packet(1, 9, 0, 0, too_large);
    log_value("entity_count=" + std::to_string(too_large.entities.size()) +
              " payload_encoded=" + std::to_string(payload.has_value()) +
              " packet_encoded=" + std::to_string(packet.has_value()));
    require(!payload.has_value(), "payload encode rejects 33 entities");
    require(!packet.has_value(), "packet encode rejects 33 entities");
}

void test_snapshot_buffer_insert_duplicate_and_too_old()
{
    SnapshotBuffer buffer;
    const auto inserted = buffer.insert(make_test_snapshot(10));
    const auto duplicate = buffer.insert(make_test_snapshot(10));
    const auto too_old = buffer.insert(make_test_snapshot(9));

    log_value("insert_status=" + std::string(to_string(inserted)) +
              " duplicate_status=" + std::string(to_string(duplicate)) +
              " too_old_status=" + std::string(to_string(too_old)) +
              " buffer_ids=" + snapshot_ids(buffer));
    require(inserted == SnapshotInsertStatus::Inserted, "single snapshot inserts");
    require(duplicate == SnapshotInsertStatus::Duplicate, "duplicate snapshot_id is rejected");
    require(too_old == SnapshotInsertStatus::TooOld, "older than oldest snapshot_id is rejected");
    require(buffer.size() == 1, "duplicate and too-old snapshots do not grow buffer");
}

void test_snapshot_buffer_out_of_order_sorting_and_capacity()
{
    SnapshotBuffer out_of_order;
    out_of_order.insert(make_test_snapshot(10));
    out_of_order.insert(make_test_snapshot(30));
    const auto middle = out_of_order.insert(make_test_snapshot(20));
    log_value("out_of_order_status=" + std::string(to_string(middle)) +
              " buffer_ids=" + snapshot_ids(out_of_order));
    require(middle == SnapshotInsertStatus::Inserted, "out-of-order newer snapshot inserts");
    require(out_of_order.snapshots().size() == 3, "out-of-order buffer has three snapshots");
    require(out_of_order.snapshots()[0].snapshot_id == 10 && out_of_order.snapshots()[1].snapshot_id == 20 &&
                out_of_order.snapshots()[2].snapshot_id == 30,
            "out-of-order insert keeps ascending snapshot_id order");

    SnapshotBuffer capped;
    SnapshotInsertStatus last_status = SnapshotInsertStatus::Inserted;
    for (SnapshotId id = 1; id <= kSnapshotBufferCapacity + 1; ++id) {
        last_status = capped.insert(make_test_snapshot(id));
    }
    log_value("capacity=" + std::to_string(kSnapshotBufferCapacity) +
              " final_size=" + std::to_string(capped.size()) +
              " last_status=" + std::string(to_string(last_status)) +
              " buffer_ids=" + snapshot_ids(capped));
    require(last_status == SnapshotInsertStatus::InsertedAndEvictedOldest,
            "inserting beyond capacity evicts oldest");
    require(capped.size() == kSnapshotBufferCapacity, "buffer remains capped at capacity");
    require(capped.snapshots().front().snapshot_id == 2, "oldest snapshot was evicted");
    require(capped.snapshots().back().snapshot_id == kSnapshotBufferCapacity + 1, "newest snapshot remains");
}

void test_snapshot_interpolation_positions_and_missing_entity()
{
    Snapshot older;
    older.snapshot_id = 1;
    older.entities.push_back(EntityState{5, Vec2f{10.0f, -10.0f}, Vec2f{}});
    older.entities.push_back(EntityState{6, Vec2f{1.0f, 1.0f}, Vec2f{}});

    Snapshot newer;
    newer.snapshot_id = 2;
    newer.entities.push_back(EntityState{5, Vec2f{20.0f, 30.0f}, Vec2f{}});

    const auto alpha_zero = interpolate_entity_position(older, newer, 5, 0.0f);
    const auto alpha_half = interpolate_entity_position(older, newer, 5, 0.5f);
    const auto alpha_one = interpolate_entity_position(older, newer, 5, 1.0f);
    const auto missing = interpolate_entity_position(older, newer, 6, 0.5f);

    log_value("entity_id=5 alpha0=(" + std::to_string(alpha_zero ? alpha_zero->x : 0.0f) + "," +
              std::to_string(alpha_zero ? alpha_zero->y : 0.0f) + ") alpha05=(" +
              std::to_string(alpha_half ? alpha_half->x : 0.0f) + "," +
              std::to_string(alpha_half ? alpha_half->y : 0.0f) + ") alpha1=(" +
              std::to_string(alpha_one ? alpha_one->x : 0.0f) + "," +
              std::to_string(alpha_one ? alpha_one->y : 0.0f) + ")");
    log_value("missing_entity_result=" + std::to_string(missing.has_value()));
    require(alpha_zero && nearly_equal(alpha_zero->x, 10.0f) && nearly_equal(alpha_zero->y, -10.0f),
            "alpha=0 returns older position");
    require(alpha_half && nearly_equal(alpha_half->x, 15.0f) && nearly_equal(alpha_half->y, 10.0f),
            "alpha=0.5 linearly interpolates position");
    require(alpha_one && nearly_equal(alpha_one->x, 20.0f) && nearly_equal(alpha_one->y, 30.0f),
            "alpha=1 returns newer position");
    require(!missing.has_value(), "missing entity in either snapshot fails");
}

void test_snapshot_rate_helpers()
{
    const auto update_interval = snapshot_update_interval_60hz();
    const auto send_interval = snapshot_send_interval_20hz();
    const auto update_before = update_interval - std::chrono::nanoseconds(1);
    const auto send_before = send_interval - std::chrono::nanoseconds(1);

    log_value("update_interval_ns=" + std::to_string(update_interval.count()) +
              " send_interval_ns=" + std::to_string(send_interval.count()));
    log_value("update_before=" + std::to_string(should_run_snapshot_update(update_before)) +
              " update_equal=" + std::to_string(should_run_snapshot_update(update_interval)) +
              " update_after=" + std::to_string(should_run_snapshot_update(update_interval + std::chrono::nanoseconds(1))));
    log_value("send_before=" + std::to_string(should_send_snapshot(send_before)) +
              " send_equal=" + std::to_string(should_send_snapshot(send_interval)) +
              " send_after=" + std::to_string(should_send_snapshot(send_interval + std::chrono::nanoseconds(1))));

    require(!should_run_snapshot_update(update_before), "elapsed below 60Hz interval does not run update");
    require(should_run_snapshot_update(update_interval), "elapsed equal to 60Hz interval runs update");
    require(should_run_snapshot_update(update_interval + std::chrono::nanoseconds(1)),
            "elapsed above 60Hz interval runs update");
    require(!should_send_snapshot(send_before), "elapsed below 20Hz interval does not send snapshot");
    require(should_send_snapshot(send_interval), "elapsed equal to 20Hz interval sends snapshot");
    require(should_send_snapshot(send_interval + std::chrono::nanoseconds(1)),
            "elapsed above 20Hz interval sends snapshot");
}

void test_snapshot_packet_type_validation_and_string()
{
    const auto snapshot = make_test_snapshot(55);
    const auto packet = make_snapshot_packet(12, 99, 3, 4, snapshot);
    require(packet.has_value(), "snapshot packet encodes");

    const auto validation = packet.has_value() ? validate_snapshot_packet(*packet) : PacketValidationResult{};
    const auto heartbeat = make_connection_packet(PacketType::Heartbeat, 13, 99);
    const auto heartbeat_validation = validate_snapshot_packet(heartbeat);
    log_value("packet_type=" + std::to_string(static_cast<int>(PacketType::Snapshot)) +
              " known=" + std::to_string(is_known_packet_type(static_cast<std::uint8_t>(PacketType::Snapshot))) +
              " validation=" + std::string(to_string(validation.reason)) +
              " heartbeat_validation=" + std::string(to_string(heartbeat_validation.reason)));
    require(is_known_packet_type(static_cast<std::uint8_t>(PacketType::Snapshot)),
            "PacketType::Snapshot is known");
    require(validation.accepted, "validate_snapshot_packet accepts Snapshot packet");
    require(!heartbeat_validation.accepted && heartbeat_validation.reason == PacketRejectReason::UnexpectedType,
            "validate_snapshot_packet rejects Heartbeat");
    require(std::string(to_string(PacketType::Snapshot)) == "Snapshot", "PacketType::Snapshot string is Snapshot");
}

void test_snapshot_send_does_not_enter_reliable_queue_or_resend()
{
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    UdpSocket fake_client = UdpSocket::open();
    const UdpEndpoint server_endpoint{"127.0.0.1", server.port()};

    fake_client.send_to(make_connection_packet(PacketType::ConnectRequest, 1, 0), server_endpoint);
    server.update(t0, std::chrono::milliseconds(200));
    const auto accept = fake_client.receive_from(std::chrono::milliseconds(200));
    require(accept.has_value(), "fake client receives ConnectAccept");
    const auto accept_header = accept.has_value() ? decode_packet_header(accept->bytes) : std::nullopt;
    require(accept_header.has_value(), "ConnectAccept decodes");
    const auto session_id = accept_header.has_value() ? accept_header->session_id : 0;

    const auto sent = server.send_snapshot(session_id, make_test_snapshot(70), t0 + std::chrono::milliseconds(10));
    const auto pending_after_snapshot = server.sessions().empty() ? 999u : server.sessions().front().reliable_sender.pending_count();
    const auto first_packet = fake_client.receive_from(std::chrono::milliseconds(200));
    const auto first_header = first_packet.has_value() ? decode_packet_header(first_packet->bytes) : std::nullopt;

    server.update(t0 + std::chrono::milliseconds(270), std::chrono::milliseconds(0));
    const auto maybe_resend = fake_client.receive_from(std::chrono::milliseconds(50));

    log_value("session_id=" + std::to_string(session_id) +
              " send_snapshot=" + std::to_string(sent) +
              " pending_count=" + std::to_string(pending_after_snapshot));
    log_value("packet_sequence=" + std::to_string(first_header ? first_header->sequence : 0) +
              " packet_type=" + std::string(first_header ? to_string(first_header->type) : "None") +
              " resent=" + std::to_string(maybe_resend.has_value()));
    require(sent, "server sends Snapshot once");
    require(pending_after_snapshot == 0, "Snapshot does not enter reliable message queue");
    require(first_header && first_header->type == PacketType::Snapshot, "fake client receives Snapshot packet");
    require(!maybe_resend.has_value(), "Snapshot loss does not trigger automatic retransmit");
}

void test_snapshot_integration_client_receives_and_caches()
{
    const auto t0 = Clock::now();
    ConnectionServer server(0);
    ConnectionClient client({"127.0.0.1", server.port()});

    client.connect(t0);
    server.update(t0, std::chrono::milliseconds(200));
    client.update(t0 + std::chrono::milliseconds(1), std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "client connects before Snapshot");
    const auto session_id = client.session_id();

    const auto snapshot = make_test_snapshot(80);
    const auto sent = server.send_snapshot(session_id, snapshot, t0 + std::chrono::milliseconds(10));
    const auto received = client.update(t0 + std::chrono::milliseconds(20), std::chrono::milliseconds(200));

    log_value("session_id=" + std::to_string(session_id) +
              " send_snapshot=" + std::to_string(sent) +
              " received_snapshot=" + std::to_string(received.received_snapshot.has_value()) +
              " reliable_messages=" + std::to_string(received.received_reliable_messages.size()));
    log_value("snapshot_id=" +
              std::to_string(received.received_snapshot ? received.received_snapshot->snapshot_id : 0) +
              " buffer_ids=" + snapshot_ids(client.snapshot_buffer()));
    require(sent, "server send_snapshot succeeds on connected session");
    require(received.received_snapshot.has_value(), "client update reports received Snapshot");
    require(received.received_reliable_messages.empty(), "Snapshot is not delivered as reliable message");
    require(client.snapshot_buffer().size() == 1, "client caches received Snapshot");
    require(client.snapshot_buffer().snapshots().front().snapshot_id == snapshot.snapshot_id,
            "client cache stores expected snapshot_id");
}

void test_snapshot_integration_unconnected_and_wrong_session_are_dropped()
{
    const auto t0 = Clock::now();
    UdpSocket fake_server = UdpSocket::bind(0);
    const UdpEndpoint server_endpoint{"127.0.0.1", fake_server.local_port()};

    ConnectionClient connecting_client(server_endpoint);
    connecting_client.connect(t0);
    const auto unconnected_request = fake_server.receive_from(std::chrono::milliseconds(200));
    require(unconnected_request.has_value(), "fake server receives ConnectRequest from unconnected client");
    const auto unconnected_packet = make_snapshot_packet(1, 77, 0, 0, make_test_snapshot(91));
    require(unconnected_packet.has_value(), "unconnected Snapshot packet encodes");
    if (unconnected_packet.has_value() && unconnected_request.has_value()) {
        fake_server.send_to(*unconnected_packet, unconnected_request->sender);
    }
    connecting_client.update(t0 + std::chrono::milliseconds(1), std::chrono::milliseconds(200));
    log_value("connecting_buffer_size=" + std::to_string(connecting_client.snapshot_buffer().size()));
    require(connecting_client.snapshot_buffer().size() == 0, "client without established virtual connection drops Snapshot");

    ConnectionClient client(server_endpoint);
    client.connect(t0);
    const auto request = fake_server.receive_from(std::chrono::milliseconds(200));
    require(request.has_value(), "fake server receives ConnectRequest");
    if (request.has_value()) {
        fake_server.send_to(make_connection_packet(PacketType::ConnectAccept, 2, 77), request->sender);
    }
    client.update(t0 + std::chrono::milliseconds(1), std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "client connects to fake server");

    const auto wrong_session_packet = make_snapshot_packet(3, 78, 0, 0, make_test_snapshot(92));
    require(wrong_session_packet.has_value(), "wrong-session Snapshot packet encodes");
    if (wrong_session_packet.has_value()) {
        fake_server.send_to(*wrong_session_packet, {"127.0.0.1", client.local_port()});
    }
    const auto wrong_session_result = client.update(t0 + std::chrono::milliseconds(2), std::chrono::milliseconds(200));
    log_value("expected_session_id=77 wrong_session_id=78 received_snapshot=" +
              std::to_string(wrong_session_result.received_snapshot.has_value()) +
              " buffer_size=" + std::to_string(client.snapshot_buffer().size()));
    require(!wrong_session_result.received_snapshot.has_value(), "wrong session_id Snapshot is not reported");
    require(client.snapshot_buffer().size() == 0, "wrong session_id Snapshot is not cached");
}

void test_snapshot_integration_client_buffer_keeps_order_after_out_of_order_arrival()
{
    const auto t0 = Clock::now();
    UdpSocket fake_server = UdpSocket::bind(0);
    const UdpEndpoint server_endpoint{"127.0.0.1", fake_server.local_port()};
    ConnectionClient client(server_endpoint);

    client.connect(t0);
    const auto request = fake_server.receive_from(std::chrono::milliseconds(200));
    require(request.has_value(), "fake server receives ConnectRequest");
    if (request.has_value()) {
        fake_server.send_to(make_connection_packet(PacketType::ConnectAccept, 10, 123), request->sender);
    }
    client.update(t0 + std::chrono::milliseconds(1), std::chrono::milliseconds(200));
    require(client.state() == ConnectionState::Connected, "client connects to fake server");

    const SnapshotId ids[] = {1, 3, 2};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto packet = make_snapshot_packet(static_cast<std::uint32_t>(20 + index), 123, 0, 0,
                                                 make_test_snapshot(ids[index]));
        require(packet.has_value(), "out-of-order Snapshot packet encodes");
        if (packet.has_value()) {
            fake_server.send_to(*packet, {"127.0.0.1", client.local_port()});
        }
        const auto result = client.update(t0 + std::chrono::milliseconds(2 + static_cast<int>(index)),
                                          std::chrono::milliseconds(200));
        log_value("arrival_snapshot_id=" + std::to_string(ids[index]) +
                  " received_snapshot=" + std::to_string(result.received_snapshot.has_value()) +
                  " buffer_ids=" + snapshot_ids(client.snapshot_buffer()));
    }

    require(client.snapshot_buffer().size() == 3, "client buffer keeps all accepted out-of-order snapshots");
    require(client.snapshot_buffer().snapshots()[0].snapshot_id == 1 &&
                client.snapshot_buffer().snapshots()[1].snapshot_id == 2 &&
                client.snapshot_buffer().snapshots()[2].snapshot_id == 3,
            "client buffer is sorted by snapshot_id after out-of-order arrival");
}

void test_network_simulator_config_validation()
{
    const auto valid_min = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(0),
                                                   std::chrono::milliseconds(0));
    const auto valid_max = network_simulator_config(1.0, 1.0, std::chrono::milliseconds(100),
                                                   std::chrono::milliseconds(100));
    const auto valid_min_result = validate_network_simulator_config(valid_min);
    const auto valid_max_result = validate_network_simulator_config(valid_max);
    log_value("loss_rate=0 duplicate_rate=0 accepted=" + std::to_string(valid_min_result.accepted));
    log_value("loss_rate=1 duplicate_rate=1 accepted=" + std::to_string(valid_max_result.accepted));
    require(valid_min_result.accepted && valid_min_result.reason == NetworkSimulatorConfigRejectReason::None,
            "boundary 0 rates are accepted");
    require(valid_max_result.accepted && valid_max_result.reason == NetworkSimulatorConfigRejectReason::None,
            "boundary 1 rates are accepted");

    const struct {
        NetworkSimulatorConfig config;
        NetworkSimulatorConfigRejectReason reason;
        const char* name;
    } cases[] = {
        {network_simulator_config(-0.01, 0.0, std::chrono::milliseconds(0), std::chrono::milliseconds(0)),
         NetworkSimulatorConfigRejectReason::InvalidLossRate, "loss_rate_negative"},
        {network_simulator_config(1.01, 0.0, std::chrono::milliseconds(0), std::chrono::milliseconds(0)),
         NetworkSimulatorConfigRejectReason::InvalidLossRate, "loss_rate_too_high"},
        {network_simulator_config(0.0, -0.01, std::chrono::milliseconds(0), std::chrono::milliseconds(0)),
         NetworkSimulatorConfigRejectReason::InvalidDuplicateRate, "duplicate_rate_negative"},
        {network_simulator_config(0.0, 1.01, std::chrono::milliseconds(0), std::chrono::milliseconds(0)),
         NetworkSimulatorConfigRejectReason::InvalidDuplicateRate, "duplicate_rate_too_high"},
        {network_simulator_config(0.0, 0.0, std::chrono::milliseconds(101), std::chrono::milliseconds(100)),
         NetworkSimulatorConfigRejectReason::InvalidLatencyRange, "latency_range_inverted"},
    };

    for (const auto& test_case : cases) {
        const auto validation = validate_network_simulator_config(test_case.config);
        const auto simulator = NetworkSimulator::create(test_case.config);
        log_value(std::string("case=") + test_case.name +
                  " loss_rate=" + std::to_string(test_case.config.loss_rate) +
                  " duplicate_rate=" + std::to_string(test_case.config.duplicate_rate) +
                  " latency_min_ms=" + std::to_string(test_case.config.min_latency.count()) +
                  " latency_max_ms=" + std::to_string(test_case.config.max_latency.count()) +
                  " accepted=" + std::to_string(validation.accepted) +
                  " reason=" + to_string(validation.reason) +
                  " create_has_value=" + std::to_string(simulator.has_value()));
        require(!validation.accepted, std::string(test_case.name) + " is rejected");
        require(validation.reason == test_case.reason, std::string(test_case.name) + " reports expected reason");
        require(!simulator.has_value(), std::string(test_case.name) + " create returns nullopt");
    }
}

void test_network_simulator_no_loss_delivery()
{
    const auto config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(0),
                                                std::chrono::milliseconds(0), 11);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    const auto payload = bytes({0x10, 0x20, 0x30});

    simulator->send(sender, receiver, payload, t0);
    const auto packets = simulator->poll(receiver, t0);

    log_value("loss_rate=" + std::to_string(config.loss_rate) +
              " latency_ms=" + std::to_string(config.min_latency.count()) +
              " in_flight_count=" + std::to_string(simulator->in_flight_count()) +
              " bytes=" + (packets.empty() ? std::string("") : byte_list(packets.front().bytes)));
    require(packets.size() == 1, "receiver polls one packet at zero latency");
    require(!packets.empty() && packets.front().bytes == payload, "raw bytes are delivered unchanged");
    require(!packets.empty() && packets.front().from.address == sender.address && packets.front().from.port == sender.port,
            "sender endpoint is preserved");
    require(!packets.empty() && packets.front().delivery_time == t0, "zero latency delivery_time equals now");
}

void test_network_simulator_full_loss()
{
    const auto config = network_simulator_config(1.0, 1.0, std::chrono::milliseconds(0),
                                                std::chrono::milliseconds(0), 12);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    simulator->send(sender, receiver, bytes({0x01}), t0);
    const auto in_flight = simulator->in_flight_count();
    const auto packets = simulator->poll(receiver, t0);

    log_value("loss_rate=" + std::to_string(config.loss_rate) +
              " duplicate_rate=" + std::to_string(config.duplicate_rate) +
              " in_flight_count=" + std::to_string(in_flight) +
              " polled_count=" + std::to_string(packets.size()));
    require(in_flight == 0, "fully lost packet is not queued");
    require(packets.empty(), "fully lost packet is not delivered");
}

void test_network_simulator_fixed_latency()
{
    const auto config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(100),
                                                std::chrono::milliseconds(100), 13);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    simulator->send(sender, receiver, bytes({0x64}), t0);

    const auto early = simulator->poll(receiver, t0 + std::chrono::milliseconds(99));
    const auto on_time = simulator->poll(receiver, t0 + std::chrono::milliseconds(100));

    log_value("latency_ms=100 early_count=" + std::to_string(early.size()) +
              " on_time_count=" + std::to_string(on_time.size()) +
              " in_flight_count=" + std::to_string(simulator->in_flight_count()));
    log_value("delivery_delay_ms=" +
              std::to_string(on_time.empty() ? -1 : delay_ms(t0, on_time.front().delivery_time)));
    require(early.empty(), "packet is not delivered before fixed latency");
    require(on_time.size() == 1, "packet is delivered at fixed latency");
    require(!on_time.empty() && on_time.front().delivery_time == t0 + std::chrono::milliseconds(100),
            "delivery_time equals t0 plus fixed latency");
}

void test_network_simulator_jitter_latency_range()
{
    const auto config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(50),
                                                std::chrono::milliseconds(150), 14);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    for (std::uint8_t value = 1; value <= 8; ++value) {
        simulator->send(sender, receiver, bytes({value}), t0);
    }

    const auto packets = simulator->poll(receiver, t0 + std::chrono::milliseconds(1000));
    std::string delays;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        if (index != 0) {
            delays += ",";
        }
        const auto delay = delay_ms(t0, packets[index].delivery_time);
        delays += std::to_string(delay);
        require(delay >= 50 && delay <= 150, "jitter delivery_time is inside configured range");
    }

    log_value("latency_min_ms=50 latency_max_ms=150 packet_count=" + std::to_string(packets.size()) +
              " delays_ms=" + delays +
              " delivery_order=" + packet_order(packets));
    require(packets.size() == 8, "all jitter packets are eventually delivered");
}

void test_network_simulator_reordering_and_poll_sorting()
{
    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    std::vector<QueuedPacket> packets;
    std::uint32_t chosen_seed = 0;

    for (std::uint32_t seed = 1; seed <= 200 && chosen_seed == 0; ++seed) {
        const auto config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(0),
                                                    std::chrono::milliseconds(200), seed);
        auto simulator = NetworkSimulator::create(config);
        require(simulator.has_value(), "valid simulator config creates simulator");
        if (!simulator.has_value()) {
            return;
        }

        for (std::uint8_t value = 1; value <= 8; ++value) {
            simulator->send(sender, receiver, bytes({value}), t0);
        }
        auto candidate = simulator->poll(receiver, t0 + std::chrono::milliseconds(1000));
        bool has_different_delivery_times = false;
        for (std::size_t index = 1; index < candidate.size(); ++index) {
            if (candidate[index - 1].delivery_time != candidate[index].delivery_time) {
                has_different_delivery_times = true;
            }
        }
        if (candidate.size() == 8 && has_different_delivery_times && packet_order(candidate) != "1,2,3,4,5,6,7,8") {
            chosen_seed = seed;
            packets = std::move(candidate);
        }
    }

    std::string delays;
    bool sorted = true;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        if (index != 0) {
            delays += ",";
            if (packets[index - 1].delivery_time > packets[index].delivery_time) {
                sorted = false;
            }
        }
        delays += std::to_string(delay_ms(t0, packets[index].delivery_time));
    }

    log_value("chosen_seed=" + std::to_string(chosen_seed) +
              " delays_ms=" + delays +
              " delivery_order=" + packet_order(packets));
    require(chosen_seed != 0, "test found deterministic seed that produces reordering");
    require(sorted, "poll returns packets sorted by delivery_time");

    const auto fixed_config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(0),
                                                       std::chrono::milliseconds(0), 21);
    auto fixed_simulator = NetworkSimulator::create(fixed_config);
    require(fixed_simulator.has_value(), "valid fixed latency simulator creates simulator");
    if (!fixed_simulator.has_value()) {
        return;
    }

    fixed_simulator->send(sender, receiver, bytes({1}), t0);
    fixed_simulator->send(sender, receiver, bytes({2}), t0);
    fixed_simulator->send(sender, receiver, bytes({3}), t0);
    const auto same_time_packets = fixed_simulator->poll(receiver, t0);
    log_value("same_delivery_time_order=" + packet_order(same_time_packets));
    require(packet_order(same_time_packets) == "1,2,3", "same delivery_time preserves enqueue order");
}

void test_network_simulator_duplicate_delivery()
{
    const auto config = network_simulator_config(0.0, 1.0, std::chrono::milliseconds(0),
                                                std::chrono::milliseconds(50), 31);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    const auto payload = bytes({0x44, 0x55});
    simulator->send(sender, receiver, payload, t0);
    const auto packets = simulator->poll(receiver, t0 + std::chrono::milliseconds(1000));

    log_value("loss_rate=" + std::to_string(config.loss_rate) +
              " duplicate_rate=" + std::to_string(config.duplicate_rate) +
              " packet_count=" + std::to_string(packets.size()) +
              " delivery_order=" + packet_order(packets));
    if (packets.size() == 2) {
        log_value("delivery_delay_ms=" + std::to_string(delay_ms(t0, packets[0].delivery_time)) +
                  "," + std::to_string(delay_ms(t0, packets[1].delivery_time)));
    }
    require(packets.size() == 2, "duplicate_rate=1 produces original plus one duplicate");
    for (const auto& packet : packets) {
        require(packet.from.address == sender.address && packet.from.port == sender.port, "duplicate from endpoint matches");
        require(packet.to.address == receiver.address && packet.to.port == receiver.port, "duplicate to endpoint matches");
        require(packet.bytes == payload, "duplicate bytes match original payload");
    }

    const auto loss_config = network_simulator_config(1.0, 1.0, std::chrono::milliseconds(0),
                                                     std::chrono::milliseconds(50), 31);
    auto loss_simulator = NetworkSimulator::create(loss_config);
    require(loss_simulator.has_value(), "valid full loss simulator creates simulator");
    if (!loss_simulator.has_value()) {
        return;
    }

    loss_simulator->send(sender, receiver, payload, t0);
    const auto loss_packets = loss_simulator->poll(receiver, t0 + std::chrono::milliseconds(1000));
    log_value("loss_rate=1 duplicate_rate=1 in_flight_count=" +
              std::to_string(loss_simulator->in_flight_count()) +
              " packet_count=" + std::to_string(loss_packets.size()));
    require(loss_packets.empty(), "lost packet does not produce duplicate");
}

void test_network_simulator_poll_consume_and_endpoint_filtering()
{
    const auto config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(0),
                                                std::chrono::milliseconds(0), 41);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver_b = endpoint("10.0.0.2", 2000);
    const auto receiver_c = endpoint("10.0.0.2", 2001);
    const auto receiver_same_port_other_address = endpoint("10.0.0.3", 2000);

    simulator->send(sender, receiver_b, bytes({0x0B}), t0);
    simulator->send(sender, receiver_c, bytes({0x0C}), t0);
    simulator->send(sender, receiver_same_port_other_address, bytes({0x0D}), t0);

    const auto b_first = simulator->poll(receiver_b, t0);
    const auto b_second = simulator->poll(receiver_b, t0);
    const auto c_packets = simulator->poll(receiver_c, t0);
    const auto other_address_packets = simulator->poll(receiver_same_port_other_address, t0);

    log_value("b_first_count=" + std::to_string(b_first.size()) +
              " b_second_count=" + std::to_string(b_second.size()) +
              " c_count=" + std::to_string(c_packets.size()) +
              " other_address_count=" + std::to_string(other_address_packets.size()) +
              " in_flight_count=" + std::to_string(simulator->in_flight_count()));
    log_value("b_bytes=" + packet_order(b_first) +
              " c_bytes=" + packet_order(c_packets) +
              " other_address_bytes=" + packet_order(other_address_packets));
    require(packet_order(b_first) == "11", "B receives only packet addressed to B");
    require(b_second.empty(), "poll consumes delivered packets for the receiver");
    require(packet_order(c_packets) == "12", "packet for different port remains available for C");
    require(packet_order(other_address_packets) == "13", "packet for different address remains available");
}

void test_network_simulator_deterministic_seed()
{
    const auto config = network_simulator_config(0.25, 0.5, std::chrono::milliseconds(10),
                                                std::chrono::milliseconds(90), 51);
    auto first = NetworkSimulator::create(config);
    auto second = NetworkSimulator::create(config);
    require(first.has_value() && second.has_value(), "same valid config creates both simulators");
    if (!first.has_value() || !second.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    for (std::uint8_t value = 1; value <= 10; ++value) {
        const auto payload = bytes({value, static_cast<std::uint8_t>(value + 10)});
        first->send(sender, receiver, payload, t0 + std::chrono::milliseconds(value));
        second->send(sender, receiver, payload, t0 + std::chrono::milliseconds(value));
    }

    const auto first_packets = first->poll(receiver, t0 + std::chrono::milliseconds(1000));
    const auto second_packets = second->poll(receiver, t0 + std::chrono::milliseconds(1000));
    bool same = first_packets.size() == second_packets.size();
    for (std::size_t index = 0; same && index < first_packets.size(); ++index) {
        same = first_packets[index].bytes == second_packets[index].bytes &&
               first_packets[index].delivery_time == second_packets[index].delivery_time &&
               first_packets[index].from.address == second_packets[index].from.address &&
               first_packets[index].from.port == second_packets[index].from.port &&
               first_packets[index].to.address == second_packets[index].to.address &&
               first_packets[index].to.port == second_packets[index].to.port;
    }

    std::string delays;
    for (std::size_t index = 0; index < first_packets.size(); ++index) {
        if (index != 0) {
            delays += ",";
        }
        delays += std::to_string(delay_ms(t0, first_packets[index].delivery_time));
    }
    log_value("random_seed=" + std::to_string(config.random_seed) +
              " first_count=" + std::to_string(first_packets.size()) +
              " second_count=" + std::to_string(second_packets.size()) +
              " delivery_order=" + packet_order(first_packets) +
              " delivery_times_ms=" + delays);
    require(same, "same seed and input sequence produce identical poll results");
}

void test_network_simulator_protocol_independent_raw_bytes()
{
    const auto config = network_simulator_config(0.0, 0.0, std::chrono::milliseconds(0),
                                                std::chrono::milliseconds(0), 61);
    auto simulator = NetworkSimulator::create(config);
    require(simulator.has_value(), "valid simulator config creates simulator");
    if (!simulator.has_value()) {
        return;
    }

    const auto t0 = Clock::time_point{};
    const auto sender = endpoint("10.0.0.1", 1000);
    const auto receiver = endpoint("10.0.0.2", 2000);
    const auto raw_payload = bytes({0xFF, 0x00, 0x13});
    const auto ping_payload = make_ping_packet(77);

    simulator->send(sender, receiver, raw_payload, t0);
    simulator->send(sender, receiver, ping_payload, t0);
    const auto packets = simulator->poll(receiver, t0);

    log_value("raw_bytes=" + byte_list(raw_payload) +
              " ping_bytes_size=" + std::to_string(ping_payload.size()) +
              " delivery_order=" + packet_order(packets));
    require(packets.size() == 2, "raw and MiniNet-looking packets are both delivered");
    require(packets.size() >= 1 && packets[0].bytes == raw_payload, "non-MiniNet header bytes are unchanged");
    require(packets.size() >= 2 && packets[1].bytes == ping_payload, "MiniNet packet bytes are unchanged and not parsed");
}

} // namespace

int main()
{
    // 不引入第三方测试框架，避免学习项目早期增加依赖和网络下载成本。
    run_test("packet header round trip and byte order", test_packet_header_round_trip_and_byte_order);
    run_test("ack tracker sequence allocation and comparison", test_ack_tracker_sequence_allocation_and_comparison);
    run_test("ack tracker empty state and initial ack", test_ack_tracker_empty_state_and_initial_ack);
    run_test("ack tracker received ordering", test_ack_tracker_record_received_ordering);
    run_test("ack tracker ack bits window", test_ack_tracker_ack_bits_window);
    run_test("ack tracker process ack", test_ack_tracker_process_ack);
    run_test("connection packet validation", test_connection_packet_validation);
    run_test("basic packet validation rejections", test_basic_packet_validation_rejections);
    run_test("invalid packet does not update ack state", test_invalid_packet_does_not_update_ack_state);
    run_test("client connect accept and timeout", test_client_connect_accept_and_timeout);
    run_test("client heartbeat interval", test_client_heartbeat_interval);
    run_test("server connect request and duplicate", test_server_connect_request_and_duplicate);
    run_test("server heartbeat and disconnect rules", test_server_heartbeat_and_disconnect_rules);
    run_test("integration acknowledged heartbeats", test_integration_acknowledged_heartbeats);
    run_test("ping pong integration", test_ping_pong_integration);
    run_test("server drops invalid packets", test_server_drops_invalid_packets);
    run_test("reliable sender message ids are independent", test_reliable_sender_message_ids_are_independent);
    run_test("reliable sender pending delivery and packet selection",
             test_reliable_sender_pending_delivery_and_packet_selection);
    run_test("reliable data encoding decoding round trip", test_reliable_data_encoding_decoding_round_trip);
    run_test("reliable data decode rejects malformed payloads", test_reliable_data_decode_rejects_malformed_payloads);
    run_test("reliable sender max datagram and available space", test_reliable_sender_max_datagram_and_available_space);
    run_test("reliable packet mapping and ack delivery", test_reliable_packet_mapping_and_ack_delivery);
    run_test("reliable resend timing and delivery", test_reliable_resend_timing_and_delivery);
    run_test("reliable receiver deduplicates per instance", test_reliable_receiver_deduplicates_per_instance);
    run_test("integration client to server reliable resend and cleanup",
             test_integration_client_to_server_reliable_resend_and_cleanup);
    run_test("integration server to client reliable resend and cleanup",
             test_integration_server_to_client_reliable_resend_and_cleanup);
    run_test("snapshot payload round trip", test_snapshot_payload_round_trip);
    run_test("snapshot payload decode rejects malformed inputs", test_snapshot_payload_decode_rejects_malformed_inputs);
    run_test("snapshot encode rejects too many entities", test_snapshot_encode_rejects_too_many_entities);
    run_test("snapshot buffer insert duplicate and too old", test_snapshot_buffer_insert_duplicate_and_too_old);
    run_test("snapshot buffer out of order sorting and capacity",
             test_snapshot_buffer_out_of_order_sorting_and_capacity);
    run_test("snapshot interpolation positions and missing entity",
             test_snapshot_interpolation_positions_and_missing_entity);
    run_test("snapshot rate helpers", test_snapshot_rate_helpers);
    run_test("snapshot packet type validation and string", test_snapshot_packet_type_validation_and_string);
    run_test("snapshot send does not enter reliable queue or resend",
             test_snapshot_send_does_not_enter_reliable_queue_or_resend);
    run_test("snapshot integration client receives and caches", test_snapshot_integration_client_receives_and_caches);
    run_test("snapshot integration unconnected and wrong session are dropped",
             test_snapshot_integration_unconnected_and_wrong_session_are_dropped);
    run_test("snapshot integration client buffer keeps order after out of order arrival",
             test_snapshot_integration_client_buffer_keeps_order_after_out_of_order_arrival);
    run_test("network simulator config validation", test_network_simulator_config_validation);
    run_test("network simulator no loss delivery", test_network_simulator_no_loss_delivery);
    run_test("network simulator full loss", test_network_simulator_full_loss);
    run_test("network simulator fixed latency", test_network_simulator_fixed_latency);
    run_test("network simulator jitter latency range", test_network_simulator_jitter_latency_range);
    run_test("network simulator reordering and poll sorting", test_network_simulator_reordering_and_poll_sorting);
    run_test("network simulator duplicate delivery", test_network_simulator_duplicate_delivery);
    run_test("network simulator poll consume and endpoint filtering",
             test_network_simulator_poll_consume_and_endpoint_filtering);
    run_test("network simulator deterministic seed", test_network_simulator_deterministic_seed);
    run_test("network simulator protocol independent raw bytes",
             test_network_simulator_protocol_independent_raw_bytes);

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All MiniNet tests passed\n";
    return EXIT_SUCCESS;
}
