cmake_minimum_required(VERSION 3.20)
project(CamSolid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Runtime de C++ dentro de cada programa
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/W4 /utf-8)
include_directories(common)

# Cámara virtual: Windows carga esta DLL en su servicio Frame Server
add_library(CamSolidSource SHARED
    source/DllMain.cpp
    source/Activator.cpp
    source/MediaSource.cpp
    source/MediaStream.cpp
    source/TestPattern.cpp
    source/Nv12.cpp
    source/H264Decoder.cpp
    source/PhoneReceiver.cpp
    source/StatusPublisher.cpp
    source/CamSolidSource.def
)
target_link_libraries(CamSolidSource PRIVATE mfplat mfuuid mfsensorgroup ole32 ws2_32)

# App que enciende y apaga la cámara virtual
add_executable(CamSolid WIN32 app/main.cpp app/Adb.cpp)
target_link_libraries(CamSolid PRIVATE mfplat mfsensorgroup ole32)

# Herramienta que comprueba que la cámara da imagen a través de Windows
add_executable(CamSolidTest tools/probe.cpp tools/Bitmap.cpp)
target_link_libraries(CamSolidTest PRIVATE mf mfplat mfreadwrite mfuuid ole32)

# Herramientas de desarrollo
# Carga la DLL directamente, sin registrarla ni pasar por Frame Server
add_executable(CamSolidSelfTest tools/selftest.cpp tools/Bitmap.cpp)
target_link_libraries(CamSolidSelfTest PRIVATE mf mfplat mfreadwrite mfuuid ole32)

# Simula la app del celular para probar el PC sin teléfono
add_executable(CamSolidFakePhone tools/fakephone.cpp)
target_link_libraries(CamSolidFakePhone PRIVATE mfplat mfuuid ole32 oleaut32 ws2_32)
