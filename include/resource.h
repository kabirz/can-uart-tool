#ifndef RESOURCE_H
#define RESOURCE_H

#include <windows.h>

// Icon resources
#define IDI_APPICON                     101

// Main window
#define IDD_MAIN                        201

// Menu
#define IDR_MAINMENU                    202
#define IDM_FILE_EXIT                   203
#define IDM_HELP_ABOUT                  204
#define IDM_FILE_OPEN                   205
#define IDR_MAINACCEL                   206

// Tab Control
#define IDC_TAB_CONTROL                 1001

// Tab1: CAN/UART 固件升级 (1100-1199)
#define IDC_COMBO_TRANSPORT             1101
#define IDC_COMBO_CHANNEL               1102
#define IDC_COMBO_BAUDRATE              1103
#define IDC_COMBO_UART_BAUDRATE         1104
#define IDC_BUTTON_REFRESH              1105
#define IDC_BUTTON_CONNECT              1106
#define IDC_EDIT_FIRMWARE               1107
#define IDC_BUTTON_BROWSE               1108
#define IDC_CHECK_TESTMODE              1109
#define IDC_PROGRESS                    1110
#define IDC_BUTTON_FLASH                1111
#define IDC_LABEL_VERSION               1112
#define IDC_BUTTON_GETVERSION           1113
#define IDC_BUTTON_REBOOT               1114
#define IDC_EDIT_LOG                    1115
#define IDC_BUTTON_CLEAR_LOG            1116
#define IDC_LABEL_PERCENT               1117
#define IDC_COMBO_ADAPTER               1118

// Tab2: CAN 命令发送 (1200-1299)
#define IDC_EDIT_CAN_ID                 1201
#define IDC_EDIT_CAN_DATA               1202
#define IDC_RADIO_STD_FRAME             1203
#define IDC_RADIO_EXT_FRAME             1204
#define IDC_RADIO_DATA_FRAME            1205
#define IDC_RADIO_REMOTE_FRAME          1206
#define IDC_BUTTON_CAN_SEND             1207
#define IDC_EDIT_CAN_MONITOR            1208
#define IDC_CHECK_AUTOSCROLL            1209
#define IDC_BUTTON_CLEAR_MONITOR        1210

// Tab2: CAN 命令 - LoRa 配置 (1220-1260)
#define IDC_EDIT_LORA_PROT              1220
#define IDC_EDIT_LORA_MODE              1221
#define IDC_COMBO_LORA_SPD1             1222
#define IDC_EDIT_LORA_CH1               1223
#define IDC_COMBO_LORA_SPD2             1224
#define IDC_EDIT_LORA_CH2               1225
#define IDC_COMBO_LORA_PNUM             1226
#define IDC_EDIT_LORA_NID               1227
#define IDC_EDIT_LORA_GWID              1228
#define IDC_BUTTON_LORA_QUERY_CFG       1229
#define IDC_BUTTON_LORA_SET_MODE        1230
#define IDC_BUTTON_LORA_SET_CH1         1231
#define IDC_BUTTON_LORA_SET_CH2         1232
#define IDC_BUTTON_LORA_SET_PNUM        1233
#define IDC_BUTTON_LORA_QUERY_NID       1234
#define IDC_BUTTON_LORA_SET_NID         1235
#define IDC_BUTTON_LORA_QUERY_GWID      1236
#define IDC_BUTTON_LORA_SET_GWID        1237
#define IDC_LABEL_LORA_STATUS           1238
#define IDC_BUTTON_LORA_POWER           1239
#define IDC_BUTTON_LORA_TEST            1240

// Tab3: UART Shell 终端 (1300-1399)
#define IDC_COMBO_UART_PORT             1301
#define IDC_COMBO_UART_TERM_BAUD        1302
#define IDC_BUTTON_UART_CONNECT         1303
#define IDC_EDIT_UART_TERMINAL          1304
#define IDC_BUTTON_UART_CLEAR           1305
#define IDC_BUTTON_UART_SENDFILE        1306

// Tab4: TCP/UDP 网络终端 (1400-1499)
#define IDC_RADIO_TCP                   1401
#define IDC_RADIO_UDP                   1402
#define IDC_EDIT_NET_HOST               1403
#define IDC_EDIT_NET_PORT               1404
#define IDC_BUTTON_NET_CONNECT          1405
#define IDC_EDIT_NET_TERMINAL           1406
#define IDC_BUTTON_NET_CLEAR            1407

// Custom messages
#define WM_UPDATE_PROGRESS              (WM_APP + 1)
#define WM_UPDATE_COMPLETE              (WM_APP + 2)
#define WM_CAN_FRAME_RECEIVED           (WM_APP + 3)
#define WM_NET_DATA_RECEIVED            (WM_APP + 4)
#define WM_UART_DATA_RECEIVED           (WM_APP + 5)
#define WM_CAN_CONNECTED                (WM_APP + 6)
#define WM_CAN_DISCONNECTED             (WM_APP + 7)
#define WM_ADAPTER_CHANGED              (WM_APP + 8)
#define WM_LOG_MESSAGE                  (WM_APP + 9)

// Layout constants
#define WINDOW_WIDTH                    1200
#define WINDOW_HEIGHT                   1080
#define TAB_HEIGHT                      36
#define STATUSBAR_HEIGHT                28
#define MARGIN                          16
#define LINE_H                          38
#define CTRL_H                          34
#define BTN_W                           110
#define COMBO_W                         160
#define LABEL_W                         72
#define FONT_H                          24

#define IDC_STATIC                      (-1)

#endif // RESOURCE_H
