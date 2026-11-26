# 网页实验台

网页桥接层使用 Python 3 标准库，界面通过 HTTP 访问桥接层，再由桥接层调用 C++ 服务。默认启动一个持续运行的 C++ 标准输入输出会话，多个作业共用该进程及其四个工作进程。图片计算、工作进程和磁盘存储均由 C++ 完成。

从项目目录启动，Windows 示例：

```powershell
python web.py --server build/os_server.exe --data demo-data
```

Linux 示例：

```bash
python3 web.py --server ./build/os_server --data demo-data
```

打开 `http://127.0.0.1:8000`。默认监听本机，网页端口可用 `--web-port 8001` 修改。若已另开 C++ TCP 服务，可运行 `python3 web.py --port 8080 --data demo-data`，其中 `--data` 仍用于桥接层的账号归属记录。

为了留出观察和点击终止的时间，可在启动前设置演示延时。PowerShell 用 `$env:OS_DEMO_DELAY_MS = '500'`，Linux 用 `OS_DEMO_DELAY_MS=500 python3 web.py --server ./build/os_server --data demo-data`。每个图块处理前延时 500 ms。性能测试时将该值设为 0，或移除该环境变量。

三个演示账号为 `alice`、`bob`、`admin`，密码均为 `demo123`。alice 和 bob 只能查看自己通过该桥接层提交的作业，admin 可以查看全部。会话使用随机 Cookie，退出后失效，重启网页进程也需重新登录。桥接层每次向 C++ 发命令时，都会添加当前用户的 `AUTH <username> demo123` 前缀，C++ 再按作业归属和管理员角色校验访问。管理员队列和终止操作使用 admin 身份。演示账号仅用于本机课程演示。

登录后下载示例 PPM，选择灰度或反色、普通或高优先级再上传。完成后会显示真正的处理结果、图块边界、工作进程时间线和每次尝试的事件。点击图块或下拉菜单可筛选其历史。高优先级命令附加 `HIGH`。勾选故障演示时，提交命令附加 `FAULT`，由 C++ 调度器终止一个忙工作进程并进行恢复。

持续会话和 TCP 模式均调用 `ENQUEUE` 快速取得作业号，再每 0.3 秒从 C++ 读取真实 `STATUS`，浏览器每 2 秒更新图块事件和 PID 时间线。已提交图块通过 `TILE` 取回实际像素，按原坐标显示在画布上，剩余区域保持底色。管理员可查看 `ACTIVE` 返回的全局作业，以及 `QUEUES` 返回的两级就绪队列和忙进程。队列按服务端顺序列出前 50 项并显示总数。管理员可选择忙 PID 并终止，桥接层校验管理员会话后调用 `KILL`。处理完成后显示真实整图。网页不会生成模拟的运行事件。

桥接层在数据目录中写 `web-jobs.json` 保存作业归属和返回状态，重启后仍可查看已完成的作业。持续会话的短请求通过锁保证请求和响应配对，ENQUEUE 后计算在后台继续，因此可继续查询、提交其他作业或终止忙进程。连接 C++ TCP 服务时，桥接层也允许两个提交同时调用后端，作业之间由 C++ 共享进程池及优先级队列处理。

## 验证

下面的测试直接调用 HTTP 处理器使用的路由分发函数，并真正运行 C++ 子进程。它适用于本地 TCP 被运行环境阻止时的验证。

```bash
python3 tests/web_smoke.py ./build/os_server
python3 tests/web_smoke.py ./build/os_server --session
```

它覆盖登录与退出、普通用户作业隔离、管理员查看、两种实际处理结果、BMP 预览、作业归属重开。`--session` 另外启用 250 ms 演示延时，验证实际 ENQUEUE/STATUS、就绪队列、管理员终止忙进程、退出后重派及真实图块预览，并直接向 C++ 发出越权请求核对拒绝行为。另用协议替身验证优先级与故障参数传递。HTTP 浏览器连通、页面布局及 TCP 路径需要在允许本机回环连接的环境中另行复跑。
