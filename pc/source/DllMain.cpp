#include <windows.h>
#include <string>
#include "Activator.h"
#include "CamSolidIds.h"

namespace
{
    HMODULE g_module = nullptr;

    // Windows pide a esta fábrica el Activator cuando necesita la cámara
    class ClassFactory final : public IClassFactory
    {
    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** result) override
        {
            if (!result)
            {
                return E_POINTER;
            }
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
            {
                *result = static_cast<IClassFactory*>(this);
                return S_OK;
            }
            *result = nullptr;
            return E_NOINTERFACE;
        }

        // Es un objeto global, así que no hace falta contar sus referencias
        STDMETHODIMP_(ULONG) AddRef() override { return 2; }
        STDMETHODIMP_(ULONG) Release() override { return 1; }

        STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** result) override
        {
            if (!result)
            {
                return E_POINTER;
            }
            *result = nullptr;
            if (outer)
            {
                return CLASS_E_NOAGGREGATION;
            }
            return Activator::Create(riid, result);
        }

        STDMETHODIMP LockServer(BOOL lock) override
        {
            if (lock)
            {
                ++g_objectCount;
            }
            else
            {
                --g_objectCount;
            }
            return S_OK;
        }
    };

    ClassFactory g_classFactory;

    std::wstring ClassKeyPath()
    {
        return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + CAMSOLID_SOURCE_ID;
    }

    HRESULT SetRegistryString(const std::wstring& key, const wchar_t* name, const std::wstring& value)
    {
        const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        return HRESULT_FROM_WIN32(RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, REG_SZ, value.c_str(), size));
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return g_objectCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID classId, REFIID riid, void** result)
{
    if (!result)
    {
        return E_POINTER;
    }
    *result = nullptr;
    if (classId != CAM_SOLID_ID)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_classFactory.QueryInterface(riid, result);
}

// Se guarda en HKEY_LOCAL_MACHINE porque el servicio Frame Server, que es quien
// carga la DLL, no puede leer el registro del usuario. Por eso requiere permisos
// de administrador.
STDAPI DllRegisterServer()
{
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(g_module, modulePath, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
    {
        return E_FAIL;
    }

    const std::wstring serverKey = ClassKeyPath() + L"\\InprocServer32";
    RETURN_IF_FAILED(SetRegistryString(ClassKeyPath(), nullptr, CAMSOLID_FRIENDLY_NAME));
    RETURN_IF_FAILED(SetRegistryString(serverKey, nullptr, modulePath));
    RETURN_IF_FAILED(SetRegistryString(serverKey, L"ThreadingModel", L"Both"));
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClassKeyPath().c_str());
    return status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}
