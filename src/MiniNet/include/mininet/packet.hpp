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
// 这里的大小和顺序就是“线上的协议格式”，修改时要同步更新测试和文档。

// 协议魔数。ASCII 是 "MNET"，放在每个包最前面，用来过滤明显不是 MiniNet 的 UDP 包。
constexpr std::uint32_t kPacketMagic = 0x4D4E4554; // "MNET"

// 当前协议版本。接收端只接受同版本包，避免不同协议格式混在一起解析。
constexpr std::uint8_t kProtocolVersion = 1;

// 当前 header 固定 10 字节：magic(4) + version(1) + type(1) + sequence(4)。
constexpr std::size_t kPacketHeaderSize = 10;

// 数据包类型。UDP 本身只传字节，不知道业务含义，所以要在 header 里显式写 type。
enum class PacketType : std::uint8_t {
    Ping = 1,
    Pong = 2,
};

// 包被拒绝的原因。测试和日志使用它来说明“为什么丢包”，而不是只返回 false。
enum class PacketRejectReason {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    UnknownType,
    NotPing,
    NotPong,
};

// MiniNet 数据包头。
// 注意这个 struct 是内存里的表达，不直接拿它做网络发送，避免 C++ 结构体 padding 和字节序问题。
struct PacketHeader {
    // 协议魔数，合法包应该等于 kPacketMagic。
    std::uint32_t magic = kPacketMagic;

    // 协议版本，合法包应该等于 kProtocolVersion。
    std::uint8_t version = kProtocolVersion;

    // 业务包类型，当前只有 Ping 和 Pong。
    PacketType type = PacketType::Ping;

    // 发送方递增序号。当前用于匹配 Ping/Pong，后续会用于 ACK 和重传。
    std::uint32_t sequence = 0;
};

// 包校验结果。
// accepted 表示是否接受进入业务逻辑；reason 说明拒绝原因；header 保存解析出的头部信息。
struct PacketValidationResult {
    // true 表示包通过了长度、magic、version、type 等校验。
    bool accepted = false;

    // 当 accepted=false 时，这里说明具体拒绝原因。
    PacketRejectReason reason = PacketRejectReason::None;

    // 如果字节长度足够且 type 可识别，这里会保存解析出的 header，方便日志输出 sequence。
    std::optional<PacketHeader> header;
};

// GCC 8.1 缺少 C++20 的 std::span，所以这里用一个只读 byte view。
// 它不持有内存，只临时指向 array/vector 里的字节，调用者要保证原始数据仍然存在。
class ByteView {
public:
    // 从裸指针和长度创建只读视图。调用者要保证 data 指向的内存在视图使用期间有效。
    ByteView(const std::uint8_t* data, std::size_t size)
        : data_(data)
        , size_(size)
    {
    }

    template <std::size_t Size>
    // 从固定大小字节数组创建视图，常用于 encode_packet_header 的结果。
    ByteView(const std::array<std::uint8_t, Size>& bytes)
        : data_(bytes.data())
        , size_(bytes.size())
    {
    }

    // 从 vector 创建视图，常用于 UDP 接收缓冲区和测试构造的数据包。
    ByteView(const std::vector<std::uint8_t>& bytes)
        : data_(bytes.data())
        , size_(bytes.size())
    {
    }

    // 返回视图起始地址，send_to 需要它把数据交给系统 socket API。
    const std::uint8_t* data() const { return data_; }

    // 返回视图长度，解析和发送时都要用它避免越界。
    std::size_t size() const { return size_; }

    // 读取指定位置的字节。调用者负责保证 index < size()。
    std::uint8_t operator[](std::size_t index) const { return data_[index]; }

private:
    // 指向外部字节内存；ByteView 不拥有这块内存。
    const std::uint8_t* data_ = nullptr;

    // 可读字节数量。
    std::size_t size_ = 0;
};

// 判断原始 type 字节是否是当前协议认识的 PacketType。
bool is_known_packet_type(std::uint8_t raw_type);

// 把内存里的 PacketHeader 编码成网络传输用的字节序列。
std::array<std::uint8_t, kPacketHeaderSize> encode_packet_header(const PacketHeader& header);

// 从收到的 UDP 字节中解析 header。字节太短或 type 未知时返回空。
std::optional<PacketHeader> decode_packet_header(ByteView bytes);

// 服务端只接受合法 Ping；客户端只接受合法 Pong。
PacketValidationResult validate_ping_packet(ByteView bytes);

// 校验一个收到的包是否是当前协议版本下的合法 Pong。
PacketValidationResult validate_pong_packet(ByteView bytes);

// 把 PacketType 转成日志可读的英文文本。
const char* to_string(PacketType type);

// 把拒绝原因转成日志可读的英文文本。
const char* to_string(PacketRejectReason reason);

} // namespace mininet
