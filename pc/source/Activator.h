#pragma once

#include <mutex>
#include "Attributes.h"
#include "MediaSource.h"

// Lo primero que recibe Windows al cargar la DLL. Guarda la configuración de
// la cámara y crea el MediaSource cuando alguien va a usarla.
class Activator final : public AttributesBase<IMFActivate>
{
public:
    static HRESULT Create(REFIID riid, void** result);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** result) override;
    STDMETHODIMP_(ULONG) AddRef() override { return AddReference(); }
    STDMETHODIMP_(ULONG) Release() override { return ReleaseReference(); }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** result) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    Activator() = default;
    ~Activator() override = default;

    HRESULT Initialize();

    std::mutex _mutex;
    ComPtr<MediaSource> _source;
};
