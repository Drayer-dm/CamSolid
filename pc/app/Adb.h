#pragma once

#include <string>

// Mantiene la redirección "adb forward" del puerto de CamSolid hacia el
// celular conectado por USB, aunque se desconecte y se vuelva a conectar.
class AdbForwarder
{
public:
    // Busca adb.exe. Devuelve false si no lo encuentra.
    bool Initialize();

    // Revisa los dispositivos conectados y crea la redirección si hace falta.
    // Devuelve un texto que describe el estado, para mostrarlo solo si cambia.
    std::wstring Update();

    void RemoveForward();

    // Instala la app CamSolid en el celular y la abre. Devuelve el resultado como texto.
    std::wstring InstallApp(const std::wstring& apkPath);

    const std::wstring& AdbPath() const { return _adbPath; }

private:
    std::wstring ChooseDevice();
    bool Run(const std::wstring& arguments, std::string& output);

    std::wstring _adbPath;
    std::wstring _forwardedDevice;
};
