#pragma once
#include <guiddef.h>


//identificador de la camara, asi windows lo usa para encontrar el DLL

//asi mismo no deberia cambiar una vez registrada
//{D0A8EF31-F742-4F7B-976C-D4028ACBDE5B}


inline constexpr
GUID CAM_SOLID_ID = { 0xd0a8ef31, 0xf742, 0x4f7b,{ 0x97, 0x6c, 0xd4, 0x02, 0x8a, 0xcb, 0xde, 0x5b } };



//Este mismo es el identificador en texto lo que pide MFCreateVirtualCamera

inline constexpr wchar_t CAMSOLID_SOURCE_ID[] = L"{D0A8EF31-F742-4F7B-976C-D4028ACBDE5B}";



//Nombre con el que aparece o deberia aparecer la cam en Windows y en las apps

inline constexpr wchar_t CAMSOLID_FRIENDLY_NAME[] = L"CamSolid";