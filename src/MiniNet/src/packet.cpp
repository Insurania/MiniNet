#include "mininet/packet.hpp"

namespace mininet {

namespace {

void write_u32_be(std::array<std::uint8_t, kPacketHeaderSize>& bytes, std::size_t offset, std::uint32_t value)
{
    // 网络协议使用 big-endian，也叫 network byte order。
    // 这样不同 CPU 架构之间传输 uint32 时不会因为本机字节序不同而解析错误。
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint32_t read_u32_be(ByteView bytes, std::size_t offset)
{
    // write_u32_be 的反向操作。调用方只会在确认 header 长度足够后进入这里。
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

PacketValidationResult validate_basic_packet(ByteView bytes)
{
    // 基础校验按“长度 -> type -> magic -> version”进行。
    // type 先从原始字节判断，避免把未知值强转成枚举后继续进入业务逻辑。
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

    return {true, PacketRejectReason::None, header};
}

} // namespace

bool is_known_packet_type(std::uint8_t raw_type)
{
    return raw_type == static_cast<std::uint8_t>(PacketType::Ping) ||
           raw_type == static_cast<std::uint8_t>(PacketType::Pong) ||
           raw_type == static_cast<std::uint8_t>(PacketType::ConnectRequest) ||
           raw_type == static_cast<std::uint8_t>(PacketType::ConnectAccept) ||
           raw_type == static_cast<std::uint8_t>(PacketType::Disconnect) ||
           raw_type == static_cast<std::uint8_t>(PacketType::Heartbeat);
}

std::array<std::uint8_t, kPacketHeaderSize> encode_packet_header(const PacketHeader& header)
{
    // UDP 发送的是字节，不是 C++ struct。
    // 显式编码可以避免结构体 padding 和平台字节序差异。
    std::array<std::uint8_t, kPacketHeaderSize> bytes{};
    write_u32_be(bytes, 0, header.magic);
    bytes[4] = header.version;
    bytes[5] = static_cast<std::uint8_t>(header.type);
    write_u32_be(bytes, 6, header.sequence);
    write_u32_be(bytes, 10, header.session_id);
    return bytes;
}

std::optional<PacketHeader> decode_packet_header(ByteView bytes)
{
    // 这里只负责“解析”，不判断 magic/version 是否匹配当前协议。
    // 具体是否接收这个包，由 validate_* 函数和 connection 状态机决定。
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
    header.session_id = read_u32_be(bytes, 10);
    return header;
}

PacketValidationResult validate_packet_type(ByteView bytes, PacketType expected_type)
{
    // 先做所有包共用的 header 合法性检查，再检查本次调用期望的 type。
    // session_id 的业务含义只在握手包中检查，连接内匹配交给 connection 层。
    auto validation = validate_basic_packet(bytes);
    if (!validation.accepted) {
        return validation;
    }

    if (validation.header->type != expected_type) {
        if (expected_type == PacketType::Ping) {
            return {false, PacketRejectReason::NotPing, validation.header};
        }
        if (expected_type == PacketType::Pong) {
            return {false, PacketRejectReason::NotPong, validation.header};
        }
        return {false, PacketRejectReason::UnexpectedType, validation.header};
    }

    return validation;
}

PacketValidationResult validate_ping_packet(ByteView bytes)
{
    return validate_packet_type(bytes, PacketType::Ping);
}

PacketValidationResult validate_pong_packet(ByteView bytes)
{
    return validate_packet_type(bytes, PacketType::Pong);
}

PacketValidationResult validate_connect_request_packet(ByteView bytes)
{
    auto validation = validate_packet_type(bytes, PacketType::ConnectRequest);
    if (!validation.accepted) {
        return validation;
    }

    if (validation.header->session_id != 0) {
        return {false, PacketRejectReason::BadSessionId, validation.header};
    }

    return validation;
}

PacketValidationResult validate_connect_accept_packet(ByteView bytes)
{
    auto validation = validate_packet_type(bytes, PacketType::ConnectAccept);
    if (!validation.accepted) {
        return validation;
    }

    if (validation.header->session_id == 0) {
        return {false, PacketRejectReason::BadSessionId, validation.header};
    }

    return validation;
}

PacketValidationResult validate_heartbeat_packet(ByteView bytes)
{
    return validate_packet_type(bytes, PacketType::Heartbeat);
}

PacketValidationResult validate_disconnect_packet(ByteView bytes)
{
    return validate_packet_type(bytes, PacketType::Disconnect);
}

const char* to_string(PacketType type)
{
    switch (type) {
    case PacketType::Ping:
        return "Ping";
    case PacketType::Pong:
        return "Pong";
    case PacketType::ConnectRequest:
        return "ConnectRequest";
    case PacketType::ConnectAccept:
        return "ConnectAccept";
    case PacketType::Disconnect:
        return "Disconnect";
    case PacketType::Heartbeat:
        return "Heartbeat";
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
    case PacketRejectReason::UnexpectedType:
        return "UnexpectedType";
    case PacketRejectReason::BadSessionId:
        return "BadSessionId";
    }

    return "Unknown";
}

} // namespace mininet
