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

// Tab1: 固件升级 (1100-1199)
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

// Tab3: LoRa 数据 (1300-1399)
#define IDC_LORA_IP_EDIT                1301
#define IDC_LORA_PORT_EDIT              1302
#define IDC_LORA_CONNECT_BTN            1303
#define IDC_LORA_DISCONNECT_BTN         1304
#define IDC_LORA_STATUS_TEXT            1305
#define IDC_LORA_NID_TEXT               1306
#define IDC_LORA_TEST_CHECK             1307
#define IDC_LORA_X_TEXT                 1308
#define IDC_LORA_Y_TEXT                 1309
#define IDC_LORA_BTN_TEXT               1310
#define IDC_LORA_RX_COUNT               1311
#define IDC_LORA_TX_COUNT               1312
#define IDC_LORA_ERR_COUNT              1313
#define IDC_LORA_LOG_EDIT               1314
#define IDC_LORA_SEND_EDIT              1315
#define IDC_LORA_SEND_BTN               1316
#define IDC_LORA_CLEAR_BTN              1317
#define IDC_LORA_HISTORY_LIST           1318
#define IDC_LORA_SAVE_CSV_BTN           1319

// Tab4: LoRa 配置 (1400-1499)
#define IDC_CFG_SEARCH_BTN              1401
#define IDC_CFG_GETNET_BTN              1402
#define IDC_CFG_QUERY_GWID              1403
#define IDC_CFG_QUERY_CSQ               1404
#define IDC_CFG_MAC_TEXT                1405
#define IDC_CFG_DEV_TEXT                1406
#define IDC_CFG_SW_TEXT                 1407
#define IDC_CFG_GWID_TEXT               1408
#define IDC_CFG_CSQ_TEXT                1409
#define IDC_CFG_DHCP_TEXT               1410
#define IDC_CFG_DHCP_QUERY              1411
#define IDC_CFG_DHCP_ON                 1412
#define IDC_CFG_DHCP_OFF                1413
#define IDC_CFG_IP_EDIT                 1414
#define IDC_CFG_IP_SET                  1415
#define IDC_CFG_IP_QUERY                1416
#define IDC_CFG_MASK_EDIT               1417
#define IDC_CFG_MASK_SET                1418
#define IDC_CFG_MASK_QUERY              1419
#define IDC_CFG_GW_EDIT                 1420
#define IDC_CFG_GW_SET                  1421
#define IDC_CFG_GW_QUERY                1422
#define IDC_CFG_OPTION_COMBO            1423
#define IDC_CFG_OPTION_SET              1424
#define IDC_CFG_OPTION_QUERY            1425
#define IDC_CFG_NWMODE_COMBO            1426
#define IDC_CFG_NWMODE_SET              1427
#define IDC_CFG_NWMODE_QUERY            1428
#define IDC_CFG_TTMODE_COMBO            1429
#define IDC_CFG_TTMODE_SET              1430
#define IDC_CFG_TTMODE_QUERY            1431
#define IDC_CFG_WMODE_COMBO             1432
#define IDC_CFG_WMODE_SET               1433
#define IDC_CFG_WMODE_QUERY             1434
#define IDC_CFG_UPWID_TEXT              1435
#define IDC_CFG_UPWID_QUERY             1436
#define IDC_CFG_UPWID_ON                1437
#define IDC_CFG_UPWID_OFF               1438
#define IDC_CFG_CH_COMBO                1439
#define IDC_CFG_CH_FREQ_COMBO           1440
#define IDC_CFG_CH_SET                  1441
#define IDC_CFG_CH_QUERY                1442
#define IDC_CFG_SPD_COMBO               1443
#define IDC_CFG_SPD_SET                 1444
#define IDC_CFG_SPD_QUERY               1445
#define IDC_CFG_PWR_COMBO               1446
#define IDC_CFG_PWR_SET                 1447
#define IDC_CFG_PWR_QUERY               1448
#define IDC_CFG_CMD_EDIT                1449
#define IDC_CFG_SEND_BTN                1450
#define IDC_CFG_QUERY_VER               1451
#define IDC_CFG_LOG_EDIT                1452
#define IDC_CFG_CLEAR_BTN               1453

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

// LoRa SDK thread-marshaling messages (WM_APP + 10 ~ 16)
#define WM_LORA_CONN_STATE              (WM_APP + 10)
#define WM_LORA_FRAME                   (WM_APP + 11)
#define WM_LORA_DEVICE_FOUND            (WM_APP + 12)
#define WM_LORA_AT_RESPONSE             (WM_APP + 13)
#define WM_LORA_NET_PARAMS              (WM_APP + 14)
#define WM_LORA_LOG                     (WM_APP + 15)
#define WM_LORA_HEX_DUMP                (WM_APP + 16)
#define WM_LORA_SEND_FRAME              (WM_APP + 17)

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
