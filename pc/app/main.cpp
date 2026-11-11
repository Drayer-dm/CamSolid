// CamSolid.exe: crea la cámara virtual "CamSolid", mantiene la conexión USB con
// el celular y muestra en una ventana el estado y la latencia.
//
// Opciones:  --stop     apaga la instancia abierta
//            --sin-adb  no usa adb (para probar con el simulador de celular)
#include <windows.h>
#include <shellapi.h>
#include <mutex>
#include <string>
#include <thread>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include "Adb.h"
#include "CamSolidIds.h"
#include "Status.h"

// Controles con el estilo actual de Windows
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

namespace
{
    // Evento con nombre: "CamSolid.exe --stop" lo activa para cerrar la app
    constexpr wchar_t STOP_EVENT_NAME[] = L"Local\\CamSolidStop";
    constexpr wchar_t WINDOW_CLASS[] = L"CamSolidWindow";
    constexpr UINT_PTR REFRESH_TIMER = 1;
    constexpr UINT REFRESH_INTERVAL_MS = 500;
    constexpr int ROW_COUNT = 4;
    constexpr int BUTTON_ID = 100;
    constexpr int INSTALL_BUTTON_ID = 101;
    constexpr ULONGLONG INSTALL_MESSAGE_MS = 10000;

    const wchar_t* const ROW_LABELS[ROW_COUNT] = { L"Cámara de Windows", L"Celular", L"Aplicación", L"Rendimiento" };

    struct App
    {
        HANDLE stopEvent = nullptr;
        HWND window = nullptr;
        ComPtr<IMFVirtualCamera> camera;
        std::wstring cameraError;

        // Conexión con el celular, actualizada por su propio hilo
        bool useAdb = true;
        AdbForwarder adb;
        std::thread adbThread;
        std::mutex adbMutex;
        std::wstring adbStatus;

        // Estado que publica la DLL
        HANDLE statusMapping = nullptr;
        const CamSolidStatus* status = nullptr;

        // Ventana
        UINT dpi = 96;
        HFONT font = nullptr;
        HFONT titleFont = nullptr;
        HWND title = nullptr;
        HWND labels[ROW_COUNT] = {};
        HWND values[ROW_COUNT] = {};
        HWND hint = nullptr;
        HWND button = nullptr;
        HWND installButton = nullptr;

        // Instalación de la app en el celular, en su propio hilo
        std::wstring apkPath;
        std::thread installThread;
        bool installing = false;
        std::wstring installMessage;
        ULONGLONG installMessageUntil = 0;
    };

    App g_app;

    int Scale(int value)
    {
        return MulDiv(value, static_cast<int>(g_app.dpi), 96);
    }

    std::wstring FormatHresult(HRESULT hr)
    {
        wchar_t text[16];
        swprintf_s(text, L"0x%08X", static_cast<unsigned int>(hr));
        return text;
    }

    // La cámara existe mientras la app siga abierta
    void CreateVirtualCamera()
    {
        HRESULT hr = MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser,
            CAMSOLID_FRIENDLY_NAME,
            CAMSOLID_SOURCE_ID,
            nullptr,
            0,
            &g_app.camera);
        if (SUCCEEDED(hr))
        {
            hr = g_app.camera->Start(nullptr);
        }
        if (FAILED(hr))
        {
            g_app.camera.Reset();
            g_app.cameraError = L"No se pudo crear (" + FormatHresult(hr) + L"). ¿Está registrada CamSolidSource.dll?";
        }
    }

    void RemoveVirtualCamera()
    {
        if (g_app.camera)
        {
            g_app.camera->Stop();
            g_app.camera->Remove();
            g_app.camera->Shutdown();
            g_app.camera.Reset();
        }
    }

    // Revisa el celular cada 2 segundos sin bloquear la ventana
    void RunAdbLoop()
    {
        do
        {
            std::wstring status = g_app.adb.Update();
            std::lock_guard lock(g_app.adbMutex);
            g_app.adbStatus = std::move(status);
        } while (WaitForSingleObject(g_app.stopEvent, 2000) == WAIT_TIMEOUT);
    }

    // Lee el estado de la DLL. Devuelve false si ninguna aplicación usa la cámara.
    bool ReadCameraStatus(CamSolidStatus& status)
    {
        if (!g_app.status)
        {
            for (const wchar_t* name : { CAMSOLID_STATUS_GLOBAL_NAME, CAMSOLID_STATUS_LOCAL_NAME })
            {
                g_app.statusMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
                if (g_app.statusMapping)
                {
                    break;
                }
            }
            if (!g_app.statusMapping)
            {
                return false;
            }
            g_app.status = static_cast<const CamSolidStatus*>(MapViewOfFile(g_app.statusMapping, FILE_MAP_READ, 0, 0, sizeof(CamSolidStatus)));
            if (!g_app.status)
            {
                CloseHandle(g_app.statusMapping);
                g_app.statusMapping = nullptr;
                return false;
            }
        }

        if (!ReadStatus(g_app.status, status))
        {
            return false;
        }
        const bool recent = GetTickCount64() - status.updatedTickMs < CAMSOLID_STATUS_TIMEOUT_MS;
        return recent && status.state != StreamState::Stopped;
    }

    void SetTextIfChanged(HWND control, const std::wstring& text)
    {
        wchar_t current[512];
        GetWindowTextW(control, current, ARRAYSIZE(current));
        if (text != current)
        {
            SetWindowTextW(control, text.c_str());
        }
    }

    void Refresh()
    {
        std::wstring cameraText = g_app.camera ? L"Activa: elígela como «CamSolid» en tu aplicación" : L"Error: " + g_app.cameraError;

        std::wstring phoneText;
        bool showInstallMessage = false;
        {
            std::lock_guard lock(g_app.adbMutex);
            phoneText = g_app.useAdb ? g_app.adbStatus : L"Sin adb (modo de prueba con el simulador)";
            if (g_app.installing || GetTickCount64() < g_app.installMessageUntil)
            {
                phoneText = g_app.installMessage;
                showInstallMessage = true;
            }
            EnableWindow(g_app.installButton, g_app.useAdb && !g_app.apkPath.empty() && !g_app.installing);
        }

        CamSolidStatus status{};
        const bool inUse = ReadCameraStatus(status);
        const bool streaming = inUse && status.state == StreamState::Streaming;

        std::wstring appText = L"Ninguna aplicación está usando la cámara";
        std::wstring performanceText = L"—";
        if (inUse)
        {
            appText = L"En uso a " + std::to_wstring(status.width) + L"x" + std::to_wstring(status.height);
            appText += streaming ? L" · recibiendo video" : L" · esperando video del celular";
        }
        if (streaming)
        {
            if (status.deviceName[0] && !showInstallMessage)
            {
                phoneText = std::wstring(status.deviceName) + L" · enviando " + std::to_wstring(status.phoneWidth) + L"x" + std::to_wstring(status.phoneHeight);
            }

            wchar_t text[160];
            if (status.latencyMicros > 0)
            {
                swprintf_s(text, L"Latencia %u ms · %.0f cuadros/s · decodificación %.1f ms",
                    (status.latencyMicros + 500) / 1000, status.framesPerSecondX100 / 100.0, status.decodeMicros / 1000.0);
            }
            else
            {
                swprintf_s(text, L"Midiendo latencia… · %.0f cuadros/s", status.framesPerSecondX100 / 100.0);
            }
            performanceText = text;
        }

        SetTextIfChanged(g_app.values[0], cameraText);
        SetTextIfChanged(g_app.values[1], phoneText);
        SetTextIfChanged(g_app.values[2], appText);
        SetTextIfChanged(g_app.values[3], performanceText);
    }

    void CreateFonts()
    {
        if (g_app.font)
        {
            DeleteObject(g_app.font);
        }
        if (g_app.titleFont)
        {
            DeleteObject(g_app.titleFont);
        }
        g_app.font = CreateFontW(-MulDiv(10, static_cast<int>(g_app.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_app.titleFont = CreateFontW(-MulDiv(16, static_cast<int>(g_app.dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }

    void Layout()
    {
        const int margin = Scale(20);
        const int labelWidth = Scale(140);
        const int rowHeight = Scale(26);
        RECT client;
        GetClientRect(g_app.window, &client);
        const int valueWidth = client.right - margin * 2 - labelWidth;

        MoveWindow(g_app.title, margin, Scale(14), client.right - margin * 2, Scale(34), TRUE);
        int y = Scale(60);
        for (int i = 0; i < ROW_COUNT; i++)
        {
            MoveWindow(g_app.labels[i], margin, y, labelWidth, rowHeight, TRUE);
            MoveWindow(g_app.values[i], margin + labelWidth, y, valueWidth, rowHeight, TRUE);
            y += rowHeight;
        }
        MoveWindow(g_app.hint, margin, y + Scale(14), client.right - margin * 2, Scale(48), TRUE);

        const int buttonWidth = Scale(130);
        const int installWidth = Scale(180);
        const int buttonHeight = Scale(30);
        const int buttonTop = client.bottom - Scale(16) - buttonHeight;
        MoveWindow(g_app.button, client.right - margin - buttonWidth, buttonTop, buttonWidth, buttonHeight, TRUE);
        MoveWindow(g_app.installButton, client.right - margin - buttonWidth - Scale(10) - installWidth, buttonTop, installWidth, buttonHeight, TRUE);

        SendMessageW(g_app.title, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.titleFont), TRUE);
        for (HWND control : { g_app.hint, g_app.button, g_app.installButton })
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
        }
        for (int i = 0; i < ROW_COUNT; i++)
        {
            SendMessageW(g_app.labels[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
            SendMessageW(g_app.values[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
        }
    }

    void CreateControls(HWND window)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const auto createStatic = [&](const wchar_t* text)
        {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                0, 0, 0, 0, window, nullptr, instance, nullptr);
        };

        g_app.title = createStatic(L"CamSolid");
        for (int i = 0; i < ROW_COUNT; i++)
        {
            g_app.labels[i] = createStatic(ROW_LABELS[i]);
            g_app.values[i] = createStatic(L"");
        }
        g_app.hint = CreateWindowExW(0, L"STATIC",
            L"1. Abre la app CamSolid en el celular y conéctalo por USB.\n"
            L"2. En Zoom, Teams, OBS u otra aplicación, elige la cámara «CamSolid».",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        g_app.button = CreateWindowExW(0, L"BUTTON", L"Apagar y salir", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(BUTTON_ID)), instance, nullptr);
        g_app.installButton = CreateWindowExW(0, L"BUTTON", L"Instalar app en el celular", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(INSTALL_BUTTON_ID)), instance, nullptr);
    }

    // Instala la APK que acompaña a CamSolid.exe, sin bloquear la ventana
    void StartInstall()
    {
        {
            std::lock_guard lock(g_app.adbMutex);
            if (g_app.installing || g_app.apkPath.empty())
            {
                return;
            }
            g_app.installing = true;
            g_app.installMessage = L"Instalando la app en el celular…";
        }
        if (g_app.installThread.joinable())
        {
            g_app.installThread.join();
        }
        g_app.installThread = std::thread([]
        {
            std::wstring result = g_app.adb.InstallApp(g_app.apkPath);
            std::lock_guard lock(g_app.adbMutex);
            g_app.installMessage = std::move(result);
            g_app.installMessageUntil = GetTickCount64() + INSTALL_MESSAGE_MS;
            g_app.installing = false;
        });
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_app.window = window;
            CreateControls(window);
            CreateFonts();
            Layout();
            Refresh();
            SetTimer(window, REFRESH_TIMER, REFRESH_INTERVAL_MS, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == REFRESH_TIMER)
            {
                // Cerrar desde "CamSolid.exe --stop"
                if (WaitForSingleObject(g_app.stopEvent, 0) == WAIT_OBJECT_0)
                {
                    DestroyWindow(window);
                    return 0;
                }
                Refresh();
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == BUTTON_ID)
            {
                DestroyWindow(window);
            }
            else if (LOWORD(wParam) == INSTALL_BUTTON_ID)
            {
                StartInstall();
                Refresh();
            }
            return 0;

        case WM_CTLCOLORSTATIC:
        {
            // Etiquetas en gris y todo sobre el fondo blanco de la ventana
            const HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            bool isLabel = false;
            for (HWND label : g_app.labels)
            {
                isLabel = isLabel || control == label;
            }
            SetTextColor(dc, isLabel ? RGB(100, 100, 100) : GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_DPICHANGED:
        {
            g_app.dpi = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            Layout();
            return 0;
        }

        case WM_DESTROY:
            KillTimer(window, REFRESH_TIMER);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND CreateMainWindow(HINSTANCE instance)
    {
        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        windowClass.lpszClassName = WINDOW_CLASS;
        RegisterClassExW(&windowClass);

        g_app.dpi = GetDpiForSystem();
        RECT bounds{ 0, 0, Scale(560), Scale(310) };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, g_app.dpi);

        return CreateWindowExW(0, WINDOW_CLASS, L"CamSolid", style, CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    }

    bool HasArgument(const wchar_t* name)
    {
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        bool found = false;
        for (int i = 1; i < count; i++)
        {
            found = found || wcscmp(arguments[i], name) == 0;
        }
        LocalFree(arguments);
        return found;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_app.stopEvent = CreateEventW(nullptr, TRUE, FALSE, STOP_EVENT_NAME);
    if (!g_app.stopEvent)
    {
        return 1;
    }
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;

    if (HasArgument(L"--stop"))
    {
        SetEvent(g_app.stopEvent);
        return alreadyRunning ? 0 : 1;
    }
    if (alreadyRunning)
    {
        // Ya está abierta: se muestra esa ventana en vez de abrir otra
        if (HWND existing = FindWindowW(WINDOW_CLASS, nullptr))
        {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    // La APK del celular viaja junto a CamSolid.exe
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
    {
        std::wstring apk = modulePath;
        apk.resize(apk.find_last_of(L'\\') + 1);
        apk += L"CamSolid.apk";
        if (GetFileAttributesW(apk.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            g_app.apkPath = apk;
        }
    }

    g_app.useAdb = !HasArgument(L"--sin-adb");
    if (g_app.useAdb && !g_app.adb.Initialize())
    {
        g_app.useAdb = false;
        g_app.adbStatus = L"No se encontró adb.exe: la conexión por USB no funcionará";
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION)))
    {
        return 1;
    }
    CreateVirtualCamera();

    if (g_app.useAdb)
    {
        g_app.adbStatus = L"Buscando el celular…";
        g_app.adbThread = std::thread(RunAdbLoop);
    }

    const HWND window = CreateMainWindow(instance);
    ShowWindow(window, showCommand);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // Apagado: primero el hilo de adb, luego la cámara
    SetEvent(g_app.stopEvent);
    if (g_app.adbThread.joinable())
    {
        g_app.adbThread.join();
    }
    if (g_app.installThread.joinable())
    {
        g_app.installThread.join();
    }
    if (g_app.useAdb)
    {
        g_app.adb.RemoveForward();
    }
    RemoveVirtualCamera();

    if (g_app.status)
    {
        UnmapViewOfFile(g_app.status);
    }
    if (g_app.statusMapping)
    {
        CloseHandle(g_app.statusMapping);
    }
    MFShutdown();
    CoUninitialize();
    CloseHandle(g_app.stopEvent);
    return 0;
}
