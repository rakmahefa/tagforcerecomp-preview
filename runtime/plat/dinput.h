/* dinput.h - DirectInput 8 compatibility shim (Linux): IDirectInput8W/IDirectInputDevice8W on
 * top of SDL2 joysticks (raw buttons/axes, which is what the generic-controller support wants).
 * Only the surface input_generic.cpp uses is implemented.
 */
#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include "windows.h"

typedef struct _GUID { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;
inline BOOL IsEqualGUID(const GUID &a, const GUID &b) { return memcmp(&a, &b, sizeof a) == 0; }
#define DIENUM_CONTINUE 1
#define DIENUM_STOP 0
#define DI8DEVCLASS_GAMECTRL 4
#define DIEDFL_ATTACHEDONLY 0
#define DISCL_NONEXCLUSIVE 1
#define DISCL_EXCLUSIVE 2
#define DISCL_FOREGROUND 1
#define DISCL_BACKGROUND 2
#define DIPH_DEVICE 0
#define DIPH_BYOFFSET 2
#define DIPROP_RANGE 1
#define DIDFT_AXIS 3
#define DIJOFS_X 0
#define DIJOFS_Y 4
#define DIJOFS_Z 8
#define DIJOFS_POV(n) (0x20 + (n) * 4)

typedef struct _DIJOYSTATE2 {
    LONG lX, lY, lZ, lRx, lRy, lRz, rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE rgbButtons[128];
    LONG lVX, lVY, lVZ, lVRx, lVRy, lVRz, rglVSlider[2], lAX, lAY, lAZ, lARx, lARy, lARz, rglASlider[2], lFX, lFY, lFZ, lFRx, lFRy, lFRz, rglFSlider[2];
} DIJOYSTATE2;

typedef struct _DIDEVICEINSTANCEW {
    DWORD dwSize, dwDevType;
    GUID guidInstance, guidProduct;
    DWORD dwDevType2, dwJoystickId;
    WCHAR tszInstanceName[260], tszProductName[260];
} DIDEVICEINSTANCEW, *LPDIDEVICEINSTANCEW;

typedef struct _DIDEVICEOBJECTINSTANCEW {
    DWORD dwSize, guidType_dummy, dwOfs, dwType, dwFlags;
    WCHAR tszName[260];
    DWORD dwFFMaxForce, dwFFForceResolution, dwFFSamplePeriod, dwEditType_dummy;
} DIDEVICEOBJECTINSTANCEW, *LPDIDEVICEOBJECTINSTANCEW;

typedef struct _DIPROPHEADER { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } DIPROPHEADER;
typedef struct _DIPROPRANGE { DIPROPHEADER diph; LONG lMin, lMax; } DIPROPRANGE;
typedef struct _DIPROPDWORD { DIPROPHEADER diph; DWORD dwData; } DIPROPDWORD;

typedef BOOL (CALLBACK *LPDIENUMDEVICESCALLBACKW)(const DIDEVICEINSTANCEW *, void *);
extern const int c_dfDIJoystick2;   /* opaque token passed to SetDataFormat */

class IDirectInputDevice8W {
public:
    virtual ~IDirectInputDevice8W() {}
    virtual HRESULT SetDataFormat(const void *) = 0;
    virtual HRESULT SetCooperativeLevel(HWND, DWORD) = 0;
    virtual HRESULT GetObjectInfo(DIDEVICEOBJECTINSTANCEW *, DWORD obj, DWORD how) = 0;
    virtual HRESULT SetProperty(DWORD prop, const DIPROPHEADER *) = 0;
    virtual HRESULT Acquire() = 0;
    virtual HRESULT Unacquire() = 0;
    virtual HRESULT Poll() = 0;
    virtual HRESULT GetDeviceState(DWORD size, void *state) = 0;
    virtual ULONG Release() = 0;
};
class IDirectInput8W {
public:
    virtual ~IDirectInput8W() {}
    virtual HRESULT CreateDevice(const GUID &inst, IDirectInputDevice8W **out, void *) = 0;
    virtual HRESULT EnumDevices(DWORD cls, LPDIENUMDEVICESCALLBACKW cb, void *ctx, DWORD flags) = 0;
    virtual ULONG Release() = 0;
};
extern const GUID IID_IDirectInput8W;   /* opaque token for DirectInput8Create */
HRESULT DirectInput8Create(HINSTANCE, DWORD, const GUID &, void **out, void *);

/*RawInput (used only to spot XInput devices so they are not listed as generic ones)*/
typedef struct { HANDLE hDevice; DWORD dwType; } RAWINPUTDEVICELIST;
typedef struct { DWORD cbSize; DWORD dwType; union { struct { DWORD dwVendorId, dwProductId, dwVersionNumber; } hid; }; } RID_DEVICE_INFO;
#define RIDI_DEVICENAME 0x2000000b
#define RIDI_DEVICEINFO 0x2000000c
#define RIM_TYPEHID 2
UINT GetRawInputDeviceList(RAWINPUTDEVICELIST *, UINT *count, UINT size);
UINT GetRawInputDeviceInfoW(HANDLE, UINT cmd, void *data, UINT *len);
