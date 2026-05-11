#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mininet {

// MiniNet 当前只有一个固定长度 header。
// 字段布局：
//   magic    4 bytes  用于快速判断“这是不是 MiniNet 包”
//   version  1 byte   协议版本，未来改协议时用来兼容或拒绝旧包
//   type     1 byte   包类型，例如 Ping / Pong
//   sequence 4 bytes  发送方递增序号，后续 ACK 和重传会继续使用
constexpr std::uint32_t kPacketMagic = 0x4D4E4554; // "MNET"
constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kPacketHeaderSize = 10;

enum class PacketType : std::uint8_t {
    Ping = 1,
    Pong = 2,
};

enum class PacketRejectReason {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    UnknownType,
    NotPing,
    NotPong,
};

struct PacketHeader {
    std::uint32_t magic = kPacketMagic;
    std::uint8_t version = kProtocolVersion;
    PacketType type = PacketType::Ping;
    std::uint32_t sequence = 0;
};

struct PacketValidationResult {
    bool accepted = false;
    PacketRejectReason reason = PacketRejectReason::None;
    std::optional<PacketHeader> header;
};

// GCC 8.1 缺少 C++20 的 std::span，所以这里用一个只读 byte view。
// 它不持有内存，只临时指向 array/vector 里的字节，调用者要保证原始数据仍然存在。
class ByteView {
public:
    ByteView(const std::uint8_t* data, std::size_t size)
        : data_(data)
        , size_(size)
    {
    }

    template <std::size_t Size>
    ByteView(const std::array<std::uint8_t, Size>& bytes)
        : data_(bytes.data())
        , size_(bytes.size())
    {
    }

    ByteView(const std::vector<std::uint8_t>& bytes)
        : data_(bytes.data())
        , size_(bytes.size())
    {
    }

    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }
    std::uint8_t operator[](std::size_t index) const { return data_[index]; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

bool is_known_packet_type(std::uint8_t raw_type);

// 把内存里的 PacketHeader 编码成网络传输用的字节序列。
std::array<std::uint8_t, kPacketHeaderSize> encode_packet_header(const PacketHeader& header);

// 从收到的 UDP 字节中解析 header。字节太短或 type 未知时返回空。
std::optional<PacketHeader> decode_packet_header(ByteView bytes);

// 服务端只接受合法 Ping；客户端只接受合法 Pong。
PacketValidationResult validate_ping_packet(ByteView bytes);

PacketValidationResult validate_pong_packet(ByteView bytes);

const char* to_string(PacketType type);

const char* to_string(PacketRejectReason reason);

} // namespace mininet
