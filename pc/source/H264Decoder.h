#pragma once

#include <mftransform.h>
#include "Attributes.h"
#include "Nv12.h"

// Decodifica video H.264 a NV12 con el decodificador de Media Foundation, en
// modo de baja latencia: cada cuadro sale en cuanto entra, sin esperar a otros.
class H264Decoder
{
public:
    HRESULT Initialize();

    // Decodifica un cuadro en formato Annex B. Devuelve S_OK y deja la imagen en
    // "frame" si salió un cuadro, o S_FALSE si el decodificador necesita más datos.
    HRESULT Decode(const BYTE* data, DWORD size, Nv12Frame& frame);

private:
    HRESULT ConfigureOutput();
    HRESULT CopyOutput(IMFSample* sample, Nv12Frame& frame);

    ComPtr<IMFTransform> _decoder;
    MFT_OUTPUT_STREAM_INFO _outputInfo{};
    UINT32 _width = 0;
    UINT32 _height = 0;
    UINT32 _planeHeight = 0;
    LONG _stride = 0;
    LONGLONG _sampleTime = 0;
};
