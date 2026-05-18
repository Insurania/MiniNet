# Learning Index

这份文档用于把 MiniNet 的协议功能、issue、PR、架构方案、关键代码和关键测试串起来，逐步形成一份自己的学习索引。

维护建议：

- 每完成一个功能 issue 后补一小节。
- 不需要写长篇教程，优先记录能帮助回忆和复盘的链接。
- 如果实现和架构方案不同，把差异也写进对应 architecture plan 的 `Implementation Notes`。

## 模板

```md
## 功能名称

相关 issue：
相关 PR：
架构方案：
关键代码：
关键测试：

核心概念：

- 

实现时踩到的点：

- 

后续可以扩展：

- 
```

## UDP Ping/Pong

相关 issue：
相关 PR：
架构方案：
关键代码：
关键测试：

核心概念：

- UDP 是无连接传输，发送和接收都基于 datagram。
- packet header 需要显式 encode/decode，不能发送 raw struct memory。

## Virtual Connection, Heartbeat, Timeout

相关 issue：
相关 PR：
架构方案：
关键代码：
关键测试：

核心概念：

- UDP 连接状态是应用层维护出来的。
- 心跳用于保持连接可观察，timeout 用于判断对端长时间无响应。

## Packet Sequence, ACK, ACK Bits

相关 issue：
相关 PR：
架构方案：
关键代码：
关键测试：

核心概念：

- sequence 标识本端发出的 packet。
- ack / ack_bits 用于确认对端最近收到的一段 packet。

## Reliable Unordered Messages

相关 issue：#8
相关 PR：#9
架构方案：
关键代码：
关键测试：

核心概念：

- 可靠消息可以复用 packet 级 ACK，不一定需要单独 message ACK。
- 无序可靠只保证“至少送到并去重”，不保证处理顺序。
- 重发可以把同一个 message_id 放进新的 packet sequence。

## Unreliable Snapshot Synchronization

相关 issue：#10
相关 PR：
架构方案：`docs/architecture-plans/issue-10-unreliable-snapshot-synchronization.md`
关键代码：`src/MiniNet/include/mininet/snapshot.hpp`、`src/MiniNet/src/snapshot.cpp`、`src/MiniNet/src/connection.cpp`
关键测试：`tests/MiniNet.Tests/test_main.cpp`

核心概念：

- Snapshot 适合高频状态同步，例如位置和速度；它不保证最终送达。
- Snapshot 可以复用 packet header 的 sequence / ACK bits，但不能把 ACK 理解成 snapshot 可靠送达。
- Client 需要 bounded buffer 来处理乱序、重复和过旧 snapshot。
- 插值可以先做成两个 snapshot 之间的 position lerp，再逐步扩展成 render delay / jitter buffer。

实现时踩到的点：

- 未绑定 UDP socket 在 Windows 下查询 local port 可能失败；测试应先通过真实发送路径让 socket 绑定。
- Snapshot 的 float 字段不能直接发送 struct memory，应显式转成 IEEE754 binary32 位模式再编码。

后续可以扩展：

- 从 `SnapshotBuffer` 和目标 `server_time_ms` 自动寻找插值区间。
- 加入延迟、丢包、乱序模拟后观察 buffer 行为。
- 后续单独设计 extrapolation、client prediction 或 reconciliation。

## Network Simulator

相关 issue：#12
相关 PR：
架构方案：`docs/architecture-plans/issue-12-network-simulator.md`
关键代码：`src/MiniNet/include/mininet/network_simulator.hpp`、`src/MiniNet/src/network_simulator.cpp`
关键测试：`tests/MiniNet.Tests/test_main.cpp`

核心概念：

- localhost 太稳定，网络模拟器能把丢包、延迟、乱序、重复包变成可复现测试。
- Network Simulator 只处理 raw bytes，不解析 `PacketHeader`、ReliableMessage 或 Snapshot。
- deterministic seed 的目标是同一实现和同一输入序列可复现，不追求跨编译器完全一致。
- `poll` 的排序规则很重要：先按 `delivery_time`，相同时间按入队顺序。

实现时踩到的点：

- 配置错误应 reject，不应静默 clamp，否则测试参数写错会被掩盖。
- duplicate packet 保留相同 bytes/from/to，但 latency 应独立抽样。
- 不要为了 simulator 修改现有 `ConnectionClient` / `ConnectionServer`，否则会把测试工具变成 transport 重构。

后续可以扩展：

- 用 Network Simulator 重写 reliable resend 的丢包测试。
- 用 Network Simulator 测 Snapshot 乱序、丢失和重复包下的 buffer 行为。
- 后续单独设计 transport interface，让真实 socket 和 simulator 可以共享更高层协议状态机。

## Network Simulation Scenario Runner

相关 issue：#16
相关 PR：
架构方案：`docs/architecture-plans/issue-16-network-simulation-scenario-runner.md`
关键代码：`src/MiniNet/include/mininet/scenario.hpp`、`src/MiniNet/src/scenario.cpp`、`src/MiniNet.Sim/main.cpp`
关键文档：`docs/simulation.md`、`docs/visualizer/index.html`
关键测试：`tests/MiniNet.Tests/test_main.cpp`

核心概念：

- Scenario runner 是组合层，用现有 `NetworkSimulator`、packet 编解码、reliable message 和 snapshot buffer 观察整体行为。
- 它不新增 transport interface，也不把 simulator 注入 `ConnectionClient` / `ConnectionServer`。
- JSON / CSV / event log 都是固定字段手写导出，避免为学习工具引入第三方依赖。
- Visualizer 是静态 HTML，通过文件选择器读取导出的 JSON。

实现时踩到的点：

- `failed_reliable_messages`、`dropped_or_missing_snapshots`、`out_of_order_snapshots` 第一版保持 `not_available`。
- 当前 Windows MinGW 工具链的 `<filesystem>` 不稳定，文件写入使用保守的字符串路径和逐级目录创建。

后续可以扩展：

- 更细的 snapshot missing / out-of-order 统计。
- 批量 seed 实验和趋势对比。
- 更完整的 event timeline 可视化。

## Protocol and Architecture Documentation

相关 issue：#14
相关 PR：
架构方案：不单独保存；本 issue 本身就是文档产出。
关键文档：`docs/protocol.md`、`docs/architecture.md`
关键代码：`src/MiniNet/include/mininet/*.hpp`

核心概念：

- `docs/protocol.md` 解释 packet header、packet type、ACK bits、ReliableData、Snapshot 和 NetworkSimulator 的协议语义。
- `docs/architecture.md` 解释 packet、connection、ack_tracker、reliable_message、snapshot、network_simulator、udp_socket 的模块职责。
- 文档要记录当前真实实现，不能提前声称 ordered delivery、fragmentation、congestion control 等尚未实现能力。

实现时踩到的点：

- issue 草稿中提到 ReliableData 有 `message_type`，但当前代码没有这个字段；文档按真实代码写为 `message_count + message_id + payload_size + payload bytes`。

后续可以扩展：

- 后续新增 reliable ordered、RTT/RTO、scenario runner 时，先同步更新协议和架构文档。
