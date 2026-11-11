#include "MediaSource.h"

#include <new>
#include <mferror.h>
#include <ksmedia.h>

HRESULT MediaSource::Create(IMFAttributes* attributes, ComPtr<MediaSource>& source)
{
    ComPtr<MediaSource> result;
    result.Attach(new (std::nothrow) MediaSource());
    if (!result)
    {
        return E_OUTOFMEMORY;
    }
    RETURN_IF_FAILED(result->Initialize(attributes));
    source = result;
    return S_OK;
}

HRESULT MediaSource::Initialize(IMFAttributes* attributes)
{
    RETURN_IF_FAILED(CreateAttributes());
    if (attributes)
    {
        RETURN_IF_FAILED(attributes->CopyAllItems(_attributes.Get()));
    }
    RETURN_IF_FAILED(MFCreateEventQueue(&_eventQueue));

    // Perfil de cámara que piden algunas aplicaciones, como la app Cámara de Windows
    ComPtr<IMFSensorProfile> profile;
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))"));

    ComPtr<IMFSensorProfileCollection> profiles;
    RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profiles));
    RETURN_IF_FAILED(profiles->AddProfile(profile.Get()));
    RETURN_IF_FAILED(_attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get()));

    RETURN_IF_FAILED(MediaStream::Create(static_cast<IMFMediaSource*>(this), _stream));

    ComPtr<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(_stream->GetStreamDescriptor(&streamDescriptor));

    IMFStreamDescriptor* streamDescriptors[] = { streamDescriptor.Get() };
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, streamDescriptors, &_descriptor));
    RETURN_IF_FAILED(_descriptor->SelectStream(0));
    return S_OK;
}

HRESULT MediaSource::GetEventQueue(ComPtr<IMFMediaEventQueue>& queue)
{
    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }
    queue = _eventQueue;
    return S_OK;
}

// IUnknown

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** result)
{
    if (!result)
    {
        return E_POINTER;
    }
    *result = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
    {
        *result = static_cast<IMFMediaSourceEx*>(this);
    }
    else if (riid == __uuidof(IMFAttributes))
    {
        *result = static_cast<IMFAttributes*>(this);
    }
    else if (riid == __uuidof(IMFGetService))
    {
        *result = static_cast<IMFGetService*>(this);
    }
    else if (riid == __uuidof(IKsControl))
    {
        *result = static_cast<IKsControl*>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

// IMFMediaEventGenerator
// GetEvent puede bloquearse, así que ningún método mantiene el candado al usar la cola

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->QueueEventParamVar(type, extendedType, status, value);
}

// IMFMediaSource

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics)
{
    if (!characteristics)
    {
        return E_POINTER;
    }
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
{
    if (!descriptor)
    {
        return E_POINTER;
    }
    std::lock_guard lock(_mutex);
    if (!_descriptor)
    {
        return MF_E_SHUTDOWN;
    }
    return _descriptor->Clone(descriptor);
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition)
{
    if (!descriptor || !startPosition)
    {
        return E_POINTER;
    }
    if (timeFormat && *timeFormat != GUID_NULL)
    {
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(0, &selected, &streamDescriptor));

    if (selected)
    {
        ComPtr<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED(streamDescriptor->GetMediaTypeHandler(&handler));

        ComPtr<IMFMediaType> type;
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));

        MF_STREAM_STATE state;
        RETURN_IF_FAILED(_stream->GetStreamState(&state));

        RETURN_IF_FAILED(_descriptor->SelectStream(0));
        const MediaEventType streamEvent = state == MF_STREAM_STATE_STOPPED ? MENewStream : MEUpdatedStream;
        RETURN_IF_FAILED(_eventQueue->QueueEventParamUnk(streamEvent, GUID_NULL, S_OK, static_cast<IMFMediaStream*>(_stream.Get())));
        RETURN_IF_FAILED(_stream->Start(type.Get()));
    }
    else
    {
        RETURN_IF_FAILED(_descriptor->DeselectStream(0));
        RETURN_IF_FAILED(_stream->Stop());
    }

    const PROPVARIANT time = CurrentTimeVariant();
    return _eventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::Stop()
{
    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }
    RETURN_IF_FAILED(_stream->Stop());
    return _eventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Pause()
{
    // Una cámara en vivo no se puede pausar
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MediaSource::Shutdown()
{
    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }
    _stream->Shutdown();
    _stream.Reset();
    _descriptor.Reset();
    _eventQueue->Shutdown();
    _eventQueue.Reset();
    return S_OK;
}

// IMFMediaSourceEx

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes)
{
    return QueryInterface(IID_PPV_ARGS(attributes));
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes)
{
    if (streamId != 0)
    {
        return MF_E_NOT_FOUND;
    }
    std::lock_guard lock(_mutex);
    if (!_stream)
    {
        return MF_E_SHUTDOWN;
    }
    return _stream->QueryInterface(IID_PPV_ARGS(attributes));
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    // La imagen de prueba se dibuja en la memoria normal, sin usar la tarjeta gráfica
    return S_OK;
}

// IMFGetService

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID*)
{
    return MF_E_UNSUPPORTED_SERVICE;
}

// IKsControl: la cámara no ofrece controles como el zoom o el enfoque

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
