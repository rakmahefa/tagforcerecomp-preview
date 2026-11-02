#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include "input_bindings.h"

namespace bindings {
namespace {
struct Device { GUID id; IDirectInputDevice8W *input; bool seen, has_x, has_y, has_pov; };
struct Controllers {
    IDirectInput8W *input=nullptr;
    HWND window=nullptr;
    ULONGLONG next_scan=0;
    std::vector<Device> devices;
    std::vector<DWORD> xinput_products;
    ~Controllers() { for(auto &d:devices) { d.input->Unacquire(); d.input->Release(); } if(input) input->Release(); }
    void find_xinput() {
        xinput_products.clear(); UINT count=0;
        if(GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))!=0) return;
        std::vector<RAWINPUTDEVICELIST> raw(count);
        UINT n=GetRawInputDeviceList(raw.data(),&count,sizeof(RAWINPUTDEVICELIST));
        if(n==(UINT)-1) return;
        for(UINT i=0;i<n;i++) {
            UINT len=0;
            if(GetRawInputDeviceInfoW(raw[i].hDevice,RIDI_DEVICENAME,nullptr,&len)==(UINT)-1 || !len) continue;
            std::wstring name(len,L'\0');
            if(GetRawInputDeviceInfoW(raw[i].hDevice,RIDI_DEVICENAME,&name[0],&len)==(UINT)-1) continue;
            std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return (wchar_t)towupper(c);});
            if(name.find(L"IG_")==std::wstring::npos) continue;
            RID_DEVICE_INFO info={}; info.cbSize=sizeof(info); UINT size=sizeof(info);
            if(GetRawInputDeviceInfoW(raw[i].hDevice,RIDI_DEVICEINFO,&info,&size)!=(UINT)-1 && info.dwType==RIM_TYPEHID)
                xinput_products.push_back(MAKELONG(info.hid.dwVendorId,info.hid.dwProductId));
        }
    }
    static BOOL CALLBACK enumerate(const DIDEVICEINSTANCEW *instance,void *context) {
        auto &self=*(Controllers *)context;
        if(std::find(self.xinput_products.begin(),self.xinput_products.end(),instance->guidProduct.Data1)!=self.xinput_products.end()) return DIENUM_CONTINUE;
        for(auto &d:self.devices) if(IsEqualGUID(d.id,instance->guidInstance)) { d.seen=true; return DIENUM_CONTINUE; }
        IDirectInputDevice8W *device=nullptr;
        if(FAILED(self.input->CreateDevice(instance->guidInstance,&device,nullptr))) return DIENUM_CONTINUE;
        if(FAILED(device->SetDataFormat(&c_dfDIJoystick2)) || FAILED(device->SetCooperativeLevel(self.window,DISCL_NONEXCLUSIVE|DISCL_BACKGROUND))) { device->Release(); return DIENUM_CONTINUE; }
        DIDEVICEOBJECTINSTANCEW axis={}; axis.dwSize=sizeof(axis);
        bool has_x=SUCCEEDED(device->GetObjectInfo(&axis,DIJOFS_X,DIPH_BYOFFSET));
        bool has_y=SUCCEEDED(device->GetObjectInfo(&axis,DIJOFS_Y,DIPH_BYOFFSET));
        bool has_pov=SUCCEEDED(device->GetObjectInfo(&axis,DIJOFS_POV(0),DIPH_BYOFFSET));
        DIPROPRANGE range={}; range.diph.dwSize=sizeof(range); range.diph.dwHeaderSize=sizeof(range.diph);
        range.diph.dwHow=DIPH_BYOFFSET; range.lMin=0; range.lMax=65535;
        range.diph.dwObj=DIJOFS_X;
        if(has_x && FAILED(device->SetProperty(DIPROP_RANGE,&range.diph))) has_x=false;
        range.diph.dwObj=DIJOFS_Y;
        if(has_y && FAILED(device->SetProperty(DIPROP_RANGE,&range.diph))) has_y=false;
        device->Acquire();
        self.devices.push_back({instance->guidInstance,device,true,has_x,has_y,has_pov});
        return DIENUM_CONTINUE;
    }
    GenericState poll(HWND owner) {
        GenericState result;
        if(!owner) return result;
        if(!input) {
            if(FAILED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,(void **)&input,nullptr))) return result;
            window=owner;
        }
        if(GetTickCount64()>=next_scan) {
            next_scan=GetTickCount64()+2000; find_xinput();
            for(auto &d:devices) d.seen=false;
            if(SUCCEEDED(input->EnumDevices(DI8DEVCLASS_GAMECTRL,enumerate,this,DIEDFL_ATTACHEDONLY))) {
                devices.erase(std::remove_if(devices.begin(),devices.end(),[](Device &d){ if(d.seen) return false; d.input->Unacquire(); d.input->Release(); return true; }),devices.end());
            }
        }
        for(auto &d:devices) {
            d.input->Poll(); DIJOYSTATE2 state={};
            HRESULT hr=d.input->GetDeviceState(sizeof(state),&state);
            if(FAILED(hr)) { if(FAILED(d.input->Acquire())) continue; d.input->Poll(); if(FAILED(d.input->GetDeviceState(sizeof(state),&state))) continue; }
            result.connected=true;
            for(int i=0;i<128;i++) result.buttons[i] |= (state.rgbButtons[i]&0x80)!=0;
            if(d.has_x && state.lX<16384) result.directions|=4;
            if(d.has_x && state.lX>49151) result.directions|=8;
            if(d.has_y && state.lY<16384) result.directions|=1;
            if(d.has_y && state.lY>49151) result.directions|=2;
            DWORD pov=state.rgdwPOV[0];
            if(d.has_pov && LOWORD(pov)!=0xffff && pov<36000) {
                if(pov>=31500 || pov<=4500) result.directions|=1;
                if(pov>=4500 && pov<=13500) result.directions|=8;
                if(pov>=13500 && pov<=22500) result.directions|=2;
                if(pov>=22500 && pov<=31500) result.directions|=4;
            }
        }
        return result;
    }
};
}
namespace {
/* Device discovery can block for over 100 ms on Windows. Keep all DirectInput
 * ownership on this worker; the game and launcher only read its latest snapshot. */
struct AsyncControllers {
    std::mutex mutex;
    std::condition_variable wake;
    HWND owner=nullptr;
    GenericState latest;
    bool stop=false;
    std::thread worker;
    AsyncControllers() : worker([this] { run(); }) {}
    ~AsyncControllers() {
        { std::lock_guard<std::mutex> lock(mutex); stop=true; }
        wake.notify_one(); worker.join();
    }
    void run() {
        const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        std::unique_ptr<Controllers> controllers;
        HWND current=nullptr;
        std::unique_lock<std::mutex> lock(mutex);
        while(!stop) {
            HWND next=owner;
            if(next) {
                lock.unlock();
                if(next!=current) { controllers.reset(new Controllers); current=next; }
                const auto state=controllers->poll(next);
                lock.lock();
                if(owner==next) latest=state;
            }
            wake.wait_for(lock,std::chrono::milliseconds(4),[&] { return stop || owner!=next; });
        }
        lock.unlock(); controllers.reset();
        if(SUCCEEDED(com)) CoUninitialize();
    }
    GenericState poll(HWND window) {
        std::lock_guard<std::mutex> lock(mutex);
        if(owner!=window) { owner=window; latest={}; wake.notify_one(); }
        return latest;
    }
};
}
GenericState poll_generic(HWND window) { static AsyncControllers controllers; return controllers.poll(window); }
unsigned generic_capture(const GenericState &state) {
    unsigned choice=state.directions;
    if(choice && (choice&(choice-1))) return 0;
    for(int i=0;i<128;i++) if(state.buttons[i]) { if(choice) return 0; choice=generic_base+i+1; }
    return choice;
}
unsigned generic_gamepad(const GenericState &state) {
    static int map[count]; static bool loaded=false;
    if(!loaded) { for(int i=0;i<count;i++) map[i]=load(i,3); loaded=true; }
    unsigned standard=state.directions;
    const unsigned first_buttons[]={0x1000,0x2000,0x4000,0x8000,0x100,0x200,0x20,0x10};
    for(int i=0;i<8;i++) if(state.buttons[i]) standard|=first_buttons[i];
    unsigned result=0;
    for(int i=0;i<count;i++) {
        unsigned binding=map[i];
        bool down=generic_code(binding)?state.buttons[binding-generic_base-1]:(binding && (standard&binding));
        if(down) result|=actions[i].mask;
    }
    return result;
}
}
