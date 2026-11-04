/* Direct3D 8 interface method tables (d3d8.h vtable order): name + stdcall
 * argument count ('this' included).  com.c lists them in g_if after its D3D9
 * interfaces; runtime/d3d8.c turns each call into the D3D9 call on the same
 * object (WC3's renderer is D3D8). */
#ifndef D3D8_TABLES_H
#define D3D8_TABLES_H
#include "d3d9_tables.h"

static const Method M_D3D8[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"RegisterSoftwareDevice",2},
 {"GetAdapterCount",1},{"GetAdapterIdentifier",4},{"GetAdapterModeCount",2},
 {"EnumAdapterModes",4},{"GetAdapterDisplayMode",3},{"CheckDeviceType",6},
 {"CheckDeviceFormat",7},{"CheckDeviceMultiSampleType",6},{"CheckDepthStencilMatch",6},
 {"GetDeviceCaps",4},{"GetAdapterMonitor",2},{"CreateDevice",7},
};

static const Method M_DEVICE8[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"TestCooperativeLevel",1},
 {"GetAvailableTextureMem",1},{"ResourceManagerDiscardBytes",2},{"GetDirect3D",2},
 {"GetDeviceCaps",2},{"GetDisplayMode",2},{"GetCreationParameters",2},
 {"SetCursorProperties",4},{"SetCursorPosition",4},{"ShowCursor",2},
 {"CreateAdditionalSwapChain",3},{"Reset",2},{"Present",5},{"GetBackBuffer",4},
 {"GetRasterStatus",2},{"SetGammaRamp",3},{"GetGammaRamp",2},
 {"CreateTexture",8},{"CreateVolumeTexture",9},{"CreateCubeTexture",7},
 {"CreateVertexBuffer",6},{"CreateIndexBuffer",6},{"CreateRenderTarget",7},
 {"CreateDepthStencilSurface",6},{"CreateImageSurface",5},{"CopyRects",6},
 {"UpdateTexture",3},{"GetFrontBuffer",2},{"SetRenderTarget",3},
 {"GetRenderTarget",2},{"GetDepthStencilSurface",2},
 {"BeginScene",1},{"EndScene",1},{"Clear",7},{"SetTransform",3},{"GetTransform",3},
 {"MultiplyTransform",3},{"SetViewport",2},{"GetViewport",2},{"SetMaterial",2},
 {"GetMaterial",2},{"SetLight",3},{"GetLight",3},{"LightEnable",3},{"GetLightEnable",3},
 {"SetClipPlane",3},{"GetClipPlane",3},{"SetRenderState",3},{"GetRenderState",3},
 {"BeginStateBlock",1},{"EndStateBlock",2},{"ApplyStateBlock",2},{"CaptureStateBlock",2},
 {"DeleteStateBlock",2},{"CreateStateBlock",3},
 {"SetClipStatus",2},{"GetClipStatus",2},{"GetTexture",3},{"SetTexture",3},
 {"GetTextureStageState",4},{"SetTextureStageState",4},{"ValidateDevice",2},
 {"GetInfo",4},{"SetPaletteEntries",3},{"GetPaletteEntries",3},
 {"SetCurrentTexturePalette",2},{"GetCurrentTexturePalette",2},
 {"DrawPrimitive",4},{"DrawIndexedPrimitive",6},{"DrawPrimitiveUP",5},
 {"DrawIndexedPrimitiveUP",9},{"ProcessVertices",6},
 {"CreateVertexShader",5},{"SetVertexShader",2},{"GetVertexShader",2},{"DeleteVertexShader",2},
 {"SetVertexShaderConstant",4},{"GetVertexShaderConstant",4},
 {"GetVertexShaderDeclaration",4},{"GetVertexShaderFunction",4},
 {"SetStreamSource",4},{"GetStreamSource",4},{"SetIndices",3},{"GetIndices",3},
 {"CreatePixelShader",3},{"SetPixelShader",2},{"GetPixelShader",2},{"DeletePixelShader",2},
 {"SetPixelShaderConstant",4},{"GetPixelShaderConstant",4},{"GetPixelShaderFunction",4},
 {"DrawRectPatch",4},{"DrawTriPatch",4},{"DeletePatch",2},
};

/* IDirect3DResource8 is IDirect3DResource9's head (RES_HEAD) */
static const Method M_TEXTURE8[] = {
 RES_HEAD, {"SetLOD",2},{"GetLOD",1},{"GetLevelCount",1},
 {"GetLevelDesc",3},{"GetSurfaceLevel",3},{"LockRect",5},{"UnlockRect",2},{"AddDirtyRect",2},
};
static const Method M_CUBE8[] = {
 RES_HEAD, {"SetLOD",2},{"GetLOD",1},{"GetLevelCount",1},
 {"GetLevelDesc",3},{"GetCubeMapSurface",4},{"LockRect",6},{"UnlockRect",3},{"AddDirtyRect",3},
};
static const Method M_VBUF8[] = { RES_HEAD, {"Lock",5},{"Unlock",1},{"GetDesc",2} };
static const Method M_IBUF8[] = { RES_HEAD, {"Lock",5},{"Unlock",1},{"GetDesc",2} };
/* IDirect3DSurface8 is no resource: no priority, preload or type */
static const Method M_SURFACE8[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},
 {"SetPrivateData",5},{"GetPrivateData",4},{"FreePrivateData",2},
 {"GetContainer",3},{"GetDesc",2},{"LockRect",4},{"UnlockRect",1},
};
#endif
