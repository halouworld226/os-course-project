# 四人分支协作

`main` 保存已验证的公共版本。每位成员从 main 建立自己的分支，完成一项修改后提交 Pull Request，其他成员检查后合并。

| 分支 | 负责范围 |
|---|---|
| `feature/scheduler` | 调度、进程通信和故障恢复 |
| `feature/storage` | inode 镜像、持久化及空间回收 |
| `feature/ui` | 网页、实际状态展示和演示 |
| `feature/integration` | Linux 与网络验收、整体测试和合并 |

接受仓库邀请后，先 clone 仓库，再进入目录。下面以调度分支为例。

```bash
git switch feature/scheduler
git pull --ff-only
python3 build.py --test
```

完成修改后，先运行对应测试，再提交明确的文件。可先用 `git status` 检查待提交列表。

```bash
git add src/job_scheduler.cpp tests/shared_scheduler_check.cpp
git commit -m "Fix priority dispatch after worker recovery"
git push -u origin feature/scheduler
```

到 GitHub 打开 Pull Request，目标选择 main，来源选择自己的分支。说明修改了什么、怎样验证、还有哪些问题。安排一位队友检查后合并。

开始下一轮工作前，将最新 main 合入自己的分支。

```bash
git fetch origin
git switch feature/scheduler
git merge origin/main
```

出现冲突时，先与修改同一文件的成员确认应保留的逻辑，再解决冲突并复跑测试。各模块负责人可以协商修改边界，涉及接口变化时同步通知调用方。

构建输出、磁盘镜像、运行数据和环境文件已经加入忽略规则。项目当前验证范围和验收事项见 CHECKPOINT.md，启动步骤见 README.md。
