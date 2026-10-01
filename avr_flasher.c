#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Gdiplus.lib")

#define IDC_PORT_COMBO 1001
#define IDC_HEX_EDIT   1002
#define IDC_BROWSE     1003
#define IDC_FLASH      1004
#define IDC_STATUS     1005

using namespace Gdiplus;

static HWND g_hPortCombo = NULL;
static HWND g_hHexEdit = NULL;
static HWND g_hStatus = NULL;
static HBRUSH g_bgBrush = NULL;
static Gdiplus::Image *g_logoImage = NULL;
static WCHAR g_logoPath[MAX_PATH] = L"";

static void SetStatusText(HWND hwnd, const char *text)
{
    SetWindowTextA(GetDlgItem(hwnd, IDC_STATUS), text);
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

        rc = RegEnumValueA(hKey,
                           index,
                           valueName,
                           &valueNameSize,
                           NULL,
                           &type,
                           (LPBYTE)data,
                           &dataSize);

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
        SetStatusText(hwnd, "No COM port selected.");
        return;
    }

    len = (int)SendMessageA(g_hPortCombo, CB_GETLBTEXTLEN, (WPARAM)portIndex, 0);
    if (len <= 0)
    {
        SetStatusText(hwnd, "Could not read COM port.");
        return;
    }

    SendMessageA(g_hPortCombo, CB_GETLBTEXT, (WPARAM)portIndex, (LPARAM)port);

    GetWindowTextA(g_hHexEdit, hexFile, sizeof(hexFile));
    if (hexFile[0] == '\0')
    {
        SetStatusText(hwnd, "Select a .hex file first.");
        return;
    }

    SetStatusText(hwnd, "Flashing...");
    BuildAvrdudeCommand(cmdLine, sizeof(cmdLine), port, hexFile);

    if (!RunCommandAndCaptureOutput(cmdLine, capture, sizeof(capture)))
    {
        SetStatusText(hwnd, "Failed to start avrdude.");
        return;
    }

    if (strstr(capture, "avrdude done") != NULL ||
        strstr(capture, "bytes of flash verified") != NULL ||
        strstr(capture, "Verification successful") != NULL)
    {
        SetStatusText(hwnd, "Flash complete.");
    }
    else
    {
        SetStatusText(hwnd, "Flash finished, check output.");
    }

    MessageBoxA(hwnd, capture, "avrdude output", MB_OK | MB_ICONINFORMATION);
}

static void FindLogoImagePath(void)
{
    WCHAR exePath[MAX_PATH];
    WCHAR dirPath[MAX_PATH];
    WCHAR candidatePng[MAX_PATH];
    WCHAR candidateSvg[MAX_PATH];

    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    WCHAR *slash = wcsrchr(exePath, L'\\');
    if (!slash)
        return;

    wcsncpy(dirPath, exePath, (size_t)(slash - exePath));
    dirPath[slash - exePath] = L'\0';

    swprintf(candidatePng, MAX_PATH, L"%s\\Logo7.png", dirPath);
    swprintf(candidateSvg, MAX_PATH, L"%s\\Logo7.svg", dirPath);

    if (GetFileAttributesW(candidatePng) != INVALID_FILE_ATTRIBUTES)
    {
        wcscpy(g_logoPath, candidatePng);
        return;
    }

    if (GetFileAttributesW(candidateSvg) != INVALID_FILE_ATTRIBUTES)
    {
        wcscpy(g_logoPath, candidateSvg);
    }
}

static void LoadLogoImage(void)
{
    if (g_logoImage)
    {
        delete g_logoImage;
        g_logoImage = NULL;
    }

    if (g_logoPath[0] == L'\0')
    {
        FindLogoImagePath();
    }

    if (g_logoPath[0] == L'\0')
        return;

    try
    {
        g_logoImage = Gdiplus::Image::FromFile(g_logoPath, FALSE);
        if (g_logoImage && g_logoImage->GetLastStatus() != Ok)
        {
            delete g_logoImage;
            g_logoImage = NULL;
        }
    }
    catch (...)
    {
        g_logoImage = NULL;
    }
}

static void PaintBackground(HWND hwnd, HDC hdc)
{
    RECT rc;
    Graphics graphics(hdc);
    SolidBrush backgroundBrush(Color(18, 20, 26));
    SolidBrush accentBrush(Color(35, 90, 140));
    Pen borderPen(Color(64, 168, 255), 2.0f);

    GetClientRect(hwnd, &rc);

    graphics.FillRectangle(&backgroundBrush, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);

    for (int i = 0; i < 8; i++)
    {
        int y = rc.top + i * 32;
        graphics.FillRectangle(&accentBrush, 0, y, rc.right, 2);
    }

    if (g_logoImage)
    {
        int w = g_logoImage->GetWidth();
        int h = g_logoImage->GetHeight();

        int targetW = min(260, rc.right - 60);
        float scale = (float)targetW / (float)w;
        int targetH = (int)(h * scale);

        int x = (rc.right - targetW) / 2;
        int y = 18;

        if (targetH > 120)
        {
            targetH = 120;
            scale = (float)targetH / (float)h;
            targetW = (int)(w * scale);
            x = (rc.right - targetW) / 2;
        }

        graphics.DrawImage(g_logoImage, x, y, targetW, targetH);
    }
    else
    {
        FontFamily fontFamily(L"Arial");
        Font font(&fontFamily, 20, FontStyleBold, UnitPixel);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);

        SolidBrush textBrush(Color(228, 234, 240));
        RectF textRect((REAL)0, (REAL)15, (REAL)rc.right, (REAL)80);
        graphics.DrawString(L"AVR Flasher", -1, &font, textRect, &format, &textBrush);
    }

    graphics.DrawRectangle(&borderPen, 8, 8, rc.right - 18, rc.bottom - 18);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            RECT rc;
            GetClientRect(hwnd, &rc);

            g_hPortCombo = CreateWindowExA(
                0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                260, 160, 220, 120,
                hwnd, (HMENU)IDC_PORT_COMBO, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            g_hHexEdit = CreateWindowExA(
                0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER,
                260, 220, 220, 24,
                hwnd, (HMENU)IDC_HEX_EDIT, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "BUTTON", "Browse...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                490, 220, 100, 24,
                hwnd, (HMENU)IDC_BROWSE, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "BUTTON", "FLASH",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                260, 270, 120, 36,
                hwnd, (HMENU)IDC_FLASH, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "STATIC", "COM Port",
                WS_CHILD | WS_VISIBLE,
                170, 165, 80, 20,
                hwnd, NULL, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            CreateWindowExA(
                0, "STATIC", "HEX File",
                WS_CHILD | WS_VISIBLE,
                180, 225, 70, 20,
                hwnd, NULL, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            g_hStatus = CreateWindowExA(
                0, "STATIC", "Ready",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                30, 340, 560, 26,
                hwnd, (HMENU)IDC_STATUS, ((LPCREATESTRUCTA)lParam)->hInstance, NULL);

            PopulateSerialPorts(g_hPortCombo);

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

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintBackground(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_logoImage)
            {
                delete g_logoImage;
                g_logoImage = NULL;
            }
            if (g_bgBrush)
            {
                DeleteObject(g_bgBrush);
                g_bgBrush = NULL;
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSA wc = {0};
    HWND hwnd;
    MSG msg;
    ULONG_PTR gdiplusToken;

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    FindLogoImagePath();
    LoadLogoImage();

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "AVRFlashWindowClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassA(&wc))
    {
        MessageBoxA(NULL, "Failed to register window class.", "Error", MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    hwnd = CreateWindowExA(
        0,
        "AVRFlashWindowClass",
        "AVR Flash Utility",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 650, 420,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        MessageBoxA(NULL, "Failed to create window.", "Error", MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}
