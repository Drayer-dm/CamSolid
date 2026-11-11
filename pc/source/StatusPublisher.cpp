#include "StatusPublisher.h"

#include <sddl.h>

namespace
{
    HANDLE CreateMapping(const wchar_t* name, SECURITY_ATTRIBUTES* security)
    {
        return CreateFileMappingW(INVALID_HANDLE_VALUE, security, PAGE_READWRITE, 0, sizeof(CamSolidStatus), name);
    }
}

StatusPublisher::StatusPublisher()
{
    // Escritura para el servicio (SYSTEM y LOCAL SERVICE) y lectura para todos,
    // para que CamSolid.exe pueda leerlo desde la sesión del usuario
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, FALSE };
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;OW)(A;;GR;;;WD)", SDDL_REVISION_1, &descriptor, nullptr))
    {
        security.lpSecurityDescriptor = descriptor;
    }

    _mapping = CreateMapping(CAMSOLID_STATUS_GLOBAL_NAME, &security);
    if (!_mapping)
    {
        // Proceso normal, sin permiso para crear objetos globales
        _mapping = CreateMapping(CAMSOLID_STATUS_LOCAL_NAME, nullptr);
    }
    if (descriptor)
    {
        LocalFree(descriptor);
    }

    if (_mapping)
    {
        _status = static_cast<CamSolidStatus*>(MapViewOfFile(_mapping, FILE_MAP_WRITE, 0, 0, sizeof(CamSolidStatus)));
    }
    Update([](CamSolidStatus& status) { status.state = StreamState::Stopped; });
}

StatusPublisher::~StatusPublisher()
{
    Update([](CamSolidStatus& status) { status.state = StreamState::Stopped; });
    if (_status)
    {
        UnmapViewOfFile(_status);
    }
    if (_mapping)
    {
        CloseHandle(_mapping);
    }
}

void StatusPublisher::Update(const std::function<void(CamSolidStatus&)>& change)
{
    std::lock_guard lock(_mutex);
    if (!_status)
    {
        return;
    }

    // La secuencia es impar mientras se escribe: así el lector sabe que debe reintentar
    InterlockedIncrement(&_status->sequence);
    _status->version = CAMSOLID_STATUS_VERSION;
    change(*_status);
    _status->updatedTickMs = GetTickCount64();
    InterlockedIncrement(&_status->sequence);
}
