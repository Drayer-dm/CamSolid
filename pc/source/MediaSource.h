#pragma once

#include <mutex>
#include "Attributes.h"
#include "MediaStream.h"
#include <ks.h>
#include <ksproxy.h>

// La cámara tal como la ve Windows: anuncia su flujo de video y responde a las
// órdenes de encender y apagar.
class MediaSource final : public AttributesBase<IMFAttributes>, public IMFMediaSourceEx, public IMFGetService, public IKsControl
{
public:
    static HRESULT Create(IMFAttributes* attributes, ComPtr<MediaSource>& source);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** result) override;
    STDMETHODIMP_(ULONG) AddRef() override { return AddReference(); }
    STDMETHODIMP_(ULONG) Release() override { return ReleaseReference(); }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* result) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength, ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength, ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength, ULONG* bytesReturned) override;

private:
    MediaSource() = default;
    ~MediaSource() override = default;

    HRESULT Initialize(IMFAttributes* attributes);
    HRESULT GetEventQueue(ComPtr<IMFMediaEventQueue>& queue);

    std::mutex _mutex;
    ComPtr<IMFMediaEventQueue> _eventQueue;
    ComPtr<IMFPresentationDescriptor> _descriptor;
    ComPtr<MediaStream> _stream;
};
