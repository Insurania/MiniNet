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

int failures = 0;

void require(bool condition, const std::string& message)
{
    // 简单测试框架：失败时记录数量，不立刻退出，方便一次看到多个失败点。
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void test_packet_header_round_trip()
{
    // 验证 header 编码后还能完整解码回来，这是协议字节格式的基础测试。
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = PacketType::Ping;
    header.sequence = 42;

    const auto encoded = encode_packet_header(header);
    const auto decoded = decode_packet_header(encoded);

    require(decoded.has_value(), "PacketHeader decodes");
    require(decoded->magic == kPacketMagic, "magic round-trips");
    require(decoded->version == kProtocolVersion, "version round-trips");
    require(decoded->type == PacketType::Ping, "type round-trips");
    require(decoded->sequence == 42, "sequence round-trips");
}

void test_server_packet_validation()
{
    // 这些 case 对应 issue 中的非法包验收标准：magic/version/type/长度。
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

void test_ping_pong_integration()
{
    // 使用 port=0 避免和本机已有端口冲突，操作系统会分配空闲端口。
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
    // 当前不引入第三方测试框架，避免学习早期增加依赖和网络下载成本。
    // 每个 test_* 函数覆盖 issue #1 的一个验收点。
    test_packet_header_round_trip();
    test_server_packet_validation();
    test_ping_pong_integration();
    test_server_drops_invalid_packets();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All MiniNet tests passed\n";
    return EXIT_SUCCESS;
}
