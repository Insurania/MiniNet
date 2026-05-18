# Issue 16 Architecture Plan: Network Simulation Scenario Runner and Visualization Export

保存路径：`docs/architecture-plans/issue-16-network-simulation-scenario-runner.md`

本方案对应 GitHub Issue #16：`Issue 8: Add Network Simulation Scenario Runner and Visualization Export`。

本 issue 的核心目标不是继续堆单元测试，而是新增一个可重复运行、可导出、可观察的模拟场景工具。它应该帮助学习者回答：“在丢包、重复包、延迟和 jitter 下，MiniNet 当前协议模块整体表现如何？”

## 1. 当前代码结构观察

当前项目是一个轻量 C++17 学习项目，顶层 `CMakeLists.txt` 已定义：

- `MiniNet`：核心库目标，源码位于 `src/MiniNet/src`，公共头文件位于 `src/MiniNet/include/mininet`。
- `MiniNet.Server` / `MiniNet.Client`：真实 UDP 示例入口。
- `MiniNet.Tests`：无第三方测试框架的测试程序，已有 `[RUN]` / `[PASS]` 风格输出。

现有模块边界比较清晰：

- `packet.hpp/cpp`：负责 `PacketHeader`、`PacketType`、基础 encode/decode 和 packet validation。
- `ack_tracker.hpp/cpp`：负责 packet-level sequence、ACK、ACK bits。
- `reliable_message.hpp/cpp`：负责 reliable unordered message 的 payload encode/decode、`ReliableSender`、`ReliableReceiver`。
- `snapshot.hpp/cpp`：负责 snapshot encode/decode、`SnapshotBuffer`、插入结果。
- `network_simulator.hpp/cpp`：实现 deterministic in-memory `NetworkSimulator`，只处理 raw bytes，不解析 MiniNet 协议。
- `connection.hpp/cpp`：把真实 `UdpSocket`、ACK、reliable、snapshot 组合成 `ConnectionClient` / `ConnectionServer`。

本 issue 不应把 `NetworkSimulator` 注入 `ConnectionClient` / `ConnectionServer`，否则会变成 transport abstraction 改造，范围会明显超过 normal。推荐新增一个独立的 scenario 组合层：它复用已有协议模块，但不改变底层协议语义。

## 2. 建议新增/修改文件清单

实现阶段建议新增：

- `src/MiniNet/include/mininet/scenario.hpp`
  - 定义 `ScenarioConfig`、`ScenarioStats`、`ScenarioEvent`、`ScenarioResult`、`ScenarioRunner`。
  - 声明 summary、JSON、CSV、event log 导出函数。

- `src/MiniNet/src/scenario.cpp`
  - 实现场景运行、packet 统计、reliable message 流程、snapshot 流程和导出逻辑。

- `src/MiniNet.Sim/main.cpp`
  - 实现 CLI 参数解析、错误输出、调用 `ScenarioRunner`、打印 summary、写导出文件。

- `docs/visualizer/index.html`
  - 实现静态 HTML visualizer，通过文件选择器加载 JSON，不引入框架。

实现阶段建议修改：

- `CMakeLists.txt`
  - 把 `scenario.cpp` 加入 `MiniNet` 库。
  - 新增 `MiniNet.Sim` executable。

- `tests/MiniNet.Tests/test_main.cpp`
  - 增加 scenario runner 相关测试。
  - 测试输出继续使用可见的 `[RUN]` / `[PASS]` 和关键统计值。

- `docs/simulation.md`
  - 记录 CLI 用法、统计口径、哪些字段第一版为 `not_available`、visualizer 使用方法。

- `docs/learning-index.md`
  - 增加 Issue #16 学习入口。

本规划阶段只新增当前架构方案文件，不写生产代码。

## 3. 模块边界设计

### ScenarioConfig

`ScenarioConfig` 是一次模拟运行的完整输入，不直接读取 `argv`。

建议字段：

- `loss_rate`
- `duplicate_rate`
- `min_latency_ms`
- `max_latency_ms`
- `seed`
- `reliable_message_count`
- `snapshot_count`
- `entity_count`
- `duration_ms`
- `tick_ms`
- `output_json_path`
- `output_csv_path`
- `event_log_path`

设计理由：CLI、测试和后续批量运行都可以构造同一个 config，避免把场景逻辑写死在 `main.cpp`。

### ScenarioStats

`ScenarioStats` 是一次模拟运行的结果统计，不负责文件格式化。

建议字段：

- packet：`sent_packets`、`delivered_packets`、`dropped_packets`、`duplicate_packets`、`average_latency_ms`。
- reliable：`sent_reliable_messages`、`delivered_reliable_messages`、`pending_reliable_messages`、`retransmission_count`、`failed_reliable_messages`。
- snapshot：`sent_snapshots`、`delivered_snapshots`、`duplicate_snapshots`、`snapshot_buffer_size`、`out_of_order_snapshots`、`dropped_or_missing_snapshots`。
- run：`simulation_duration_ms`、`seed`、`result`。

第一版允许部分字段以 `"not_available"` 导出，但 C++ 内部建议用 enum 或 optional-like 结构表达，不要在核心统计里到处散落字符串。

### ScenarioEvent

`ScenarioEvent` 是可选 timeline 的结构化事件。

建议字段：

- `time_ms`
- `event_type`
- `source`
- `target`
- `packet_sequence`
- `message_id`
- `snapshot_id`
- `latency_ms`
- `details`

不适用字段可以保持空值，在 JSON 导出时省略或写 `null`。第一版重点支持：

- `packet_sent`
- `packet_delivered`
- `packet_duplicated`
- `packet_dropped`
- `reliable_message_sent`
- `reliable_message_delivered`
- `reliable_message_retransmitted`
- `snapshot_sent`
- `snapshot_delivered`
- `snapshot_buffer_updated`

### ScenarioRunner

`ScenarioRunner` 是组合层，负责：

- 根据 `ScenarioConfig` 创建 `NetworkSimulatorConfig`。
- 生成 reliable message traffic。
- 生成 snapshot traffic。
- 使用 `NetworkSimulator::send` 发送 raw bytes。
- 使用 `NetworkSimulator::poll` 获取 delivery。
- 使用现有 packet/reliable/snapshot 模块解析和处理 payload。
- 收集 `ScenarioStats` 和 `ScenarioEvent`。

它不负责：

- 解析 CLI。
- 直接写文件。
- 打印 UI。
- 修改底层协议模块语义。

## 4. CLI 参数解析策略

`MiniNet.Sim` 第一版可以手写轻量参数解析，不引入第三方库。

建议规则：

- 支持 `--name value` 形式。
- 未提供参数时使用默认值。
- 未知参数返回非零退出码。
- 缺少 value 返回非零退出码。
- 数值转换失败返回非零退出码。
- `loss` / `duplicate` 必须在 `[0, 1]`。
- `min-latency`、`max-latency`、`messages`、`snapshots`、`entities`、`duration-ms`、`tick-ms` 不能为负。
- `min-latency <= max-latency`。
- `tick-ms > 0`。

CLI 层只做两件事：

1. 把 `argv` 转为 `ScenarioConfig`。
2. 调用 `ScenarioRunner` 并处理输出。

这能让测试绕过 CLI 直接测试 runner，也能让 CLI 参数错误测试更集中。

## 5. 如何复用现有模块

### NetworkSimulator

`NetworkSimulator` 应保持 raw bytes 模拟器定位。

Scenario runner 使用它来模拟：

- loss
- duplicate
- min/max latency
- deterministic seed

统计口径建议由 runner 推导：

- 发送前记录每个原始 datagram 的 packet key。
- `sent_packets` 统计原始发送尝试数，不包含 duplicate 副本。
- 第一次 poll 到同 key 的 datagram 计为 delivered。
- 后续 poll 到同 key 的 datagram 计为 duplicate。
- drain 后仍未 delivered 的 key 计为 dropped。

packet key 可以使用：

```text
source + target + packet_type + sequence
```

### ReliableSender / ReliableReceiver

推荐流程：

1. Runner 调用 `ReliableSender::enqueue` 生成 pending message。
2. 每个 tick 调用 sender 选择需要发送或重发的 message。
3. Runner 使用 `make_reliable_data_packet` 构造 packet bytes。
4. bytes 经由 `NetworkSimulator`。
5. receiver poll 到 packet 后 decode payload。
6. 对每个 message 调用 `ReliableReceiver::receive`。
7. 只有第一次业务可处理的 message 计入 `delivered_reliable_messages`。
8. receiver 侧发 ACK-only packet 回 sender。
9. sender 侧处理 ACK 并清理 pending。

`retransmission_count` 可以基于同一个 `message_id` 的发送次数推导：第一次发送不算重传，第二次及以后算 retransmission。

第一版不实现 message failure 语义，`failed_reliable_messages` 导出为 `"not_available"`。

### SnapshotBuffer

推荐流程：

1. Runner 生成 `Snapshot`，包含 `entities` 个 `EntityState`。
2. 使用现有 snapshot encode/make packet 函数构造 packet bytes。
3. bytes 经由 `NetworkSimulator`。
4. client poll 到 snapshot packet 后 decode。
5. 调用 `SnapshotBuffer::insert`。
6. 根据插入结果统计：
   - 新 snapshot：`delivered_snapshots++`
   - duplicate：`duplicate_snapshots++`
   - buffer size：读取当前 buffer 大小

snapshot 不进入 `ReliableSender`，不触发 reliable resend。测试里应明确验证 snapshot traffic 不影响 reliable retransmission count。

## 6. JSON / CSV / Event Log 导出设计

不引入第三方 JSON 库。原因：这是学习项目，第一版字段固定，手写导出更直观，也避免依赖安装问题。

建议新增 helper：

- `std::string scenario_summary_to_json(const ScenarioConfig&, const ScenarioStats&)`
- `std::string scenario_events_to_json(const std::vector<ScenarioEvent>&)`
- `std::string scenario_summary_to_csv_header()`
- `std::string scenario_summary_to_csv_row(const ScenarioConfig&, const ScenarioStats&)`
- `bool write_text_file(const std::filesystem::path&, std::string_view, std::string* error)`

注意点：

- 字符串字段需要最小 JSON escaping。
- 输出目录不存在时，优先创建目录。
- 目录创建失败或文件写入失败时，CLI 返回非零退出码并输出清晰错误。
- `not_available` 字段在 JSON 中可导出为字符串 `"not_available"`。
- CSV 中 `not_available` 直接写成 `not_available`。

Event log 可以有两种模式：

- summary JSON：只包含 `config` 和 `stats`。
- event log JSON：只包含 `events` 数组。

Visualizer 第一版可以同时支持这两种文件：如果 JSON 有 `events`，就显示 event table；如果没有，就只显示 summary。

## 7. Visualizer 最小设计

文件：`docs/visualizer/index.html`

第一版使用纯 HTML/CSS/JS，不引入第三方库。

页面结构：

- 文件选择器：`<input type="file" accept=".json,application/json">`
- Config 区域：表格显示 config。
- Packet summary：sent / delivered / dropped / duplicate / average latency。
- Reliable summary：sent / delivered / pending / retransmission / failed。
- Snapshot summary：sent / delivered / duplicate / buffer size / unavailable fields。
- Delivered vs dropped：用简单条形 DOM 或 SVG 表示。
- Event timeline：如果存在 `events`，显示可滚动 table。

重要限制：

- 静态 HTML 不能可靠扫描本地 `sim-output/*.json` 目录。
- 因此只支持用户手动选择 JSON 文件。
- Visualizer 不运行 C++，不依赖真实网络。

## 8. 测试策略

测试仍放在 `tests/MiniNet.Tests/test_main.cpp`，保持无第三方依赖。

输出要求：

- 每个测试开始打印 `[RUN] test_name`。
- 每个测试成功打印 `[PASS] test_name`。
- 关键 scenario 打印核心统计，例如：
  - `sent_packets`
  - `delivered_packets`
  - `dropped_packets`
  - `duplicate_packets`
  - `delivered_reliable_messages`
  - `pending_reliable_messages`
  - `retransmission_count`
  - `delivered_snapshots`
  - `snapshot_buffer_size`

建议测试清单：

- `scenario_default_config_runs`
- `scenario_loss_zero_drops_zero`
- `scenario_loss_one_delivers_zero`
- `scenario_duplicate_one_records_duplicates`
- `scenario_fixed_latency_reports_expected_latency`
- `scenario_jitter_latency_stays_in_range`
- `scenario_same_seed_is_deterministic`
- `scenario_invalid_loss_is_rejected`
- `scenario_invalid_duplicate_is_rejected`
- `scenario_invalid_latency_range_is_rejected`
- `scenario_reliable_loss_zero_delivers_all`
- `scenario_duplicate_reliable_not_processed_twice`
- `scenario_snapshot_duplicate_not_inserted_twice`
- `scenario_json_export_contains_config_and_stats`
- `scenario_csv_export_contains_header_and_row`
- `scenario_event_log_contains_basic_events`

CLI 可用少量 smoke test 覆盖，核心逻辑优先测 `ScenarioRunner`，这样更稳定。

## 9. 实现步骤拆分

建议按以下顺序实现，便于小步提交和 review：

1. 新增 `scenario.hpp/cpp` 空骨架和 CMake 接入。
2. 实现 `ScenarioConfig` 默认值和 validation。
3. 实现最小 `ScenarioRunner::run`，只跑 packet send/poll 和 packet stats。
4. 加入 packet simulation 测试：loss 0、loss 1、duplicate 1、latency。
5. 接入 reliable message traffic，统计 delivered/pending/retransmission。
6. 加入 reliable 测试：loss 0 全部 delivered、duplicate 不重复业务处理。
7. 接入 snapshot traffic，统计 delivered/duplicate/buffer size。
8. 加入 snapshot 测试。
9. 实现 JSON / CSV / event log 字符串导出和文件写入。
10. 实现 `MiniNet.Sim` CLI 参数解析、summary 输出和导出路径。
11. 新增 `docs/visualizer/index.html`。
12. 新增或更新 `docs/simulation.md` 与 `docs/learning-index.md`。
13. 运行完整测试和多个 CLI 示例命令。

## 10. 风险和不做事项

主要风险：

- 把 issue 做成 transport abstraction，导致牵连 `ConnectionClient` / `ConnectionServer`。
- 为了精确统计修改 `NetworkSimulator` 语义，造成已有测试回归。
- 过早引入第三方 JSON 或图表库，增加 Windows/网络依赖风险。
- 试图保证任意丢包率下 reliable message 都在有限时长内 100% delivered，这不稳定。
- Visualizer 试图自动扫描本地目录，会被浏览器安全模型阻止。

明确不做：

- 不新增 transport interface。
- 不把 simulator 注入真实 socket connection。
- 不修改 `PacketHeader`。
- 不修改 ACK bits 语义。
- 不修改 reliable resend 语义。
- 不修改 snapshot sync 语义。
- 不修改 `NetworkSimulator` 核心语义。
- 不做实时 GUI。
- 不做 WebSocket。
- 不做 Monte Carlo 批量实验。
- 不做数据库存储。
- 不做拥塞控制、flow control、fragmentation。

## 11. 给实现阶段的 PR 说明建议

PR 应重点说明：

- 新增了 `MiniNet.Sim` CLI。
- Scenario runner 是测试/观察工具，不是协议模块。
- JSON / CSV / event log 的字段口径。
- 哪些字段第一版为 `not_available`。
- Visualizer 是本地静态 HTML，通过文件选择器加载 JSON。
- 本 PR 不修改协议语义。

PR 结尾应包含：

```text
Closes #16
```

## 12. 实现后复盘 Checklist

实现完成后建议回到本文件补充：

- 实际实现和本方案的差异。
- 哪些统计字段最终实现，哪些仍为 `not_available`。
- 哪些测试最有学习价值。
- 运行 `MiniNet.Sim` 后观察到的协议行为。
- 下一个 issue 是否应该做更细的 RTT/RTO、可视化增强或批量实验。
