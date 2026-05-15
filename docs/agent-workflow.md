# AI Agent Workflow

这份文档定义 MiniNet 处理 GitHub issue 时的标准 AI agent 协作流程。

目标不是让 AI 一次性替你完成所有判断，而是让每个阶段都有明确输入、输出和人工确认点。

## 标准流程

GitHub issue 不再一律使用同样重的流程。开始 issue 前，Codex 应先判断规模：

- `small`：文档、注释、模板、README、小测试修正。使用轻量流程，不启动完整 subagent。
- `normal`：边界清楚的普通功能。使用精简四 agent 工作流。
- `large`：协议设计变化、复杂状态机、跨模块重构或高风险改动。使用完整四 agent 工作流。

### small 轻量流程

适用场景：文档、规则、模板、注释、小型配置、README 更新。

流程：

```text
阅读相关文件 -> 最小修改 -> 构建/测试或说明无需测试 -> 提交/推送
```

### normal 精简四 agent 工作流

适用场景：大多数功能 issue。

流程仍然是：

1. `issue_reviewer`
2. `architecture_planner`
3. `implementation_worker`
4. `test_worker`

但每个 agent 只接收与当前 issue 直接相关的 issue 内容、项目规则摘要、相关文件和上一步结论。输出优先给结论、风险、修改点和测试结果，避免重复项目背景。

### large 完整四 agent 工作流

适用场景：协议格式变化、状态机重构、可靠传输策略变化、跨模块设计、风险较高的 issue。

流程同标准四 agent，但允许更详细的方案、风险分析和测试矩阵。

除非你明确要求跳过某一步，否则 `normal` 和 `large` issue 不要直接进入实现。

## Step 1: issue_reviewer

目的：检查 issue 是否已经适合实现。

输入：

- GitHub issue 编号或 issue 内容。
- 当前项目规则。
- 相关路线图阶段。

检查重点：

- 目标是否清楚。
- 范围是否太大。
- non-goals 是否明确。
- 验收标准是否可观察、可验证。
- 测试用例是否覆盖正常路径和关键边界情况。
- 是否把 ACK、重传、连接、心跳等后续能力意外混进当前 issue。

输出：

- Issue 是否 Ready。
- 主要问题。
- 建议修改文字。
- 完整的修订版 issue 草稿。
- 缺失的验收标准。
- 缺失的测试。
- scope creep 风险。

人工确认点：

- 如果 issue 是 `Not Ready`，应先修改 issue。
- `issue_reviewer` 必须给出一份完整的修订版 issue 正文，方便你直接查看和复制。
- Codex 不能在没有确认的情况下直接修改 GitHub issue。
- 如果你确认修订版 issue，可以让 Codex 使用 `gh issue edit <编号> --body-file <文件>` 直接更新 GitHub。
- 如果只是小问题，可以由你确认后继续进入架构设计。

推荐确认方式：

```text
这个修订版可以，帮我更新到 GitHub issue #N
```

如果 GitHub CLI、登录或网络失败，Codex 应输出修订版 issue 正文，让你手动复制到 GitHub，不要反复要求重装或重新登录。

GitHub 更新方式：

1. Codex 先把修订版 issue 正文展示给你。
2. 你确认后，Codex 可以把正文写入临时文件，例如 `docs/tmp/issue-N-body.md`。
3. Codex 执行：

```powershell
gh issue edit N --repo Insurania/MiniNet --body-file docs/tmp/issue-N-body.md
```

4. 更新成功后，Codex 应删除临时文件，或在你希望保留复盘记录时把它移动到正式文档目录。

## Step 2: architecture_planner

目的：把已确认的 issue 转成最小实现方案。

输入：

- 已通过 review 的 issue。
- `issue_reviewer` 的结论。
- 当前代码结构。

检查重点：

- 模块边界。
- 数据流。
- 文件布局。
- public 类型和职责。
- 最小实现顺序。
- 风险和限制。
- 测试如何映射验收标准。

输出：

- Proposed Architecture。
- Data Flow。
- Files to Create or Modify。
- Key Types and Functions。
- Implementation Order。
- Test Mapping。
- Non-goals Preserved。

保存规则：

- `architecture_planner` 必须把最终架构方案保存到 `docs/architecture-plans/`。
- 文件命名使用 `issue-N-short-title.md`，例如 `issue-8-reliable-unordered-messages.md`。
- 保存的文档应包含与最终回复相同的主要章节，方便你后续学习和复盘。
- `architecture_planner` 只允许写这个架构方案文档，不应该修改生产代码、测试、构建文件或 README。

人工确认点：

- 如果方案引入了过多抽象，应缩小。
- 如果方案遗漏验收标准，应补齐后再实现。

## Step 3: implementation_worker

目的：按 issue 和架构方案实现最小代码改动。

输入：

- 已确认的 issue。
- `architecture_planner` 输出的实现方案。
- 当前项目规则和注释规则。

实现规则：

- 不扩大范围。
- 不实现 non-goals。
- 不发送 raw struct memory。
- 使用固定宽度整数。
- 保持协议解析和 socket IO 尽量分离。
- 保持改动小而可 review。
- 头文件和实现文件按项目注释规则补充说明。

输出：

- Implementation Summary。
- Files Changed。
- Design Decisions。
- Commands Run。
- Known Limitations。
- Follow-up Needed。

人工确认点：

- 实现后先看 diff 是否符合 issue 范围。
- 不理解的代码要先要求解释，不急着进入测试。

## Step 4: test_worker

目的：补齐并运行测试，确认实现满足验收标准。

输入：

- issue 验收标准。
- `architecture_planner` 的 Test Mapping。
- `implementation_worker` 的代码改动。

测试重点：

- packet encode/decode。
- packet validation。
- 正常路径。
- 非法输入。
- 超时或失败路径。
- 和当前 issue 相关的回归测试。
- 终端可见的测试过程和关键结果，而不是只有最终成功信息。

测试输出要求：

- 每个测试组应输出开始和通过信息。
- 对学习有帮助的关键值应输出，例如 `sequence`、`ack`、`ack_bits`、`session_id`、packet type、timeout 结果。
- 输出要简洁，避免大量无意义逐行 dump。
- 运行测试时优先使用能显示 stdout 的命令，例如：

```powershell
ctest --test-dir build --output-on-failure --verbose
```

必要时可以直接运行测试可执行文件，以便查看完整测试过程。

输出：

- Test Plan。
- Tests Added。
- Acceptance Criteria Mapping。
- Commands Run。
- Results。
- Bugs Found。
- Suggested Fixes。

人工确认点：

- 如果测试失败，回到 implementation_worker 修复。
- 如果测试覆盖不足，先补测试，再写 PR。

## GitHub Issue 工作方式

当你在 GitHub 上写好 issue 后，可以这样告诉 Codex：

```text
按标准 agent workflow 处理 issue #2
```

Codex 应该按顺序执行：

```text
issue_reviewer -> architecture_planner -> implementation_worker -> test_worker
```

Codex 应先说明本次 issue 属于 `small`、`normal` 还是 `large`，并采用对应流程。

如果 GitHub CLI 或网络不可用，Codex 应要求你粘贴 issue 内容，不要反复要求登录或重装。

## PR 前检查

进入 PR 前应确认：

- issue 已经过 review。
- 实现方案没有扩大范围。
- 代码已按注释规则补充必要说明。
- 构建通过。
- 测试通过。
- PR 描述包含测试结果和未实现内容。
- 如果该 PR 完成 issue，应在 PR 描述里写 `Closes #issue_number`。
