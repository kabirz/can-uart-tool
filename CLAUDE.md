# CLAUDE.md — ModHandler PC Tool 项目指南

## 项目概述

Win32 桌面应用 (v0.4.2)，C 语言，基于 Tab 控件的激光测距系统 PC 端配套工具。集成 LoRa 网关通信 (TCP/UDP/串口)、CAN 总线调试 (PCAN/IXXAT) 和固件升级功能。

## 编译命令

```bash
# Visual Studio 2026 (MSVC) — 推荐
cmake -B build -A x64
cmake --build build --config Release

# 清理重建
rm -rf build && cmake -B build -A x64 && cmake --build build --config Release
```

输出：`build/Release/modhandler-pc-tool.exe`

## 架构

- **主窗口** (`main.c`) — 创建 Tab 控件和状态栏，管理全局对象生命周期 (CanHal/CanDispatcher/CanManager/UartManager/CanCommand/LoRaSdk)，处理自定义消息 (WM_CAN_CONNECTED / WM_ADAPTER_CHANGED 等)
- **Tab 基础框架** (`tab_base.c` + `tab_base.h`) — 统一的 Win32 子窗口生命周期管理。`TAB_BASE` 作为各 Tab 数据结构的首成员，`TAB_IFACE` vtable 定义 on_create/on_size/on_destroy/on_message 钩子。框架统一处理 WM_NCCREATE(calloc)、WM_CREATE(字体创建+SetWindowTheme)、WM_SIZE(最小尺寸保护)、WM_DESTROY(DeleteObject+free)、WM_CTLCOLORSTATIC(GroupBox 蓝色标题)。各 Tab 通过 `TabBase_CreatePage()` 一行创建，无需各自注册窗口类或编写 WndProc。
- **Tab 0: LoRa 数据** (`tab_lora_data.c`) — TCP 网关连接，遥测数据解析 (X/Y/按键)，测试帧回显，RSSI 查询，历史 ListView，CSV 导出
- **Tab 1: LoRa 配置** (`tab_lora_cfg.c`) — UDP/串口双传输，设备搜索，网络设置 (IP/掩码/网关/DHCP/SOCKA)，LoRa 协议参数 (NWMODE/TTMODE/WMODE/CH/SPD/PWR/UPWID)，AT 命令控制台
- **Tab 2: 固件升级** (`tab_can_upgrade.c`) — CAN/UART 双通道，适配器选择 (PCAN/IXXAT)，设备枚举，进度条，版本查询/板卡重启
- **Tab 3: CAN 命令** (`tab_can_command.c`) — 自定义帧发送 (标准/扩展，数据/远程)，总线监视器 (帧 ID 自动标注)，LoRa 远程配参 (协议/模式/通道/NID/GWID/上电/测试模式)
- **CAN HAL 抽象层** (`can_hal.c` + `can_hal_pcan.c` + `can_hal_ixxat.c`) — PCAN-Basic 和 IXXAT VCI 适配器的统一抽象接口，运行时动态加载 DLL，支持适配器热切换
- **CAN 分发器** (`can_dispatcher.c`) — CAN 帧接收线程 + 发布/订阅模式，最多 4 个订阅者
- **CAN 管理器** (`can_manager.c`) — 固件升级协议封装 (连接/版本/重启/OTA)，使用 CanHal + CanDispatcher
- **CAN 命令模块** (`can_command.c`) — 通用帧收发 + 总线监视 + LoRa 配参命令封装
- **UART 管理器** (`uart_manager.c`) — 串口固件升级，SetupAPI 枚举 COM 口
- **LoRa Gateway SDK** (`loralib/`) — 独立 SDK 库，TCP 数据流 + UDP 设备发现 + 串口 AT 配置，无 GUI 依赖

## 关键设计决策

- **适配器热切换**: 固件升级页选择适配器时通过 WM_ADAPTER_CHANGED 通知主窗口，替换全局 CanHal/CanDispatcher，同步更新所有消费者
- **CAN 连接共享**: 固件升级页连接 CAN 后通过 WM_CAN_CONNECTED 通知 CAN 命令页同步通道号，断开时同步 WM_CAN_DISCONNECTED
- **LoRa SDK 线程编组**: SDK 回调在后台线程触发，通过 PostMessage + 自定义 WM_LORA_* 消息编组到 UI 线程，堆分配载荷由接收端 free
- **LoRa 双传输**: 配置页支持 UDP 网络和串口直连两种 AT 指令传输方式，通过 lora_sdk_set_at_transport() 切换
- **Tab 插件化架构**: 所有 Tab 页共用 `TabBaseClass` 窗口类和统一 WndProc。`TAB_BASE` 作为各 Tab 数据结构的首成员（C99 指针可互换），包含标准字体句柄和 vtable 指针。`TAB_IFACE` vtable 定义 4 个钩子：on_create (WM_CREATE 控件创建)、on_size (WM_SIZE 布局)、on_destroy (特有资源清理，字体由框架释放)、on_message (自定义消息/COMMAND 分发，返回 TAB_MSG_HANDLED 或 TAB_MSG_NOT_HANDLED)。框架在 on_create 后自动执行 SetWindowTheme 禁用 GroupBox 视觉主题。新增 Tab 只需：定义含 TAB_BASE 的 data struct → 实现 4 个钩子 → 定义静态 TAB_IFACE → TabBase_CreatePage 一行创建。
- **窗口自适应**: 主窗口自动适配屏幕工作区 (排除任务栏)，Tab 页面响应 WM_SIZE 重排 GroupBox 和 Edit 控件
- **GroupBox 蓝色标题**: 使用 SetWindowTheme(L"", L"") 禁用视觉主题 + WM_CTLCOLORSTATIC 设置蓝色文本 (RGB(0,80,180))
- **终端样式控件**: 监视器和日志使用等宽字体 Consolas，UI 控件使用 Microsoft YaHei
- **CAN 帧标注**: 总线监视器自动标注已知帧 ID (心跳/手柄状态/激光/坐标/LoRa 配参等)
- **LoRa 配参协议**: CAN 命令页通过 0x105/0x106 帧与嵌入式设备通信，命令码匹配 mod-can.h (SET_MODE/QUERY_MODE/SET_CH/QUERY_CH/SET_NID/SET_GWID/SET_TEST/SET_POWER)

## 资源 ID 分配

- 101: IDI_APPICON (应用图标)
- 201-206: IDR_MAINMENU, IDM_*, IDR_MAINACCEL
- 1001: IDC_TAB_CONTROL
- 1100-1199: Tab2 固件升级 (传输模式/适配器/通道/波特率/固件/进度/版本/日志)
- 1200-1299: Tab3 CAN 命令 (帧配置/监视器/LoRa 配置: 协议/模式/通道/频率/NID/GWID)
- 1300-1399: Tab0 LoRa 数据 (连接/IP/端口/遥测/日志/历史/CSV)
- 1400-1499: Tab1 LoRa 配置 (传输/设备/网络/LoRa 协议/AT 命令/日志/SOCKA)
- WM_APP+1 ~ WM_APP+9: 系统自定义消息 (进度/完成/CAN 帧/CAN 连接/适配器切换/日志)
- WM_APP+10 ~ WM_APP+17: LoRa SDK 线程编组消息 (连接状态/帧/设备发现/AT 响应/网络参数/日志/HEX/发送帧)

## 编码规范

- Unicode 构建 (UNICODE / _UNICODE)，MSVC 编译选项 `/utf-8`
- 中文界面，字体定义在 resource.h: FONT_FACE_UI (Microsoft YaHei), FONT_FACE_MONO (Consolas)
- 字号定义在 resource.h: FONT_SIZE_UI (24), FONT_SIZE_TAB (28), FONT_SIZE_MONO (20)
- 窗口尺寸: 1200x1080，定义在 resource.h 的 WINDOW_WIDTH/WINDOW_HEIGHT
- 控件布局常量在 resource.h: MARGIN=16, LINE_H=38, CTRL_H=34, BTN_W=110, COMBO_W=160, LABEL_W=72
- Tab 页面使用 GWLP_USERDATA 存储实例数据 (pData)，TAB_BASE 作为首成员，pData 通过 (TAB_BASE*)GetWindowLongPtrW 获取
- 线程安全: 后台线程通过 PostMessage 发送堆分配数据到 UI 线程，UI 线程处理完后 free
- 新增 Tab 页遵循 vtable 模式: data struct (TAB_BASE 首成员) → 实现 TAB_IFACE 钩子 → 静态 vtable → TabBase_CreatePage
- GroupBox 禁用视觉主题实现蓝色标题 (uxtheme.dll SetWindowTheme)
- CAN 多字节字段使用大端序: PutBE16/PutBE32/GetBE16/GetBE32
- LoRa SDK 使用静态链接 (LORA_SDK_STATIC)，add_subdirectory(loralib) 集成
