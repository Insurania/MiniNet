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
