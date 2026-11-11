#include "Activator.h"

#include <new>
#include <mftransform.h>
#include <mfvirtualcamera.h>
#include "CamSolidIds.h"

HRESULT Activator::Create(REFIID riid, void** result)
{
    ComPtr<Activator> activator;
    activator.Attach(new (std::nothrow) Activator());
    if (!activator)
    {
        return E_OUTOFMEMORY;
    }
    RETURN_IF_FAILED(activator->Initialize());
    return activator->QueryInterface(riid, result);
}

HRESULT Activator::Initialize()
{
    RETURN_IF_FAILED(CreateAttributes());
    RETURN_IF_FAILED(_attributes->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    RETURN_IF_FAILED(_attributes->SetGUID(MFT_TRANSFORM_CLSID_Attribute, CAM_SOLID_ID));
    return S_OK;
}

// IUnknown

STDMETHODIMP Activator::QueryInterface(REFIID riid, void** result)
{
    if (!result)
    {
        return E_POINTER;
    }
    *result = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate))
    {
        *result = static_cast<IMFActivate*>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

// IMFActivate

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** result)
{
    std::lock_guard lock(_mutex);
    if (!_source)
    {
        RETURN_IF_FAILED(MediaSource::Create(_attributes.Get(), _source));
    }
    return _source->QueryInterface(riid, result);
}

STDMETHODIMP Activator::ShutdownObject()
{
    std::lock_guard lock(_mutex);
    if (_source)
    {
        _source->Shutdown();
        _source.Reset();
    }
    return S_OK;
}

STDMETHODIMP Activator::DetachObject()
{
    std::lock_guard lock(_mutex);
    _source.Reset();
    return S_OK;
}
