#include "mininet/network_simulator.hpp"

#include <algorithm>

namespace mininet {

namespace {

bool is_probability(double value)
{
    return value >= 0.0 && value <= 1.0;
}

std::vector<std::uint8_t> copy_bytes(ByteView bytes)
{
    std::vector<std::uint8_t> copied(bytes.size());
    if (bytes.size() != 0) {
        std::copy(bytes.data(), bytes.data() + bytes.size(), copied.begin());
    }
    return copied;
}

} // namespace

NetworkSimulatorConfigValidation validate_network_simulator_config(const NetworkSimulatorConfig& config)
{
    // 配置错误直接 reject，不做 clamp。这样调用方能尽早发现测试参数写错，
    // 不会把 1.5 这类值静默改成 1.0 后掩盖问题。
    if (!is_probability(config.loss_rate)) {
        return {false, NetworkSimulatorConfigRejectReason::InvalidLossRate};
    }

    if (!is_probability(config.duplicate_rate)) {
        return {false, NetworkSimulatorConfigRejectReason::InvalidDuplicateRate};
    }

    if (config.min_latency > config.max_latency) {
        return {false, NetworkSimulatorConfigRejectReason::InvalidLatencyRange};
    }

    return {true, NetworkSimulatorConfigRejectReason::None};
}

std::optional<NetworkSimulator> NetworkSimulator::create(const NetworkSimulatorConfig& config)
{
    const auto validation = validate_network_simulator_config(config);
    if (!validation.accepted) {
        return std::nullopt;
    }

    return NetworkSimulator(config, ConstructionTag{});
}

NetworkSimulator::NetworkSimulator(const NetworkSimulatorConfig& config, ConstructionTag)
    : config_(config)
    , rng_(config.random_seed)
    , probability_distribution_(0.0, 1.0)
{
}

void NetworkSimulator::send(const UdpEndpoint& from,
                            const UdpEndpoint& to,
                            ByteView bytes,
                            std::chrono::steady_clock::time_point now)
{
    // 固定随机消耗顺序：
    // 1. loss 判定；
    // 2. 未丢弃时为原始 packet 抽样 latency 并入队；
    // 3. duplicate 判定；
    // 4. duplicate 成立时为副本独立抽样 latency 并入队。
    const auto loss_roll = probability_distribution_(rng_);
    if (loss_roll < config_.loss_rate) {
        return;
    }

    const auto original_latency = sample_latency();
    enqueue_packet(from, to, bytes, now + original_latency);

    const auto duplicate_roll = probability_distribution_(rng_);
    if (duplicate_roll < config_.duplicate_rate) {
        const auto duplicate_latency = sample_latency();
        enqueue_packet(from, to, bytes, now + duplicate_latency);
    }
}

std::vector<QueuedPacket> NetworkSimulator::poll(const UdpEndpoint& receiver,
                                                 std::chrono::steady_clock::time_point now)
{
    struct ReadyEntry {
        QueuedPacket packet;
        std::uint64_t enqueue_order = 0;
    };

    std::vector<ReadyEntry> ready;
    std::vector<QueuedEntry> remaining;
    ready.reserve(queue_.size());
    remaining.reserve(queue_.size());

    // poll 同时负责过滤和移除。匹配 receiver 且到期的项进入 ready，
    // 其他项保留在 remaining，最后整体替换内部队列。
    for (auto& entry : queue_) {
        if (same_endpoint(entry.packet.to, receiver) && entry.packet.delivery_time <= now) {
            ready.push_back(ReadyEntry{std::move(entry.packet), entry.enqueue_order});
        } else {
            remaining.push_back(std::move(entry));
        }
    }
    queue_ = std::move(remaining);

    // 返回顺序不依赖内部 vector 当前排列：先按投递时间，再按入队顺序。
    std::sort(ready.begin(), ready.end(), [](const ReadyEntry& left, const ReadyEntry& right) {
        if (left.packet.delivery_time != right.packet.delivery_time) {
            return left.packet.delivery_time < right.packet.delivery_time;
        }
        return left.enqueue_order < right.enqueue_order;
    });

    std::vector<QueuedPacket> packets;
    packets.reserve(ready.size());
    for (auto& entry : ready) {
        packets.push_back(std::move(entry.packet));
    }
    return packets;
}

std::size_t NetworkSimulator::in_flight_count() const
{
    return queue_.size();
}

bool NetworkSimulator::same_endpoint(const UdpEndpoint& left, const UdpEndpoint& right)
{
    return left.address == right.address && left.port == right.port;
}

std::chrono::milliseconds NetworkSimulator::sample_latency()
{
    const auto min_count = config_.min_latency.count();
    const auto max_count = config_.max_latency.count();
    std::uniform_int_distribution<std::chrono::milliseconds::rep> latency_distribution(min_count, max_count);
    return std::chrono::milliseconds(latency_distribution(rng_));
}

void NetworkSimulator::enqueue_packet(const UdpEndpoint& from,
                                      const UdpEndpoint& to,
                                      ByteView bytes,
                                      std::chrono::steady_clock::time_point delivery_time)
{
    QueuedPacket packet;
    packet.from = from;
    packet.to = to;
    packet.bytes = copy_bytes(bytes);
    packet.delivery_time = delivery_time;

    queue_.push_back(QueuedEntry{std::move(packet), next_enqueue_order_++});
}

} // namespace mininet
