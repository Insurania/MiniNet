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

默认方向是 C# / .NET，原因是它在 Windows 上稳定、UDP 支持直接、测试工具成熟，也方便未来迁移到 Unity 场景。

当前仓库先只放项目结构和协作模板，暂不引入具体代码。后续建议从第一个 issue 开始：实现 UDP Ping/Pong。

## 计划功能

- UDP Ping/Pong
- 逻辑连接握手
- 心跳与超时
- 包序号
- ACK 与 ACK bits
- 可靠消息
- 丢包重传
- 不可靠状态同步
- 延迟、丢包、乱序、重复包模拟

## 目录结构

```text
MiniNet/
  .github/
    ISSUE_TEMPLATE/
      bug_report.md
      feature_request.md
    pull_request_template.md
  docs/
    roadmap.md
  src/
    MiniNet/
  tests/
    MiniNet.Tests/
```

## 推荐协作流程

1. 写一个小 issue，说明背景、目标、范围、验收标准和测试用例。
2. 让 AI agent 根据 issue 实现最小可用版本。
3. 本地运行测试和 demo。
4. 写 PR 描述，说明改了什么、怎么测的、哪些内容刻意没做。
5. 做 code review，重点看边界情况、职责划分、测试覆盖和后续可维护性。
6. 根据 review 修改，再合并。

## 当前状态

- 已建立项目目录结构。
- 已添加 README、路线图、issue 模板和 PR 模板。
- 尚未添加源码、测试项目或网络协议实现。

