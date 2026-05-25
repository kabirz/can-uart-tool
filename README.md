# ModHandler PC Tool

基于 Win32 API 的激光测距系统 PC 端配套工具，集成 LoRa 网关通信、CAN 总线调试和固件升级功能。

## 功能

- **LoRa 数据** — TCP 网关连接，遥测数据实时显示 (X/Y 角度/按键)，测试帧回显与延迟记录，RSSI 查询，历史列表，CSV 导出
- **LoRa 配置** — UDP/串口双传输方式，设备搜索，网关重启，网络参数管理 (IP/掩码/网关/DHCP/SOCKA)，LoRa 协议配置 (组网/透传模式/通道/频率/速率/功率)，AT 命令控制台
- **固件升级** — CAN (PCAN/IXXAT) 或 UART 双通道固件烧录，适配器热切换，进度显示，版本查询/板卡重启
- **CAN 命令** — 自定义 CAN 帧发送 (标准帧/扩展帧/数据帧/远程帧)，总线监视器 (帧 ID 自动标注)，LoRa 远程配参 (协议/模式/通道/NID/GWID/上电/测试模式)

## 依赖

- Windows 10/11
- Visual Studio 2026 (MSVC) 或 MinGW-w64
- CMake >= 3.25
- [PCAN-Basic](https://www.peak-system.com/PCAN-Basic.199.0.html) — PCAN USB 适配器驱动 (运行时动态加载，未安装不影响 IXXAT 使用)
- [IXXAT VCI](https://www.ixxat.com/) — IXXAT USB-to-CAN 适配器驱动 (可选，运行时动态加载)

## 项目结构

```
can-uart-tool/
├── src/
│   ├── main.c              # 主窗口，Tab 控件，消息分发，全局对象管理
│   ├── tab_base.c           # Tab 基础框架 (统一 WndProc + vtable 生命周期 + SetWindowTheme)
│   ├── can_hal.c           # CAN 硬件抽象层 (适配器注册/帧收发/设备检测)
│   ├── can_hal_pcan.c      # PCAN-Basic 适配器实现 (动态加载 PCANBasic.dll)
│   ├── can_hal_ixxat.c     # IXXAT VCI 适配器实现 (动态加载 vcinpl.dll)
│   ├── can_dispatcher.c    # CAN 帧分发器 (接收线程 + 发布/订阅)
│   ├── can_manager.c       # CAN 固件升级协议 (连接/版本/重启/OTA)
│   ├── uart_manager.c      # UART 串口固件升级 (SetupAPI 枚举 COM 口)
│   ├── can_command.c       # CAN 帧收发 + 总线监视 + LoRa 配参
│   ├── tab_lora_data.c     # Tab: LoRa 数据 (TCP 连接/遥测/测试/历史/CSV)
│   ├── tab_lora_cfg.c      # Tab: LoRa 配置 (UDP/串口/设备/网络/协议/AT)
│   ├── tab_can_upgrade.c   # Tab: 固件升级 (CAN/UART 双通道/进度/版本)
│   └── tab_can_command.c   # Tab: CAN 命令 (帧发送/总线监视/LoRa 配参)
├── loralib/                 # LoRa Gateway SDK (静态库/DLL)
│   ├── lora_sdk.h          # 公共 API 头文件
│   ├── lora_sdk.c          # SDK 生命周期管理
│   ├── lora_sdk_internal.h # 内部数据结构
│   ├── lora_sdk_tcp.c      # TCP 数据流收发 (WSAEventSelect 事件驱动)
│   ├── lora_sdk_udp.c      # UDP 设备搜索与 AT 指令
│   ├── lora_sdk_serial.c   # 串口 AT 指令传输 (COM 口直连)
│   ├── lora_sdk_at.c       # AT 命令公共模块 (CRLF/查询检测/worker 封装/网关重启)
│   ├── cJSON.h/cJSON.c     # JSON 解析 (UDP 响应)
│   ├── crc16.h/crc16.c     # CRC16-CCITT 帧校验
│   └── lora_sdk_example.c  # SDK 独立示例程序
├── include/
│   ├── resource.h           # 资源 ID、布局常量、字体定义、自定义消息
│   ├── tab_base.h           # Tab 基础框架接口 (TAB_BASE, TAB_IFACE, TAB_MSG_*)
│   ├── can_hal.h            # CAN HAL 抽象接口 (CanHalOps 虚表)
│   ├── can_manager.h        # CAN 管理器接口
│   ├── can_dispatcher.h     # CAN 帧分发器接口
│   ├── can_command.h        # CAN 命令模块接口
│   └── uart_manager.h       # UART 管理器接口
├── resources/
│   ├── resource.rc          # 资源脚本 (菜单/图标/快捷键/版本信息)
│   └── icon.ico             # 应用图标
└── libs/x64/
    └── PCANBasic.lib        # PCAN-Basic 导入库 (链接用，运行时动态加载 DLL)
```

## 编译

### Visual Studio (推荐)

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

1. 连接 PCAN USB 适配器、IXXAT USB-to-CAN 适配器或 LoRa 网关
2. 运行 `modhandler-pc-tool.exe`
3. **LoRa 数据页**: 输入网关 IP/端口并连接，实时查看遥测数据
4. **LoRa 配置页**: 搜索网关设备或打开串口，配置网络和 LoRa 参数，重启网关
5. **固件升级页**: 选择通道/适配器/波特率并连接，选择固件文件开始升级
6. **CAN 命令页**: 连接 CAN 后发送自定义帧、监视总线、配置 LoRa 参数

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| Ctrl+O | 打开固件文件 |

## CAN 通信协议

帧 ID 与嵌入式端 `mod-can.h` 一致：

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台→手柄 | 控制命令 (升级/确认/版本/重启) |
| 0x102 | 手柄→平台 | 响应帧 |
| 0x103 | 平台→手柄 | 固件数据传输 |
| 0x763 | 手柄→平台 | 心跳 |
| 0x1E3 | 手柄→平台 | 手柄状态 (X/Y BE + 按键) |
| 0x263 | 平台→手柄 | 超欠挖 + 激光测距 |
| 0x363 | 平台→手柄 | X/Y 坐标 |
| 0x463 | 平台→手柄 | Z 坐标 |
| 0x105 | 平台→手柄 | LoRa 参数配置命令 |
| 0x106 | 手柄→平台 | LoRa 参数配置响应 |

## 许可

Apache-2.0
