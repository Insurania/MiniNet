# Architecture Plans

这个目录用于保存 `architecture_planner` 为每个功能 issue 生成的架构方案，方便后续学习和复盘。

命名规则：

```text
issue-N-short-title.md
```

示例：

```text
issue-8-reliable-unordered-messages.md
```

每份方案建议包含：

- Proposed Architecture
- Data Flow
- Files to Create or Modify
- Key Types and Functions
- Implementation Order
- Test Mapping
- Non-goals Preserved
- Implementation Notes

`Implementation Notes` 是实现后的复盘区，用来记录：

- 哪些地方和原计划一致。
- 哪些地方在实现时调整了。
- 为什么调整。
- 后续还想继续学习或改进什么。

这些文档是学习材料，不要求和最终实现完全一字不差；重要的是把“为什么计划这样写”和“为什么实现时改变”记录下来。
