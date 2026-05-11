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
  CMakeLists.txt
  docs/
    project-rules.md
    roadmap.md
  src/
    MiniNet/
    MiniNet.Client/
    MiniNet.Server/
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
- 尚未实现连接、心跳、ACK、重传、可靠消息和不可靠状态同步。
