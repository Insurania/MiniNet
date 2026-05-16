#pragma once

#include "mininet/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mininet {

// Snapshot 在应用层的唯一编号；本 issue 不处理 uint32 回绕。
using SnapshotId = std::uint32_t;

// 服务器模拟 tick；用于让客户端知道该 snapshot 对应的服务端逻辑帧。
using ServerTick = std::uint32_t;

// 服务器时间戳，单位为毫秒；用于上层做时间轴和插值参考。
using ServerTimeMs = std::uint64_t;

// 实体唯一编号；同一个 entity_id 在不同 snapshot 中表示同一对象。
using EntityId = std::uint32_t;

// 单个 snapshot 最多携带的实体数量，避免 UDP datagram 过大。
constexpr std::size_t kMaxEntitiesPerSnapshot = 32;

// 客户端缓存的 snapshot 数量上限；超过后丢弃最旧 snapshot。
constexpr std::size_t kSnapshotBufferCapacity = 32;

// 二维 float 向量，用于位置和速度。网络编码时按 IEEE 754 binary32 位模式写入。
struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

// 单个实体在某个 snapshot 中的状态。velocity 只保存，不参与插值计算。
struct EntityState {
    EntityId entity_id = 0;
    Vec2f position;
    Vec2f velocity;
};

// 服务器发给客户端的一帧不可靠世界状态。
struct Snapshot {
    SnapshotId snapshot_id = 0;
    ServerTick server_tick = 0;
    ServerTimeMs server_time_ms = 0;
    std::vector<EntityState> entities;
};

// Snapshot payload 解码结果。ok=false 时 snapshot 内容不应被调用方使用。
struct SnapshotDecodeResult {
    bool ok = false;
    Snapshot snapshot;
};

// SnapshotBuffer 插入结果，便于测试和上层观察去重、过旧和淘汰行为。
enum class SnapshotInsertStatus {
    Inserted,
    Duplicate,
    TooOld,
    InsertedAndEvictedOldest,
};

// 把 Snapshot 编码成 PacketHeader 后面的 payload；实体数量超过限制时返回 nullopt。
std::optional<std::vector<std::uint8_t>> encode_snapshot_payload(const Snapshot& snapshot);

// 从 Snapshot payload 字节解码；长度不足、实体数量过多或尾部多余字节都会失败。
SnapshotDecodeResult decode_snapshot_payload(ByteView bytes);

// 构造完整 Snapshot datagram。Snapshot 不进入 ReliableSender，也没有 snapshot-level ACK。
std::optional<std::vector<std::uint8_t>> make_snapshot_packet(std::uint32_t sequence,
                                                              std::uint32_t session_id,
                                                              std::uint32_t ack,
                                                              std::uint32_t ack_bits,
                                                              const Snapshot& snapshot);

// 客户端按 snapshot_id 升序保存最近 snapshot，用于上层插值和测试观察。
class SnapshotBuffer {
public:
    // 插入 snapshot：重复 id 不插入；非空时 id <= 当前最旧 id 视为 TooOld。
    SnapshotInsertStatus insert(const Snapshot& snapshot);

    // 返回当前按 snapshot_id 升序保存的 snapshot 列表。
    const std::vector<Snapshot>& snapshots() const;

    // 返回当前缓存数量。
    std::size_t size() const;

private:
    // 升序缓存；容量超过 kSnapshotBufferCapacity 时移除 begin() 处最旧元素。
    std::vector<Snapshot> snapshots_;
};

// 在两个 snapshot 之间对同一个实体的位置做线性插值；任一 snapshot 缺实体则返回 nullopt。
std::optional<Vec2f> interpolate_entity_position(const Snapshot& older,
                                                 const Snapshot& newer,
                                                 EntityId entity_id,
                                                 float alpha);

// 60Hz update 的固定间隔，纯 duration helper，不启动线程或循环。
std::chrono::nanoseconds snapshot_update_interval_60hz();

// 20Hz snapshot 发送的固定间隔，纯 duration helper，不启动线程或循环。
std::chrono::nanoseconds snapshot_send_interval_20hz();

// elapsed 达到 60Hz 间隔时返回 true。
bool should_run_snapshot_update(std::chrono::steady_clock::duration elapsed);

// elapsed 达到 20Hz 间隔时返回 true。
bool should_send_snapshot(std::chrono::steady_clock::duration elapsed);

} // namespace mininet
