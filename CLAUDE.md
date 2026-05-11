# CLAUDE.md — ModHandler PC Tool 项目指南

## 项目概述

Win32 桌面应用，C 语言，基于 Tab 控件的激光测距系统 PC 端配套工具。集成 LoRa 网关通信、CAN 总线调试和固件升级功能。

## 编译命令

```bash
# Visual Studio 2026 (MSVC) — 推荐
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release

# 清理重建
rm -rf build && cmake -B build -G "Visual Studio 18 2026" -A x64 && cmake --build build --config Release
```

输出：`build/Release/modhandler-pc-tool.exe`

## 架构

- **主窗口** (`main.c`) — 创建 Tab 控件和状态栏，管理各模块生命周期，处理自定义消息（WM_CAN_CONNECTED 等）
- **Tab 页面** (`tab_*.c`) — 每个标签页是独立子窗口，通过 `TabXxx_Create/Destroy` 管理生命周期
- **管理器** (`can_manager.c`, `uart_manager.c`) — 底层通信封装，提供连接/断开/收发接口
- **终端通用** (`terminal_common.c`) — ANSI 转义序列解析（SGR 颜色、CSI C/D/J），RichEdit 文本渲染，键盘 passthrough
- **终端后端** (`uart_terminal.c`, `net_terminal.c`) — 连接管理和数据回调

## 关键设计决策

- 终端使用 RichEdit (MSFTEDIT_CLASS)，Dracula 主题（背景 #282A36，前景 #F8F8F2）
- 终端为纯 raw passthrough 模式：用户按键直接发送原始字节，设备回显负责显示
- ANSI CSI 'D' (Cursor Back) 用于设备退格，CSI 'C' (Cursor Forward) 用于 Tab 补全
- UART 终端使用 16ms 定时器批量刷新 + WM_SETREDRAW 优化高波特率输出性能
- CAN 连接状态通过 WM_CAN_CONNECTED/DISCONNECTED 自定义消息在 Tab 间同步

## 资源 ID 分配

- 1001: Tab 控件
- 1100-1199: Tab0 固件升级
- 1200-1299: Tab1 CAN 命令
- 1300-1399: Tab2 UART 终端
- 1400-1499: Tab3 网络终端
- WM_APP+1 ~ WM_APP+7: 自定义窗口消息

## 编码规范

- Unicode 构建（UNICODE / _UNICODE）
- 中文界面，MSVC 编译选项 `/utf-8`
- 字体：Microsoft YaHei（UI 控件），Consolas（终端）
- 窗口尺寸：1200x1080，定义在 resource.h 的 WINDOW_WIDTH/WINDOW_HEIGHT
- 控件布局常量在 resource.h：MARGIN=16, LINE_H=38, CTRL_H=34, BTN_W=110, COMBO_W=160
