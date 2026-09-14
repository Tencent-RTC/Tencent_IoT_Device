# Tencent IoT SDK —— Linux 源码交付包

本包含 demo 源码 + 预编译 SDK（自包含静态库 + 公开头文件），客户可直接编译运行，
**demo 只依赖 SDK 的 public API**（`sdk/include` 下的 `tc_iot_*.h`）。

## 目录结构

```
.
├── CMakeLists.txt          # 独立构建脚本（只用 sdk/include）
├── demo/
│   ├── iot_video_demo/     # 音视频通话 demo（严格只依赖 public API；内含 demo_media/ 测试素材）
│   └── iot_aitalk_demo/    # AI 语音 demo（TRTC/PCM 路径）
└── sdk/
    ├── include/            # 全量 public API 头
    ├── lib/libtc_iot_sdk.a # 自包含静态库（c 模式已含 TRTC-c）
    └── module_config.cmake # 本包 .a 实际启用的模块（构建据此对齐）
```

## 编译

需要 **CMake 3.21 或更高**（含 `cmake -B` 与交叉包 `toolchain.cmake` 的链接阶段注入）。

```bash
cmake -B build
cmake --build build -j
```

产物：`build/iot_video_demo`（启用 AV 时）、`build/iot_aitalk_demo`（启用 AITalk 时）。

> 本包启用的模块见 `sdk/module_config.cmake`。若某模块未启用，对应 demo 不会构建。

## 运行

两个 demo 都用命令行传入设备三元组（`product_id` / `device_id` / `device_secret`，从腾讯云 IoT 控制台获取），region 默认 `ap-guangzhou`。
**必须在包根目录运行**：demo 用相对路径读取 `demo/iot_video_demo/demo_media/` 下的音视频素材，换目录会找不到文件。

### iot_video_demo（音视频通话）

```bash
./build/iot_video_demo -p <product_id> -d <device_id> -s <device_secret>
```

启动并连云后，通过**命名管道 `/tmp/tciot`** 下发命令（程序启动时自动创建该 FIFO）。支持以下命令：

发起呼叫：

```bash
echo "call -u <呼叫目标 callee，格式 modelId/wxAppId/openId> [-c <1=纯音频，其它=音视频>]" > /tmp/tciot
```

接听来电：

```bash
echo "accept -u <对端 user_id>" > /tmp/tciot
```

拒接来电：

```bash
echo "reject -u <对端 user_id>" > /tmp/tciot
```

挂断当前通话：

```bash
echo "hangup -u <对端 user_id>" > /tmp/tciot
```

拉取联系人列表：

```bash
echo "get_contacts -n <数量>" > /tmp/tciot
```

结束 demo（收尾退出）：

```bash
echo "stop" > /tmp/tciot
```

- `accept` / `reject` / `hangup` 的 `-u` 即来电时回调 `on_call_requested` 打印的对端 `user_id`（微信端来电同为 `modelId/wxAppId/openId` 形式，直接复制日志里的值即可）。
- 本端推流用 demo_media 里的素材（音频 16k/mono/16bit PCM、视频 640x360 H.264）。
- 通话中收到的远端音/视频会落盘到运行目录：`recv_audio.pcm`、`recv_video.h264`。

### iot_aitalk_demo（AI 语音对话）

```bash
./build/iot_aitalk_demo -p <product_id> -d <device_id> -s <device_secret> [-b <bot_id>]
```

启动后自动运行文件回放的语音对话流程（无交互命令行），`Ctrl+C` 退出。`-b` 指定智能体 ID，可选。

## TRTC 模式

- **c 模式**：`libtc_iot_sdk.a` 已自包含 TRTC-c，只需系统库 `z m pthread dl`。
- **cc 模式**：额外随包 `sdk/lib/libliteav.so`、`libtxffmpeg.so`，运行时需
  `LD_LIBRARY_PATH=sdk/lib`。

## 说明

- `iot_video_demo` 严格只 include `sdk/include` 下的 public API。
- `iot_aitalk_demo` 走 TRTC/PCM 路径，不编译、不链接 Opus。

## SDK 体积（firmware，linux / trtc=cc / p8_full）

| .text | .rodata | .data | .bss | Flash |
|---:|---:|---:|---:|---:|
| 1,126,383 | 219,337 | 30,148 | 15,873 | **1,375,868** |
