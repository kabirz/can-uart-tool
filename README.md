# ModHandler PC Tool

基于 Win32 API 的激光测距系统 PC 端配套工具，集成 LoRa 网关通信、CAN 总线调试和固件升级功能。

## 功能

- **LoRa 数据** — 网关 TCP 数据流收发，遥测解析，测试帧记录，CSV 导出
- **LoRa 配置** — UDP 设备搜索，AT 指令配置，网络参数管理
- **固件升级** — 通过 CAN 或 UART 通道进行固件烧录，支持进度显示
- **CAN 命令** — 发送/接收 CAN 帧，支持标准帧/扩展帧，LoRa 远程配参

## 依赖

- Windows 10/11
- Visual Studio 2026（MSVC）或 MinGW-w64
- [PCAN-Basic](https://www.peak-system.com/PCAN-Basic.199.0.html) — CAN 总线驱动库
- CMake >= 3.25

## 项目结构

```
can-uart-tool/
├── src/
│   ├── main.c              # 主窗口，Tab 控件，消息分发
│   ├── can_hal.c           # CAN 硬件抽象层 (PCAN / IXXAT)
│   ├── can_hal_pcan.c      # PCAN-Basic 适配器实现
│   ├── can_hal_ixxat.c     # IXXAT VCI 适配器实现
│   ├── can_dispatcher.c    # CAN 帧分发器
│   ├── can_manager.c       # CAN 总线通信管理
│   ├── uart_manager.c      # UART 串口通信管理
│   ├── can_command.c       # CAN 帧收发逻辑
│   ├── tab_can_upgrade.c   # Tab: 固件升级界面
│   ├── tab_can_command.c   # Tab: CAN 命令界面
│   ├── tab_lora_data.c     # Tab: LoRa 数据界面
│   └── tab_lora_cfg.c      # Tab: LoRa 配置界面
├── loralib/                 # LoRa Gateway SDK (静态库)
│   ├── lora_sdk.h          # 公共 API 头文件
│   ├── lora_sdk.c          # 生命周期管理
│   ├── lora_sdk_internal.h # 内部数据结构
│   ├── lora_tcp.c          # TCP 数据流收发
│   ├── lora_udp.c          # UDP 设备搜索与 AT 指令
│   └── lora_frame.c        # 统一帧格式编解码
├── include/
│   └── resource.h           # 资源 ID 定义，布局常量
├── resources/
│   ├── resource.rc          # 资源脚本（菜单、图标、版本信息）
│   ├── app.manifest         # DPI 感知 + 通用控件 v6
│   └── icon.ico             # 应用图标
└── libs/x64/
    └── PCANBasic.lib        # PCAN-Basic 静态库
```

## 编译

### Visual Studio（推荐）

```bash
cmake -B build -A x64
cmake --build build --config Release
```

生成的可执行文件：`build/Release/modhandler-pc-tool.exe`

### MinGW-w64

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

生成的可执行文件：`build/modhandler-pc-tool.exe`

### 清理重建

```bash
rm -rf build
cmake -B build -A x64
cmake --build build --config Release
```

## 使用

1. 连接 PCAN USB 适配器或 LoRa 网关
2. 运行 `modhandler-pc-tool.exe`
3. 在 LoRa 数据页输入网关 IP/端口并连接，或在固件升级页选择通道、波特率并连接

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| Ctrl+O | 打开固件文件 |

## 许可

Apache-2.0
