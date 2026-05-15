#pragma once

#include "mininet/ack_tracker.hpp"
#include "mininet/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace mininet {

// MiniNet 可靠消息层使用的 UDP datagram 目标上限。
// 学习项目先固定为 1200 字节，避免靠近常见 MTU 后被 IP 分片。
constexpr std::size_t kMaxDatagramSize = 1200;

// 未被 ACK 的可靠消息再次进入新 packet 前需要等待的最小时间。
constexpr std::chrono::milliseconds kReliableResendInterval{250};

// ReliableData payload 中每条消息的固定开销：message_id(4) + payload_size(2)。
constexpr std::size_t kReliableMessageOverhead = 6;

// 一条可靠无序消息。发送端给每条消息分配 message_id，接收端用它做去重。
struct ReliableMessage {
    // 发送端分配的可靠消息 id；每个 ReliableSender 从 1 开始递增。
    std::uint32_t message_id = 0;

    // 上层应用传入的原始 payload 字节；可靠层不解释内容。
    std::vector<std::uint8_t> payload;
};

// 接收端处理一条 ReliableMessage 后返回的结果。
struct ReliableReceiveResult {
    // 收到的可靠消息 id，便于测试和日志观察。
    std::uint32_t message_id = 0;

    // true 表示这是当前 receiver 第一次看到该 message_id，上层应该处理 payload。
    // false 表示这是重传导致的重复消息，上层应该忽略 payload。
    bool should_process = false;

    // 上层应用 payload；即使重复收到，也保留出来方便测试观察。
    std::vector<std::uint8_t> payload;
};

// 解析 ReliableData payload 后的结果。
struct ReliableDecodeResult {
    // true 表示 payload 格式完整，message_count 和实际内容长度匹配。
    bool ok = false;

    // 按 packet 中出现顺序解析出的消息列表。
    std::vector<ReliableMessage> messages;
};

// 把多条可靠消息编码成 ReliableData 的 payload 字节，不包含 packet header。
// 格式为：message_count:uint8，然后重复 message_id:uint32 + payload_size:uint16 + payload bytes。
std::vector<std::uint8_t> encode_reliable_data_payload(const std::vector<ReliableMessage>& messages);

// 解析 ReliableData payload 字节。格式不完整、长度不匹配或尾部多字节时返回 ok=false。
ReliableDecodeResult decode_reliable_data_payload(ByteView bytes);

// 构造完整 ReliableData packet：先写普通连接包 header，再追加 reliable payload。
// sequence/ack/ack_bits 仍然使用 packet 级 ACK 机制，不引入单独 message ACK。
std::vector<std::uint8_t> make_reliable_data_packet(std::uint32_t sequence,
                                                    std::uint32_t session_id,
                                                    std::uint32_t ack,
                                                    std::uint32_t ack_bits,
                                                    const std::vector<ReliableMessage>& messages);

// 面向单个 peer/session 的可靠无序发送器。
// 它只负责排队、选择到期消息、记录 packet->message 映射、根据 packet ACK 清理 delivered 消息。
class ReliableSender {
public:
    // 排队一条 payload。过大的 payload 因为本 issue 不做 fragmentation，会直接返回 false。
    bool enqueue(const std::vector<std::uint8_t>& payload, std::chrono::steady_clock::time_point now);

    // 选择本轮应该塞进 packet 的消息。
    // 新消息会立即可选；已发送但未 ACK 的消息只有超过 kReliableResendInterval 才会再次可选。
    std::vector<ReliableMessage> select_messages_for_packet(std::chrono::steady_clock::time_point now,
                                                            std::size_t available_bytes) const;

    // 记录一个 packet sequence 携带了哪些 message_id，并更新这些消息的最后发送时间。
    void mark_packet_sent(std::uint32_t sequence,
                          const std::vector<ReliableMessage>& messages,
                          std::chrono::steady_clock::time_point now);

    // 根据 AckTracker 的 sent packet 历史，清理已经 delivered 的消息。
    // 只要任意一个携带该消息的 packet 被 ACK，该 message 就视为送达。
    void process_acked_packets(const std::vector<SentPacketRecord>& sent_packets);

    // 返回仍在等待 packet ACK 的消息数量。
    std::size_t pending_count() const;

    // 查询指定 message_id 是否仍在 pending 队列中。
    bool is_pending(std::uint32_t message_id) const;

private:
    // 发送端内部维护的 pending 消息状态。
    struct PendingMessage {
        // 发送端分配的可靠消息 id。
        std::uint32_t message_id = 0;

        // 上层应用 payload。
        std::vector<std::uint8_t> payload;

        // true 表示该消息至少已经进入过一个 packet。
        bool has_been_sent = false;

        // 最近一次把该消息放入 packet 的时间，用于 250ms 重发判断。
        std::chrono::steady_clock::time_point last_sent_time{};
    };

    // 一个已发送 packet 和它携带的可靠消息 id 列表。
    struct SentPacketMessages {
        // packet 级 sequence number。
        std::uint32_t sequence = 0;

        // 该 packet 携带的 reliable message id。
        std::vector<std::uint32_t> message_ids;
    };

    // 下一个要分配的 reliable message id；每个发送器独立从 1 开始。
    std::uint32_t next_message_id_ = 1;

    // 等待 packet ACK 的消息队列。
    std::vector<PendingMessage> pending_;

    // 已发送 packet 到 message_id 的映射，用于 ACK 到来时反查哪些消息已送达。
    std::vector<SentPacketMessages> packet_messages_;
};

// 面向单个 peer/session 的可靠无序接收去重器。
class ReliableReceiver {
public:
    // 处理一条已解码消息，并告诉上层这条消息是否应该第一次被处理。
    ReliableReceiveResult receive(const ReliableMessage& message);

private:
    // 当前 peer/session 已经处理过的 message_id 集合。
    std::unordered_set<std::uint32_t> received_message_ids_;
};

} // namespace mininet
