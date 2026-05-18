#pragma once

#include "mininet/packet.hpp"
#include "mininet/udp_socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace mininet {

// NetworkSimulator 的运行参数。模拟器只处理原始字节、UDP 端点和调用方传入的虚拟时间。
struct NetworkSimulatorConfig {
    // 每次 send 丢包的概率，合法范围是 [0, 1]。非法值会被拒绝，不会被 clamp。
    double loss_rate = 0.0;

    // 每次成功发送后额外复制一份 packet 的概率，合法范围是 [0, 1]。最多复制一次，不递归。
    double duplicate_rate = 0.0;

    // 每个入队 packet 可抽到的最小延迟。delivery_time = now + sampled_latency。
    std::chrono::milliseconds min_latency{0};

    // 每个入队 packet 可抽到的最大延迟。必须大于等于 min_latency。
    std::chrono::milliseconds max_latency{0};

    // 伪随机种子。同一实现、同一 seed、同一输入序列会得到一致结果。
    std::uint32_t random_seed = 0;
};

// 配置被拒绝的原因。None 表示配置可接受。
enum class NetworkSimulatorConfigRejectReason {
    None,
    InvalidLossRate,
    InvalidDuplicateRate,
    InvalidLatencyRange,
};

// 配置校验结果。accepted=false 时 reason 保存第一个拒绝原因。
struct NetworkSimulatorConfigValidation {
    // true 表示配置合法，可以用来创建 NetworkSimulator。
    bool accepted = false;

    // 配置被拒绝的原因；accepted=true 时为 None。
    NetworkSimulatorConfigRejectReason reason = NetworkSimulatorConfigRejectReason::None;
};

// 校验 NetworkSimulatorConfig。非法配置直接 reject，不自动修正调用方传入的值。
NetworkSimulatorConfigValidation validate_network_simulator_config(const NetworkSimulatorConfig& config);

// 已进入模拟网络队列的一份 packet。它只保存原始字节，不解释 MiniNet 协议内容。
struct QueuedPacket {
    // 发送方 UDP endpoint。
    UdpEndpoint from;

    // 接收方 UDP endpoint。poll(receiver, now) 会用 address + port 匹配这里。
    UdpEndpoint to;

    // packet 原始字节副本。模拟器不解析、不修改这段内容。
    std::vector<std::uint8_t> bytes;

    // 这份 packet 在虚拟时间线上可以被接收方取出的时间。
    std::chrono::steady_clock::time_point delivery_time{};
};

// 可确定性网络模拟器，用于在测试或 demo 中模拟 UDP 丢包、重复和延迟。
class NetworkSimulator {
public:
    // 通过校验后的配置创建模拟器。配置非法时返回 nullopt，避免绕过校验构造对象。
    static std::optional<NetworkSimulator> create(const NetworkSimulatorConfig& config);

    // 模拟一次 UDP sendto。bytes 会立即复制，调用方随后修改原缓冲区不会影响队列内容。
    void send(const UdpEndpoint& from,
              const UdpEndpoint& to,
              ByteView bytes,
              std::chrono::steady_clock::time_point now);

    // 取出所有已经投递到 receiver 的 packet，并从内部队列移除。
    // 返回顺序是 delivery_time 升序；delivery_time 相同则按入队顺序升序。
    std::vector<QueuedPacket> poll(const UdpEndpoint& receiver, std::chrono::steady_clock::time_point now);

    // 返回尚未被 poll 移除的 packet 数量，便于测试观察网络中仍在飞行的数据。
    std::size_t in_flight_count() const;

private:
    struct ConstructionTag {
    };

    // 私有构造要求调用方走 create()，这样非法配置不能绕过 validate_network_simulator_config。
    NetworkSimulator(const NetworkSimulatorConfig& config, ConstructionTag);

    // 内部队列项，比公开 QueuedPacket 多记录入队顺序，用于稳定排序相同投递时间的 packet。
    struct QueuedEntry {
        QueuedPacket packet;
        std::uint64_t enqueue_order = 0;
    };

    // 按 address + port 比较 endpoint，避免给 UdpEndpoint 增加全局 operator==。
    static bool same_endpoint(const UdpEndpoint& left, const UdpEndpoint& right);

    // 从配置的 [min_latency, max_latency] 闭区间内抽样一个延迟。
    std::chrono::milliseconds sample_latency();

    // 把一份 packet 副本加入内部队列，并记录当前递增的入队顺序。
    void enqueue_packet(const UdpEndpoint& from,
                        const UdpEndpoint& to,
                        ByteView bytes,
                        std::chrono::steady_clock::time_point delivery_time);

    // 创建时保存的合法配置。
    NetworkSimulatorConfig config_;

    // 伪随机数引擎。使用 mt19937 以便同一实现和 seed 下行为可重复。
    std::mt19937 rng_;

    // 概率判定分布，用于 loss 和 duplicate 判定。
    std::uniform_real_distribution<double> probability_distribution_;

    // 尚未被 receiver poll 取走的 packet 队列。
    std::vector<QueuedEntry> queue_;

    // 递增入队序号，用于 delivery_time 相同时保持稳定顺序。
    std::uint64_t next_enqueue_order_ = 0;
};

} // namespace mininet
