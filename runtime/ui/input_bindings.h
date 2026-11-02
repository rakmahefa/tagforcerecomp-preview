#pragma once
#include <windows.h>
#include <string>
#include "util.h"

/* Shared by the launcher and runtime. Zero explicitly unbinds an input. */
namespace bindings {
constexpr unsigned generic_base = 0x40000000;
inline bool generic_code(unsigned code) { return code > generic_base && code <= generic_base + 128; }
struct GenericState { bool connected=false; bool buttons[128]={}; unsigned directions=0; };
GenericState poll_generic(HWND window);
unsigned generic_gamepad(const GenericState &state);
unsigned generic_capture(const GenericState &state);
constexpr int count = 13;
struct Action { const wchar_t *name, *key; unsigned mask; int keys[3]; unsigned pad; };
inline const Action actions[count] = {
    {L"Up", L"up", 0x10, {VK_UP,'W',0},1},
    {L"Down", L"down",0x40,{VK_DOWN,'S',0},2},
    {L"Left", L"left",0x80,{VK_LEFT,'A',0},4},
    {L"Right", L"right",0x20,{VK_RIGHT,'D',0},8},
    {L"Confirm / Cross", L"cross",0x4000,{VK_SPACE,'E','Z'},0x1000},
    {L"Cancel / Circle", L"circle",0x2000,{VK_BACK,'X','C'},0x2000},
    {L"Square", L"square",0x8000,{'F',0,0},0x4000},
    {L"Triangle", L"triangle",0x1000,{'R',0,0},0x8000},
    {L"L shoulder", L"l",0x100,{'Q',0,0},0x10100},
    {L"R shoulder", L"r",0x200,{VK_TAB,0,0},0x20200},
    {L"Start", L"start",8,{VK_RETURN,0,0},0x10},
    {L"Select", L"select",1,{VK_CONTROL,0,0},0x20},
    {L"Run (hold Cancel)", L"run",0x2000,{VK_SHIFT,0,0},0x2000}
};
inline const wchar_t *pad_names[] = {L"Unbound",L"D-pad Up",L"D-pad Down",L"D-pad Left",L"D-pad Right",L"Menu (Start)",L"View (Back)",L"LB",L"RB",L"A",L"B",L"X",L"Y",L"LT",L"RT",L"LB / LT",L"RB / RT"};
inline const unsigned pads[] = {0,1,2,4,8,0x10,0x20,0x100,0x200,0x1000,0x2000,0x4000,0x8000,0x10000,0x20000,0x10100,0x20200};
constexpr int pad_count = sizeof(pads)/sizeof(pads[0]);
inline unsigned pressed_pad() {
    struct Pad { WORD buttons; BYTE lt,rt; SHORT lx,ly,rx,ry; };
    struct State { DWORD packet; Pad pad; };
    using GetState = DWORD (WINAPI *)(DWORD,State *);
    static GetState get=[]() -> GetState {
        for(const wchar_t *dll : {L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll"}) {
            HMODULE m=LoadLibraryW(dll); if(!m) continue;
            auto fn=(GetState)(void *)GetProcAddress(m,"XInputGetState"); if(fn) return fn;
            FreeLibrary(m);
        }
        return nullptr;
    }();
    unsigned result=0;
    if(get) for(DWORD i=0;i<4;i++) { State s={}; if(!get(i,&s)) result |= (s.pad.buttons & 0xf33f) | (s.pad.lt>100?0x10000:0) | (s.pad.rt>100?0x20000:0); }
    return result;
}
inline std::wstring key_name(int vk) {
    if (!vk) return L"Unbound";
    wchar_t s[64] = {}; UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END) scan |= 0x100;
    if (GetKeyNameTextW(scan << 16, s, 64)) return s;
    return L"Key " + std::to_wstring(vk);
}
inline std::wstring setting(int row, int slot) {
    /* The game uses held Circle to run. Both controller fields edit that one binding.
     * Ignore obsolete run_pad entries so an old zero cannot hide the effective input. */
    if(row==12 && slot==3) row=5;
    return std::wstring(actions[row].key) + (slot == 3 ? L"_pad" : L"_key" + std::to_wstring(slot));
}
inline int load(int row, int slot) {
    int def = slot == 3 ? actions[row].pad : actions[row].keys[slot];
    int v = GetPrivateProfileIntW(L"bindings", setting(row,slot).c_str(), def, app_ini_path().c_str());
    if (slot != 3) return v >= 0 && v <= 255 ? v : def;
    if (generic_code((unsigned)v)) return v;
    for (unsigned p : pads) if (p == (unsigned)v) return v;
    return def;
}
inline bool save(int row, int slot, int value) {
    return WritePrivateProfileStringW(L"bindings", setting(row,slot).c_str(), std::to_wstring(value).c_str(), app_ini_path().c_str()) != 0;
}
inline std::wstring controller_label(int row) {
    unsigned code=load(row,3);
    std::wstring label=L"Unbound";
    if(generic_code(code)) label=L"Button " + std::to_wstring(code-generic_base);
    else for(int i=0;i<pad_count;i++) if(pads[i]==code) label=pad_names[i];
    if(row==12 && code) label=L"Hold " + label;
    return label;
}
inline unsigned keyboard() {
    static int keys[count][3]; static bool loaded;
    if (!loaded) { for (int i=0;i<count;i++) for(int j=0;j<3;j++) keys[i][j]=load(i,j); loaded=true; }
    unsigned result=0;
    for(int i=0;i<count;i++) for(int vk : keys[i]) if(vk && (GetAsyncKeyState(vk)&0x8000)) result |= actions[i].mask;
    return result;
}
inline unsigned gamepad(unsigned buttons) {
    static int map[count]; static bool loaded;
    if(!loaded) { for(int i=0;i<count;i++) map[i]=load(i,3); loaded=true; }
    unsigned result=0;
    for(int i=0;i<count;i++) if(map[i] && !generic_code(map[i]) && (buttons & map[i])) result |= actions[i].mask;
    return result;
}
}
