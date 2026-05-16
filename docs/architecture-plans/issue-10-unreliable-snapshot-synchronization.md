## Proposed Architecture

新增 Unreliable Snapshot Sync 应拆成独立 `snapshot.hpp/cpp` 模块，而不是放进 `reliable_message`。原因是 snapshot 的生命周期、去重、缓存和插值都和可靠消息不同：它不进入 reliable queue，不重传，不做 snapshot-level ACK，只复用连接包头里的 packet sequence/ack/ack_bits。

最小模块结构：

- `packet` 层：只认识 `PacketType::Snapshot`，提供 header/type 校验，不解析 snapshot payload。
- `snapshot` 层：定义 `Vec2f`、`EntityState`、`Snapshot`、`SnapshotBuffer`，负责显式 encode/decode、容量限制、按 `snapshot_id` 排序缓存、同 entity 位置 lerp，以及 60Hz/20Hz rate helper。
- `connection` 层：只在虚拟连接已建立后发送和接收 snapshot packet。服务端暴露最小发送 API；客户端在 `update` 中接收 snapshot、解码并插入本地 `SnapshotBuffer`。
- `reliable_message` 层：不修改行为，不引用 snapshot，不为 snapshot 提供重传、消息 id、可靠去重或 delivery cleanup。

`snapshot` 应作为独立模块创建，文件为 `src/MiniNet/include/mininet/snapshot.hpp` 和 `src/MiniNet/src/snapshot.cpp`。这能让 encode/decode、buffer、interpolation、rate helper 都被单元测试直接覆盖，也避免 connection 层膨胀成业务数据格式解析层。

Snapshot packet 仍使用现有 `PacketHeader`：

- `type = PacketType::Snapshot`
- `sequence` 由对应 session 的 `AckTracker` 分配
- `ack/ack_bits` 使用对应 session 当前收到的 packet ACK 状态
- payload 为显式编码的 snapshot bytes

收到 Snapshot 后，connection 层可以像其他连接内 packet 一样更新 packet-level ACK 状态；但这只是确认 UDP packet sequence，不表示 snapshot 可靠送达，也不触发 snapshot 重传。

## Data Flow

1. Server 只在已经存在 `ServerSession` 时发送 snapshot；未建立 virtual connection 时 `send_snapshot` 返回 `false`。
2. Server 调用 `ConnectionServer::send_snapshot(session_id, snapshot, now)`。
3. ConnectionServer 按 `session_id` 找到 session，使用该 session 的 `AckTracker` 分配 packet `sequence`，生成 `ack/ack_bits`。
4. Snapshot 模块按固定顺序编码 payload：`snapshot_id`、`server_tick`、`server_time_ms`、`entity_count`、重复的 `entity_id`、`position.x`、`position.y`、`velocity.x`、`velocity.y`。
5. ConnectionServer 构造 `PacketType::Snapshot` datagram，并 `send_to(session.endpoint)`。
6. Server 记录该 packet sequence 到 `AckTracker::record_sent`，用于后续 packet-level ACK 观测；不把 snapshot 放入 `ReliableSender`。
7. Client `update(now, receive_timeout)` 最多接收一个 datagram。
8. Client 校验 sender endpoint、连接状态、`session_id` 和 `PacketType::Snapshot`。
9. Client 记录收到的 packet sequence，并处理 header 中的 packet-level `ack/ack_bits`。
10. Client 调用 `decode_snapshot_payload` 显式解析 payload；解析失败则不插入 buffer。
11. Client 将合法 snapshot 插入 `SnapshotBuffer`：按 `snapshot_id` 升序；先检查重复并不插入；其余新到 snapshot 若 `snapshot_id <= buffer` 当前最旧 `snapshot_id`，视为过旧丢弃；超过 32 条时移除最旧 snapshot。
12. 上层或测试通过客户端暴露的只读 buffer 观察 snapshot，并可调用 interpolation helper 在两个 snapshot 之间对同一 entity 的 position 做 lerp。

## Files to Create or Modify

- `src/MiniNet/include/mininet/snapshot.hpp`
  - 新增 snapshot 公共数据类型、常量、encode/decode API、`SnapshotBuffer`、interpolation helper、rate helper。

- `src/MiniNet/src/snapshot.cpp`
  - 实现显式字节编码和解析。
  - 实现按 `snapshot_id` 排序的 buffer 插入、去重、过旧丢弃和容量淘汰。
  - 实现两个 snapshot 间同一 entity 的 position lerp。
  - 实现纯时间判断的 60Hz update / 20Hz snapshot send helper。

- `src/MiniNet/include/mininet/packet.hpp`
  - 新增 `PacketType::Snapshot = 8`。
  - 声明 `validate_snapshot_packet(ByteView bytes)`。

- `src/MiniNet/src/packet.cpp`
  - 将 Snapshot 加入 `is_known_packet_type`。
  - 实现 `validate_snapshot_packet`，只校验 header/type。
  - `to_string(PacketType)` 增加 `"Snapshot"`。

- `src/MiniNet/include/mininet/connection.hpp`
  - include `mininet/snapshot.hpp`。
  - `ConnectionClientUpdateResult` 增加 `std::optional<Snapshot> received_snapshot`，用于本轮最多一个 datagram 的可控测试。
  - `ConnectionClient` 增加 `const SnapshotBuffer& snapshot_buffer() const`。
  - `ConnectionServer` 增加 `bool send_snapshot(std::uint32_t session_id, const Snapshot& snapshot, std::chrono::steady_clock::time_point now)`。

- `src/MiniNet/src/connection.cpp`
  - 将 Snapshot 视为连接内 packet 类型。
  - Client 在 Connected 状态下识别 Snapshot，解码 payload，并插入 `SnapshotBuffer`。
  - Server 实现 `send_snapshot` 的一次性发送逻辑，不接入 reliable sender。

- `CMakeLists.txt`
  - 将 `src/MiniNet/src/snapshot.cpp` 加入 `MiniNet` library。

- `tests/MiniNet.Tests/test_main.cpp`
  - 增加 snapshot encode/decode、payload validation、buffer ordering/capacity、interpolation、rate helper、connection integration 测试。

## Key Types and Functions

- `constexpr std::size_t kMaxEntitiesPerSnapshot = 32`
  - 单个 snapshot 最大 entity 数。

- `constexpr std::size_t kSnapshotBufferCapacity = 32`
  - Client snapshot buffer 最大 snapshot 数。

- `struct Vec2f { float x; float y; }`
  - 位置和速度的二维 float 表达。

- `struct EntityState`
  - 字段：`std::uint32_t entity_id`、`Vec2f position`、`Vec2f velocity`。
  - 只表示状态数据，不包含行为。

- `struct Snapshot`
  - 字段：`std::uint32_t snapshot_id`、`std::uint32_t server_tick`、`std::uint64_t server_time_ms`、`std::vector<EntityState> entities`。
  - `snapshot_id` 不处理回绕。

- `struct SnapshotDecodeResult`
  - 字段：`bool ok`、`Snapshot snapshot`。
  - decode 失败时 `ok=false`，调用方不使用 `snapshot`。

- `std::optional<std::vector<std::uint8_t>> encode_snapshot_payload(const Snapshot& snapshot)`
  - 输入：`Snapshot`。
  - 输出：显式编码 payload；entity 数超过 32 时返回 `std::nullopt`。
  - 责任：按固定顺序编码整数和 float，不发送 raw struct memory。

- `SnapshotDecodeResult decode_snapshot_payload(ByteView bytes)`
  - 输入：payload bytes，不含 packet header。
  - 输出：解析结果。
  - 责任：校验长度、`entity_count <= 32`、无截断、无尾部多余字节。

- `std::optional<std::vector<std::uint8_t>> make_snapshot_packet(std::uint32_t sequence, std::uint32_t session_id, std::uint32_t ack, std::uint32_t ack_bits, const Snapshot& snapshot)`
  - 输入：packet header 字段和 snapshot。
  - 输出：完整 datagram。
  - 责任：复用 `PacketHeader`，追加 snapshot payload，并确保总长度不超过 `kMaxDatagramSize`。

- `enum class SnapshotInsertStatus`
  - 建议值：`Inserted`、`Duplicate`、`TooOld`、`InsertedAndEvictedOldest`。
  - 责任：让 buffer 行为可测试。

- `class SnapshotBuffer`
  - `SnapshotInsertStatus insert(const Snapshot& snapshot)`
    - 按 `snapshot_id` 升序插入；重复不插入；过旧丢弃；超过容量移除最旧。
  - `const std::vector<Snapshot>& snapshots() const`
    - 返回只读快照列表，便于测试顺序和容量。
  - `std::size_t size() const`
    - 返回当前缓存数量。

- `std::optional<Vec2f> interpolate_entity_position(const Snapshot& older, const Snapshot& newer, std::uint32_t entity_id, float alpha)`
  - 输入：两个 snapshot、entity id、`0.0f..1.0f` 的插值参数。
  - 输出：同一 entity 的 position lerp；任一 snapshot 缺失该 entity 时返回 `std::nullopt`。
  - 责任：只做 position interpolation；velocity 只缓存，不 extrapolate。

- `std::chrono::nanoseconds snapshot_update_interval_60hz()`
  - 返回 60Hz update 间隔。

- `std::chrono::nanoseconds snapshot_send_interval_20hz()`
  - 返回 20Hz snapshot send 间隔。

- `bool should_run_snapshot_update(std::chrono::steady_clock::duration elapsed)`
  - 输入：距离上次 update 的 elapsed time。
  - 输出：是否达到 60Hz update 间隔。

- `bool should_send_snapshot(std::chrono::steady_clock::duration elapsed)`
  - 输入：距离上次 snapshot send 的 elapsed time。
  - 输出：是否达到 20Hz send 间隔。

- `bool ConnectionServer::send_snapshot(std::uint32_t session_id, const Snapshot& snapshot, std::chrono::steady_clock::time_point now)`
  - 输入：目标 session、snapshot、当前时间。
  - 输出：是否发送成功。
  - 责任：只对已连接 session 立即发送一次 snapshot；不排队、不重传。

- `const SnapshotBuffer& ConnectionClient::snapshot_buffer() const`
  - 输出：客户端当前 snapshot buffer 的只读视图。
  - 责任：让 integration test 和上层读取已接收 snapshot。

## Implementation Order

1. 新增 `snapshot.hpp/cpp`，先实现数据类型、常量、payload encode/decode 和 malformed payload 测试。
2. 实现 `SnapshotBuffer` 插入规则和对应单元测试：升序、重复、过旧、容量 32 淘汰最旧。
3. 实现 interpolation helper 和 rate helper，并用纯函数测试覆盖。
4. 扩展 `PacketType::Snapshot`、`is_known_packet_type`、`validate_snapshot_packet` 和 `to_string`。
5. 增加 `make_snapshot_packet`，复用 `PacketHeader` 和 `kMaxDatagramSize` 限制。
6. 扩展 `ConnectionClient`：增加内部 `SnapshotBuffer`，在 Connected 状态下处理 Snapshot packet，并在 update result 暴露本轮收到的 snapshot。
7. 扩展 `ConnectionServer`：实现 `send_snapshot`，按 session 直接发送一次 Snapshot packet。
8. 增加 server-to-client integration test：手动握手、服务端调用 `send_snapshot`、客户端 `update` 接收并检查 buffer；不要引入线程、sleep 或真实 game loop。
9. 更新 CMake 添加 `snapshot.cpp`，最后跑现有 tests，确认 reliable message 行为未被改动。

## Test Mapping

- 新增 `PacketType::Snapshot`
  - 测试 `is_known_packet_type(static_cast<uint8_t>(PacketType::Snapshot))` 为 true。
  - 测试 `validate_snapshot_packet` 接受 Snapshot header，拒绝非 Snapshot type。
  - 测试 `to_string(PacketType::Snapshot)` 返回 `"Snapshot"`。

- 新增 `EntityState` / `Snapshot` / `SnapshotBuffer`
  - 测试构造包含多个 entity 的 snapshot。
  - 测试 buffer 按 `snapshot_id` 升序保存。
  - 测试重复 `snapshot_id` 不插入。
  - 测试非重复且 `snapshot_id <= buffer` 当前最旧 `snapshot_id` 的 snapshot 被丢弃。
  - 测试超过 32 条后移除最旧 snapshot。

- 固定限制：`max_entities_per_snapshot=32`、`snapshot_buffer_capacity=32`
  - 测试 32 个 entity 可编码。
  - 测试 33 个 entity 编码失败。
  - 测试 payload 中 `entity_count > 32` 解码失败。
  - 测试 buffer 插入 33 个递增 snapshot 后只保留最新 32 个。

- Payload 必须显式 encode/decode，不能发送 raw struct memory
  - 测试编码后的字节顺序和字段顺序。
  - 测试整数使用网络字节序。
  - 测试 float 通过 IEEE 754 binary32 字节显式 round-trip。
  - 测试截断 payload、尾部多余字节、entity count 不匹配都 decode 失败。

- 编码顺序
  - 构造已知值 snapshot，断言 payload 顺序为 `snapshot_id, server_tick, server_time_ms, entity_count, entity_id, position.x, position.y, velocity.x, velocity.y`。

- Snapshot 只在 virtual connection 建立后使用
  - 测试未连接 client 收到 Snapshot 不进入 buffer。
  - 测试 wrong `session_id` 或 wrong endpoint 的 Snapshot 不进入 buffer。
  - 测试完成 ConnectRequest/ConnectAccept 后，合法 Snapshot 进入 client buffer。

- Snapshot 不进入 reliable message queue，不重传，不产生 snapshot-level ACK
  - 测试 `ConnectionServer::send_snapshot` 后该 session 的 `reliable_sender.pending_count()` 仍为 0。
  - 测试客户端收到 Snapshot 后 `received_reliable_messages` 为空。
  - 测试不调用 `send_snapshot` 时，后续 `server.update` 不会因 snapshot 自动重发。
  - 可检查 Snapshot packet 仍正常携带 header `sequence/ack/ack_bits`，但没有 reliable `message_id`。

- Client buffer 规则
  - 测试接收 out-of-order snapshots 后 buffer 内按升序排列。
  - 测试 duplicate id 不插入。
  - 测试非重复且 `snapshot_id <= buffer` 最旧 id 被丢弃。
  - 测试容量超过 32 淘汰最旧。

- Interpolation
  - 测试两个 snapshot 都有同一 entity 时，`alpha=0.5f` 得到 position 中点。
  - 测试 velocity 不参与 position 计算。
  - 测试任一 snapshot 缺失 entity 时返回 `std::nullopt`。
  - 不测试 extrapolation，因为本 issue 不实现。

- Rate helper
  - 测试 elapsed 小于 60Hz interval 时 `should_run_snapshot_update=false`，等于或大于时为 true。
  - 测试 elapsed 小于 20Hz interval 时 `should_send_snapshot=false`，等于或大于时为 true。
  - 不使用线程、sleep 或真实 game loop；只传入人工构造的 duration。

- Integration test 可控性
  - 使用现有 `ConnectionServer` + `ConnectionClient` 手动推进 `connect/update` 完成握手。
  - 调用一次 `server.send_snapshot(session_id, snapshot, now)`。
  - 调用一次 `client.update(now, timeout)` 接收。
  - 断言 `received_snapshot.has_value()`、`client.snapshot_buffer().size()==1`、entity 字段 round-trip、`received_reliable_messages.empty()`。

## Non-goals Preserved

- 不实现 snapshot 可靠传输。
- 不实现 snapshot 重传。
- 不实现 snapshot-level ACK。
- 不把 snapshot payload 包进 `ReliableData`。
- 不引入可靠消息 `message_id`、可靠去重或 delivery cleanup。
- 不处理 `snapshot_id` 回绕。
- 不做 extrapolation。
- 不基于 velocity 预测位置。
- 不实现 snapshot delta compression。
- 不实现 fragmentation。
- 不实现真实 game loop、线程、sleep、定时器调度或后台发送循环。
- 不修改 server/client 示例程序来模拟完整游戏状态同步，除非后续 issue 明确要求。

## Implementation Notes

- 和计划一致：
  - Snapshot 被实现为独立 `snapshot.hpp/cpp` 模块，没有混入 `reliable_message`。
  - `PacketType::Snapshot = 8`、`validate_snapshot_packet`、`to_string(PacketType::Snapshot)` 已接入 packet 层。
  - `ConnectionServer::send_snapshot` 立即发送一次 Snapshot，不排队、不重传、不进入 `ReliableSender`。
  - `ConnectionClient` 只在 `Connected` 且 `session_id` 匹配时接收 Snapshot，并插入本地 `SnapshotBuffer`。
  - Snapshot payload 使用显式字段 encode/decode；整数为 big-endian，float 以 IEEE754 binary32 位模式编码。
  - 测试覆盖 codec、buffer、interpolation、rate helper、unreliable semantics 和连接集成路径。

- 实现时的调整：
  - Interpolation API 采用 `alpha` 参数做两个 snapshot 间 position lerp，没有额外实现“按 target server_time_ms 自动查找区间”的 buffer API。
  - 未连接场景测试中，先让 `ConnectionClient` 进入 `Connecting` 后再投递 Snapshot，避免 Windows 下未绑定 socket 调用 `local_port()` 触发 `WSA error 10022`。

- 调整原因：
  - `alpha` 版本更小、更可测，符合本 issue “只做两个 snapshot 间插值”的范围。
  - Windows socket 在发送前未必已经绑定本地端口，测试用真实 ConnectRequest 的 sender endpoint 更贴近实际 UDP 使用方式。

- 后续学习点：
  - 后续 issue 可以把 interpolation 扩展为“从 SnapshotBuffer + target server_time_ms 自动选择前后帧”。
  - 如果要模拟真实游戏同步，可单独设计 render delay、jitter buffer、extrapolation 和丢包/乱序模拟。
