#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include "Attributes.h"
#include "PhoneReceiver.h"

// El flujo de video de la cámara. Windows le pide cuadros con RequestSample y
// un hilo propio se los entrega: los del celular en cuanto llegan, o la imagen
// de prueba al ritmo de la cámara mientras no hay celular conectado.
class MediaStream final : public AttributesBase<IMFAttributes>, public IMFMediaStream2
{
public:
    static HRESULT Create(IMFMediaSource* source, ComPtr<MediaStream>& stream);

    // Llamadas desde MediaSource
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** result) override;
    STDMETHODIMP_(ULONG) AddRef() override { return AddReference(); }
    STDMETHODIMP_(ULONG) Release() override { return ReleaseReference(); }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

private:
    MediaStream() = default;
    ~MediaStream() override;

    HRESULT Initialize(IMFMediaSource* source);
    HRESULT GetEventQueue(ComPtr<IMFMediaEventQueue>& queue);
    void StopFrameLoop();
    void RunFrameLoop();
    HRESULT DeliverFrame(IUnknown* token, bool fromPhone);
    void OnPhoneFrame();

    std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _thread;
    bool _stopRequested = false;
    bool _phoneFrameReady = false;
    std::unique_ptr<PhoneReceiver> _receiver;

    ComPtr<IMFMediaSource> _source;
    ComPtr<IMFStreamDescriptor> _descriptor;
    ComPtr<IMFMediaEventQueue> _eventQueue;
    ComPtr<IMFMediaType> _type;
    ComPtr<IMFVideoSampleAllocatorEx> _allocator;
    std::deque<ComPtr<IUnknown>> _pendingRequests;

    MF_STREAM_STATE _state = MF_STREAM_STATE_STOPPED;
    UINT32 _width = 0;
    UINT32 _height = 0;
    UINT32 _frameRateNumerator = 30;
    UINT32 _frameRateDenominator = 1;
    UINT64 _frameNumber = 0;
};
