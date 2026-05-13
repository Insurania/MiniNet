#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mininet {

// 协议魔数，ASCII 是 "MNET"，用于快速过滤明显不是 MiniNet 的 UDP 包。
constexpr std::uint32_t kPacketMagic = 0x4D4E4554;

// 当前协议版本。接收端只接受同版本包，避免不同 header 格式混在一起解析。
constexpr std::uint8_t kProtocolVersion = 1;

// 固定 header 长度：magic(4) + version(1) + type(1) + sequence(4) + session_id(4) + ack(4) + ack_bits(4)。
constexpr std::size_t kPacketHeaderSize = 22;

// MiniNet 当前认识的包类型。UDP 只有字节，type 用来表达业务层语义。
enum class PacketType : std::uint8_t {
    Ping = 1,
    Pong = 2,
    ConnectRequest = 3,
    ConnectAccept = 4,
    Disconnect = 5,
    Heartbeat = 6,
};

// 包被拒绝的原因。调用方可以用它输出日志或在测试中判断具体失败点。
enum class PacketRejectReason {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    UnknownType,
    NotPing,
    NotPong,
    UnexpectedType,
    BadSessionId,
};

// MiniNet 数据包头。这个结构是内存表达，不直接按 struct 内存布局发送。
struct PacketHeader {
    // 协议魔数，合法包应等于 kPacketMagic。
    std::uint32_t magic = kPacketMagic;

    // 协议版本，合法包应等于 kProtocolVersion。
    std::uint8_t version = kProtocolVersion;

    // 业务包类型，例如 Ping、ConnectRequest、Heartbeat。
    PacketType type = PacketType::Ping;

    // 发送方递增序号。当前只用于匹配请求/响应，可靠传输留给后续 issue。
    std::uint32_t sequence = 0;

    // 逻辑连接 id。ConnectRequest 必须为 0，ConnectAccept 和连接内包必须为非 0。
    std::uint32_t session_id = 0;

    // 对端最近收到的本方 sequence；没有收到过任何包时为 0。
    std::uint32_t ack = 0;

    // 对 ack 前 32 个 sequence 的位图确认；bit0 表示 ack-1，bit31 表示 ack-32。
    std::uint32_t ack_bits = 0;
};

// 包校验结果。accepted 表示是否可进入业务逻辑，header 保存能解析出的头部信息。
struct PacketValidationResult {
    // true 表示包通过长度、magic、version、type 等基础校验。
    bool accepted = false;

    // 当 accepted=false 时说明拒绝原因。
    PacketRejectReason reason = PacketRejectReason::None;

    // 如果 header 可解析，这里保存解析结果，方便上层日志和状态机使用。
    std::optional<PacketHeader> header;
};

// C++17 没有 std::span，这里提供只读 byte view，避免复制 UDP 缓冲区。
class ByteView {
public:
    // 从裸指针和长度创建只读视图；调用方要保证这段内存在视图使用期间有效。
    ByteView(const std::uint8_t* data, std::size_t size)
        : data_(data)
        , size_(size)
    {
    }

    template <std::size_t Size>
    // 从固定数组创建视图，常用于 encode_packet_header 的结果。
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

    // 返回视图长度，解析和发送时都用它避免越界。
    std::size_t size() const { return size_; }

    // 读取指定位置的字节。调用方负责保证 index < size()。
    std::uint8_t operator[](std::size_t index) const { return data_[index]; }

private:
    // 指向外部字节内存，ByteView 不拥有这块内存。
    const std::uint8_t* data_ = nullptr;

    // 可读字节数量。
    std::size_t size_ = 0;
};

// 判断原始 type 字节是否是当前协议认识的 PacketType。
bool is_known_packet_type(std::uint8_t raw_type);

// 把内存里的 PacketHeader 编码成网络传输用的固定字节序列。
std::array<std::uint8_t, kPacketHeaderSize> encode_packet_header(const PacketHeader& header);

// 从收到的 UDP 字节中解析 header；长度不足或 type 未知时返回空。
std::optional<PacketHeader> decode_packet_header(ByteView bytes);

// 校验一个包是否是当前协议版本下的指定类型；不额外检查 session_id 语义。
PacketValidationResult validate_packet_type(ByteView bytes, PacketType expected_type);

// 服务端只接受合法 Ping；客户端只接受合法 Pong。
PacketValidationResult validate_ping_packet(ByteView bytes);

// 校验一个收到的包是否是合法 Pong。
PacketValidationResult validate_pong_packet(ByteView bytes);

// 校验 ConnectRequest；握手请求必须使用 session_id=0。
PacketValidationResult validate_connect_request_packet(ByteView bytes);

// 校验 ConnectAccept；握手接受必须携带非 0 session_id。
PacketValidationResult validate_connect_accept_packet(ByteView bytes);

// 校验 Heartbeat；这里只做包头和类型校验，session 匹配交给 connection 层。
PacketValidationResult validate_heartbeat_packet(ByteView bytes);

// 校验 Disconnect；这里只做包头和类型校验，session 匹配交给 connection 层。
PacketValidationResult validate_disconnect_packet(ByteView bytes);

// 把 PacketType 转成日志可读的英文文本。
const char* to_string(PacketType type);

// 把拒绝原因转成日志可读的英文文本。
const char* to_string(PacketRejectReason reason);

} // namespace mininet
