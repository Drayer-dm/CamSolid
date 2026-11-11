#include "Adb.h"

#include <windows.h>
#include <sstream>
#include <vector>
#include "Protocol.h"

namespace
{
    bool FileExists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring GetEnvironment(const wchar_t* name)
    {
        wchar_t value[MAX_PATH];
        const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
        return length > 0 && length < MAX_PATH ? std::wstring(value, length) : std::wstring();
    }

    // Los nombres de dispositivo y la salida de adb son ASCII
    std::wstring Widen(const std::string& text)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (const wchar_t character : text)
        {
            result.push_back(character < 128 ? static_cast<char>(character) : '?');
        }
        return result;
    }
}

bool AdbForwarder::Initialize()
{
    std::vector<std::wstring> candidates;

    // Junto a CamSolid.exe: así se distribuirá en el instalador
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
    {
        std::wstring folder = modulePath;
        folder.resize(folder.find_last_of(L'\\') + 1);
        candidates.push_back(folder + L"adb.exe");
        candidates.push_back(folder + L"platform-tools\\adb.exe");
    }
    for (const wchar_t* variable : { L"ANDROID_HOME", L"ANDROID_SDK_ROOT" })
    {
        const std::wstring sdk = GetEnvironment(variable);
        if (!sdk.empty())
        {
            candidates.push_back(sdk + L"\\platform-tools\\adb.exe");
        }
    }
    const std::wstring localAppData = GetEnvironment(L"LOCALAPPDATA");
    if (!localAppData.empty())
    {
        candidates.push_back(localAppData + L"\\Android\\Sdk\\platform-tools\\adb.exe");
    }

    for (const std::wstring& candidate : candidates)
    {
        if (FileExists(candidate))
        {
            _adbPath = candidate;
            return true;
        }
    }

    // Por último, el PATH
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) > 0)
    {
        _adbPath = found;
        return true;
    }
    return false;
}

// Ejecuta adb sin ventana y devuelve lo que escribe
bool AdbForwarder::Run(const std::wstring& arguments, std::string& output)
{
    output.clear();

    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0))
    {
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring commandLine = L"\"" + _adbPath + L"\" " + arguments;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!started)
    {
        CloseHandle(readPipe);
        return false;
    }

    char buffer[4096];
    DWORD count = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &count, nullptr) && count > 0)
    {
        output.append(buffer, count);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(process.hProcess, 10'000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return exitCode == 0;
}

// El celular a usar: el primero listo, prefiriendo uno real a un emulador
std::wstring AdbForwarder::ChooseDevice()
{
    std::string output;
    if (!Run(L"devices", output))
    {
        return {};
    }

    std::string emulator;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream fields(line);
        std::string serial;
        std::string state;
        if (!(fields >> serial >> state) || state != "device")
        {
            continue;
        }
        if (serial.rfind("emulator-", 0) == 0)
        {
            if (emulator.empty())
            {
                emulator = serial;
            }
            continue;
        }
        return Widen(serial);
    }
    return Widen(emulator);
}

std::wstring AdbForwarder::Update()
{
    const std::wstring device = ChooseDevice();
    if (device.empty())
    {
        _forwardedDevice.clear();
        return L"Esperando un celular por USB (activa la depuración USB y acepta el aviso)";
    }

    // adb olvida la redirección cuando el celular se desconecta: se comprueba cada vez
    const std::wstring port = std::to_wstring(CAMSOLID_PORT);
    std::string forwards;
    Run(L"forward --list", forwards);
    const std::string expected = Narrow(device) + " tcp:" + std::to_string(CAMSOLID_PORT);
    if (device == _forwardedDevice && forwards.find(expected) != std::string::npos)
    {
        return L"Conectado por USB: " + device;
    }

    std::string output;
    if (!Run(L"-s " + device + L" forward tcp:" + port + L" tcp:" + port, output))
    {
        _forwardedDevice.clear();
        return L"No se pudo preparar la conexión con " + device + L": " + Widen(output);
    }
    _forwardedDevice = device;
    return L"Conectado por USB: " + device;
}

std::wstring AdbForwarder::InstallApp(const std::wstring& apkPath)
{
    const std::wstring device = ChooseDevice();
    if (device.empty())
    {
        return L"Conecta el celular por USB para instalar la app";
    }

    std::string output;
    if (!Run(L"-s " + device + L" install -r \"" + apkPath + L"\"", output))
    {
        // En Xiaomi hay que activar "Instalar vía USB" en las opciones de desarrollador
        if (output.find("INSTALL_FAILED_USER_RESTRICTED") != std::string::npos)
        {
            return L"El celular bloqueó la instalación: activa «Instalar vía USB» en las opciones de desarrollador";
        }
        return L"No se pudo instalar la app en el celular";
    }
    Run(L"-s " + device + L" shell am start -n com.camsolid.app/.MainActivity", output);
    return L"App instalada y abierta en el celular";
}

void AdbForwarder::RemoveForward()
{
    if (!_adbPath.empty())
    {
        std::string output;
        Run(L"forward --remove tcp:" + std::to_wstring(CAMSOLID_PORT), output);
    }
}
