#include "mininet/snapshot.hpp"

#include "mininet/reliable_message.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace mininet {

namespace {

constexpr std::size_t kSnapshotBasePayloadSize = 4 + 4 + 8 + 4;
constexpr std::size_t kSnapshotEntityPayloadSize = 4 + 4 + 4 + 4 + 4;

void append_u32_be(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    // 所有多字节整数都显式写成 network byte order，避免依赖本机 CPU 字节序。
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u64_be(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    // uint64 同样按大端序逐字节写入，保证跨平台解码一致。
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint32_t read_u32_be(ByteView bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_u64_be(ByteView bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

std::uint32_t float_to_bits(float value)
{
    // C++17 没有 std::bit_cast；memcpy 只复制 float 的 IEEE754 binary32 位模式，不发送 struct 内存布局。
    static_assert(sizeof(float) == sizeof(std::uint32_t), "MiniNet snapshot requires 32-bit float");
    static_assert(std::numeric_limits<float>::is_iec559, "MiniNet snapshot requires IEEE754 float");

    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_to_float(std::uint32_t bits)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t), "MiniNet snapshot requires 32-bit float");
    static_assert(std::numeric_limits<float>::is_iec559, "MiniNet snapshot requires IEEE754 float");

    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void append_float_be(std::vector<std::uint8_t>& bytes, float value)
{
    // float 先转成 binary32 位模式，再按 uint32 network byte order 写入。
    append_u32_be(bytes, float_to_bits(value));
}

float read_float_be(ByteView bytes, std::size_t offset)
{
    // 读取时先拿到 big-endian uint32 位模式，再还原为 float。
    return bits_to_float(read_u32_be(bytes, offset));
}

const EntityState* find_entity(const Snapshot& snapshot, EntityId entity_id)
{
    const auto found = std::find_if(snapshot.entities.begin(), snapshot.entities.end(), [&](const EntityState& entity) {
        return entity.entity_id == entity_id;
    });
    if (found == snapshot.entities.end()) {
        return nullptr;
    }
    return &*found;
}

} // namespace

std::optional<std::vector<std::uint8_t>> encode_snapshot_payload(const Snapshot& snapshot)
{
    if (snapshot.entities.size() > kMaxEntitiesPerSnapshot) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kSnapshotBasePayloadSize + snapshot.entities.size() * kSnapshotEntityPayloadSize);

    append_u32_be(bytes, snapshot.snapshot_id);
    append_u32_be(bytes, snapshot.server_tick);
    append_u64_be(bytes, snapshot.server_time_ms);
    append_u32_be(bytes, static_cast<std::uint32_t>(snapshot.entities.size()));

    for (const auto& entity : snapshot.entities) {
        append_u32_be(bytes, entity.entity_id);
        append_float_be(bytes, entity.position.x);
        append_float_be(bytes, entity.position.y);
        append_float_be(bytes, entity.velocity.x);
        append_float_be(bytes, entity.velocity.y);
    }

    return bytes;
}

SnapshotDecodeResult decode_snapshot_payload(ByteView bytes)
{
    SnapshotDecodeResult result;
    if (bytes.size() < kSnapshotBasePayloadSize) {
        return result;
    }

    std::size_t offset = 0;
    result.snapshot.snapshot_id = read_u32_be(bytes, offset);
    offset += 4;
    result.snapshot.server_tick = read_u32_be(bytes, offset);
    offset += 4;
    result.snapshot.server_time_ms = read_u64_be(bytes, offset);
    offset += 8;

    const auto entity_count = read_u32_be(bytes, offset);
    offset += 4;
    if (entity_count > kMaxEntitiesPerSnapshot) {
        result.snapshot = Snapshot{};
        return result;
    }

    const auto expected_size = kSnapshotBasePayloadSize + static_cast<std::size_t>(entity_count) * kSnapshotEntityPayloadSize;
    // Snapshot payload 必须刚好匹配 entity_count；截断和尾部多余字节都按畸形包处理。
    if (bytes.size() != expected_size) {
        result.snapshot = Snapshot{};
        return result;
    }

    result.snapshot.entities.reserve(static_cast<std::size_t>(entity_count));
    for (std::uint32_t index = 0; index < entity_count; ++index) {
        EntityState entity;
        entity.entity_id = read_u32_be(bytes, offset);
        offset += 4;
        entity.position.x = read_float_be(bytes, offset);
        offset += 4;
        entity.position.y = read_float_be(bytes, offset);
        offset += 4;
        entity.velocity.x = read_float_be(bytes, offset);
        offset += 4;
        entity.velocity.y = read_float_be(bytes, offset);
        offset += 4;
        result.snapshot.entities.push_back(entity);
    }

    result.ok = true;
    return result;
}

std::optional<std::vector<std::uint8_t>> make_snapshot_packet(std::uint32_t sequence,
                                                              std::uint32_t session_id,
                                                              std::uint32_t ack,
                                                              std::uint32_t ack_bits,
                                                              const Snapshot& snapshot)
{
    auto payload = encode_snapshot_payload(snapshot);
    if (!payload.has_value()) {
        return std::nullopt;
    }

    if (kPacketHeaderSize + payload->size() > kMaxDatagramSize) {
        return std::nullopt;
    }

    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = PacketType::Snapshot;
    header.sequence = sequence;
    header.session_id = session_id;
    header.ack = ack;
    header.ack_bits = ack_bits;

    const auto encoded_header = encode_packet_header(header);
    auto packet = std::vector<std::uint8_t>(encoded_header.begin(), encoded_header.end());
    packet.insert(packet.end(), payload->begin(), payload->end());
    return packet;
}

SnapshotInsertStatus SnapshotBuffer::insert(const Snapshot& snapshot)
{
    const auto duplicate = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const Snapshot& existing) {
        return existing.snapshot_id == snapshot.snapshot_id;
    });
    if (duplicate != snapshots_.end()) {
        return SnapshotInsertStatus::Duplicate;
    }

    // buffer 非空时，任何不大于当前最旧 id 的新 snapshot 都不可能改善插值窗口，直接丢弃。
    if (!snapshots_.empty() && snapshot.snapshot_id <= snapshots_.front().snapshot_id) {
        return SnapshotInsertStatus::TooOld;
    }

    const auto insert_at = std::lower_bound(snapshots_.begin(), snapshots_.end(), snapshot.snapshot_id, [](const Snapshot& existing,
                                                                                                           SnapshotId snapshot_id) {
        return existing.snapshot_id < snapshot_id;
    });
    snapshots_.insert(insert_at, snapshot);

    if (snapshots_.size() > kSnapshotBufferCapacity) {
        snapshots_.erase(snapshots_.begin());
        return SnapshotInsertStatus::InsertedAndEvictedOldest;
    }

    return SnapshotInsertStatus::Inserted;
}

const std::vector<Snapshot>& SnapshotBuffer::snapshots() const
{
    return snapshots_;
}

std::size_t SnapshotBuffer::size() const
{
    return snapshots_.size();
}

std::optional<Vec2f> interpolate_entity_position(const Snapshot& older,
                                                 const Snapshot& newer,
                                                 EntityId entity_id,
                                                 float alpha)
{
    const auto* older_entity = find_entity(older, entity_id);
    const auto* newer_entity = find_entity(newer, entity_id);
    if (older_entity == nullptr || newer_entity == nullptr) {
        return std::nullopt;
    }

    Vec2f position;
    position.x = older_entity->position.x + (newer_entity->position.x - older_entity->position.x) * alpha;
    position.y = older_entity->position.y + (newer_entity->position.y - older_entity->position.y) * alpha;
    return position;
}

std::chrono::nanoseconds snapshot_update_interval_60hz()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / 60.0));
}

std::chrono::nanoseconds snapshot_send_interval_20hz()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / 20.0));
}

bool should_run_snapshot_update(std::chrono::steady_clock::duration elapsed)
{
    return elapsed >= snapshot_update_interval_60hz();
}

bool should_send_snapshot(std::chrono::steady_clock::duration elapsed)
{
    return elapsed >= snapshot_send_interval_20hz();
}

} // namespace mininet
