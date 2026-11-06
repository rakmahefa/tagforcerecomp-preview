/* psapi.h - Win32 compatibility shim (Linux): process memory counters for the session log.
 * K32GetProcessMemoryInfo is resolved dynamically through GetProcAddress; the shim's
 * kernel32 tag exports it (shim_kern) over getrusage. */
#pragma once
#include "windows.h"
typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;
BOOL K32GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS_EX *, DWORD);
