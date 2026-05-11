#include "mininet/packet.hpp"

namespace mininet {

namespace {

void write_u32_be(std::array<std::uint8_t, kPacketHeaderSize>& bytes, std::size_t offset, std::uint32_t value)
{
    // 网络协议一般使用 big-endian，也叫 network byte order。
    // 这样不同 CPU 架构之间传输 uint32 时不会因为本机字节序不同而解析错。
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint32_t read_u32_be(ByteView bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

PacketValidationResult validate_packet(ByteView bytes, PacketType expected_type)
{
    // 校验顺序保持从便宜到具体：长度 -> type -> magic -> version -> 期望类型。
    // 这样非法输入会被明确拒绝，不会继续进入业务逻辑。
    if (bytes.size() < kPacketHeaderSize) {
        return {false, PacketRejectReason::TooShort, std::nullopt};
    }

    const auto raw_type = bytes[5];
    if (!is_known_packet_type(raw_type)) {
        return {false, PacketRejectReason::UnknownType, std::nullopt};
    }

    auto header = decode_packet_header(bytes);
    if (header->magic != kPacketMagic) {
        return {false, PacketRejectReason::BadMagic, header};
    }

    if (header->version != kProtocolVersion) {
        return {false, PacketRejectReason::BadVersion, header};
    }

    if (header->type != expected_type) {
        return {false,
                expected_type == PacketType::Ping ? PacketRejectReason::NotPing : PacketRejectReason::NotPong,
                header};
    }

    return {true, PacketRejectReason::None, header};
}

} // namespace

bool is_known_packet_type(std::uint8_t raw_type)
{
    return raw_type == static_cast<std::uint8_t>(PacketType::Ping) ||
           raw_type == static_cast<std::uint8_t>(PacketType::Pong);
}

std::array<std::uint8_t, kPacketHeaderSize> encode_packet_header(const PacketHeader& header)
{
    // UDP 发送的是字节，不是 C++ struct。显式编码可以避免结构体 padding 问题。
    std::array<std::uint8_t, kPacketHeaderSize> bytes{};
    write_u32_be(bytes, 0, header.magic);
    bytes[4] = header.version;
    bytes[5] = static_cast<std::uint8_t>(header.type);
    write_u32_be(bytes, 6, header.sequence);
    return bytes;
}

std::optional<PacketHeader> decode_packet_header(ByteView bytes)
{
    // 注意：这里只负责“解析”，不负责判断 magic/version 是否符合当前协议。
    // 具体是否接受这个包，交给 validate_ping_packet / validate_pong_packet。
    if (bytes.size() < kPacketHeaderSize) {
        return std::nullopt;
    }

    const auto raw_type = bytes[5];
    if (!is_known_packet_type(raw_type)) {
        return std::nullopt;
    }

    PacketHeader header;
    header.magic = read_u32_be(bytes, 0);
    header.version = bytes[4];
    header.type = static_cast<PacketType>(raw_type);
    header.sequence = read_u32_be(bytes, 6);
    return header;
}

PacketValidationResult validate_ping_packet(ByteView bytes)
{
    return validate_packet(bytes, PacketType::Ping);
}

PacketValidationResult validate_pong_packet(ByteView bytes)
{
    return validate_packet(bytes, PacketType::Pong);
}

const char* to_string(PacketType type)
{
    switch (type) {
    case PacketType::Ping:
        return "Ping";
    case PacketType::Pong:
        return "Pong";
    }

    return "Unknown";
}

const char* to_string(PacketRejectReason reason)
{
    switch (reason) {
    case PacketRejectReason::None:
        return "None";
    case PacketRejectReason::TooShort:
        return "TooShort";
    case PacketRejectReason::BadMagic:
        return "BadMagic";
    case PacketRejectReason::BadVersion:
        return "BadVersion";
    case PacketRejectReason::UnknownType:
        return "UnknownType";
    case PacketRejectReason::NotPing:
        return "NotPing";
    case PacketRejectReason::NotPong:
        return "NotPong";
    }

    return "Unknown";
}

} // namespace mininet
