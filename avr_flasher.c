#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_PORT_COMBO 1001
#define IDC_HEX_EDIT   1002
#define IDC_BROWSE     1003
#define IDC_FLASH      1004
#define IDC_STATUS     1005
#define IDC_LOGO       1006

static HWND g_hPortCombo = NULL;
static HWND g_hHexEdit = NULL;
static HWND g_hStatus = NULL;
static HWND g_hLogoStatic = NULL;
static HBITMAP g_logoBitmap = NULL;

static void SetStatusText(const char *text)
{
    SetWindowTextA(g_hStatus, text);
}

static void PopulateSerialPorts(HWND combo)
{
    HKEY hKey = NULL;
    DWORD index = 0;
    char valueName[256], data[256];
    DWORD valueNameSize, dataSize, type;
    LONG rc;

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);

    rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                       0,
                       KEY_READ,
                       &hKey);

    if (rc != ERROR_SUCCESS)
    {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"No COM ports found");
        return;
    }

    while (1)
    {
        valueNameSize = sizeof(valueName);
        dataSize = sizeof(data);

        rc = RegEnumValueA(hKey, index, valueName, &valueNameSize, NULL, &type, (LPBYTE)data, &dataSize);

        if (rc == ERROR_NO_MORE_ITEMS)
            break;

        if (rc == ERROR_SUCCESS && type == REG_SZ)
        {
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)data);
        }

        index++;
    }

    RegCloseKey(hKey);

    if (SendMessageA(combo, CB_GETCOUNT, 0, 0) == 0)
    {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"No COM ports found");
    }

    SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

static void BrowseForHexFile(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char filePath[MAX_PATH] = "";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "HEX Files (*.hex)\0*.hex\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = sizeof(filePath);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameA(&ofn))
    {
        SetWindowTextA(g_hHexEdit, filePath);
    }
}

static void BuildAvrdudeCommand(char *out, size_t outSize, const char *port, const char *hexFile)
{
    char exeDir[MAX_PATH];
    char avrdudeExe[MAX_PATH];
    char avrdudeConf[MAX_PATH];

    GetModuleFileNameA(NULL, exeDir, sizeof(exeDir));
    char *slash = strrchr(exeDir, '\\');
    if (slash)
        *slash = '\0';

    snprintf(avrdudeExe, sizeof(avrdudeExe), "%s\\avrdude.exe", exeDir);
    snprintf(avrdudeConf, sizeof(avrdudeConf), "%s\\avrdude.conf", exeDir);

    snprintf(out, outSize,
             "\"%s\" -C\"%s\" -v -p atmega328p -c stk500 -P%s -U flash:w:\"%s\":i",
             avrdudeExe,
             avrdudeConf,
             port,
             hexFile);
}

static int RunCommandAndCaptureOutput(const char *cmdLine, char *output, size_t outputSize)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    BOOL result;
    DWORD bytesRead = 0;
    char buffer[4096];
    size_t outPos = 0;

    ZeroMemory(output, outputSize);

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return 0;

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES;

    ZeroMemory(&pi, sizeof(pi));

    result = CreateProcessA(
        NULL,
        (LPSTR)cmdLine,
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &si,
        &pi);

    if (!result)
    {
        CloseHandle(hWritePipe);
        CloseHandle(hReadPipe);
        return 0;
    }

    CloseHandle(hWritePipe);
    WaitForSingleObject(pi.hProcess, INFINITE);

    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
    {
        if (outPos + bytesRead < outputSize)
        {
            memcpy(output + outPos, buffer, bytesRead);
            outPos += bytesRead;
            output[outPos] = '\0';
        }
    }

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 1;
}

static void FlashSelectedHex(HWND hwnd)
{
    int portIndex;
    char port[64] = "";
    char hexFile[MAX_PATH] = "";
    char cmdLine[2048];
    char capture[8192];
    int len;

    portIndex = (int)SendMessageA(g_hPortCombo, CB_GETCURSEL, 0, 0);
    if (portIndex == CB_ERR)
    {
        SetStatusText("No COM port selected.");
        return;
    }

    len = (int)SendMessageA(g_hPortCombo, CB_GETLBTEXTLEN, (WPARAM)portIndex, 0);
    if (len <= 0)
    {
        SetStatusText("Could not read COM port.");
        return;
    }

    SendMessageA(g_hPortCombo, CB_GETLBTEXT, (WPARAM)portIndex, (LPARAM)port);

    GetWindowTextA(g_hHexEdit, hexFile, sizeof(hexFile));
    if (hexFile[0] == '\0')
    {
        SetStatusText("Select a .hex file first.");
        return;
    }

    SetStatusText("Flashing...");
    BuildAvrdudeCommand(cmdLine, sizeof(cmdLine), port, hexFile);

    if (!RunCommandAndCaptureOutput(cmdLine, capture, sizeof(capture)))
    {
        SetStatusText("Failed to start avrdude.");
        return;
    }

    if (strstr(capture, "avrdude done") != NULL ||
        strstr(capture, "bytes of flash verified") != NULL ||
        strstr(capture, "Verification successful") != NULL)
    {
        SetStatusText("Flash complete.");
    }
    else
    {
        SetStatusText("Flash finished, check output.");
    }

    MessageBoxA(hwnd, capture, "avrdude output", MB_OK | MB_ICONINFORMATION);
}

static HBITMAP LoadPNGFromFile(const char *filepath)
{
    HBITMAP hBitmap = NULL;
    WCHAR wFilepath[MAX_PATH];
    int len;

    if (!filepath || filepath[0] == '\0')
        return NULL;

    len = MultiByteToWideChar(CP_ACP, 0, filepath, -1, wFilepath, MAX_PATH);
    if (len == 0)
        return NULL;

    hBitmap = (HBITMAP)LoadImageW(NULL, wFilepath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);

    return hBitmap;
}

static void LoadLogoImage(HWND hwnd, HINSTANCE hInstance)
{
    char exeDir[MAX_PATH];
    char logoPath[MAX_PATH];
    HBITMAP hBitmap;

    GetModuleFileNameA(NULL, exeDir, sizeof(exeDir));
    char *slash = strrchr(exeDir, '\\');
    if (slash)
        *slash = '\0';

    snprintf(logoPath, sizeof(logoPath), "%s\\Logo7.png", exeDir);

    hBitmap = LoadPNGFromFile(logoPath);

    if (hBitmap)
    {
        g_logoBitmap = hBitmap;
        SendMessageA(g_hLogoStatic, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBitmap);
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HFONT hFont = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");

            g_hLogoStatic = CreateWindowExA(
                0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
                20, 10, 560, 90,
                hwnd, (HMENU)IDC_LOGO, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "STATIC", "COM Port:",
                WS_CHILD | WS_VISIBLE,
                30, 120, 80, 20,
                hwnd, NULL, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            g_hPortCombo = CreateWindowExA(
                0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                120, 115, 200, 120,
                hwnd, (HMENU)IDC_PORT_COMBO, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "STATIC", "HEX File:",
                WS_CHILD | WS_VISIBLE,
                30, 155, 80, 20,
                hwnd, NULL, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            g_hHexEdit = CreateWindowExA(
                0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER,
                120, 150, 200, 22,
                hwnd, (HMENU)IDC_HEX_EDIT, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "BUTTON", "Browse...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                330, 150, 90, 22,
                hwnd, (HMENU)IDC_BROWSE, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "BUTTON", "FLASH",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                120, 190, 120, 40,
                hwnd, (HMENU)IDC_FLASH, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            g_hStatus = CreateWindowExA(
                0, "STATIC", "Ready",
                WS_CHILD | WS_VISIBLE | SS_LEFT | WS_BORDER,
                20, 250, 560, 60,
                hwnd, (HMENU)IDC_STATUS, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);
            SendMessageA(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

            PopulateSerialPorts(g_hPortCombo);
            LoadLogoImage(hwnd, ((LPCREATESTRUCTA)lParam)->hInstance);

            return 0;
        }

        case WM_COMMAND:
        {
            if (LOWORD(wParam) == IDC_BROWSE && HIWORD(wParam) == BN_CLICKED)
            {
                BrowseForHexFile(hwnd);
                return 0;
            }

            if (LOWORD(wParam) == IDC_FLASH && HIWORD(wParam) == BN_CLICKED)
            {
                FlashSelectedHex(hwnd);
                return 0;
            }
        }
        break;

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(240, 240, 240));
            SetTextColor(hdc, RGB(0, 0, 0));
            return (LRESULT)GetStockObject(LTGRAY_BRUSH);
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_logoBitmap)
            {
                DeleteObject(g_logoBitmap);
                g_logoBitmap = NULL;
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSA wc = {0};
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "AVRFlashWindowClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_VREDRAW | CS_HREDRAW;

    if (!RegisterClassA(&wc))
    {
        MessageBoxA(NULL, "Failed to register window class.", "Error", MB_ICONERROR);
        return 1;
    }

    hwnd = CreateWindowExA(
        0,
        "AVRFlashWindowClass",
        "AVR Flash Utility",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 350,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        MessageBoxA(NULL, "Failed to create window.", "Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
