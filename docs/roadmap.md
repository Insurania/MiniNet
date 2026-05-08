# MiniNet Roadmap

这份路线图用于指导项目迭代。每个阶段都应该对应一个或多个小 issue，不建议一次性实现太多功能。

## Phase 0: 项目骨架与协作模板

目标：建立仓库结构，明确协作方式。

建议产出：

- README
- roadmap
- issue 模板
- PR 模板
- 基础源码和测试目录

完成标准：

- 新成员能通过 README 理解项目目标。
- 后续 issue 可以直接引用路线图中的阶段。
- PR 描述和 review 有统一格式。

## Phase 1: UDP Ping/Pong

目标：实现最小 UDP 收发。

建议范围：

- 服务端监听 UDP 端口。
- 客户端发送 Ping。
- 服务端返回 Pong。
- 客户端打印 RTT。
- 非法数据包不会导致进程崩溃。

不包含：

- 连接状态
- 重传
- ACK
- 心跳

建议测试：

- 客户端能收到 Pong。
- 服务端能忽略非法包。
- 服务端未启动时客户端能超时退出。

## Phase 2: 逻辑连接与心跳

目标：在 UDP 之上维护自己的连接状态。

建议范围：

- ClientHello
- ServerWelcome
- connection id
- connected / disconnected 状态
- heartbeat
- heartbeat timeout

关键学习点：

- UDP 本身没有连接。
- 连接状态需要应用层自己维护。
- 超时是网络系统的正常分支。

## Phase 3: 序号与 ACK

目标：给每个数据包增加序号和确认信息。

建议范围：

- sequence
- ack
- ack bits
- received packet tracking
- duplicate packet detection

关键学习点：

- sequence 用于标识本端发出的包。
- ack 表示已收到对方的最新包。
- ack bits 表示最近收到过哪些旧包。

## Phase 4: 可靠消息与重传

目标：在 UDP 上实现简化可靠消息。

建议范围：

- reliable message id
- pending reliable queue
- retransmit timeout
- duplicate reliable message handling
- at-most-once delivery to application layer

不包含：

- 分片
- 加密
- 拥塞控制
- NAT 打洞
- 多频道优先级

关键学习点：

- 重传解决的是可靠性，不保证低延迟。
- 收到重复可靠消息时不能重复执行业务逻辑。
- ACK 和 reliable message delivery 是相关但不同的概念。

## Phase 5: 不可靠状态同步

目标：实现适合游戏状态的非可靠同步。

建议范围：

- entity id
- state tick
- position payload
- newer state overwrites older state
- old tick ignored

关键学习点：

- 位置、朝向、动画状态通常不需要重传。
- 新状态比旧状态更重要。
- 不可靠消息不是“不重要”，而是“可以被下一次更新覆盖”。

## Phase 6: 网络模拟器

目标：模拟常见网络问题，帮助测试协议行为。

建议范围：

- fixed latency
- random latency
- packet loss
- packet duplication
- packet reordering

关键学习点：

- 网络代码不能只在 localhost 的完美网络下验证。
- 可控模拟器能把偶发问题变成可复现测试。

## Phase 7: Demo 与文档整理

目标：把 MiniNet 变成一个可演示、可解释的小项目。

建议范围：

- client/server demo
- reliable chat message
- unreliable position update
- simulated packet loss
- protocol.md
- review examples

完成标准：

- 可以运行一个本地 demo 观察日志。
- 可以通过文档解释协议如何工作。
- 可以通过测试验证核心行为。

