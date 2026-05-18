# MiniNet Simulation Runner

`MiniNet.Sim` 是一个本地命令行模拟工具，用来把现有 `NetworkSimulator`、packet encode/decode、`ReliableSender` / `ReliableReceiver` 和 `SnapshotBuffer` 组合起来观察整体行为。它不接入真实 UDP socket，也不修改 `ConnectionClient` / `ConnectionServer`。

## 构建和运行

```powershell
cmake --build build --config Debug
.\build\MiniNet.Sim.exe
```

无参数运行会使用默认配置完成一次模拟并打印 summary。

常用参数：

```powershell
.\build\MiniNet.Sim.exe --loss 0.2 --duplicate 0.1 --min-latency 50 --max-latency 150 --messages 100 --snapshots 200 --entities 10 --output-json sim-output\run-001.json --output-csv sim-output\run-001.csv --event-log sim-output\run-001-events.json
```

支持参数：

- `--loss <float>`，丢包率，范围 `[0, 1]`。
- `--duplicate <float>`，重复包概率，范围 `[0, 1]`。
- `--min-latency <ms>` / `--max-latency <ms>`，模拟延迟范围。
- `--seed <int>`，确定性随机种子。
- `--messages <int>`，reliable message 数量。
- `--snapshots <int>`，snapshot 数量。
- `--entities <int>`，每个 snapshot 的实体数量，当前最多 32。
- `--duration-ms <int>` / `--tick-ms <int>`，模拟时间和 tick 间隔。
- `--output-json <path>`，写 summary JSON。
- `--output-csv <path>`，写 summary CSV。
- `--event-log <path>`，写 event timeline JSON。

## 统计口径

Packet 统计：

- `sent_packets`：原始 packet 发送尝试数，不包含 simulator 自动产生的 duplicate 副本。
- `delivered_packets`：至少送达一次的原始 packet 数。
- `dropped_packets`：模拟结束并 drain 后仍未送达的原始 packet 数。
- `duplicate_packets`：额外送达的重复 datagram 数。
- `average_latency_ms`：已送达 datagram 的平均延迟；没有送达时导出为 `not_available`。

Reliable message 统计：

- `sent_reliable_messages`：成功进入 `ReliableSender` 队列的消息数量。
- `delivered_reliable_messages`：`ReliableReceiver` 第一次处理的消息数量。
- `pending_reliable_messages`：模拟结束后 sender 侧仍未被 packet ACK 清理的消息数量。
- `retransmission_count`：同一 `message_id` 第二次及以后进入新 packet 的次数。
- `failed_reliable_messages`：第一版为 `not_available`，因为当前协议没有独立 message failure 语义。

Snapshot 统计：

- `sent_snapshots`：成功编码并发送的 snapshot 数量。
- `delivered_snapshots`：成功解码并插入 `SnapshotBuffer` 的新 snapshot 数量。
- `duplicate_snapshots`：重复 snapshot 到达且未插入 buffer 的数量。
- `snapshot_buffer_size`：模拟结束时 buffer 中的 snapshot 数量。
- `dropped_or_missing_snapshots`：第一版为 `not_available`。
- `out_of_order_snapshots`：第一版为 `not_available`。

## 导出格式

JSON summary 包含固定的 `config` 和 `stats` 对象。`not_available` 字段以字符串导出。

CSV summary 包含一行 header 和一行数据，便于复制到表格工具中分析。

Event log JSON 包含 `events` 数组。第一版记录：

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

## Visualizer

静态页面位于：

```text
docs/visualizer/index.html
```

直接用本地浏览器打开，然后通过文件选择器加载 `--output-json` 或 `--event-log` 生成的 JSON 文件。页面不会扫描本地目录，也不依赖 C++ 程序运行。

## 边界

本工具是 scenario 组合层，不是新的 transport abstraction。它保持以下边界：

- 不新增 transport interface。
- 不把 `NetworkSimulator` 注入 `ConnectionClient` / `ConnectionServer`。
- 不修改 `PacketHeader`、ACK bits、reliable resend、snapshot sync 或 `NetworkSimulator` 的核心语义。
- 不引入第三方 JSON 或图表库。
