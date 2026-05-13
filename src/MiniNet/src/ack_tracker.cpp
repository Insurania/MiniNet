#include "mininet/ack_tracker.hpp"

#include <algorithm>

namespace mininet {

namespace {

bool sequence_equals(std::uint32_t left, std::uint32_t right)
{
    return left == right;
}

bool is_sequence_acked_by_bits(std::uint32_t sequence, std::uint32_t ack, std::uint32_t ack_bits)
{
    // ack_bits 只覆盖 ack 之前的 32 个 sequence。
    // uint32 减法天然按 2^32 回绕，正好符合协议对 sequence wrap-around 的要求。
    const auto distance = ack - sequence;
    if (distance == 0 || distance > 32) {
        return false;
    }

    const auto bit_index = distance - 1;
    return (ack_bits & (std::uint32_t{1} << bit_index)) != 0;
}

} // namespace

bool sequence_greater_than(std::uint32_t a, std::uint32_t b)
{
    // 常见的半区间比较：当 a-b 落在 uint32 空间前半段时认为 a 更新。
    // 相等必须返回 false，避免重复包错误推进 latest_received_sequence_。
    return a != b && static_cast<std::uint32_t>(a - b) < 0x80000000u;
}

std::uint32_t AckTracker::allocate_send_sequence()
{
    // 自然溢出就是协议需要的 uint32 回绕，不额外跳过 0。
    const auto sequence = next_send_sequence_;
    ++next_send_sequence_;
    return sequence;
}

void AckTracker::record_sent(std::uint32_t sequence, std::chrono::steady_clock::time_point sent_time)
{
    // 只记录连接内包；握手包不通过这里，避免把握手变成可靠传输语义。
    SentPacketRecord record;
    record.sequence = sequence;
    record.sent_time = sent_time;
    sent_packets_.push_back(record);
}

void AckTracker::record_received(std::uint32_t sequence)
{
    // 收到重复包不重复写入历史，但仍允许它参与后续 ACK 生成。
    const auto exists = std::find(received_sequences_.begin(), received_sequences_.end(), sequence) != received_sequences_.end();
    if (!exists) {
        received_sequences_.push_back(sequence);
    }

    if (!has_received_sequence_ || sequence_greater_than(sequence, latest_received_sequence_)) {
        latest_received_sequence_ = sequence;
        has_received_sequence_ = true;
    }

    // ACK 位图只需要最近 32 个旧 sequence；保留 64 个给乱序和重复包一点余量。
    if (received_sequences_.size() > 64) {
        received_sequences_.erase(received_sequences_.begin(), received_sequences_.begin() + (received_sequences_.size() - 64));
    }
}

AckState AckTracker::make_ack_state() const
{
    AckState state;
    if (!has_received_sequence_) {
        return state;
    }

    state.has_received_sequence = true;
    state.ack = latest_received_sequence_;

    for (const auto sequence : received_sequences_) {
        const auto distance = state.ack - sequence;
        if (distance >= 1 && distance <= 32) {
            state.ack_bits |= std::uint32_t{1} << (distance - 1);
        }
    }

    return state;
}

void AckTracker::process_ack(std::uint32_t ack, std::uint32_t ack_bits)
{
    // ack 确认单个最新包，ack_bits 补充确认它之前最多 32 个包。
    // 全零 ACK 代表对端还没有接收历史，必须跳过，避免误确认回绕后的 sequence 0。
    if (ack == 0 && ack_bits == 0) {
        return;
    }

    for (auto& record : sent_packets_) {
        if (record.acked) {
            continue;
        }

        if (sequence_equals(record.sequence, ack) || is_sequence_acked_by_bits(record.sequence, ack, ack_bits)) {
            record.acked = true;
        }
    }
}

const std::vector<SentPacketRecord>& AckTracker::sent_packets() const
{
    return sent_packets_;
}

} // namespace mininet
