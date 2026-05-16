# Post-Merge Checklist

这份清单用于 PR 合并后同步本地仓库、清理分支和补充学习记录。

## 推荐流程

每次 PR 合并后，优先使用保守命令：

```powershell
git switch main
git pull --ff-only origin main
git branch --merged
```

`git pull --ff-only` 只允许快进更新。如果失败，说明本地和远端发生分叉，不要直接 reset，先确认原因。

## 检查项

- [ ] 本地已切回 `main`。
- [ ] 本地 `main` 已更新到远端最新。
- [ ] 已确认 PR 对应 issue 被关闭。
- [ ] 已确认哪些本地 feature 分支已经合并。
- [ ] 已删除确认合并且不再需要的本地 feature 分支。
- [ ] 如 GitHub 没自动删除远端 feature 分支，再确认是否需要删除。
- [ ] 如功能状态变化影响 README / roadmap，单独更新文档。
- [ ] 如实现和架构方案有差异，补充对应 `docs/architecture-plans/issue-N-*.md` 的 `Implementation Notes`。

## 常用命令

查看已合并到当前分支的本地分支：

```powershell
git branch --merged
```

删除已确认合并的本地分支：

```powershell
git branch -d branch-name
```

查看远端分支：

```powershell
git branch -r
```

删除远端分支前要确认 GitHub PR 已合并，且不再需要该分支：

```powershell
git push origin --delete branch-name
```
