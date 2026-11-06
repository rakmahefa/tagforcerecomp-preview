/* tlhelp32.h - Win32 compatibility shim (Linux): the thread snapshot used by the crash log.
 * Implemented in shim_kern.cpp over /proc/self/task. */
#pragma once
#include "windows.h"
typedef struct tagTHREADENTRY32 {
    DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID, tpBasePri, tpDeltaPri, dwFlags;
} THREADENTRY32, *LPTHREADENTRY32;
#define TH32CS_SNAPTHREAD 4
#define TH32CS_SNAPPROCESS 2
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL Thread32First(HANDLE snap, LPTHREADENTRY32 te);
BOOL Thread32Next(HANDLE snap, LPTHREADENTRY32 te);
