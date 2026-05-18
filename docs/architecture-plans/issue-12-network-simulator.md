## Proposed Architecture

保存路径：`docs/architecture-plans/issue-12-network-simulator.md`

新增独立 `network_simulator.hpp/cpp` 模块，实现 deterministic in-memory `NetworkSimulator`。它只负责模拟 UDP datagram 在内存中的投递行为：loss、duplication、latency/jitter 和 reordering。模块只处理 raw packet bytes，不解析 `PacketHeader`、`ReliableMessage`、`Snapshot`，也不改变现有 `ConnectionClient` / `ConnectionServer` 的真实 socket 路径。

最小模块边界：

- `network_simulator`：新增公共测试辅助模块，维护 in-flight queue、随机源和配置校验。
- `udp_socket`：复用现有 `UdpEndpoint` 作为 endpoint 标识符。它当前是 `{ std::string address; std::uint16_t port; }` 的纯值 struct，不依赖 socket 实例；为避免额外拆分 endpoint 头文件，本 issue 不引入 `endpoint.hpp`。
- `packet` / `reliable_message` / `snapshot` / `connection`：不被 simulator 依赖或修改语义。测试可以把这些模块产出的 bytes 交给 simulator，但 simulator 本身不理解这些 bytes。

模块命名采用：

- `src/MiniNet/include/mininet/network_simulator.hpp`
- `src/MiniNet/src/network_simulator.cpp`

配置 reject 使用最小、可测试 API：

- 提供 `NetworkSimulatorConfigValidation validate_network_simulator_config(const NetworkSimulatorConfig& config)`，返回是否接受和拒绝原因。
- 提供 `static std::optional<NetworkSimulator> NetworkSimulator::create(NetworkSimulatorConfig config)`。非法配置返回 `std::nullopt`，不 clamp，不隐式修正。
- `NetworkSimulator` 的实际构造函数保持私有或使用私有 tag，避免调用方绕过校验。

随机实现：

- `NetworkSimulator` 内部持有一个按 `random_seed` 初始化的 `std::mt19937`。
- 每次 `send` 按固定顺序消耗随机数：先 loss 判定；未丢弃时抽样原始 packet latency 并入队；再做 duplicate 判定；若复制成立，再独立抽样 duplicate latency 并入队。
- `duplicate_rate` 表示最多创建一个额外副本，不递归复制 duplicate。
- deterministic 范围限定为同一实现、同一 seed、同一输入调用序列得到一致结果；不承诺跨编译器 bit-for-bit 一致。

## Data Flow

1. 测试或上层代码构造 raw packet bytes，通常可以来自现有 packet/reliable/snapshot 编码函数，但 simulator 不解析内容。
2. 调用 `NetworkSimulator::send(from, to, bytes, now)`。
3. `send` 先按 `loss_rate` 判定原始 packet 是否丢弃；丢弃时不入队，也不产生 duplicate。
4. 未丢弃时，`send` 在 `[min_latency, max_latency]` 范围内抽样 latency，计算 `delivery_time = now + latency`。
5. 原始 packet 以 `{ from, to, bytes, delivery_time }` 形式加入内部 in-flight queue，并记录内部递增 `enqueue_order`。
6. `send` 再按 `duplicate_rate` 判定是否创建一个 duplicate。
7. duplicate 保留相同 `from`、`to`、`bytes`，但重新独立抽样 latency，得到自己的 `delivery_time`，再加入 in-flight queue。
8. 调用 `NetworkSimulator::poll(receiver, now)`。
9. `poll` 找出所有 `to == receiver && delivery_time <= now` 的 in-flight packet，复制到结果列表，并从内部 queue 移除。
10. `poll` 返回前按 `delivery_time` 升序排序；相同 `delivery_time` 按内部 `enqueue_order` 升序排序。
11. 未到期 packet、发给其他 receiver 的 packet 保留在内部 queue 中，等待后续 `poll`。

## Files to Create or Modify

- `src/MiniNet/include/mininet/network_simulator.hpp`
  - 声明 `NetworkSimulatorConfig`、`NetworkSimulatorConfigRejectReason`、`NetworkSimulatorConfigValidation`、`QueuedPacket`、`NetworkSimulator`。
  - include `mininet/udp_socket.hpp` 以复用 `UdpEndpoint`。
  - 暴露 `send`、`poll`、`in_flight_count` 和配置校验 API。

- `src/MiniNet/src/network_simulator.cpp`
  - 实现配置校验、随机抽样、packet 入队、poll 排序与移除。
  - 内部定义 `InFlightPacket`，额外保存 `std::uint64_t enqueue_order`，不暴露可变内部队列。
  - 内部定义 endpoint 比较 helper，按 address string 和 port 精确匹配。

- `CMakeLists.txt`
  - 将 `src/MiniNet/src/network_simulator.cpp` 加入 `MiniNet` library。
  - 架构规划阶段不修改该文件；由实现阶段按此计划修改。

- `tests/MiniNet.Tests/test_main.cpp`
  - 增加 simulator 单元测试，覆盖配置 reject、loss、duplication、latency、jitter、reordering、poll 移除、deterministic seed、raw bytes 不变。
  - 架构规划阶段不修改该文件；由实现阶段按此计划修改。

不需要修改：

- `src/MiniNet/include/mininet/connection.hpp`
- `src/MiniNet/src/connection.cpp`
- `src/MiniNet/include/mininet/packet.hpp`
- `src/MiniNet/src/packet.cpp`
- `src/MiniNet/include/mininet/reliable_message.hpp`
- `src/MiniNet/include/mininet/snapshot.hpp`
- server/client 示例程序

## Key Types and Functions

- `struct NetworkSimulatorConfig`
  - 字段：`double loss_rate = 0.0`
  - 字段：`double duplicate_rate = 0.0`
  - 字段：`std::chrono::milliseconds min_latency{0}`
  - 字段：`std::chrono::milliseconds max_latency{0}`
  - 字段：`std::uint32_t random_seed = 0`
  - 责任：描述网络模拟参数，不做自动修正。

- `enum class NetworkSimulatorConfigRejectReason`
  - 建议值：`None`、`InvalidLossRate`、`InvalidDuplicateRate`、`InvalidLatencyRange`
  - 责任：让测试能明确断言非法配置为什么被拒绝。

- `struct NetworkSimulatorConfigValidation`
  - 字段：`bool accepted = false`
  - 字段：`NetworkSimulatorConfigRejectReason reason`
  - 责任：承载配置校验结果。

- `NetworkSimulatorConfigValidation validate_network_simulator_config(const NetworkSimulatorConfig& config)`
  - 输入：config。
  - 输出：校验结果。
  - 规则：`loss_rate` 和 `duplicate_rate` 必须在 `[0, 1]`；`min_latency <= max_latency`；非法配置 reject，不 clamp。

- `struct QueuedPacket`
  - 字段：`UdpEndpoint from`
  - 字段：`UdpEndpoint to`
  - 字段：`std::vector<std::uint8_t> bytes`
  - 字段：`std::chrono::steady_clock::time_point delivery_time`
  - 责任：表示可投递 packet 的只读值拷贝。它可以作为 `poll` 返回值，但不是内部队列的可变引用。

- `class NetworkSimulator`
  - `static std::optional<NetworkSimulator> create(NetworkSimulatorConfig config)`
    - 输入：配置。
    - 输出：合法时返回 simulator；非法时返回 `std::nullopt`。
    - 责任：阻止非法配置进入运行时。
  - `void send(const UdpEndpoint& from, const UdpEndpoint& to, ByteView bytes, std::chrono::steady_clock::time_point now)`
    - 输入：发送方、接收方、raw bytes、当前时间。
    - 输出：无直接返回值。
    - 责任：按 loss/latency/duplication 规则把 packet 加入 in-flight queue。
  - `std::vector<QueuedPacket> poll(const UdpEndpoint& receiver, std::chrono::steady_clock::time_point now)`
    - 输入：接收方 endpoint、当前时间。
    - 输出：所有到期且发给该 receiver 的 packet 值拷贝。
    - 责任：返回并移除到期 packet，按 `delivery_time` 和入队顺序排序。
  - `std::size_t in_flight_count() const`
    - 输入：无。
    - 输出：内部 in-flight packet 数量。
    - 责任：只读测试辅助，不暴露内部队列。

- 内部 `struct InFlightPacket`
  - 字段：`QueuedPacket packet`
  - 字段：`std::uint64_t enqueue_order`
  - 责任：保存稳定排序所需的入队顺序。仅 `.cpp` 内部使用。

- 内部 helper `bool same_endpoint(const UdpEndpoint& left, const UdpEndpoint& right)`
  - 输入：两个 endpoint。
  - 输出：`address` 和 `port` 都相同才返回 true。
  - 责任：避免要求 `UdpEndpoint` 新增 `operator==`，从而不改 `udp_socket.hpp` 的公共类型。

## Implementation Order

1. 新增 `network_simulator.hpp`，先定义 config、reject reason、validation result、`QueuedPacket` 和 `NetworkSimulator` 公共 API。
2. 在 `network_simulator.cpp` 实现 `validate_network_simulator_config`，覆盖所有非法配置，不做 clamp。
3. 实现 `NetworkSimulator::create`，合法配置初始化 `std::mt19937` 和空 in-flight queue，非法配置返回 `std::nullopt`。
4. 实现固定随机消耗顺序：loss 判定、原始 latency、duplicate 判定、duplicate latency。
5. 实现 `send`：复制 `ByteView` 内容到 `std::vector<std::uint8_t>`，原始 packet 和 duplicate 都加入内部 queue，duplicate 重新抽样 latency。
6. 实现 `poll`：扫描内部 queue，拆出到期且 receiver 匹配的 packet，重建 remaining queue，再对 delivered 按 `delivery_time` 和 `enqueue_order` 排序后返回。
7. 实现 `in_flight_count`，只返回数量。
8. 更新 CMake，把 `network_simulator.cpp` 加入 `MiniNet` library。
9. 在 `test_main.cpp` 增加 focused tests，先测固定 latency/rate 的确定行为，再测 jitter/reordering/deterministic seed。
10. 跑现有测试，确认新增模块没有改变 connection、ACK、reliable、snapshot 行为。

## Test Mapping

- Config: `loss_rate`、`duplicate_rate` 必须在 `[0,1]`
  - 测试 `loss_rate < 0`、`loss_rate > 1` 被 reject。
  - 测试 `duplicate_rate < 0`、`duplicate_rate > 1` 被 reject。
  - 测试边界值 `0.0` 和 `1.0` 被接受。

- Config: `min_latency <= max_latency`
  - 测试 `min_latency > max_latency` 被 reject。
  - 测试 `min_latency == max_latency` 被接受，并产生固定 delivery time。

- 非法配置 reject，不 clamp
  - 测试 `NetworkSimulator::create(invalid)` 返回 `std::nullopt`。
  - 测试 `validate_network_simulator_config` 返回具体 reject reason。

- `send` 按 `loss_rate` 丢包
  - `loss_rate=1.0` 时发送后 `in_flight_count()==0`，`poll` 返回空。
  - `loss_rate=0.0` 时发送后 packet 可按 latency 投递。

- `send` 按 latency 分配 `delivery_time`
  - `min_latency == max_latency` 时，`delivery_time == now + min_latency`。
  - `poll(receiver, now + min_latency - 1ms)` 返回空。
  - `poll(receiver, now + min_latency)` 返回 packet。

- duplicate 规则
  - `duplicate_rate=1.0` 且 `loss_rate=0.0` 时，一次 `send` 产生两个 packet。
  - duplicate 的 `from`、`to`、`bytes` 与原始 packet 相同。
  - `min_latency < max_latency` 时，duplicate 独立抽样 latency；测试不要求一定不同，只要求两次抽样按固定 seed 可复现。
  - `loss_rate=1.0` 时不产生 duplicate。

- reordering 和 poll 排序
  - 使用不同发送时间或固定 seed jitter 构造 delivery_time 乱序场景。
  - `poll` 返回按 `delivery_time` 升序。
  - 相同 `delivery_time` 时按入队顺序返回。

- poll 移除行为
  - 第一次 `poll` 返回到期 packet 后，第二次同条件 `poll` 返回空。
  - 发给其他 receiver 的 packet 不被当前 receiver 的 `poll` 移除。

- endpoint 纯值匹配
  - address 相同、port 相同才投递。
  - address 相同但 port 不同不投递。
  - port 相同但 address 不同不投递。

- deterministic seed
  - 两个 simulator 使用相同 config、相同 seed、相同 `send/poll` 输入序列，得到相同投递数量、顺序、bytes 和 delivery_time。
  - 不测试跨编译器 bit-for-bit 随机序列。

- raw bytes 不被解析或修改
  - 使用任意非 MiniNet header bytes 发送，`poll` 返回完全相同 bytes。
  - 使用现有 packet 编码 bytes 发送，simulator 不检查 `PacketType`、session 或 payload。

- 内部队列不可变暴露
  - 测试只能通过 `in_flight_count()` 观察数量，通过 `poll()` 获取值拷贝。
  - 不提供 `queue()`、`mutable_packets()` 或返回内部 vector 引用的 API。

## Non-goals Preserved

- 不实现真实 socket。
- 不实现 transport interface，也不把 simulator 注入 `ConnectionClient` / `ConnectionServer`。
- 不改 `ConnectionClient` / `ConnectionServer` 的构造函数、update 流程或 socket 使用方式。
- 不改 ACK、reliable message、snapshot 的语义。
- 不解析 `PacketHeader`。
- 不解析 `ReliableMessage` payload。
- 不解析 `Snapshot` payload。
- 不实现拥塞控制。
- 不实现带宽限制。
- 不实现 MTU、fragmentation 或 datagram size enforcement。
- 不实现多线程、后台投递循环、sleep 或真实时间等待。
- 不暴露可变内部队列。
- 不保证跨编译器 bit-for-bit 随机一致性。

## Implementation Notes

- 和计划一致：
  - 新增独立 `network_simulator.hpp/cpp` 模块，没有接入或改造 `ConnectionClient` / `ConnectionServer`。
  - 复用 `UdpEndpoint` 作为纯值 endpoint，并在 simulator 内部按 `address + port` 比较。
  - `NetworkSimulator::create` 会先校验配置；非法配置返回 `std::nullopt`，不做 clamp。
  - `send` 的随机消耗顺序保持为 loss 判定、原始 latency、duplicate 判定、duplicate latency。
  - `poll` 返回后会移除已投递 packet，并按 `delivery_time`、`enqueue_order` 排序。
  - 测试覆盖配置拒绝、丢包、固定延迟、jitter、乱序、重复包、endpoint 过滤、确定性 seed 和 raw bytes 透传。

- 实现时的调整：
  - 没有新增专门 endpoint 类型；直接复用现有 `UdpEndpoint`。
  - 没有给 `UdpEndpoint` 增加 `operator==`，避免修改真实 socket 模块的公共语义。

- 调整原因：
  - `UdpEndpoint` 当前只是 `address` 和 `port` 的值对象，足够表达模拟网络里的 sender/receiver。
  - endpoint 比较保持在 simulator 内部，可以减少跨模块 API 扩散。

- 后续学习点：
  - 后续 issue 可以用 `NetworkSimulator` 构造可靠消息丢包重传、Snapshot 乱序/丢失等更接近真实网络的测试。
  - 如果将来要把 simulator 接入真实 transport，需要单独设计 transport interface，不能在这个 issue 中顺手改造。
