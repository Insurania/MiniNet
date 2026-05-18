# MiniNet

MiniNet 是一个学习型项目，目标是在实现一个简化版游戏网络传输层的过程中，练习和 AI agent 协作的完整工作流。

项目会尽量保持小而清楚：先写 issue，再明确验收标准和测试用例，然后实现、测试、写 PR 描述、做 review。网络层本身基于 UDP，逐步支持连接、心跳、序号、ACK、丢包重传、可靠消息、不可靠状态同步，以及延迟和丢包模拟。

## 学习目标

- 学会把需求拆成适合 AI agent 实现的小 issue。
- 学会写清楚的验收标准和测试用例。
- 学会阅读 AI 生成代码并提出 review comment。
- 理解 UDP 游戏网络传输层的核心概念。
- 用可运行、可测试的小步骤逐步构建 MiniNet。

## 技术方向

默认方向是 C++ / CMake，原因是它更贴近网络传输层的底层细节，也适合学习 UDP packet 编解码、socket API 和跨平台构建基础。

当前仓库从第一个 issue 开始逐步实现网络传输层能力。

## 计划功能

- UDP Ping/Pong：已完成
- 逻辑连接握手：已完成
- 心跳与超时：已完成
- 包序号：已完成
- ACK 与 ACK bits：已完成
- 可靠消息：已完成
- 丢包重传：已完成
- 不可靠状态同步：已完成
- 延迟、丢包、乱序、重复包模拟：已完成

## 目录结构

```text
MiniNet/
  .github/
    ISSUE_TEMPLATE/
      bug_report.md
      feature_request.md
    pull_request_template.md
  CMakeLists.txt
  docs/
    agent-workflow.md
    architecture-plans/
    learning-index.md
    post-merge-checklist.md
    project-rules.md
    roadmap.md
  src/
    MiniNet/
      include/mininet/
      src/
    MiniNet.Client/
    MiniNet.Server/
  tests/
    MiniNet.Tests/
```

## 已实现能力

当前 MiniNet 已实现：

- Typed packet header：`magic / version / type / sequence / session_id / ack / ack_bits`
- UDP Ping/Pong
- 基础 packet 编码、解码和校验
- UDP virtual connection
- Client 状态：`Disconnected / Connecting / Connected / TimedOut`
- Server session 管理
- `ConnectRequest / ConnectAccept / Disconnect / Heartbeat`
- 1 秒 heartbeat
- 5 秒 timeout
- Packet sequence 分配
- `ack` 和 `ack_bits` 生成
- Sent packet acked/unacked 状态追踪
- `uint32_t` sequence wrap-around comparison
- Reliable unordered message
- Packet-level ACK 驱动的可靠消息 delivery cleanup
- Reliable message resend
- Reliable receiver duplicate suppression
- Unreliable snapshot sync
- Snapshot payload 显式 encode/decode
- Snapshot buffer、乱序插入、重复/过旧丢弃和插值 helper
- Deterministic in-memory network simulator
- 延迟、抖动、丢包、重复包和乱序投递模拟

当前仍未实现：

- Ordered delivery
- Congestion control
- RTT 平滑估计
- Reliable ordered delivery
- Fragmentation / 大包传输
- Client prediction / rollback / reconciliation
- 真实 transport abstraction，把 simulator 注入现有 connection 层

## 核心模块

- `packet`：负责固定 packet header 的编码、解码和基础校验。
- `udp_socket`：封装 UDP socket 的发送、接收和超时等待。
- `ping`：实现最小 Ping/Pong 示例。
- `connection`：实现 UDP 之上的 virtual connection、heartbeat、timeout 和 disconnect。
- `ack_tracker`：实现 sequence 分配、received history、sent history、ACK 和 ACK bits。
- `reliable_message`：实现可靠无序消息、packet ACK delivery cleanup 和重传选择。
- `snapshot`：实现不可靠状态同步的 snapshot 编解码、buffer 和插值 helper。
- `network_simulator`：实现测试用 deterministic in-memory 网络模拟器。

## 推荐协作流程

1. 写一个小 issue，说明背景、目标、范围、验收标准和测试用例。
2. 让 AI agent 根据 issue 实现最小可用版本。
3. 本地运行测试和 demo。
4. 写 PR 描述，说明改了什么、怎么测的、哪些内容刻意没做。
5. 做 code review，重点看边界情况、职责划分、测试覆盖和后续可维护性。
6. 根据 review 修改，再合并。

详细项目规则见 [docs/project-rules.md](docs/project-rules.md)。

## 构建与测试

配置 CMake 构建目录：

```powershell
cmake -S . -B build -G "MinGW Makefiles"
```

构建项目：

```powershell
cmake --build build
```

运行测试：

```powershell
ctest --test-dir build --output-on-failure
```

启动本地 server：

```powershell
.\build\MiniNet.Server.exe 40000
```

另开一个 PowerShell 窗口运行 client：

```powershell
.\build\MiniNet.Client.exe 127.0.0.1 40000
```

## 当前状态

- 已建立项目目录结构。
- 已添加 README、路线图、issue 模板和 PR 模板。
- 已实现 issue #1：UDP Ping/Pong、typed packet header、基础包校验和本地测试。
- 已实现 issue #3：UDP virtual connection、session、heartbeat、timeout 和 disconnect。
- 已实现 issue #6：packet sequence、ACK 和 ACK bits。
- 已实现 issue #8：Reliable unordered messages、ACK 后清理、丢包重传和去重。
- 已实现 issue #10：Unreliable snapshot synchronization、SnapshotBuffer 和插值 helper。
- 已实现 issue #12：Deterministic network simulator，支持延迟、抖动、丢包、重复包和乱序。
