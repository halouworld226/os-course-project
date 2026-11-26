# 并行图像处理与故障恢复系统

2026 秋季操作系统课程实训项目。使用 C++17 实现客户端／服务端协议、共享四进程池、两级图块调度、工作进程故障恢复及 inode 磁盘镜像，使用 Python 标准库提供网页和自动测试。

## 快速开始

先安装 Git、Python 3.9+ 和支持 C++17 的 g++，再克隆仓库。

```bash
git clone https://github.com/halouworld226/os-course-project.git
cd os-course-project
```

### Linux

```bash
python3 build.py --test
python3 web.py --server ./build/os_server --data demo-data-v3
```

### Windows PowerShell

使用 UCRT MinGW g++，确保 `g++` 和 `python` 在 PATH 中。

```powershell
python build.py --test
python web.py --server build/os_server.exe --data demo-data-v3
```

打开浏览器访问 `http://127.0.0.1:8000`。默认网页通过常驻管道与同一个 C++ 服务通信，作业会异步入队，页面轮询服务端真实状态。

| 演示账号 | 密码 | 权限 |
|---|---|---|
| `alice` | `demo123` | 提交、查看和下载自己的作业 |
| `bob` | `demo123` | 提交、查看和下载自己的作业 |
| `admin` | `demo123` | 查看全局队列、图块历史和忙进程，终止工作进程 |

这些预置账号用于本地课程演示。

## 输入与演示

输入为 P6 PPM，最大通道值 255，宽高各为 1–2048 像素。支持灰度和反色，图像按 64×64 图块划分，边缘使用实际尺寸。

生成一张可上传的样图，在网页中选择该文件。

```bash
python3 client.py sample sample.ppm
```

Windows 使用 `python`。演示时依次执行以下操作。

1. 用两个浏览器会话分别登录 alice 和 bob，提交普通和高优先级作业。
2. 登录 admin，观察高／普通就绪队列、工作进程 PID、图块画布及尝试记录。
3. 终止一个忙 worker，观察退出、重新入队、再次派发及完成事件。
4. 下载最终图，与串行参考结果比较，正常重启后再次下载已完成作业。

处理很快时，可在启动服务前设置每图块演示延迟。Linux 示例：

```bash
OS_DEMO_DELAY_MS=300 python3 web.py --server ./build/os_server --data demo-data-v3
```

PowerShell 示例：

```powershell
$env:OS_DEMO_DELAY_MS='300'
python web.py --server build/os_server.exe --data demo-data-v3
```

延迟范围为 0–2000 毫秒，默认 0。性能测试使用默认值，并记录环境与参数。PowerShell 演示结束后运行 `Remove-Item Env:OS_DEMO_DELAY_MS`。

## 独立 TCP 模式

Linux 的终端 A 启动 C++ 服务，终端 B 启动网页桥接。

```bash
./build/os_server 8080 demo-data-v3
```

```bash
python3 web.py --port 8080 --data web-data-v3
```

Windows 将可执行文件改为 `build/os_server.exe`，Python 命令改为 `python`。服务及网页均监听本机回环地址。

命令行也支持同步提交，命令输出作业号。后续用该值替换 `JOB_ID`。

```bash
python3 client.py --user alice submit sample.ppm --operation invert --priority high --fault-once
python3 client.py --user alice status JOB_ID
python3 client.py --user alice download JOB_ID inverted.ppm
```

`--fault-once` 实际终止一个本服务的 worker，以便验证图块重派。

## 构建与测试

`build.py` 直接调用编译器，无需 Make 或 Ninja。默认构建到 `build/`，可用 `--compiler clang++` 选择编译器。

```bash
python3 build.py --test
```

此命令验证图像处理、协议边界、共享调度池、故障恢复、文件系统、权限、常驻服务及网页路由。需要本机允许 TCP 连接时，再运行：

```bash
python3 build.py --test --tcp
```

项目也提供 CMake 工程，需要本机已有 Make、Ninja 或 Visual Studio 生成器。

```bash
cmake -S . -B build
cmake --build build --parallel
cd build
ctest --output-on-failure -V
```

## 实现与源码

四个常驻工作进程共享普通／高优先级队列。调度以图块为单位，优先领取高优先级任务，同级 FIFO，正在执行的图块继续完成。工作进程退出后，调度器回收进程、补充 worker，并按原优先级重派未成功提交的图块，每块最多尝试三次。完成计数和最终图只纳入一次有效结果。

自建文件系统默认使用 128 MiB 的 `DATA_DIR/disk.img`，包含超级块、inode 表、空闲块位图和数据区，支持多级目录、文件读写删除、块分配及回收。原图、作业描述、所有者、已提交图块和最终图均写入镜像，完成标记最后提交。失败作业会回收已分配空间。

| 路径 | 内容 |
|---|---|
| `src/server.cpp`、`src/net.*` | C++ 请求服务、认证、归属权限及 TCP |
| `src/job_scheduler.*`、`src/worker.*` | 共享调度池、进程通信与恢复 |
| `src/inode_fs.*`、`src/storage.*` | inode 镜像、持久化和空间回收 |
| `src/image.*` | 图像格式、切块、拼图、灰度及反色 |
| `web.py`、`static/` | 登录、画布、队列、时间线和管理入口 |
| `client.py` | 命令行客户端及本地管道会话 |
| `tests/` | 模块测试、集成测试和独立串行参考 |

TCP 请求以 `AUTH username password` 行开头，服务端检查账号及作业所有权。主要指令为 `SUBMIT`、`ENQUEUE`、`STATUS`、`GET`、`TILE`，管理员使用 `ACTIVE`、`QUEUES` 和 `KILL`。标准输入输出开发入口允许省略 AUTH，此时视为本机管理员。

每帧使用 4 字节大端长度后接字节，单帧限 16 MiB。服务最多同时接受 8 个请求连接和 8 个在途作业。详细存储布局见 [STORAGE.md](STORAGE.md)，网页及演示说明见 [WEB.md](WEB.md)。

## 当前验证范围

Windows UCRT MinGW g++ 的 C++17 严格编译及非网络测试已通过，覆盖真实进程终止与重派、最终像素一致性、磁盘空间不足回滚、失败作业回收、镜像重开和权限检查。常驻服务及网页路由已使用真实 C++ 后端验证。

Linux 原生构建、真实 TCP 客户端、Linux 终端 kill 和浏览器现场操作仍需组员在目标环境验收。详细记录和剩余任务见 [CHECKPOINT.md](CHECKPOINT.md)。

v1/v2 普通目录历史尚无导入工具，升级请使用新的 v3 数据目录。服务器异常退出后的未完成作业续算、掉电恢复、LRU 缓存和整盘备份作为后续拓展。

## 四人协作

`main` 用作共同基线。成员在各自分支修改代码，提交 Pull Request，经队友检查和对应测试后合并。

| 分支 | 负责范围 |
|---|---|
| `feature/scheduler` | 调度、进程和故障恢复 |
| `feature/storage` | 文件系统和持久化 |
| `feature/ui` | 网页展示和演示流程 |
| `feature/integration` | Linux、TCP、集成测试及合并 |

完整操作示例见 [COLLABORATION.md](COLLABORATION.md)。构建产物、磁盘镜像、运行数据和环境文件由 `.gitignore` 排除。
