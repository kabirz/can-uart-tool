#include <windows.h>
#include "resource.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    MessageBoxW(NULL, L"构建成功", L"CAN/UART 工具", MB_OK | MB_ICONINFORMATION);
    return 0;
}
