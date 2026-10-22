# omni-arm-link

面向比赛机器人的低延迟、双向 IEEE 802.11 原始链路通信栈，基于
**WFB-NG + RTL8812EU** 构建。

本项目实现了 [`通信方案.md`](通信方案.md) 中描述的应用层与协议层：

- **Controller（遥控器端）**（Raspberry Pi 4B）：摇杆 / 按键 / 模式 / 急停
- **Robot（机器人端）**（NVIDIA Jetson Orin）：200 Hz 发送 5 组四元数 +
  15 个激光数据 + 50 个状态量（约 544 kbps 原始负载，约 1 Mbps 链路预算）
- **Monitor（第三节点）**：被动、只收的数据包记录与链路分析

WFB-NG 提供 Monitor Mode、原始帧注入、FEC 与加密。本仓库提供自定义机器人
协议、三个应用程序、WFB-NG 配置、配置脚本以及测试/仿真工具。

---

## 系统架构

```
                    ┌──────────────────┐
                    │    Monitor       │
                    │ oal_monitor      │  只收，JSONL 记录 + 统计
                    └────────▲─────────┘
                             │ 被动接收（上下行）
                             │
┌────────────────────┐              ┌────────────────────┐
│   Controller       │              │       Robot        │
│   oal_controller   │              │   oal_robot        │
│   RPi 4B           │              │   Jetson Orin      │
└─────────┬──────────┘              └─────────┬──────────┘
          │ UDP（本机）                        │ UDP（本机）
          ▼                                    ▼
     wfb_tx / wfb_rx                       wfb_tx / wfb_rx
          │                                    │
          └──────── Raw IEEE 802.11 ───────────┘
                    (WFB-NG, RTL8812EU)
```

应用程序不会直接操作无线网卡，而是与本机的 `wfb_tx` / `wfb_rx` 进程交换 UDP
数据报，由 WFB-NG 完成 FEC、加密以及原始帧注入 / Monitor Mode 接收。

### 逻辑数据流

| 流   | ID   | 方向                 | 速率    | FEC        | 用途                   |
|------|------|----------------------|---------|------------|------------------------|
| 0    | 0x01 | Robot → Controller   | 200 Hz  | K=8 N=12   | 核心遥测               |
| 1    | 0x02 | Robot → Controller   | ~5 Hz   | K=4 N=8    | 调试 / 日志            |
| 2    | 0x80 | Controller → Robot   | 200 Hz  | K=1 N=3    | 控制（最高优先级）     |
| test | 0x7E | Robot → Controller   | 可配置  | K=2 N=4    | 链路测试               |
| test | 0xFE | Controller → Robot   | 可配置  | K=2 N=4    | 链路测试               |

低优先级的调试数据为独立数据流，绝不会阻塞控制数据。

---

## 线上协议

每个 UDP 数据报承载恰好一个帧：

```
FrameHeader（28 字节，小端）
  u32 magic          0x314C414F ("OAL1")
  u8  version        1
  u8  stream         逻辑流 ID
  u16 flags          bit0 急停, bit1 心跳, bit2 应答, bit3 关键
  u32 seq            每流序列号
  u64 timestamp_ns   发送方墙上时钟（epoch）纳秒
  u16 payload_len
  u16 reserved
  u32 payload_crc    负载的 CRC-32 (IEEE)
```

负载：

- **遥测 Telemetry**（340 B）：`float[5][4]` 四元数、`float[15]` 激光、
  `float[50]` 状态量 —— 与比赛要求的格式完全一致。
- **控制 Control**（36 B）：`float[4]` 轴、`u16` 按键、`u8` 模式、
  `u8` 标志、`float[4]` 控制参数。
- **调试 Debug**（变长）：等级 + 长度前缀的 UTF-8 文本。

帧 CRC 与 magic 使得在 WFB-NG 自身完整性校验之上，仍可在应用层检测出损坏或
外来的数据包。

---

## 构建

依赖：CMake ≥ 3.16、C++17 编译器、pthread。无需任何第三方库。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

可执行文件：`oal_robot`、`oal_controller`、`oal_monitor`、`oal_linktest`、`oal_sim`。

常用选项：`-DOAL_BUILD_TOOLS=OFF`、`-DOAL_WARNINGS_AS_ERRORS=ON`、
`-DOAL_ENABLE_ASAN=ON`。

---

## 无电台快速开始（仿真）

仿真工具使用普通 UDP，并复现完全相同的应用层协议，因此整条链路可在一台主机上
验证：

```bash
# 终端 1：被动监听
./build/oal_monitor --duration 10 --no-measure-latency

# 终端 2：仿真机器人，向监听端的遥测端口发送
./build/oal_sim robot --dest 127.0.0.1:5622 --rate 200 --loss 0.02 --jitter-ms 2

# 终端 3：仿真遥控器，向监听端的控制端口发送
./build/oal_sim controller --dest 127.0.0.1:5621 --rate 200
```

也可以把真实应用程序直接对接（绕过 WFB-NG）：

```bash
./build/oal_robot --telemetry-tx 127.0.0.1:5612 --debug-tx 127.0.0.1:5613 \
                  --control-port 5601
./build/oal_controller --telemetry-port 5612 --debug-port 5613 \
                       --control-tx 127.0.0.1:5601 --interactive
```

### 双终端直连自测（模拟数据 + 实时打印）

开两个终端，一个当机器人端、一个当遥控器端，用合成数据互通，并用
`--print-rate HZ` 实时打印各自解码出的接收数据（0 = 关闭）。机器人端内置
合成遥测，遥控器端默认合成摇杆运动，无需电台：

```bash
# 终端 1：机器人端（接收控制，打印 control 数据）
./build/oal_robot --telemetry-tx 127.0.0.1:5612 --debug-tx 127.0.0.1:5613 \
                  --control-port 5601 --print-rate 5 --no-measure-latency

# 终端 2：遥控器端（接收遥测/调试，打印 telemetry 数据）
./build/oal_controller --telemetry-port 5612 --debug-port 5613 \
                       --control-tx 127.0.0.1:5601 --print-rate 2 --no-measure-latency
```

终端 1 会持续打印 `rx control seq=... mode=... buttons=... estop=... axes=[...]`，
终端 2 会打印 `rx telemetry seq=...`（四元数、激光、状态量）与 `rx debug:` 文本。
停止终端 2 后，终端 1 应在 `--control-timeout-ms`（默认 500 ms）内进入 SAFE。

---

## 配合 WFB-NG 部署

### 1. 安装驱动 + WFB-NG（每台主机一次）

每台主机都需要联网（能访问 apt 源和 GitHub），并执行一次：

```bash
sudo scripts/install_wfbng.sh 8812eu   # 网卡为 RTL8812AU 时改用 8812au
```

脚本会自动完成：

1. 用 `apt-get` 安装编译依赖（`git`、`build-essential`、`libpcap`、`libsodium`、
   `libevent`、Python 等）；
2. 从 GitHub 克隆并 DKMS 安装补丁版驱动：RTL8812EU 来自
   `https://github.com/svpcom/rtl8812eu.git`（分支 `v5.15.0.1`），RTL8812AU 来自
   `https://github.com/svpcom/rtl8812au.git`（分支 `v5.2.20`）；
3. 从 `https://github.com/svpcom/wfb-ng.git` 克隆 WFB-NG 到 `/opt/wfb-ng`；
4. 构建 `all_bin wfb_rtsp` 并用 `setup.py` 安装 `wfb_tx` / `wfb_rx` /
   `wfb_keygen` / `wfb-server` / `wfb-cli`。

安装完成后确认驱动和命令：

```bash
ethtool -i <wlan>        # driver 应为 rtl8812eu 或 rtl88xxau_wfb
command -v wfb_keygen wfb_tx wfb_rx wfb-server wfb-cli
```

**手动安装 WFB-NG**（脚本失败、只需用户态工具，或想使用较新的 `master` 时）：

```bash
sudo apt-get update
sudo apt-get install -y git build-essential libpcap-dev libsodium-dev \
    libevent-dev python3 python3-pip python3-all python3-all-dev python3-setuptools

git clone https://github.com/svpcom/wfb-ng.git ~/wfb-ng
cd ~/wfb-ng
make -j"$(nproc)" all_bin wfb_rtsp
sudo env VERSION="$(make -s version | sed -n 's/^VERSION=//p')" \
    COMMIT="$(git rev-parse HEAD)" python3 setup.py install

command -v wfb_keygen wfb_tx wfb_rx wfb-server wfb-cli
```

> 不要直接执行裸 `make`：WFB-NG 的默认目标还会构建依赖 catch2 的测试程序，而
> 安装脚本没有安装 catch2，会因此失败。请使用 `make all_bin wfb_rtsp`。
>
> 如果 `wfb-server` 安装在 `~/.local/bin`（例如用 `pip --user` 安装），本仓库的
> `scripts/wfb_server.sh` 会自动补上用户级路径；直接用 `sudo wfb-server` 可能
> 因 root 的 `PATH` / `PYTHONPATH` 不同而报 `command not found`。

驱动也可以单独安装（例如已有 WFB-NG，只需重装驱动）：按上面的仓库地址克隆后，
在对应目录执行 `sudo ./dkms-install.sh`。

### 2. 生成密钥对（只生成一次）

> 密钥流程以本节为准；[通信方案](通信方案.md) 只讨论链路与协议，不包含密钥
> 生成步骤。

WFB-NG 的 `drone.key` 与 `gs.key` 是同一次 `wfb_keygen` 生成的一对密钥：
`drone.key` 是「无人机私钥 + 地面站公钥」，`gs.key` 是「地面站私钥 + 无人机
公钥」。两者必须配对使用，**只在一台主机上生成一次**：

```bash
scripts/gen_keys.sh config/keys
```

脚本会打印两个文件的 SHA-256 指纹，请记录下来用于后续核对。

然后把**同一对**密钥分发到三端：

| 目标 | 需要安装的文件 |
|------|----------------|
| 机器人端 | `drone.key` → `/etc/drone.key` |
| 遥控器端 | `gs.key` → `/etc/gs.key` |
| 监听端 | 两个都要 → `/etc/drone.key`、`/etc/gs.key` |

> **不要在三台主机上分别运行 `gen_keys.sh`。** 不带密码时每次都会生成全新的
> 随机密钥对；分别生成会导致空口加密无法互相解密——`wfb-cli` 上通常表现为
> 能收到包但 `dec_err` / 解密错误增长，而应用层完全收不到数据。同理，在任意
> 一台主机上重新生成密钥后，必须把新的 `drone.key` / `gs.key` 重新分发到所有
> 节点，否则旧节点解不开新包。
>
> 如果确实需要在多台主机上各自生成（例如批量部署），每次调用都传入**同一个
> 密码**：`scripts/gen_keys.sh config/keys 'shared-password'`。`wfb_keygen` 会用
> 固定 salt 从密码派生出相同的密钥对。
>
> 已经遇到该问题（`d_err` 增长、应用收不到数据）时，排查和修复步骤见
> [故障排查](#故障排查)。

分发后在各主机核对指纹，确认与生成主机上记录的一致：

```bash

```

另请注意：`scripts/setup_radio.sh` 会用仓库里的 `config/keys/` 覆盖 `/etc/*.key`。
因此请先把正确的一对密钥放进每台主机的 `<repo>/config/keys/` 再运行它，或者在
运行之后再手工把密钥复制到 `/etc`。

如果报错 `error: wfb_keygen not found, install wfb-ng first`，说明第 1 步尚未
完成，或只安装了无线驱动。请执行第 1 步；若暂时不需要驱动，可按其中的
“手动安装 WFB-NG”只构建并安装用户态工具。

安装后确认命令可用；只有从未生成过密钥时才执行生成命令（同样不要在各主机
分别生成）：

```bash
command -v wfb_keygen wfb_tx wfb_rx wfb-server wfb-cli
scripts/gen_keys.sh config/keys
```

> 不要直接执行 `make`：WFB-NG 的默认目标还会构建依赖 catch2 的测试程序，而
> 安装脚本并没有安装 catch2。本仓库的 `scripts/install_wfbng.sh` 已改为
> `make all_bin wfb_rtsp`。
>
> 重新生成密钥会覆盖 `config/keys` 中的旧密钥，并且**必须**把新密钥重新分发到
> 所有节点，否则各端密钥不再配对。如需保留旧密钥请先备份：
> `cp -a config/keys config/keys.bak-$(date +%Y%m%d-%H%M%S)`。

### 3. 配置各主机

```bash
# 在机器人端
sudo scripts/setup_radio.sh robot wlx140a022213ac --start

# 在遥控器端
sudo scripts/setup_radio.sh controller wlx140a022213a5 --start

# 在监听端（只收）
sudo scripts/setup_radio.sh monitor wlx140a022213ac --start
```

该脚本会安装 `/etc/wifibroadcast.cfg`、复制密钥、校验驱动并启动 WFB-NG
配置。三台主机必须使用相同的 `wifi_channel` 与 `link_domain`
（参见 `config/wifibroadcast-*.cfg`）。

#### 省电模式导致丢包率升高

**现象：** 打开系统 / 网卡的“节能模式”后，链路仍能连通，但 `oal_monitor`、
`oal_linktest`、`wfb-cli` 或 `scripts/link_report.sh` 报告的丢包率和延迟明显上升，
且通常伴随阵发性（突发）丢包。

**原因：** WFB-NG 在 Monitor Mode 下直接注入原始 802.11 帧。网卡或系统一旦进入
省电 / 节能模式，射频会在 DTIM 间隔休眠，帧被缓存到下一个唤醒周期或被直接丢弃。
另外 RTL8812AU/EU 是 USB 网卡，USB 自动挂起、CPU 降频或 TLP / power-profiles-daemon
等电源管理策略也会让网卡在空闲时休眠，造成注入突发丢失。

> 本项目代码不控制省电：应用层只是**观测**到丢包。丢包由序列号缺口统计得出，
> 见 `src/stats.cpp:39` 与 `src/stats.cpp:54-56`，因此应用层无法“修复”它，只能
> 在系统 / 驱动层关闭省电。

按下面顺序排查并尝试解决：

1. **确认并关闭 802.11 省电。** `get power_save` 应为 `off`：

   ```bash
   iw dev <wlan> get power_save
   sudo iw dev <wlan> set power_save off
   ```

2. **让 NetworkManager 不再重新打开省电。** 在
   `/etc/NetworkManager/NetworkManager.conf` 中加入，然后重启服务：

   ```ini
   [connection]
   wifi.powersave=2
   ```

   ```bash
   sudo systemctl restart NetworkManager
   ```

3. **禁用 USB 自动挂起（仅 USB 网卡，如 RTL8812AU/EU）。** 把 `<bus>-<port>`
   换成 `lsusb -t` 中对应网卡的端口：

   ```bash
   echo on | sudo tee /sys/bus/usb/devices/<bus>-<port>/power/control
   ```

   用 udev 规则持久化（Realtek 网卡 vendor id 为 `0bda`）：

   ```ini
   # /etc/udev/rules.d/50-wifi-no-autosuspend.rules
   ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{power/control}="on"
   ```

   也可以在内核启动参数中全局关闭：`usbcore.autosuspend=-1`。

4. **检查驱动是否编译了省电支持。** 本仓库附带的驱动默认已关闭
   （`rtl8812au/Makefile:59` 的 `CONFIG_POWER_SAVING = n`）；若换用发行版自带驱动，
   请确认其没有启用 `CONFIG_POWER_SAVING`。

5. **避免其它电源管理策略再次降频 / 休眠。** 测试时关闭 TLP、
   `power-profiles-daemon` 或桌面环境的“节能 / 省电”开关，并把 CPU governor 设为
   `performance`，确认结果稳定后再逐项恢复。

6. **验证效果。** 重复运行 `scripts/link_test_2host.sh` 或
   `scripts/link_report.sh`，确认丢包率与 p99 延迟恢复到阈值以内；也可用
   `wfb-cli <role>` 观察射频层统计。

> 当前 `scripts/setup_radio.sh` 不会自动关闭省电；部署时请手动执行上述命令，或
> 将它们加入启动脚本。监听端同样需要关闭省电，否则也会出现“看起来丢包”的现象。

### 4. 运行应用程序

```bash
scripts/run_robot.sh          # 在机器人端
scripts/run_controller.sh     # 在遥控器端（加 --interactive 使用按键）
scripts/run_monitor.sh        # 在监听端
```

或安装随附的 systemd 单元：

```bash
sudo install -Dm644 systemd/oal-robot.service /etc/systemd/system/oal-robot.service
# ... controller、monitor 同理
sudo systemctl enable --now oal-robot
```

WFB-NG 的链路统计可独立于本项目获取：

```bash
wfb-cli robot      # 或 controller / monitor
```

#### 随机数据链路测试（可选）

不启动完整应用时，也可以用 `oal_linktest` 通过 WFB-NG 测试流发送随机生成的
负载，验证空口收发。`--random` 会把每个探测包负载填充为随机字节（首字节保留
ping/pong 标记），延迟、丢包与 FEC 统计和普通测试完全一致：

```bash
# 机器人端（应答方）
scripts/link_test.sh robot --random --size 340

# 遥控器端（发起方，30 秒后自动退出）
scripts/link_test.sh controller --random --size 340 --duration 30

# 测试结束后生成汇总报告
scripts/link_test.sh report
```

两台主机各用一块网卡时，可让双机测试脚本在两端都启用随机负载：

```bash
scripts/link_test_2host.sh run \
    --robot pi@robot.local --controller pi@controller.local \
    --robot-arg --random --controller-arg --random
```

### 5. 两台主机各一块网卡链路测试（虚拟数据）

前置条件：两台主机都已按第 1–3 步安装 WFB-NG、复制密钥，并分别用各自的网卡
启动了对应 profile（机器人端 `wfb-server robot <wlan>`，遥控器端
`wfb-server controller <wlan>`）。两块网卡必须使用相同的 `wifi_channel` 与
`link_domain`。两台主机之间需配置免密 SSH，且仓库已在两端构建完成。

#### 连接另一台主机（免密 SSH）

双机测试脚本会用 SSH 在两端执行命令并取回报告，因此执行脚本的主机必须能免密
登录另一端。先确认连通性（以下示例在遥控器端操作，目标是机器人端）：

```bash
ssh pi@robot.local 'hostname; uname -m'
```

如果需要输入密码，生成密钥并复制到对端（每台主机只需一次）：

```bash
ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519   # 已有密钥可跳过
ssh-copy-id pi@robot.local
ssh pi@robot.local 'echo ok'
```

只要求“运行 `link_test_2host.sh` 的那一端”能登录另一端；若其中一端就是本机，
把对应的 `--robot` / `--controller` 写成 `local` 即可，无需为它配置 SSH。注意
SSH 只用于测试编排，实际控制 / 遥测数据走 WFB-NG 空口，不要求两台主机处于同一
IP 网络。主机名或 IP 换成实际地址；非默认端口用 `--ssh-port`，其它 SSH 选项通过
`OAL_SSH_OPTS` 传入：

```bash
OAL_SSH_OPTS="-o StrictHostKeyChecking=accept-new" \
scripts/link_test_2host.sh run \
    --robot pi@192.168.1.20 --controller pi@192.168.1.10 \
    --duration 30 --rate 200 --size 340
```

若两端仓库路径不同，加 `--robot-repo` / `--controller-repo`。脚本启动前会检查
对端是否存在 `<repo>/build/oal_linktest` 和 `<repo>/scripts/link_test.sh`，因此
两端都要先按[构建](#构建)完成编译。手工联调时也可以直接 `ssh <host>` 登录到
另一台终端，分别在两端运行 `scripts/run_robot.sh` / `scripts/run_controller.sh`。

`scripts/link_test_2host.sh` 会通过 WFB-NG 的测试流（0xFE/0x7E）发送合成
ping/pong 数据（默认 340 B），不依赖真实传感器或摇杆。在任意一台主机上执行
下面这一条命令，即可自动启动两端测试、取回两份报告并生成汇总：

```bash
scripts/link_test_2host.sh run \
    --robot pi@robot.local --controller pi@controller.local \
    --duration 30 --rate 200 --size 340
```

- `--robot` / `--controller`：两端的 SSH 目标；本机可写 `local`。
- `--robot-repo` / `--controller-repo`：两端仓库路径，默认与当前仓库相同。
- `--dir`：汇总报告目录，默认 `/tmp/oal-linktest-2host`。
- `--remote-dir`：两端报告临时目录，默认 `/tmp/oal-linktest`。
- `--robot-arg` / `--controller-arg`：透传给 `oal_linktest` 的额外参数（可重复）。

如果已手工在两台主机上分别运行了 `scripts/link_test.sh robot` 和
`scripts/link_test.sh controller`，测试结束后只需执行下面这一条命令，即可
通过 SSH 取回两份报告并生成汇总：

```bash
scripts/link_test_2host.sh report \
    --robot pi@robot.local --controller pi@controller.local
```

报告包含上下行 TX/RX、丢包/重复/乱序、往返延迟 p50/p95/p99/max、FEC 恢复率
（两端 `wfb-server` 的 `api_port` 可访问时）以及 PASS/FAIL 判定，并同时保存为
`<dir>/report.txt`。阈值沿用 `OAL_LINK_MAX_LOSS_PCT`（默认 5）和
`OAL_LINK_MAX_P99_MS`（默认 100）；设置 `OAL_LINK_MIN_FEC_RECOVERY_PCT` 可将
最低 FEC 恢复率纳入判定。SSH 参数可通过 `OAL_SSH_OPTS` 覆盖。

> 两台主机必须实际启动 `wfb-server`。否则测试数据不会进入 WFB-NG，报告通常
> 会显示 `FAIL no-traffic`，FEC 数据也不可用。

### 6. 单机双电台自测（机器人 + 遥控器同机）

只需一台主机、两块支持 Monitor Mode + 注入的无线网卡（例如两块 RTL8812EU），
即可在一台机器上同时充当机器人端与遥控器端，跑通完整的 WFB-NG 空口链路。
两块网卡使用相同信道（`wifi_channel = 165`）与 `link_domain`，一块（`wlx140a022213ac`）
作机器人，另一块（`wlx140a022213a5`）作遥控器。

两份 profile 的 `stats_port` / `api_port`（8002/8102 与 8003/8103）本就不同，
因此两个 `wfb-server` 实例可以并存。遥测/控制仍走 `127.0.0.1`；链路测试的
接收端绑定 `127.0.0.2`（`wfb_rx` 也转发到该地址），避免与同机 `wfb_tx`
在 5631/5632 上的监听端口冲突，同时仍能通过本机回环把数据送进 WFB-NG。

```bash
# 1. 装驱动 + 生成密钥
sudo scripts/install_wfbng.sh 8812eu
scripts/gen_keys.sh config/keys
sudo install -m600 config/keys/drone.key config/keys/gs.key /etc/

# 2. 合并 profile：以 robot 配置为基础，追加 controller 专有部分
sudo cp config/wifibroadcast-robot.cfg /etc/wifibroadcast.cfg
sudo tee -a /etc/wifibroadcast.cfg >/dev/null <<'EOF'

[gs_base]
keypair = 'gs.key'

[controller]
streams = [
    {'name': 'telemetry', 'stream_tx': None, 'stream_rx': 0x01, 'service_type': 'udp_direct_rx',
     'profiles': ['base', 'radio_base', 'gs_base'],
     'peer': 'connect://127.0.0.1:5612'},
    {'name': 'debug', 'stream_tx': None, 'stream_rx': 0x02, 'service_type': 'udp_direct_rx',
     'profiles': ['base', 'radio_base', 'gs_base'],
     'peer': 'connect://127.0.0.1:5613'},
    {'name': 'control', 'stream_tx': 0x80, 'stream_rx': None, 'service_type': 'udp_direct_tx',
     'profiles': ['base', 'radio_base', 'gs_base', 'control_fec'],
     'peer': 'listen://127.0.0.1:5611'},
    {'name': 'test_up', 'stream_tx': 0xFE, 'stream_rx': None, 'service_type': 'udp_direct_tx',
     'profiles': ['base', 'radio_base', 'gs_base', 'test_fec'],
     'peer': 'listen://127.0.0.1:5631'},
    {'name': 'test_down', 'stream_tx': None, 'stream_rx': 0x7E, 'service_type': 'udp_direct_rx',
     'profiles': ['base', 'radio_base', 'gs_base'],
     'peer': 'connect://127.0.0.2:5632'}
    ]
stats_port = 8003
api_port = 8103
link_domain = 'oal-link'
EOF

# 3. 调好两块网卡
for w in wlx140a022213ac wlx140a022213a5; do
    sudo ip link set "$w" up
    sudo ip link set "$w" mtu 2312 txqueuelen 1000
done
sudo iw reg set BO

# 4. 起两个 WFB-NG 实例（同信道、同 link_domain）
sudo scripts/wfb_server.sh robot      wlx140a022213ac &
sudo scripts/wfb_server.sh controller wlx140a022213a5 &
ss -ltnup | grep -E '563[12]|810[23]'   # 563x=UDP wfb_tx；810x=TCP 统计 API
wfb-cli robot          # 另一个终端查看射频统计（controller 同理）

# 5. 跑应用（均使用 config/link.conf，走 127.0.0.1）
./build/oal_robot      --config config/link.conf &
./build/oal_controller --config config/link.conf --interactive

# 6. 双向往返链路测试（可选；两个终端，先起应答方）
# 终端 A（responder）
scripts/link_test.sh robot
# 终端 B（initiator）
scripts/link_test.sh controller --size 340

# 7. 先在终端 B Ctrl-C，再在终端 A Ctrl-C；然后任一端执行
scripts/link_test.sh report
```

如果 wfb-ng 是用 `pip --user` 安装的（`wfb-server` 位于 `~/.local/bin`），
直接 `sudo wfb-server` 会因 root 的 `PATH`/`PYTHONPATH` 不同而报
`command not found`；`scripts/wfb_server.sh` 会自动补上用户级安装路径。

链路测试必须让两个 `wfb-server` 先成功启动：`linktest` 发到
`127.0.0.1:5631`（上行测试流）和 `127.0.0.1:5632`（下行测试流）的数据会进入
`wfb_tx`，经空口后由对端 `wfb_rx` 转发到 `127.0.0.2:5631/5632` 交给
`linktest`。若 `wfb-server` 没起来，两个 `linktest` 会直接在回环上互通，
看起来也能跑通，但完全没有经过 WFB-NG，报告里也不会有 FEC 数据。测试过程中
可另开终端运行 `wfb-cli robot` / `wfb-cli controller`，确认 `recv` / `fec_r`
计数随测试增长。

测试结束时，两端会分别把结果写入 `/tmp/oal-linktest/robot.report` 和
`/tmp/oal-linktest/controller.report`（可用 `OAL_LINKTEST_REPORT_DIR` 改目录）。
`report` 子命令读取这两个文件，打印上下行收发/丢包/往返延迟分位数与 PASS/FAIL
判定；也可直接运行 `scripts/link_report.sh --dir DIR`。非交互场景可加
`--duration N` 让两端自动退出（robot 多跑 5 s，确保覆盖 controller 的结束）：

```bash
scripts/link_test.sh robot --duration 35 &
sleep 1
scripts/link_test.sh controller --duration 30 --size 340
wait
scripts/link_test.sh report
```

如果本机 `wfb-server` 的 JSON 统计接口可访问（robot profile `api_port = 8102`，
controller profile `api_port = 8103`），测试开始和结束时会各采样一次 WFB-NG 的
接收计数器。可先手动探测：

```bash
scripts/wfb_stats.py --port 8102 --service test_up
scripts/wfb_stats.py --port 8103 --service test_down
```

能输出 `fec_rec=...`、`lost=...` 即正常。报告因此会额外给出上下行的
**FEC 恢复率**：

- `Recovered`：窗口内经 FEC 恢复的包数（`fec_rec` 增量）
- `Lost`：FEC 之后仍然丢失的包数（`lost` 增量）
- `Recovery %`：`fec_rec / (fec_rec + lost)`
- `K/N`：该流实际协商到的 FEC 编码参数

链路完全干净时没有丢块，`Recovered` 和 `Lost` 都会是 0，恢复率显示为 `n/a`；
要验证 FEC，可适当降低发射功率、增加距离/遮挡或使用更弱的 FEC 配置来制造空口丢块。

可用 `OAL_WFB_API_HOST`、`OAL_WFB_API_PORT`、`OAL_WFB_TEST_SERVICE` 覆盖默认
采样地址/服务名；`OAL_WFB_API_PORT=0` 关闭采样。若要把恢复率纳入 PASS/FAIL，
设置 `OAL_LINK_MIN_FEC_RECOVERY_PCT`（默认 0，表示只报告不判定）。

判定标准：机器人端打印 `control <- 0.0.0.0:5601` 后不再报 SAFE，遥控器端持续输出
遥测统计；在 `--interactive` 下按 `a d e m c q` 验证按键与急停。停止遥控器后，
机器人端应在 `control_timeout_ms`（默认 500 ms）内进入 SAFE 状态。

---

## 命令行参考

所有选项也可写入配置文件（`config/link.conf`），并由命令行覆盖。

### `oal_robot`

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--rate` | 200 | 遥测速率（Hz） |
| `--telemetry-tx` | 127.0.0.1:5602 | 遥测的 WFB-TX 端点 |
| `--debug-tx` | 127.0.0.1:5603 | 调试的 WFB-TX 端点 |
| `--control-port` | 5601 | 控制的本机 WFB-RX 端口 |
| `--control-timeout-ms` | 500 | 控制丢失后进入 SAFE 状态的超时 |
| `--debug-rate` | 5 | 调试流速率（Hz） |
| `--duration` | 0 | 运行秒数（0 = 一直运行） |
| `--print-rate` | 0 | 实时打印收到的控制帧（Hz，0 = 关闭） |
| `--no-measure-latency` | | 关闭单向延迟测量（时钟未同步时） |

### `oal_controller`

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--rate` | 200 | 控制速率（Hz） |
| `--telemetry-port` | 5612 | 遥测的本机 WFB-RX 端口 |
| `--debug-port` | 5613 | 调试的本机 WFB-RX 端口 |
| `--control-tx` | 127.0.0.1:5611 | 控制的 WFB-TX 端点 |
| `--interactive` | false | 从 stdin 读取 `a d e m c q` 命令 |
| `--no-auto-motion` | | 停止合成摇杆运动 |
| `--print-rate` | 0 | 实时打印收到的遥测/调试帧（Hz，0 = 关闭） |
| `--telemetry-timeout-ms` | 500 | 遥测缺失时告警的超时 |

### `oal_monitor`

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--telemetry-port` | 5622 | 遥测的本机 WFB-RX 端口 |
| `--debug-port` | 5623 | 调试的本机 WFB-RX 端口 |
| `--control-port` | 5621 | 控制的本机 WFB-RX 端口 |
| `--log-file` | `monitor-<时间>.jsonl` | 逐帧 JSONL 日志 |--control-port
| `--no-frame-log` | | 关闭逐帧记录 |

### `oal_linktest`

```bash
# 两个终端：先起应答方，再起发起方
scripts/link_test.sh robot                    # responder（应答方）
scripts/link_test.sh controller --size 340    # initiator（发起方）

# 先停 controller，再停 robot；然后一条命令打印汇总报告
scripts/link_test.sh report

# 或非交互运行（robot 多跑 5 s）
scripts/link_test.sh robot --duration 35 &
sleep 1
scripts/link_test.sh controller --duration 30 --size 340
wait
scripts/link_test.sh report
```

单机双电台时，`linktest` 发送到 `127.0.0.1:5631/5632`（交给 `wfb_tx`），
接收端默认绑定 `127.0.0.2:5631/5632`，避免与同机 `wfb_tx` 的 UDP 监听端口冲突。
无需时钟同步即可报告双向的往返延迟分位数（p50/p95/p99/max）、抖动与丢包。
每次运行会写出 `--report PATH` 指定的报告文件（`link_test.sh` 默认写入
`/tmp/oal-linktest/<role>.report`）；`report` 子命令汇总两份文件，阈值可用
`OAL_LINK_MAX_LOSS_PCT`（默认 5）和 `OAL_LINK_MAX_P99_MS`（默认 100）调整。

测试期间 `link_test.sh` 还会从本机 `wfb-server` 的 `api_port` 采样 rx 计数器，
在报告中给出每个方向的 FEC 恢复包数、FEC 后丢失包数、FEC 恢复率及编码参数
`K/N`。默认
端口为 robot `8102`、controller `8103`；可用 `OAL_WFB_API_PORT`（设为 `0`
可关闭）、`OAL_WFB_API_HOST`、`OAL_WFB_TEST_SERVICE` 覆盖。设置
`OAL_LINK_MIN_FEC_RECOVERY_PCT` 可把最低恢复率纳入 PASS/FAIL 判定。

### `link_test_2host.sh`

```bash
# 两台主机各一块网卡：启动测试并自动生成汇总报告
scripts/link_test_2host.sh run --robot pi@robot.local --controller pi@controller.local

# 已手工跑完测试：只取回两端报告并生成汇总
scripts/link_test_2host.sh report --robot pi@robot.local --controller pi@controller.local

# 两端都通过 SSH 访问，仓库不在默认路径，并调整测试参数
scripts/link_test_2host.sh run \
    --robot root@robot --controller root@controller \
    --robot-repo /opt/omni-arm-link --controller-repo /opt/omni-arm-link \
    --duration 60 --rate 200 --size 340 --dir ./linktest-report
```

`run` 会先启动机器人端应答方（多运行 5 s），再启动遥控器端发起方；结束后通过
SSH 取回 `<remote-dir>/<role>.report` 及 FEC 采样文件，调用
`scripts/link_report.sh` 输出汇总并保存到 `<dir>/report.txt`。`report` 模式
只执行取回与汇总，不再启动测试。两端均需已启动 `wfb-server`，否则数据不会
经过 WFB-NG，报告会缺少 FEC 数据，并可能判定为 `FAIL no-traffic`。

### `oal_sim`

```bash
oal_sim robot --dest HOST:PORT --rate 200 --loss 0.05 --jitter-ms 5
oal_sim controller --dest HOST:PORT --rate 200 --estop
```

---

## 监控与日志

`oal_monitor` 对每个接收到的帧写入一个 JSON 对象：

```json
{"recv_ns":1729000000000000000,"direction":"robot->controller","stream":"telemetry",
 "seq":4211,"flags":0,"sender_ns":1729000000000000000,"size":368,"latency_ms":1.42}
```

它还会按流统计：包速率、吞吐量、序列丢包、重复、乱序、CRC 错误以及单向延迟
分位数。`crc_errors` 统计疑似损坏的帧，`bidirectional` 用于确认上下行均被观测
到。射频层面的实时丢包 / FEC 统计也可用 `wfb-cli` 查看，或由 `oal_linktest`
报告的 FEC 恢复率给出测试窗口内的汇总值。

**时钟同步：** 跨节点的单向延迟只有在两台主机时钟同步（例如通过 chrony/PTP）
时才有意义。若未同步，请使用 `--no-measure-latency`，并依赖 `oal_linktest`
的往返测量。

---

## 故障排查

遇到“两端进程都在跑，但互相收不到数据”时，先按下面顺序自查：

1. `iw dev <wlan> info` 显示 `type monitor`，且信道与 `wifi_channel` 一致；
2. `wfb-cli <role>` 的 `tx` / `rx` 在增长，且 `d_err`（解密错误）基本不增长；
3. 两端的 `/etc/drone.key`、`/etc/gs.key` 指纹一致（见下文）；
4. `ss -ltnup | grep -E '56[0-9][0-9]'` 能看到本端 `wfb_tx` / `wfb_rx`；
5. 应用配置的端口与 WFB-NG profile 中的 `peer` 端口一致。

### 症状：能启动但完全收不到数据，`d_err` 持续增长（密钥不匹配）

**现象：** 两个 `wfb-server` 都能启动，`wfb-cli <role>` 里 `rx` / `recv` 有增长，
但 `d_err`（decryption error）一直涨，应用层却什么都收不到；`wfb_rx` 日志里可能
出现 `Unable to decrypt session key` 或 `Unable to decrypt packet`。

**原因：** WFB-NG 的 `drone.key` 与 `gs.key` 必须来自**同一次** `wfb_keygen`。
每个文件都包含“自己的私钥 + 对端公钥”：机器人/监听端用 `drone.key`，遥控器/
监听端用 `gs.key`。如果三端各自运行过 `gen_keys.sh`，或者 `setup_radio.sh` 用
仓库里另一对 `config/keys` 覆盖了 `/etc/*.key`，两端密钥就不配对——空口帧能收到，
但解不开，所以射频统计里 `rx` 在涨、`d_err` 也在涨，应用层没有任何数据。

**排查：** 在所有节点核对指纹。`drone.key` 在机器人端和监听端必须一致，`gs.key`
在遥控器端和监听端必须一致；**不要拿 `drone.key` 和 `gs.key` 互相比**，它们本来
就不同：

```bash
sha256sum /etc/drone.key /etc/gs.key
```

**修复：**

1. 只在**一台**主机上重新生成一次（会覆盖旧密钥）：

   ```bash
   scripts/gen_keys.sh config/keys
   sha256sum config/keys/drone.key config/keys/gs.key
   ```

2. 把**同一对**密钥分发到三端（机器人端只需 `drone.key`，遥控器端只需
   `gs.key`，监听端两个都要）。例如：

   ```bash
   scp config/keys/drone.key pi@robot.local:/tmp/drone.key
   ssh pi@robot.local 'sudo install -m600 /tmp/drone.key /etc/drone.key'

   scp config/keys/gs.key pi@controller.local:/tmp/gs.key
   ssh pi@controller.local 'sudo install -m600 /tmp/gs.key /etc/gs.key'
   ```

   也可以在每台主机运行 `setup_radio.sh` 前，先把正确的一对密钥放进该主机的
   `<repo>/config/keys/`，脚本会自动安装。

3. **重启所有节点的 `wfb-server`**，旧进程仍持有旧密钥：

   ```bash
   sudo systemctl restart 'wifibroadcast@*'
   # 或重新执行 sudo scripts/setup_radio.sh <role> <wlan> --start
   ```

4. 再次运行 `wfb-cli <role>`，确认 `rx` 增长而 `d_err` 不再增长，然后观察
   `oal_monitor` / `oal_linktest` 是否恢复。

> 还需确认 profile 没有改错：机器人端的 `drone_base` 使用 `drone.key`，遥控器端
> 的 `gs_base` 使用 `gs.key`（见 `config/wifibroadcast-*.cfg`）。修改密钥后不要
> 只重启一端，否则仍未配对。

### 症状：`iw dev <wlan> info` 显示 `type managed`（WFB-NG 没有接管网卡）

**现象：** `iw dev <wlan> info` 显示 `type managed`，网卡可能还带着 IP 或连着
某个 SSID；WFB-NG 无法注入帧，两端自然不通。

**原因：** Monitor Mode 是 `wfb-server` 启动时自动设置的：它先检查
`iw dev <wlan> info`，如果不是 `type monitor`，就执行
`ip link set <wlan> down` → `iw dev <wlan> set monitor otherbss` →
`ip link set <wlan> up`。而 `scripts/setup_radio.sh` 本身只做 `ip link set up`、
MTU 和 `txqueuelen`，**不负责设置 monitor 模式**。因此以下情况网卡会停留在
managed：

- `setup_radio.sh` 没有带 `--start`，或 `wfb-server` 启动后立即失败；
- 启动时传错网卡名或 profile；
- `wfb-server` 因权限、驱动、密钥或信道不支持等原因启动失败；
- NetworkManager / wpa_supplicant 重新接管网卡，或主机重启后没有重新启动 WFB；
- 这块网卡同时被当作上网口/uplink 使用。

**排查：**

```bash
iw dev <wlan> info                 # 应为 type monitor，channel 与配置一致
ip -br addr show <wlan>            # 不应有 IP 地址
sudo systemctl status 'wifibroadcast@*' --no-pager
journalctl -u 'wifibroadcast@*' -n 50 --no-pager
wfb-cli <role>                     # tx/rx 是否增长，d_err 是否增长
```

如果 `wfb-server` 在运行但仍是 managed，查看它的日志中是否有 `iw` / `ip` 报错。

**修复：** 先停掉占用网卡的进程，再用正确参数启动，让 WFB-NG 自己设置 monitor：

```bash
sudo scripts/setup_radio.sh robot <wlan> --start
# 或直接：sudo scripts/wfb_server.sh robot <wlan>
```

确认结果：

```bash
iw dev <wlan> info | grep -E 'type|channel'
# 期望：type monitor，channel 165 (5180 MHz)
```

仅用于临时验证时，也可以手动设置（长期仍应由 `wfb-server` 管理）：

```bash
sudo ip link set <wlan> down
sudo iw dev <wlan> set monitor otherbss
sudo ip link set <wlan> up
sudo iw dev <wlan> set channel 165 HT20
```

若 NetworkManager 反复接管，除了 WFB-NG 配置里的 `set_nm_unmanaged = True`
（它会执行 `nmcli device set <wlan> managed no`），还可以在
`/etc/NetworkManager/NetworkManager.conf` 中固定排除该网卡：

```ini
[keyfile]
unmanaged-devices=interface-name:<wlan>
```

然后 `sudo systemctl restart NetworkManager`。建议 WFB 使用**专用网卡**，不要和
上网/uplink 共用；主机重启后要重新启动 `wfb-server`（或启用 `wifibroadcast@`
服务）。

> 密钥不匹配和 managed 模式可能同时出现。建议先让网卡进入 monitor 模式、确认
> `wfb-server` 正常运行，再核对密钥与 `d_err`，最后检查两端的 `wifi_channel`
> 和 `link_domain` 是否一致。

---

## 测试计划对应关系

| 通信方案测试 | 运行方式 |
|---|---|
| 1. 基础双向通信 | 双机：`scripts/link_test_2host.sh run --robot ... --controller ...`；单机：`scripts/link_test.sh robot` / `... controller`，结束后 `scripts/link_test.sh report`，再运行 `oal_robot` + `oal_controller` |
| 2. 延迟测试 | 汇总报告中的 p95/p99/max + `oal_monitor --measure-latency`（时钟已同步） |
| 3. 丢包 / 干扰测试 | `oal_sim ... --loss --duplicate --reorder --jitter-ms`，然后引入真实干扰；观察 PER、序列丢包、`oal_linktest` 的 FEC 恢复率与 `wfb-cli` 的实时统计 |
| 4. 第三节点监听 | 运行 `oal_monitor`；验证 `bidirectional=yes`、对主链路无影响，并获得完整 JSONL 日志 |
| 5. 比赛环境模拟 | 在密集 Wi-Fi、机器人运动、人体/金属遮挡及电机运行条件下重复测试 1–4 |

---

## 仓库结构

```
include/oal/        公共头文件（协议、编解码、传输、统计、配置）
src/                库实现 + robot / controller / monitor / tools
config/             WFB-NG 配置、应用配置、密钥目录
scripts/            安装 / 配置 / 运行辅助脚本
systemd/            三个应用的 service 单元
```
