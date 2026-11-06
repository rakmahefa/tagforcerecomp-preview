/* shim_xinput.cpp - XInput and DirectInput 8 on top of SDL2 (see dinput.h).
 *
 * XInputGetState maps the first four SDL game controllers onto the XInput layout (the runtime
 * loads "xinput1_4.dll" dynamically and resolves XInputGetState from it: shim_kern hands out a
 * fake module and GetProcAddress lands in tf_shim_proc below).
 *
 * DirectInput8 enumerates the same SDL joysticks as raw devices (buttons 1..N and axes), which
 * is exactly what the generic-controller support of input_generic.cpp wants; gamepads that SDL
 * maps as XInput are still listed (the RawInput XInput filter of the Windows path reports none
 * on Linux, so they show up as generic too - harmless duplication the remapping UI tolerates).
 */
#include "windows.h"
#include "dinput.h"
#include <SDL.h>
#include <string>
#include <vector>
#include <cstring>

/* ------------------------------------------------------------------ fake modules of the shim itself */
void *const tf_fake_xinput = (void *)0x11;
void *const tf_fake_uxtheme = (void *)0x12;
void *const tf_fake_avrt = (void *)0x13;

/*uxtheme stubs: everything is already dark in this port*/
static BOOL tf_ux_allow_dark(HWND, BOOL) { return TRUE; }
static int tf_ux_set_app_mode(int) { return 0; }
static void tf_ux_flush(void) {}
static HRESULT tf_ux_set_window_theme(HWND, LPCWSTR, LPCWSTR) { return S_OK; }
/*avrt stubs (pro audio priority: pthreads already keep the scheduler happy enough)*/
static HANDLE tf_av_set(LPCWSTR, DWORD *) { return (HANDLE)1; }
static BOOL tf_av_revert(HANDLE) { return TRUE; }

BOOL K32GetProcessMemoryInfo(HANDLE, void *mc, DWORD);   /* shim_kern.cpp (psapi.h types) */
/*K32GetProcessMemoryInfo: implemented in shim_kern.cpp over getrusage (session log)*/
extern "C" BOOL tf_k32_get_process_memory_info(HANDLE, void *, DWORD);
static BOOL k32_gpmi_wrap(HANDLE h, void *mc, DWORD n) { return tf_k32_get_process_memory_info(h, mc, n); }

extern "C" void *tf_shim_proc(void *module, const char *name) {
    if (module == tf_fake_xinput) {
        if (!strcmp(name, "XInputGetState")) return (void *)XInputGetState;
        if (!strcmp(name, "XInputGetCapabilities")) return (void *)XInputGetCapabilities;
        if (!strcmp(name, "XInputEnable")) return (void *)XInputEnable;
        if (!strcmp(name, "XInputSetState")) return (void *)XInputSetState;
        return nullptr;
    }
    if (module == tf_fake_uxtheme) {
        /*the runtime resolves the undocumented ordinal exports by number (MAKEINTRESOURCEA)*/
        if ((uintptr_t)name < 0x10000) {
            UINT ord = (UINT)(uintptr_t)name;
            if (ord == 133) return (void *)&tf_ux_allow_dark;         /* AllowDarkModeForWindow */
            if (ord == 135) return (void *)&tf_ux_set_app_mode;       /* SetPreferredAppMode */
            if (ord == 136) return (void *)&tf_ux_flush;              /* FlushMenuThemes */
            return nullptr;
        }
        if (!strcmp(name, "SetWindowTheme")) return (void *)&tf_ux_set_window_theme;
        return nullptr;
    }
    if ((void *)module == (void *)0x14) {   /* fake kernel32: K32GetProcessMemoryInfo (resolved by diag.cpp) */
        if (!strcmp(name, "K32GetProcessMemoryInfo")) return (void *)&k32_gpmi_wrap;
        return nullptr;
    }
    if (module == tf_fake_avrt) {
        if (!strcmp(name, "AvSetMmThreadCharacteristicsW")) return (void *)&tf_av_set;
        if (!strcmp(name, "AvRevertMmThreadCharacteristics")) return (void *)&tf_av_revert;
        return nullptr;
    }
    return nullptr;
}

/* ------------------------------------------------------------------ SDL game controller bootstrap */
static bool g_gc_up = false;
static void gc_init(void) {
    if (g_gc_up) return;
    SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    SDL_GameControllerEventState(SDL_IGNORE);   /* polled, not pumped through the event queue*/
    g_gc_up = true;
}
static SDL_GameController *g_pads[4] = {};
static void gc_open_all(void) {
    gc_init();
    int n = SDL_NumJoysticks();
    int slot = 0;
    for (int i = 0; i < n && slot < 4; i++)
        if (SDL_IsGameController(i)) {
            if (!g_pads[slot]) g_pads[slot] = SDL_GameControllerOpen(i);
            slot++;
        }
}
static SDL_GameController *gc_get(DWORD idx) {
    if (idx >= 4) return nullptr;
    if (!g_pads[idx]) gc_open_all();
    return g_pads[idx];
}

/* ------------------------------------------------------------------ XInput */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state) {
    if (!state) return ERROR_DEVICE_NOT_CONNECTED;
    memset(state, 0, sizeof *state);
    SDL_GameController *c = gc_get(idx);
    if (!c || !SDL_GameControllerGetAttached(c)) return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber++;
    XINPUT_GAMEPAD &g = state->Gamepad;
    auto b = [&](SDL_GameControllerButton i) { return SDL_GameControllerGetButton(c, i) != 0; };
    if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) g.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) g.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) g.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) g.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (b(SDL_CONTROLLER_BUTTON_START)) g.wButtons |= XINPUT_GAMEPAD_START;
    if (b(SDL_CONTROLLER_BUTTON_BACK)) g.wButtons |= XINPUT_GAMEPAD_BACK;
    if (b(SDL_CONTROLLER_BUTTON_LEFTSTICK)) g.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (b(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) g.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) g.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) g.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (b(SDL_CONTROLLER_BUTTON_A)) g.wButtons |= XINPUT_GAMEPAD_A;
    if (b(SDL_CONTROLLER_BUTTON_B)) g.wButtons |= XINPUT_GAMEPAD_B;
    if (b(SDL_CONTROLLER_BUTTON_X)) g.wButtons |= XINPUT_GAMEPAD_X;
    if (b(SDL_CONTROLLER_BUTTON_Y)) g.wButtons |= XINPUT_GAMEPAD_Y;
    g.bLeftTrigger = (BYTE)SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128;
    g.bRightTrigger = (BYTE)SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128;
    g.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    g.sThumbLY = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    g.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    g.sThumbRY = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    return ERROR_SUCCESS;
}
DWORD XInputGetCapabilities(DWORD idx, DWORD, XINPUT_CAPABILITIES *caps) {
    if (!caps) return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0, sizeof *caps);
    return gc_get(idx) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
void XInputEnable(BOOL) {}
DWORD XInputSetState(DWORD, void *) { return ERROR_DEVICE_NOT_CONNECTED; }

/* ------------------------------------------------------------------ DirectInput (SDL joysticks as generic pads) */
const int c_dfDIJoystick2 = 0;   /* SetDataFormat's opaque token */
const GUID IID_IDirectInput8W = {0x5944e682, 0xc92e, 0x446b, {0xbf, 0x01, 0x67, 0x2f, 0x0e, 0x0a, 0x10, 0x5f}};

struct DiDevice : IDirectInputDevice8W {
    SDL_Joystick *joy = nullptr;
    int idx = 0;
    ~DiDevice() override { if (joy) SDL_JoystickClose(joy); }
    HRESULT SetDataFormat(const void *) override { return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) override { return S_OK; }
    HRESULT GetObjectInfo(DIDEVICEOBJECTINSTANCEW *info, DWORD obj, DWORD how) override {
        if (!info || how != DIPH_BYOFFSET) return E_INVALIDARG;
        if (!joy) joy = SDL_JoystickOpen(idx);
        if (!joy) return E_FAIL;
        if (obj == DIJOFS_X) return SDL_JoystickNumAxes(joy) > 0 ? S_OK : E_FAIL;
        if (obj == DIJOFS_Y) return SDL_JoystickNumAxes(joy) > 1 ? S_OK : E_FAIL;
        if (obj >= DIJOFS_POV(0) && obj <= DIJOFS_POV(3)) return SDL_JoystickNumHats(joy) > 0 ? S_OK : E_FAIL;
        return E_FAIL;
    }
    HRESULT SetProperty(DWORD, const DIPROPHEADER *) override { return S_OK; }
    HRESULT Acquire() override {
        if (!joy) joy = SDL_JoystickOpen(idx);
        return joy ? S_OK : E_FAIL;
    }
    HRESULT Unacquire() override { return S_OK; }
    HRESULT Poll() override { return S_OK; }
    HRESULT GetDeviceState(DWORD size, void *out) override {
        if (!out || size < sizeof(DIJOYSTATE2) || !joy) return E_FAIL;
        SDL_JoystickUpdate();
        DIJOYSTATE2 *st = (DIJOYSTATE2 *)out;
        memset(st, 0, sizeof *st);
        int nax = SDL_JoystickNumAxes(joy);
        st->lX = nax > 0 ? SDL_JoystickGetAxis(joy, 0) + 32768 : 32768;
        st->lY = nax > 1 ? SDL_JoystickGetAxis(joy, 1) + 32768 : 32768;
        if (SDL_JoystickNumHats(joy) > 0) {
            const Uint8 h = SDL_JoystickGetHat(joy, 0);
            if (h & SDL_HAT_UP) st->rgdwPOV[0] = 0;
            else if (h & SDL_HAT_RIGHTUP) st->rgdwPOV[0] = 4500;
            else if (h & SDL_HAT_RIGHT) st->rgdwPOV[0] = 9000;
            else if (h & SDL_HAT_RIGHTDOWN) st->rgdwPOV[0] = 13500;
            else if (h & SDL_HAT_DOWN) st->rgdwPOV[0] = 18000;
            else if (h & SDL_HAT_LEFTDOWN) st->rgdwPOV[0] = 22500;
            else if (h & SDL_HAT_LEFT) st->rgdwPOV[0] = 27000;
            else if (h & SDL_HAT_LEFTUP) st->rgdwPOV[0] = 31500;
            else st->rgdwPOV[0] = 0xFFFF;
        } else st->rgdwPOV[0] = 0xFFFF;
        int nbtn = std::min(SDL_JoystickNumButtons(joy), 128);
        for (int i = 0; i < nbtn; i++) st->rgbButtons[i] = SDL_JoystickGetButton(joy, i) ? 0x80 : 0;
        return S_OK;
    }
    ULONG Release() override { delete this; return 0; }
};

struct DiInput : IDirectInput8W {
    HRESULT CreateDevice(const GUID &inst, IDirectInputDevice8W **out, void *) override {
        if (!out) return E_INVALIDARG;
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
            GUID w{};
            memcpy(&w, &g, std::min(sizeof w, sizeof g));
            if (IsEqualGUID(w, inst)) {
                auto *d = new DiDevice();
                d->idx = i;
                *out = d;
                return S_OK;
            }
        }
        return E_FAIL;
    }
    HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKW cb, void *ctx, DWORD) override {
        gc_init();
        SDL_JoystickUpdate();
        int n = SDL_NumJoysticks();
        for (int i = 0; i < n; i++) {
            DIDEVICEINSTANCEW inst{};
            inst.dwSize = sizeof inst;
            SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
            memcpy(&inst.guidInstance, &g, std::min(sizeof(GUID), sizeof g));
            inst.guidInstance.Data1 ^= (DWORD)i;             /*unique per device*/
            memcpy(&inst.guidProduct, &inst.guidInstance, sizeof(GUID));
            const char *nm = SDL_JoystickNameForIndex(i);
            std::wstring w;
            for (const char *p = nm ? nm : ""; *p; p++) w += (wchar_t)(unsigned char)*p;
            wcsncpy(inst.tszInstanceName, w.c_str(), 259);
            wcsncpy(inst.tszProductName, w.c_str(), 259);
            if (cb(&inst, ctx) == DIENUM_STOP) break;
        }
        return S_OK;
    }
    ULONG Release() override { delete this; return 0; }
};

HRESULT DirectInput8Create(HINSTANCE, DWORD, const GUID &, void **out, void *) {
    if (!out) return E_INVALIDARG;
    gc_init();
    *out = new DiInput();
    return S_OK;
}

/*RawInput: no XInput filter on Linux (every SDL joystick is enumerable as generic)*/
UINT GetRawInputDeviceList(RAWINPUTDEVICELIST *, UINT *count, UINT) { if (count) *count = 0; return 0; }
UINT GetRawInputDeviceInfoW(HANDLE, UINT, void *, UINT *) { return (UINT)-1; }

/*CoInitialize (input_generic's worker thread calls it; COM does not exist here)*/
HRESULT CoInitializeEx(LPVOID, DWORD) { return S_OK; }
void CoUninitialize(void) {}
