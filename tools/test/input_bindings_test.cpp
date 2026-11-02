/* Isolated input configuration checks; run once normally and once with "custom".
 * Link runtime/ui/input_generic.cpp, -ldinput8 -ldxguid -luser32. */
#include <windows.h>
#include <cassert>
#include <cstdio>
#include "input_bindings.h"
static std::wstring test_ini;
std::wstring app_ini_path() { return test_ini; }
int main(int argc,char **) {
    wchar_t directory[MAX_PATH],path[MAX_PATH];
    assert(GetTempPathW(MAX_PATH,directory));
    assert(GetTempFileNameW(directory,L"tfi",0,path));
    test_ini=path;
    using namespace bindings;
    /* Running is the held Cancel command, including older INIs with run_pad=0. */
    assert(WritePrivateProfileStringW(L"bindings",L"run_pad",L"0",test_ini.c_str()));
    assert(load(5,3)==0x2000);
    assert(load(12,3)==load(5,3));
    assert(controller_label(12)==L"Hold B");
    assert(save(5,3,generic_base+7));
    assert(load(12,3)==(int)(generic_base+7));
    assert(controller_label(12)==L"Hold Button 7");
    assert(save(12,3,0x4000));
    assert(load(5,3)==0x4000);
    assert(save(12,3,0));
    assert(load(5,3)==0);
    assert(save(5,3,0x2000));
    GenericState state; state.connected=true;
    assert(generic_capture(state)==0);
    state.buttons[127]=true;
    assert(generic_capture(state)==generic_base+128);
    state.buttons[0]=true;
    assert(generic_capture(state)==0);
    state.buttons[127]=false;
    assert(generic_capture(state)==generic_base+1);
    state.buttons[0]=false; state.directions=5;
    assert(generic_capture(state)==0);
    state.directions=1; assert(generic_capture(state)==1);
    state.directions=0;
    if(argc>1) {
        assert(save(4,3,generic_base+128));
        assert(save(5,3,0));
        assert(save(6,3,generic_base+1));
        assert(load(4,3)==(int)(generic_base+128));
        state.buttons[127]=true;
        assert(generic_gamepad(state)==0x4000);
        state.buttons[127]=false; state.buttons[0]=true;
        assert(generic_gamepad(state)==0x8000);
        assert(gamepad(0x1000)==0);
        state.buttons[0]=false; state.buttons[1]=true;
        assert(generic_gamepad(state)==0);
    } else {
        state.buttons[0]=true;
        assert(generic_gamepad(state)==0x4000);
        state.buttons[0]=false; state.buttons[4]=true;
        assert(generic_gamepad(state)==0x100);
        state.buttons[4]=false; state.directions=1|8;
        assert(generic_gamepad(state)==(0x10|0x20));
        assert(gamepad(0x10000)==0x100);
        assert(gamepad(0x1000)==0x4000);
        assert(gamepad(0x2000)==0x2000);
    }
    assert(save(7,3,generic_base+129));
    assert(load(7,3)==0x8000);
    assert(DeleteFileW(test_ini.c_str()));
    puts("Input bindings: PASS");
}
