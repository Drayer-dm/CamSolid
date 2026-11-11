#include "H264Decoder.h"

#include <cstring>
#include <mferror.h>

HRESULT H264Decoder::Initialize()
{
    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_NV12 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    RETURN_IF_FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activates, &count));

    HRESULT hr = count > 0 ? activates[0]->ActivateObject(IID_PPV_ARGS(&_decoder)) : MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
    {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    RETURN_IF_FAILED(hr);

    // Sin esto el decodificador retiene varios cuadros antes de entregar el primero
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(_decoder->GetAttributes(&attributes)) && attributes)
    {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    ComPtr<IMFMediaType> inputType;
    RETURN_IF_FAILED(MFCreateMediaType(&inputType));
    RETURN_IF_FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    RETURN_IF_FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(_decoder->SetInputType(0, inputType.Get(), 0));
    RETURN_IF_FAILED(ConfigureOutput());

    RETURN_IF_FAILED(_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    RETURN_IF_FAILED(_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    return S_OK;
}

// Elige la salida NV12 y anota su tamaño. Se repite cada vez que el video
// cambia de resolución.
HRESULT H264Decoder::ConfigureOutput()
{
    for (DWORD i = 0;; i++)
    {
        ComPtr<IMFMediaType> type;
        RETURN_IF_FAILED(_decoder->GetOutputAvailableType(0, i, &type));

        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12)
        {
            continue;
        }
        RETURN_IF_FAILED(_decoder->SetOutputType(0, type.Get(), 0));

        UINT32 frameWidth = 0;
        UINT32 frameHeight = 0;
        RETURN_IF_FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &frameWidth, &frameHeight));
        _width = frameWidth;
        _height = frameHeight;
        _planeHeight = frameHeight;
        _stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, frameWidth));

        // El decodificador redondea el alto a múltiplos de 16 (1080 pasa a 1088);
        // la zona visible indica el tamaño real de la imagen
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0)
        {
            _width = static_cast<UINT32>(area.Area.cx);
            _height = static_cast<UINT32>(area.Area.cy);
        }

        return _decoder->GetOutputStreamInfo(0, &_outputInfo);
    }
}

HRESULT H264Decoder::Decode(const BYTE* data, DWORD size, Nv12Frame& frame)
{
    ComPtr<IMFMediaBuffer> inputBuffer;
    RETURN_IF_FAILED(MFCreateMemoryBuffer(size, &inputBuffer));

    BYTE* inputData = nullptr;
    RETURN_IF_FAILED(inputBuffer->Lock(&inputData, nullptr, nullptr));
    memcpy(inputData, data, size);
    inputBuffer->Unlock();
    RETURN_IF_FAILED(inputBuffer->SetCurrentLength(size));

    ComPtr<IMFSample> inputSample;
    RETURN_IF_FAILED(MFCreateSample(&inputSample));
    RETURN_IF_FAILED(inputSample->AddBuffer(inputBuffer.Get()));
    RETURN_IF_FAILED(inputSample->SetSampleTime(_sampleTime));
    _sampleTime += 333'333;

    RETURN_IF_FAILED(_decoder->ProcessInput(0, inputSample.Get(), 0));

    // Se sacan todos los cuadros listos; si hay varios, se queda el más reciente
    bool gotFrame = false;
    for (;;)
    {
        const bool decoderAllocates = (_outputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        ComPtr<IMFSample> ownSample;
        if (!decoderAllocates)
        {
            ComPtr<IMFMediaBuffer> outputBuffer;
            RETURN_IF_FAILED(MFCreateMemoryBuffer(_outputInfo.cbSize, &outputBuffer));
            RETURN_IF_FAILED(MFCreateSample(&ownSample));
            RETURN_IF_FAILED(ownSample->AddBuffer(outputBuffer.Get()));
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = ownSample.Get();
        DWORD status = 0;
        const HRESULT hr = _decoder->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents)
        {
            output.pEvents->Release();
        }

        ComPtr<IMFSample> outputSample;
        if (decoderAllocates)
        {
            outputSample.Attach(output.pSample);
        }
        else
        {
            outputSample = ownSample;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            RETURN_IF_FAILED(ConfigureOutput());
            continue;
        }
        RETURN_IF_FAILED(hr);

        RETURN_IF_FAILED(CopyOutput(outputSample.Get(), frame));
        gotFrame = true;
    }
    return gotFrame ? S_OK : S_FALSE;
}

HRESULT H264Decoder::CopyOutput(IMFSample* sample, Nv12Frame& frame)
{
    ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->ConvertToContiguousBuffer(&buffer));

    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> buffer2D;
    const bool locked2D = SUCCEEDED(buffer.As(&buffer2D)) && SUCCEEDED(buffer2D->Lock2D(&scanline0, &pitch));
    if (!locked2D)
    {
        RETURN_IF_FAILED(buffer->Lock(&scanline0, nullptr, nullptr));
        pitch = _stride;
    }

    frame.width = _width;
    frame.height = _height;
    frame.data.resize(static_cast<size_t>(_width) * _height * 3 / 2);

    BYTE* destinationY = frame.data.data();
    BYTE* destinationUv = destinationY + static_cast<size_t>(_width) * _height;
    const BYTE* sourceUv = scanline0 + static_cast<size_t>(pitch) * _planeHeight;

    for (UINT32 y = 0; y < _height; y++)
    {
        memcpy(destinationY + static_cast<size_t>(_width) * y, scanline0 + static_cast<size_t>(pitch) * y, _width);
    }
    for (UINT32 y = 0; y < _height / 2; y++)
    {
        memcpy(destinationUv + static_cast<size_t>(_width) * y, sourceUv + static_cast<size_t>(pitch) * y, _width);
    }

    if (locked2D)
    {
        buffer2D->Unlock2D();
    }
    else
    {
        buffer->Unlock();
    }
    return S_OK;
}
