# IoT Video Demo 编译和运行指南

本 demo 演示 `tc_iot_sdk` 在两种平台上的音视频通话和云存储能力：

| 平台    | 运行形态                      | 命令通道           | 音视频源                          |
| ------- | ----------------------------- | ------------------ | --------------------------------- |
| BK7258  | 嵌入式固件（FreeRTOS） | 串口 cli | 真实硬件采集（摄像头/麦克风）     |
| Linux   | 用户态进程                    | 命名管道 `/tmp/tciot` | 文件回放（`demo_media/*.pcm`/`*.h264`） |

> 所有相对路径均以仓库根 `iot/` 为基准。

---

## 0. 公共前置准备

| 项 | 说明 |
|---|---|
| 设备三元组 | 在[腾讯云 IoT 开发平台](https://console.cloud.tencent.com/iotexplorer)创建产品+设备，获取 `product_id` / `device_id` / `device_secret` |
| 腾讯连连 App | 平台 → 应用 → 腾讯连连配置 → 调试二维码，扫码绑定设备 |
| 呼叫小程序（可选） | 微信打开 P2P Player 小程序，进入需要的房间，记下 `openId`；`modelId` / `wxaAppId` 来自平台应用配置 |
| 云存套餐（可选） | 通过 [云 API CreateCloudStorage](https://console.cloud.tencent.com/api/explorer?Product=iotvideo&Version=2021-11-25&Action=CreateCloudStorage) 为设备开通云存套餐（`FULL_TIME` 全时 / `EVENT` 事件）。默认为非多通道设备开通，通道 ID 为 0；多通道设备可按指定通道分别购买套餐。**未开通则 `cloud_storage_demo` 不会驱动任何通道**。 |

---

## 1. BK7258

### 1.1 前置

| 项 | 说明 |
|---|---|
| 交叉编译工具链 | `/opt/gcc-arm-none-eabi-10.3-2021.10/bin`（`build.sh` 默认值，可用 `TOOLCHAIN_DIR` 覆盖） |
| BK7258 平台 SDK | `iot/platform/bk7258/sdk/`，子模块；首次运行 `git submodule update --init platform/bk7258/sdk` |

### 1.2 改三个地方

**① 设备三元组**——`iot/src/demo/iot_video_demo/iot_demo_config.h`：

```c
#ifndef IOT_PRODUCT_ID
#  define IOT_PRODUCT_ID "你的 product_id"
#endif
#ifndef IOT_DEVICE_ID
#  define IOT_DEVICE_ID "你的 device_id"
#endif
#ifndef IOT_DEVICE_SECRET
#  define IOT_DEVICE_SECRET "你的 device_secret"
#endif
```

**② WiFi**——`iot/src/demo/iot_video_demo/iot_demo_config.h`：

```c
#define IOT_WIFI_SSID     "你的 WiFi"
#define IOT_WIFI_PASSWORD "你的 WiFi 密码"
```

**③ 默认呼叫目标**（可选）——`iot/src/demo/iot_video_demo/iot_demo_config.h`：

```c
#define IOT_DEFAULT_CALLEE "modelId/wxAppId/openId"
```

### 1.3 编译

```bash
cd iot

# CC TRTC（默认，libliteav.a 预编译，功能全）
bash build.sh --platform=bk7258 --trtc=cc

# 或 C TRTC（纯 C 源码，体积小，仅基础功能）
bash build.sh --platform=bk7258 --trtc=c
```

产物：

```
out/cc-bk7258/tc_iot_demo/all-app.bin   # 烧录用
out/cc-bk7258/tc_iot_demo/app.elf       # 配 addr2line 解栈用
out/cc-bk7258/tc_iot_demo.zip           # 完整交付包
out/cc-bk7258/tc_iot_sdk.zip            # SDK 交付包（含头 + .a）
```

### 1.4 烧录

`all-app.bin` 用 BK 官方烧录工具（`BKFIL.exe` / Windows / 串口）：

- 波特率 921600（或 1500000）
- 起始地址 `0x0`
- 选择 `all-app.bin`，断电按 BOOT 烧录

### 1.5 串口操作（115200 8N1）

上电等 WiFi 自动连接，然后输入命令：

| 命令 | 说明 |
|---|---|
| `iot_start -p <product_id> -d <device_id> -s <device_secret>` | 启动 demo（音+视+播放都开） |
| `iot_start novideo` | 仅音频 |
| `iot_start noaudio noplayout` | 仅视频上行，不播放下行音 |
| `iot_start testvideo` | 用内置测试视频（不依赖摄像头） |
| `iot_call` | 用 `IOT_DEFAULT_CALLEE` 发起呼叫（模式 2：设备摄像头） |
| `iot_call <callee>` | 指定呼叫目标 |
| `iot_call <callee> 0` | 模式 0：仅音频 |
| `iot_call <callee> 1` | 模式 1：微信摄像头（仅看微信端画面） |
| `iot_call <callee> 3` | 模式 3：双向视频 |
| `iot_accept <user_id>` | 接受呼入 |
| `iot_reject <user_id>` | 拒接 |
| `iot_hangup [<callee>]` | 挂断 |
| `iot_stop` | 停止 demo |
| `cs_event` | 触发云存事件（固定 channel 0） |
| `iot_play_test` | 不依赖 TRTC，本地播 PCM，验证音频通路 |

### 1.6 三种典型场景

**App 拉流（被动）**

1. `iot_start`
2. 等串口出现 `tc_iot_av_init success` 与 `subscribed: ...`
3. App 端进入设备页 → 自动触发 `start_pull_stream` 信令
4. 串口应见 `_on_monitor_begin: channel_id=N` → 设备开始上推音视频
5. App 显示画面/听到声音
6. App 关闭 → 串口见 `_on_monitor_end`

**设备主呼小程序**

1. `iot_start`
2. `iot_call`（用默认 `IOT_DEFAULT_CALLEE`）
3. 小程序端弹出来电 → 接通后双向通话
4. 任意一方挂断 → `_on_call_hangup` 触发

**不依赖 TRTC 的本地音频自检**

`iot_play_test` → 应听到内置 PCM 播放，验证扬声器/I2S 通路。

### 1.7 实时监控（自动开启）

`iot_start` 会同时启动 `_resource_monitor_thread`，每 10 秒打印：

- SRAM/PSRAM 总量、空闲、使用、历史最低
- 任务列表（`rtos_dump_task_list`）
- CPU 运行时统计（`rtos_dump_task_runtime_stats`）

观察 `[RES]` 行，可定位内存泄漏或线程异常。

### 1.8 崩溃排查

1. 当前 BK7258 配置已设 `CONFIG_DUMP_ENABLE=y` + `CONFIG_ASSERT_REBOOT=n`，assert 会打完整寄存器+栈
2. 抓串口里的地址（如 `pc=0xXXXXXXXX lr=0xYYYYYYYY`）
3. 用工具链解栈：

   ```bash
   /opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-addr2line \
       -e iot/out/cc-bk7258/tc_iot_demo/app.elf \
       -fpC 0xXXXXXXXX 0xYYYYYYYY ...
   ```

---

## 2. Linux

### 2.1 前置

x86_64 Linux，有 `libstdc++`、`pthread`（CC 版本依赖 `libliteav.so` 是 C++ 库）。

### 2.2 编译

```bash
cd iot

# CC TRTC（默认）
bash build.sh --platform=linux --trtc=cc

# C TRTC（纯 C，体积小）
bash build.sh --platform=linux --trtc=c

# Debug 模式（带 ASan）
bash build.sh --platform=linux --trtc=cc --debug
```

产物：

```
out/cc-linux/tc_iot_demo/iot_video_demo     # 可执行文件 (~3.9 MB)
out/cc-linux/tc_iot_demo.zip                # 演示包
out/cc-linux/tc_iot_sdk/lib/libtc_iot_sdk.a # 静态库
out/cc-linux/tc_iot_sdk/lib/libliteav.so    # CC TRTC 动态库
out/cc-linux/tc_iot_sdk/lib/libtxffmpeg.so  # CC TRTC 依赖
out/cc-linux/tc_iot_sdk/include/*.h         # 公开头
out/cc-linux/tc_iot_sdk.zip                 # SDK 包
```

> Debug 包名为 `out/cc-linux_debug/`。

### 2.3 运行

日志同时输出到控制台和 `logs/run-<timestamp>.log`，便于实时观察 + 事后排查。

**CC 版本**（必须设 `LD_LIBRARY_PATH`）：

```bash
cd iot
mkdir -p logs && LOG=logs/run-$(date +%Y%m%d-%H%M%S).log

LD_LIBRARY_PATH=src/third_party/iot_trtc/lib/x64 \
stdbuf -oL -eL ./out/cc-linux/tc_iot_demo/iot_video_demo \
  -p YOUR_PRODUCT_ID \
  -d YOUR_DEVICE_NAME \
  -s 'YOUR_DEVICE_SECRET' 2>&1 | tee "$LOG"
```

**C 版本**（无外部依赖）：

```bash
cd iot
mkdir -p logs && LOG=logs/run-$(date +%Y%m%d-%H%M%S).log

stdbuf -oL -eL ./out/c-linux/tc_iot_demo/iot_video_demo \
  -p YOUR_PRODUCT_ID \
  -d YOUR_DEVICE_NAME \
  -s 'YOUR_DEVICE_SECRET' 2>&1 | tee "$LOG"
```

> 把 `YOUR_PRODUCT_ID / YOUR_DEVICE_NAME / YOUR_DEVICE_SECRET` 替换成你在腾讯云 IoT
> 控制台创建的设备三元组（`product_id / device_id / device_secret`）。

> ⚠️ **必须在 `iot/` 目录运行**：demo 用相对路径 `./src/demo/iot_video_demo/demo_media/...` 读音视频源文件。

**关键参数说明**：

- `2>&1` —— 把 stderr 合并到 stdout，确保 TRTC 和 demo 的日志都进文件
- `| tee "$LOG"` —— 同时写控制台和文件
- `stdbuf -oL -eL` —— stdout/stderr 行缓冲（默认 pipe 是块缓冲，会让 `tee` 看似卡住）
- 不再需要 `set -o pipefail`，因为我们要的就是看实时输出

如果不想保留历史日志，可以用固定文件名（每次启动覆盖）：

```bash
LD_LIBRARY_PATH=src/third_party/iot_trtc/lib/x64 \
stdbuf -oL -eL ./out/cc-linux/tc_iot_demo/iot_video_demo \
  -p YOUR_PRODUCT_ID -d YOUR_DEVICE_NAME -s 'YOUR_DEVICE_SECRET' 2>&1 | tee run.log
```

成功标志：

```
tc_iot_av_init success
pipe created: /tmp/tciot
... subscribed: topic=$trtc/down/service/...
```

### 2.4 命令交互（命名管道 `/tmp/tciot`）

Linux demo 没有交互式 shell，通过 **`echo` 写命名管道** 发命令（从**另一个终端**操作）。命令按业务分组如下：

**通话类**

| 命令 | 说明 |
|---|---|
| `call -u <user_id> [-n <name>] [-w <wxa_appid>] [-m <model_id>]` | 主动呼叫 |
| `accept -u <user_id>` | 接受呼入 |
| `reject -u <user_id>` | 拒接 |
| `hangup -u <user_id>` | 挂断 |

**联系人**

| 命令 | 说明 |
|---|---|
| `get_contacts -n <num>` | 拉好友列表 |

**云存储**

| 命令 | 说明 |
|---|---|
| `cs_event <channel_id>` | 触发一次随机事件（80% 录像 / 20% 抓图） |

> 云存储随 `iot_demo_start` 自动初始化、`stop` 退出时自动反初始化，无需手动 init/deinit。

**通用**

| 命令 | 说明 |
|---|---|
| `help` | 列出所有命令 |
| `stop` | 停止并退出 |

举例（另一终端）：

```bash
# 呼叫小程序（callee 格式 modelId/wxAppId/openId，替换成你自己的）
echo "call -u YOUR_MODEL_ID/YOUR_WX_APPID/YOUR_OPEN_ID" > /tmp/tciot

# 挂断
echo "hangup -u YOUR_MODEL_ID/YOUR_WX_APPID/YOUR_OPEN_ID" > /tmp/tciot

# 优雅退出
echo "stop" > /tmp/tciot
```

> 上面 callee 取自 `src/demo/iot_video_demo/iot_demo_config.h` 的 `IOT_DEFAULT_CALLEE`（`<modelId>/<wxAppId>/<openId>` 三段）。
> 生产部署请替换成你自己产品的对应值。

### 2.5 三种典型场景

**App 拉流（被动，最常用）**

1. 启动 demo，等到 `tc_iot_av_init success`
2. 腾讯连连 App 进入设备页 → 自动触发 `start_pull_stream`
3. 终端日志见：

   ```
   _on_monitor_begin, channel_id:N
   AV sender initialized for channel_id: N
   ```

4. App 端能看到 `demo_media/video_size640x360_gop50_fps25.h264` 的循环画面，
   听到 `demo_media/audio_sample16000_mono_16bit_le.pcm` 的循环音频
5. App 关闭 → `_on_monitor_end`

**设备主呼小程序**

```bash
# 终端 A：启动 demo（日志同时落盘）
mkdir -p logs && LOG=logs/run-$(date +%Y%m%d-%H%M%S).log
LD_LIBRARY_PATH=src/third_party/iot_trtc/lib/x64 \
stdbuf -oL -eL ./out/cc-linux/tc_iot_demo/iot_video_demo \
  -p YOUR_PRODUCT_ID -d YOUR_DEVICE_NAME -s 'YOUR_DEVICE_SECRET' 2>&1 | tee "$LOG"

# 终端 B：发起呼叫
echo "call -u YOUR_MODEL_ID/YOUR_WX_APPID/YOUR_OPEN_ID" > /tmp/tciot
```

小程序端弹来电 → 接通后双向通话。

**App 呼叫设备 → 设备接听**

1. App 端发起呼叫
2. 终端见 `_on_call_requested`
3. 在终端 B：`echo "accept -u <user_id>" > /tmp/tciot`

### 2.6 接收侧自动落盘

Linux demo 把收到的远端音视频自动写入当前目录：

| 文件 | 内容 |
|---|---|
| `recv_audio.pcm` | 远端音频（原始 PCM，默认 16kHz/mono/16bit-LE） |
| `recv_video.h264` | 远端视频（H.264 裸流） |

回放检验：

```bash
# 听音频
ffplay -f s16le -ar 16000 -ac 1 recv_audio.pcm
aplay -r 16000 -c 1 -f S16_LE recv_audio.pcm

# 看视频
ffplay recv_video.h264
```

### 2.7 调试技巧

带 ASan 的 Debug 构建（日志同时落盘）：

```bash
bash build.sh --platform=linux --trtc=cc --debug

mkdir -p logs && LOG=logs/asan-$(date +%Y%m%d-%H%M%S).log
LD_LIBRARY_PATH=src/third_party/iot_trtc/lib/x64 \
stdbuf -oL -eL ./out/cc-linux_debug/tc_iot_demo/iot_video_demo \
  -p YOUR_PRODUCT_ID -d YOUR_DEVICE_NAME -s 'YOUR_DEVICE_SECRET' 2>&1 | tee "$LOG"
```

gdb 调试（gdb 是交互式，不要 `tee`，否则输入会被吞）：

```bash
LD_LIBRARY_PATH=src/third_party/iot_trtc/lib/x64 gdb --args \
  ./out/cc-linux_debug/tc_iot_demo/iot_video_demo \
    -p YOUR_PRODUCT_ID -d YOUR_DEVICE_NAME -s 'YOUR_DEVICE_SECRET'
```

回看历史日志：

```bash
ls -lt logs/                    # 按时间倒序
tail -f logs/run-*.log          # 跟随最新一条
grep -n "ERROR\|LOGE\|hangup\|on_call" logs/run-*.log    # 关键事件检索
```

替换音视频源：把 `demo_media/` 下的 `.pcm` / `.h264` 替换成自己的素材即可。要求：

- 音频：16 kHz、单声道、16-bit LE PCM
- 视频：H.264 裸流（Baseline，GOP 50，25 fps），可能要同步更新 `_h264_index.txt` 帧索引

---

## 3. 云存储（Cloud Storage）

云存储 demo 模块（`cloud_storage_demo.c` / `cloud_storage_demo.h`）跨平台共用，通过外部命令/按键触发事件。

### 3.1 架构设计

```
┌────────────────────────────────────────────────────────┐
│         cloud_storage_demo.c (共用)                     │
│  cloud_storage_demo_init / deinit / trigger_event      │
├────────────────────────────────────────────────────────┤
│  图片获取: av_device_get_event_picture │
│  推流共享: iot_demo_acquire/release_sender │
└────────────────────────────────────────────────────────┘
       ▲                      ▲
       │                      │
  ┌────┴─────┐         ┌─────┴─────┐
  │  Linux   │         │  BK7258   │
  │ main.c:  │         │ ap_main.c:│
  │ auto init│         │ auto init │
  │ cs_event │         │ cs_event  │
  │          │         │ (CLI/按键)│
  └──────────┘         └───────────┘
```

### 3.2 生命周期

| 平台 | 初始化 | 反初始化 | 事件触发 |
|------|--------|----------|----------|
| Linux | `iot_demo_start` 时自动 | `iot_demo_stop` 时自动 | 命令 `cs_event <channel_id>` |
| BK7258 | `iot_start` 时自动 | `iot_stop` 时自动 | ① 串口 `cs_event`（固定触发 channel 0）<br>② S3 按键短按（固定触发 channel 0） |

### 3.3 初始化后自动行为

1. 查询通道 0~N 的云存套餐
2. 套餐为 `FULL_TIME`：acquire sender（含音视频） + 开启全时录像
3. 套餐为 `EVENT`：acquire sender（仅 video，audio 由全时通道的全局 stream 提供）
4. 套餐为 `NONE` / `UNKNOWN`：跳过，不 acquire sender

### 3.4 事件触发行为

调用 `cloud_storage_demo_trigger_event(channel_id)` 或对应命令/按键时：

| 项 | 说明 |
|---|---|
| 选择策略 | 80% 录像事件 / 20% 抓图事件（随机） |
| 录像事件 ID | 从 `1~6` 中随机选取 |
| 抓图事件 ID | 从 `100`、`101` 中随机选取 |
| 录像时长 | [5, 15] 秒随机 |
| 图片 | 调用 `av_device_get_event_picture` 获取 JPEG 缩略图 |
| extra_info | JSON 格式附加信息 `{"source":"cloud_storage_demo","trigger":"manual"}` |
| 并发限制 | 同一通道同时仅允许 1 个录像事件 |

> ⚠️ 不含预录事件（`pre_record_seconds=0`），简化 demo 逻辑。

### 3.5 事件 ID 参考

详见 `cloud_storage_event_id.md`：

| 类型 | ID 范围 | 示例 |
|------|---------|------|
| 录像事件（需配对 start/stop） | 1~99 | 1=门铃, 2=运动检测, 3=人形, 4=区域入侵, 5=徘徊, 6=异常声音 |
| 抓图事件（单次上报） | 100~199 | 100=人脸识别, 101=开门抓拍 |

### 3.6 按键配置（BK7258，通用模块）

按键使用独立的通用模块，基于 BK7258 SDK `key` 组件
（6ms 定时器轮询 + multi_button 状态机，内置 18ms 防抖）。

#### 架构设计

```
app_key_config.h   ← 纯数据：GPIO号 / 有效电平 / 事件绑定（改这里即可）
app_key.h          ← 接口：事件枚举 + 配置结构体 + init 函数
app_key.c          ← 实现：遍历配置表注册按键 + 统一事件分发
```

#### 新增/修改按键

编辑 `platform/bk7258/solution/ap/app_key_config.h`：

```c
#define APP_KEY_CONFIG_TABLE                              \
{                                                         \
    {                                                     \
        .gpio_id      = GPIO_8,                           \
        .active_level = LOW_LEVEL_TRIGGER,                \
        .short_event  = APP_KEY_EVENT_CS_TRIGGER,         \
    },                                                    \
    /* 示例：新增一个按键控制 IoT 启停 */                    \
    /* {                                                  */ \
    /*     .gpio_id      = GPIO_13,                      */ \
    /*     .active_level = LOW_LEVEL_TRIGGER,             */ \
    /*     .short_event  = APP_KEY_EVENT_IOT_START,       */ \
    /*     .long_event   = APP_KEY_EVENT_IOT_STOP,        */ \
    /* },                                                 */ \
}
```

#### 新增业务功能

1. 在 `app_key.h` 的 `app_key_event_t` 枚举中（`APP_KEY_EVENT_MAX` 之前）添加新事件
2. 在 `app_key.c` 的 `_default_event_handler()` 中添加对应 `case` 处理

当前枚举仅保留已实现和预留的事件，如需支持音量控制、WiFi 重置等功能，按上述步骤扩展即可。

不使用按键时，清空 `APP_KEY_CONFIG_TABLE` 为 `{}` 即可。串口命令 `cs_event` 始终可用（固定触发 channel 0）。

### 3.7 示例日志

```
[cloud_storage_demo] init success (channels=3)
[cloud_storage_demo] on_init_result: code=0 msg=
[cloud_storage_demo] === Cloud Storage Plan ===
  channel_0: FULL_TIME
  channel_1: EVENT
  channel_2: NONE
[cloud_storage_demo] ===========================
[demo] acquire_sender channel_id=0 refcount=1 (new, quality=1)
[cloud_storage_demo] channel_0 start_continuous_recording rc=0
[demo] acquire_sender channel_id=1 refcount=1 (new, quality=1)
[cloud_storage_demo] channel_0 recording_start event_id=3 pic=8746 rc=0
[cloud_storage_demo] channel_0 event=3 duration=12s
[cloud_storage_demo] channel_0 recording_stop event_id=3 rc=0
[cloud_storage_demo] event_result channel_id=0 event_id=3 report=0 pic=0
[cloud_storage_demo] channel_1 snapshot event_id=100 pic=5432 rc=0
```

### 3.8 图片来源

| 平台 | 实现 |
|------|------|
| Linux | `av_device_linux.c`：从 `demo_media/event_pic/` 读取 JPEG 文件（`01.jpg`~`06.jpg`、`100.jpg`、`101.jpg`） |
| BK7258 AMP AI | `av_device_beken_amp_ai.c`：media_app DVP + H264 |
| BK7258 AMP AV | `av_device_beken_amp_av.c`：UVC MJPEG→H264 pipeline |
| BK7258 SMP AI | `av_device_beken_smp_ai.c`：DVP + bk_camera / voice_service |
| BK7258 SMP AV | `av_device_beken_smp_av.c`：UVC via `dev_media` |

### 3.9 注意事项

- 云存功能需要设备在腾讯云 IoT 开发平台已开通云存套餐
- sender 与通话场景共用（`iot_demo_acquire_sender` / `release_sender` refcount 机制）
- 事件图片 `picture_data` 生命周期由 `cloud_storage_demo` 内部管理（预分配 150KB per channel）
- `cloud_storage_demo_deinit` 会等待进行中的录像事件停止后再清理

---

## 4. 平台差异速查

| 项 | Linux | BK7258 |
|---|---|---|
| 三元组配置 | 命令行 `-p / -d / -s` | 编译期宏（`iot_demo_config.h`） |
| WiFi 配置 | 不需要 | `ap_main.c` 硬编码 / `tc_wifi_save()` 持久化 |
| 音视频源 | 文件回放（`demo_media/`） | 真实硬件采集 |
| 启动方式 | 进程启动即跑 | 串口 `iot_start` 触发 |
| 命令通道 | 命名管道 `/tmp/tciot` | 串口 cli（`iot_call` / `iot_hangup` / …） |
| 云存初始化 | `iot_demo_start` 内自动 | `iot_start` 内自动 |
| 云存事件触发 | 命令 `cs_event <channel_id>` | ① 串口 `cs_event`（固定 channel 0）② S3 按键短按（固定 channel 0） |
| 接收侧 | 写文件（`recv_audio.pcm` / `recv_video.h264`） | 实时播放（扬声器） |
| 退出 | `echo "stop" > /tmp/tciot` 或 `Ctrl+C` | `iot_stop` |
| 监控线程 | 无（自行用 `top` / ASan） | `_resource_monitor_thread` 每 10s 打印 |
| 崩溃排查 | gdb / ASan | `app.elf` + `arm-none-eabi-addr2line` |

---

## 5. 常见问题

| 现象 | 排查 |
|---|---|
| Linux：`error while loading shared libraries: libliteav.so` | 没设 `LD_LIBRARY_PATH=src/third_party/iot_trtc/lib/x64` |
| Linux：启动后 `Failed to open file: demo_media/...` | 工作目录不对，必须在 `iot/` 下运行 |
| Linux：`mkfifo` 报 `Permission denied` | `/tmp/tciot` 残留旧文件，`rm -f /tmp/tciot` 后重试 |
| Linux：`recv_audio.pcm` 是空的 | 对端没推上行音频（部分场景仅设备推流） |
| 通用：`tc_iot_login timeout` | 三元组错 / 网络不通 / 系统时间不对 |
| 通用：App 已登录但拉不到流 | 看 `_on_monitor_begin` 是否触发；看 `LOGE` 进房失败码 |
| 通用：编译失败 `opus.h not found` | 确认 `HAL_Audio_linux.c` include 是 `"opus.h"` 不是 `"opus/opus.h"` |
| 通用：`cs_event` 返回 -1 "not initialized" | 确认 `iot_demo_start`（Linux）/ `iot_start`（BK7258）成功，云存随其自动初始化 |
| 通用：`cs_event` 返回 -1 "no plan" | 设备未开通云存套餐，在 IoT 平台配置 |
| 通用：`cs_event` 返回 -1 "busy" | 该通道有录像事件正在进行中，等待结束后重试 |
| BK7258：`iot_start` 后立刻崩 / 长时间不响应 | 看 `[RES]` PSRAM 趋势；用 `app.elf` + `addr2line` 解栈 |
| BK7258：改了三元组没生效 | `build.sh` 内已 `find ... -exec touch`，仍可手动 `rm -rf build/cc_bk7258` 后重编 |
