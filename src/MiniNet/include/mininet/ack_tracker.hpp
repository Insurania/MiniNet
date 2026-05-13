#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace mininet {

// 判断 a 是否比 b 更新；使用 uint32 回绕规则，相等时返回 false。
bool sequence_greater_than(std::uint32_t a, std::uint32_t b);

// 已发送包的轻量记录；当前只用于确认状态，后续可靠传输可在此基础上重传。
struct SentPacketRecord {
    // 本地发送时分配的 sequence。
    std::uint32_t sequence = 0;

    // 发送发生的 steady_clock 时间，用于未来计算 RTT 或重传间隔。
    std::chrono::steady_clock::time_point sent_time{};

    // true 表示对端已经通过 ack 或 ack_bits 确认收到该包。
    bool acked = false;
};

// 准备写入 PacketHeader 的确认状态。
struct AckState {
    // true 表示本端至少收到过一个连接内包，ack/ack_bits 才有确认语义。
    bool has_received_sequence = false;

    // 最近收到的 sequence。
    std::uint32_t ack = 0;

    // ack 之前 32 个 sequence 的接收位图；bit0 表示 ack-1。
    std::uint32_t ack_bits = 0;
};

// 维护单条连接上的发送 sequence、接收历史和发送确认状态。
class AckTracker {
public:
    // 分配下一个本地发送 sequence；初始值为 1，之后按 uint32 自然回绕。
    std::uint32_t allocate_send_sequence();

    // 记录一个已经发出的连接内包，等待后续 process_ack 标记确认。
    void record_sent(std::uint32_t sequence, std::chrono::steady_clock::time_point sent_time);

    // 记录一个合法收到的连接内包 sequence，用于生成 ack/ack_bits。
    void record_received(std::uint32_t sequence);

    // 生成当前应写入出站 header 的 ACK 状态；未收到过包时返回无确认语义的零值。
    AckState make_ack_state() const;

    // 处理对端发来的 ack/ack_bits，把本地 sent history 中已确认的包标记为 acked。
    void process_ack(std::uint32_t ack, std::uint32_t ack_bits);

    // 返回发送历史，便于测试和调试观察确认状态。
    const std::vector<SentPacketRecord>& sent_packets() const;

private:
    // 下一个要分配给出站连接内包的 sequence。
    std::uint32_t next_send_sequence_ = 1;

    // true 表示已经收到过至少一个连接内 sequence。
    bool has_received_sequence_ = false;

    // 最近收到的 sequence，按 sequence_greater_than 规则更新。
    std::uint32_t latest_received_sequence_ = 0;

    // 最近收到的 sequence 历史；保留小窗口即可生成 32 位 ACK 位图。
    std::vector<std::uint32_t> received_sequences_;

    // 本地已发送但未必已确认的包记录。
    std::vector<SentPacketRecord> sent_packets_;
};

} // namespace mininet
