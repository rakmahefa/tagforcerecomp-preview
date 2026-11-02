/* Native timing regression: link runtime/ui/input_generic.cpp with
 * -Iruntime/ui -Iruntime/core -static -ldinput8 -ldxguid -lole32 -luser32.
 * Run with normal Windows device access. Restricted sandboxes can hide the
 * slow DirectInput discovery that this test needs to exercise. */
#include <windows.h>
#include <cstdio>
#include <string>
#include "input_bindings.h"
std::wstring app_ini_path() { return L""; }
int main() {
    HWND w=CreateWindowW(L"STATIC",L"Input polling benchmark",0,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    LARGE_INTEGER f,a,b; QueryPerformanceFrequency(&f);
    unsigned slow=0; double worst=0;
    ULONGLONG end=GetTickCount64()+7000;
    while(GetTickCount64()<end) {
        QueryPerformanceCounter(&a); bindings::poll_generic(w); QueryPerformanceCounter(&b);
        double ms=(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart;
        if(ms>worst) worst=ms;
        if(ms>50) { ++slow; printf("slow poll %.1f ms\n",ms); }
        Sleep(16);
    }
    DestroyWindow(w); printf("polls above 50 ms: %u; worst %.1f ms\n",slow,worst);
    return slow?1:0;
}
