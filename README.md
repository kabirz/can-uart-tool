# CAN/UART 工具

基于 Win32 API 的 CAN/UART 调试工具，支持固件升级、CAN 命令收发、UART 终端和网络终端。

## 功能

- **CAN/UART 固件升级** — 通过 CAN 或 UART 通道进行固件烧录，支持进度显示
- **CAN 命令** — 发送/接收 CAN 帧，支持标准帧/扩展帧，32 组快捷命令预设，LoRa 配置
- **UART 终端** — 串口 Shell 终端，ANSI 颜色渲染，Dracula 主题，Tab 补全支持
- **网络终端** — TCP/UDP 客户端终端，同样支持 ANSI 颜色渲染

## 依赖

- Windows 10/11
- Visual Studio 2026（MSVC）或 MinGW-w64
- [PCAN-Basic](https://www.peak-system.com/PCAN-Basic.199.0.html) — CAN 总线驱动库
- CMake >= 3.25

## 项目结构

```
can-uart/
├── src/
│   ├── main.c              # 主窗口，Tab 控件，消息分发
│   ├── can_manager.c       # CAN 总线通信管理（PCAN-Basic 封装）
│   ├── uart_manager.c      # UART 串口通信管理
│   ├── can_command.c       # CAN 帧收发逻辑
│   ├── uart_terminal.c     # UART 终端后端
│   ├── net_terminal.c      # TCP/UDP 网络终端后端
│   ├── terminal_common.c   # 终端通用：ANSI 解析、颜色、键盘处理
│   ├── tab_can_upgrade.c   # Tab0: 固件升级界面
│   ├── tab_can_command.c   # Tab1: CAN 命令界面
│   ├── tab_uart_terminal.c # Tab2: UART 终端界面
│   └── tab_net_terminal.c  # Tab3: 网络终端界面
├── include/
│   ├── resource.h           # 资源 ID 定义，布局常量
│   ├── can_manager.h
│   ├── uart_manager.h
│   ├── can_command.h
│   ├── uart_terminal.h
│   ├── net_terminal.h
│   └── terminal_common.h
├── resources/
│   ├── resource.rc          # 资源脚本（菜单、加速键、图标、清单）
│   ├── app.manifest         # DPI 感知 + 通用控件 v6
│   └── icon.ico             # 应用图标（终端风格，7 尺寸）
└── libs/x64/
    └── PCANBasic.lib        # PCAN-Basic 静态库
```

## 编译

### Visual Studio（推荐）

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

生成的可执行文件：`build/Release/can-uart-tool.exe`

### MinGW-w64

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

生成的可执行文件：`build/can-uart-tool.exe`

### 清理重建

```bash
rm -rf build
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

## 使用

1. 连接 PCAN USB 适配器或串口设备
2. 运行 `can-uart-tool.exe`
3. 在 **CAN/UART 升级** 页面选择通道、波特率并连接
4. 通过 **文件 → 打开固件** 或浏览按钮选择 `.bin` 固件文件
5. 点击 **烧录** 开始升级

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| Ctrl+O | 打开固件文件 |

## 许可

私有项目
