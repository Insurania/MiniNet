#include "mininet/reliable_message.hpp"

#include <algorithm>
#include <limits>

namespace mininet {

namespace {

constexpr std::size_t kReliableCountBytes = 1;
constexpr std::size_t kReliableMaxPayloadSize = kMaxDatagramSize - kPacketHeaderSize - kReliableCountBytes - kReliableMessageOverhead;

void append_u16_be(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    // 显式按网络常用的大端序写入，避免直接发送 C++ 对象内存造成平台差异。
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u32_be(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    // payload 内的整数编码也和 packet header 保持一致，便于抓包和调试。
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t read_u16_be(ByteView bytes, std::size_t offset)
{
    // 调用方会先检查长度，这里只负责把两个字节组合回 uint16。
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                      static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_u32_be(ByteView bytes, std::size_t offset)
{
    // 调用方会先检查长度，这里只负责把四个字节组合回 uint32。
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool can_fit_message(std::size_t used_bytes, std::size_t payload_size, std::size_t available_bytes)
{
    // used_bytes 已经包含 message_count 这个 1 字节；这里再加单条消息固定开销和 payload。
    return payload_size <= std::numeric_limits<std::uint16_t>::max() &&
           used_bytes + kReliableMessageOverhead + payload_size <= available_bytes;
}

} // namespace

std::vector<std::uint8_t> encode_reliable_data_payload(const std::vector<ReliableMessage>& messages)
{
    // ReliableData payload 格式：
    //   message_count:uint8
    //   repeated message_count times:
    //     message_id:uint32
    //     payload_size:uint16
    //     payload bytes
    // 注意这里不写 packet header；header 由 make_reliable_data_packet 统一追加。
    std::vector<std::uint8_t> bytes;
    bytes.push_back(static_cast<std::uint8_t>(messages.size()));
    for (const auto& message : messages) {
        append_u32_be(bytes, message.message_id);
        append_u16_be(bytes, static_cast<std::uint16_t>(message.payload.size()));
        bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    }
    return bytes;
}

ReliableDecodeResult decode_reliable_data_payload(ByteView bytes)
{
    // 解析时始终先检查剩余长度，再移动 offset。
    // 这样畸形包只会返回 ok=false，不会越界读取，也不会把半条消息交给上层。
    ReliableDecodeResult result;
    if (bytes.size() < kReliableCountBytes) {
        return result;
    }

    const auto message_count = bytes[0];
    std::size_t offset = kReliableCountBytes;
    result.messages.reserve(message_count);

    for (std::uint8_t index = 0; index < message_count; ++index) {
        if (bytes.size() - offset < kReliableMessageOverhead) {
            result.messages.clear();
            return result;
        }

        ReliableMessage message;
        message.message_id = read_u32_be(bytes, offset);
        offset += 4;
        const auto payload_size = read_u16_be(bytes, offset);
        offset += 2;

        if (bytes.size() - offset < payload_size) {
            result.messages.clear();
            return result;
        }

        message.payload.insert(message.payload.end(), bytes.data() + offset, bytes.data() + offset + payload_size);
        offset += payload_size;
        result.messages.push_back(message);
    }

    if (offset != bytes.size()) {
        result.messages.clear();
        return result;
    }

    result.ok = true;
    return result;
}

std::vector<std::uint8_t> make_reliable_data_packet(std::uint32_t sequence,
                                                    std::uint32_t session_id,
                                                    std::uint32_t ack,
                                                    std::uint32_t ack_bits,
                                                    const std::vector<ReliableMessage>& messages)
{
    // ReliableData 是连接建立后的普通连接内包。
    // 它的可靠性仍然依赖 packet 级 ACK：header.sequence 被 ACK 后，
    // ReliableSender 才能通过 packet->message 映射清理其中的消息。
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = PacketType::ReliableData;
    header.sequence = sequence;
    header.session_id = session_id;
    header.ack = ack;
    header.ack_bits = ack_bits;

    const auto encoded_header = encode_packet_header(header);
    auto packet = std::vector<std::uint8_t>(encoded_header.begin(), encoded_header.end());
    auto payload = encode_reliable_data_payload(messages);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

bool ReliableSender::enqueue(const std::vector<std::uint8_t>& payload, std::chrono::steady_clock::time_point)
{
    // 本 issue 不实现 fragmentation（分片）。
    // 如果单条 payload 连“空 ReliableData packet”都塞不下，就直接拒绝，避免永远 pending。
    if (payload.size() > kReliableMaxPayloadSize || payload.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    PendingMessage message;
    message.message_id = next_message_id_++;
    message.payload = payload;
    pending_.push_back(message);
    return true;
}

std::vector<ReliableMessage> ReliableSender::select_messages_for_packet(std::chrono::steady_clock::time_point now,
                                                                        std::size_t available_bytes) const
{
    // 选择本轮要发送的消息：
    // - 新消息从未进入 packet，立刻可选；
    // - 旧消息已经发送过但未 ACK，只有超过重发间隔才可选；
    // - 每条消息都必须完整塞进当前 packet，本 issue 不做拆分。
    // 重发不是复用旧 packet，而是把同一 message_id 放进一个新的 packet sequence。
    std::vector<ReliableMessage> selected;
    if (available_bytes < kReliableCountBytes) {
        return selected;
    }

    std::size_t used_bytes = kReliableCountBytes;
    for (const auto& pending : pending_) {
        if (selected.size() == std::numeric_limits<std::uint8_t>::max()) {
            break;
        }

        const auto due = !pending.has_been_sent || now - pending.last_sent_time >= kReliableResendInterval;
        if (!due || !can_fit_message(used_bytes, pending.payload.size(), available_bytes)) {
            continue;
        }

        ReliableMessage message;
        message.message_id = pending.message_id;
        message.payload = pending.payload;
        selected.push_back(message);
        used_bytes += kReliableMessageOverhead + pending.payload.size();
    }

    return selected;
}

void ReliableSender::mark_packet_sent(std::uint32_t sequence,
                                      const std::vector<ReliableMessage>& messages,
                                      std::chrono::steady_clock::time_point now)
{
    // 记录 packet sequence 到 message_id 的映射。
    // 之后 AckTracker 只告诉我们“哪个 packet sequence 被 ACK”，
    // 所以这里必须保存反查表，才能知道哪些 reliable message 可以视为 delivered。
    if (messages.empty()) {
        return;
    }

    SentPacketMessages packet;
    packet.sequence = sequence;
    for (const auto& message : messages) {
        packet.message_ids.push_back(message.message_id);
        for (auto& pending : pending_) {
            if (pending.message_id == message.message_id) {
                pending.has_been_sent = true;
                pending.last_sent_time = now;
                break;
            }
        }
    }
    packet_messages_.push_back(packet);
}

void ReliableSender::process_acked_packets(const std::vector<SentPacketRecord>& sent_packets)
{
    // packet 级 ACK 到来后，先找出所有被 ACK packet 携带过的 message_id。
    // 对同一 message_id 来说，只要任意一个携带它的 packet 被 ACK，就认为消息送达。
    // 随后从 pending_ 和所有 packet_messages_ 中清理它，避免再次重发。
    std::vector<std::uint32_t> delivered_message_ids;
    for (const auto& sent_packet : sent_packets) {
        if (!sent_packet.acked) {
            continue;
        }

        const auto found = std::find_if(packet_messages_.begin(), packet_messages_.end(), [&](const SentPacketMessages& packet) {
            return packet.sequence == sent_packet.sequence;
        });
        if (found != packet_messages_.end()) {
            delivered_message_ids.insert(delivered_message_ids.end(), found->message_ids.begin(), found->message_ids.end());
        }
    }

    if (delivered_message_ids.empty()) {
        return;
    }

    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const PendingMessage& message) {
                       return std::find(delivered_message_ids.begin(), delivered_message_ids.end(), message.message_id) !=
                              delivered_message_ids.end();
                   }),
                   pending_.end());

    for (auto& packet : packet_messages_) {
        packet.message_ids.erase(std::remove_if(packet.message_ids.begin(), packet.message_ids.end(), [&](std::uint32_t message_id) {
                                     return std::find(delivered_message_ids.begin(), delivered_message_ids.end(), message_id) !=
                                            delivered_message_ids.end();
                                 }),
                                 packet.message_ids.end());
    }

    packet_messages_.erase(std::remove_if(packet_messages_.begin(), packet_messages_.end(), [](const SentPacketMessages& packet) {
                               return packet.message_ids.empty();
                           }),
                           packet_messages_.end());
}

std::size_t ReliableSender::pending_count() const
{
    return pending_.size();
}

bool ReliableSender::is_pending(std::uint32_t message_id) const
{
    return std::find_if(pending_.begin(), pending_.end(), [&](const PendingMessage& message) {
               return message.message_id == message_id;
           }) != pending_.end();
}

ReliableReceiveResult ReliableReceiver::receive(const ReliableMessage& message)
{
    // Receiver 实例按 peer/session 独立保存，因此 message_id 只在同一条连接内去重。
    // insert 返回的 second 为 true 表示第一次插入，也就是上层应该真正处理这条 payload。
    ReliableReceiveResult result;
    result.message_id = message.message_id;
    result.payload = message.payload;
    result.should_process = received_message_ids_.insert(message.message_id).second;
    return result;
}

} // namespace mininet
