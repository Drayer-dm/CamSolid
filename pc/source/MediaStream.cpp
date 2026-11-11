#include "MediaStream.h"

#include <chrono>
#include <new>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>
#include "TestPattern.h"

namespace
{
    struct VideoFormat
    {
        UINT32 width, height, frameRate;
    };

    // Formatos que ofrece la cámara. El primero es el predeterminado: 1080p a 30
    // cuadros por segundo, el máximo que dan muchos celulares sin perder latencia.
    constexpr VideoFormat VIDEO_FORMATS[] = {
        { 1920, 1080, 30 },
        { 1280, 720, 30 },
    };
    constexpr DWORD VIDEO_FORMAT_COUNT = ARRAYSIZE(VIDEO_FORMATS);
    constexpr DWORD STREAM_ID = 0;
    constexpr DWORD SAMPLE_POOL_SIZE = 10;

    HRESULT CreateVideoType(const VideoFormat& format, IMFMediaType** type)
    {
        const UINT32 frameSize = format.width * format.height * 3 / 2;

        ComPtr<IMFMediaType> result;
        RETURN_IF_FAILED(MFCreateMediaType(&result));
        RETURN_IF_FAILED(result->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(result->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        RETURN_IF_FAILED(MFSetAttributeSize(result.Get(), MF_MT_FRAME_SIZE, format.width, format.height));
        RETURN_IF_FAILED(MFSetAttributeRatio(result.Get(), MF_MT_FRAME_RATE, format.frameRate, 1));
        RETURN_IF_FAILED(MFSetAttributeRatio(result.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        RETURN_IF_FAILED(result->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED(result->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        RETURN_IF_FAILED(result->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
        RETURN_IF_FAILED(result->SetUINT32(MF_MT_DEFAULT_STRIDE, format.width));
        RETURN_IF_FAILED(result->SetUINT32(MF_MT_SAMPLE_SIZE, frameSize));
        RETURN_IF_FAILED(result->SetUINT32(MF_MT_AVG_BITRATE, frameSize * 8 * format.frameRate));

        *type = result.Detach();
        return S_OK;
    }

    // Windows lee estos datos tanto del flujo como de su descriptor
    HRESULT SetStreamInfo(IMFAttributes* attributes)
    {
        RETURN_IF_FAILED(attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, STREAM_ID));
        RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
        return S_OK;
    }
}

HRESULT MediaStream::Create(IMFMediaSource* source, ComPtr<MediaStream>& stream)
{
    ComPtr<MediaStream> result;
    result.Attach(new (std::nothrow) MediaStream());
    if (!result)
    {
        return E_OUTOFMEMORY;
    }
    RETURN_IF_FAILED(result->Initialize(source));
    stream = result;
    return S_OK;
}

MediaStream::~MediaStream()
{
    StopFrameLoop();
    _receiver.reset();
}

HRESULT MediaStream::Initialize(IMFMediaSource* source)
{
    _source = source;
    _receiver = std::make_unique<PhoneReceiver>([this] { OnPhoneFrame(); });
    RETURN_IF_FAILED(CreateAttributes());
    RETURN_IF_FAILED(MFCreateEventQueue(&_eventQueue));

    ComPtr<IMFMediaType> types[VIDEO_FORMAT_COUNT];
    IMFMediaType* typePointers[VIDEO_FORMAT_COUNT];
    for (DWORD i = 0; i < VIDEO_FORMAT_COUNT; i++)
    {
        RETURN_IF_FAILED(CreateVideoType(VIDEO_FORMATS[i], &types[i]));
        typePointers[i] = types[i].Get();
    }
    RETURN_IF_FAILED(MFCreateStreamDescriptor(STREAM_ID, VIDEO_FORMAT_COUNT, typePointers, &_descriptor));

    ComPtr<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0].Get()));

    RETURN_IF_FAILED(SetStreamInfo(_attributes.Get()));
    RETURN_IF_FAILED(SetStreamInfo(_descriptor.Get()));
    return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
    StopFrameLoop();

    UINT32 frameRate = 30;
    {
        std::lock_guard lock(_mutex);
        if (!_eventQueue)
        {
            return MF_E_SHUTDOWN;
        }
        if (type)
        {
            _type = type;
        }
        if (!_type)
        {
            return MF_E_INVALIDREQUEST;
        }

        RETURN_IF_FAILED(MFGetAttributeSize(_type.Get(), MF_MT_FRAME_SIZE, &_width, &_height));
        RETURN_IF_FAILED(MFGetAttributeRatio(_type.Get(), MF_MT_FRAME_RATE, &_frameRateNumerator, &_frameRateDenominator));
        frameRate = std::max(1u, _frameRateNumerator / std::max(1u, _frameRateDenominator));

        _allocator.Reset();
        RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&_allocator)));
        RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(SAMPLE_POOL_SIZE, _type.Get()));

        const PROPVARIANT time = CurrentTimeVariant();
        RETURN_IF_FAILED(_eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &time));

        _state = MF_STREAM_STATE_RUNNING;
        _stopRequested = false;
        _phoneFrameReady = false;
        _frameNumber = 0;
        _thread = std::thread(&MediaStream::RunFrameLoop, this);
    }

    // Fuera del candado: el receptor llama a OnPhoneFrame, que también lo usa
    _receiver->Start(_width, _height, frameRate);
    return S_OK;
}

void MediaStream::OnPhoneFrame()
{
    {
        std::lock_guard lock(_mutex);
        _phoneFrameReady = true;
    }
    _wake.notify_all();
}

HRESULT MediaStream::Stop()
{
    StopFrameLoop();
    _receiver->Stop();

    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }

    _pendingRequests.clear();
    if (_state == MF_STREAM_STATE_STOPPED)
    {
        return S_OK;
    }
    _state = MF_STREAM_STATE_STOPPED;
    return _eventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown()
{
    StopFrameLoop();
    _receiver->Stop();

    std::lock_guard lock(_mutex);
    if (_eventQueue)
    {
        _eventQueue->Shutdown();
        _eventQueue.Reset();
    }
    _pendingRequests.clear();
    _state = MF_STREAM_STATE_STOPPED;
    _allocator.Reset();
    _type.Reset();
    _descriptor.Reset();
    _source.Reset();
}

void MediaStream::StopFrameLoop()
{
    {
        std::lock_guard lock(_mutex);
        _stopRequested = true;
    }
    _wake.notify_all();
    if (_thread.joinable())
    {
        _thread.join();
    }
}

void MediaStream::RunFrameLoop()
{
    using Clock = std::chrono::steady_clock;

    const auto framePeriod = std::chrono::nanoseconds(1'000'000'000LL * _frameRateDenominator / _frameRateNumerator);
    auto nextFrame = Clock::now();

    for (;;)
    {
        ComPtr<IUnknown> token;
        bool hasRequest = false;
        bool fromPhone = false;
        {
            std::unique_lock lock(_mutex);
            // Despierta al llegar un cuadro del celular (si alguien lo espera) o al
            // tocar el siguiente cuadro de la imagen de prueba
            _wake.wait_until(lock, nextFrame, [this]
            {
                return _stopRequested || (_phoneFrameReady && !_pendingRequests.empty());
            });
            if (_stopRequested)
            {
                return;
            }

            fromPhone = _phoneFrameReady && !_pendingRequests.empty();
            if (!fromPhone && Clock::now() < nextFrame)
            {
                continue;
            }
            if (fromPhone)
            {
                _phoneFrameReady = false;
            }

            if (_state == MF_STREAM_STATE_RUNNING && !_pendingRequests.empty())
            {
                token = std::move(_pendingRequests.front());
                _pendingRequests.pop_front();
                hasRequest = true;
            }
        }

        if (!fromPhone)
        {
            nextFrame = std::max(nextFrame + framePeriod, Clock::now());
            _frameNumber++;

            // Mientras llega video del celular no se mezcla con la imagen de prueba
            if (hasRequest && _receiver->IsReceiving())
            {
                std::lock_guard lock(_mutex);
                _pendingRequests.push_front(token);
                continue;
            }
        }

        // Si nadie espera este cuadro se descarta: nunca se acumula video atrasado
        if (!hasRequest)
        {
            continue;
        }

        const HRESULT hr = DeliverFrame(token.Get(), fromPhone);
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
        {
            // Todos los búferes siguen en uso: la petición espera al siguiente cuadro
            std::lock_guard lock(_mutex);
            _pendingRequests.push_front(token);
        }
        else if (FAILED(hr))
        {
            _eventQueue->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
            return;
        }
    }
}

HRESULT MediaStream::DeliverFrame(IUnknown* token, bool fromPhone)
{
    ComPtr<IMFSample> sample;
    RETURN_IF_FAILED(_allocator->AllocateSample(&sample));

    ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

    ComPtr<IMF2DBuffer2> buffer2D;
    RETURN_IF_FAILED(buffer.As(&buffer2D));

    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    BYTE* bufferStart = nullptr;
    DWORD bufferLength = 0;
    RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &bufferStart, &bufferLength));
    int64_t captureTime = 0;
    int64_t decodedTime = 0;
    const bool copied = fromPhone && _receiver->CopyLatestFrame(scanline0, pitch, _width, _height, captureTime, decodedTime);
    if (!copied)
    {
        DrawTestPattern(scanline0, pitch, _width, _height, _frameNumber);
    }
    RETURN_IF_FAILED(buffer2D->Unlock2D());
    RETURN_IF_FAILED(buffer->SetCurrentLength(bufferLength));

    RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
    RETURN_IF_FAILED(sample->SetSampleDuration(10'000'000LL * _frameRateDenominator / _frameRateNumerator));
    if (token)
    {
        RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
    }
    RETURN_IF_FAILED(_eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get()));

    if (copied)
    {
        _receiver->ReportDelivered(captureTime, decodedTime);
    }
    return S_OK;
}

HRESULT MediaStream::GetEventQueue(ComPtr<IMFMediaEventQueue>& queue)
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

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** result)
{
    if (!result)
    {
        return E_POINTER;
    }
    *result = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2))
    {
        *result = static_cast<IMFMediaStream2*>(this);
    }
    else if (riid == __uuidof(IMFAttributes))
    {
        *result = static_cast<IMFAttributes*>(this);
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

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value)
{
    ComPtr<IMFMediaEventQueue> queue;
    RETURN_IF_FAILED(GetEventQueue(queue));
    return queue->QueueEventParamVar(type, extendedType, status, value);
}

// IMFMediaStream

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source)
{
    if (!source)
    {
        return E_POINTER;
    }
    std::lock_guard lock(_mutex);
    if (!_source)
    {
        return MF_E_SHUTDOWN;
    }
    return _source.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor)
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
    return _descriptor.CopyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token)
{
    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }
    if (_state != MF_STREAM_STATE_RUNNING)
    {
        return MF_E_MEDIA_SOURCE_WRONGSTATE;
    }
    _pendingRequests.emplace_back(token);
    // Si ya hay un cuadro del celular esperando, el hilo lo entrega de inmediato
    if (_phoneFrameReady)
    {
        _wake.notify_all();
    }
    return S_OK;
}

// IMFMediaStream2

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state)
{
    MF_STREAM_STATE current;
    RETURN_IF_FAILED(GetStreamState(&current));
    if (state == current)
    {
        return S_OK;
    }

    switch (state)
    {
    case MF_STREAM_STATE_RUNNING:
        return Start(nullptr);
    case MF_STREAM_STATE_STOPPED:
        return Stop();
    case MF_STREAM_STATE_PAUSED:
    {
        if (current != MF_STREAM_STATE_RUNNING)
        {
            return MF_E_INVALID_STATE_TRANSITION;
        }
        std::lock_guard lock(_mutex);
        _state = MF_STREAM_STATE_PAUSED;
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state)
{
    if (!state)
    {
        return E_POINTER;
    }
    std::lock_guard lock(_mutex);
    if (!_eventQueue)
    {
        return MF_E_SHUTDOWN;
    }
    *state = _state;
    return S_OK;
}
