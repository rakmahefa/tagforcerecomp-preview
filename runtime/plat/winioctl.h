/* winioctl.h - Win32 compatibility shim (Linux): storage query types. The seek-penalty probe
 * itself is Windows-only (texrepl.cpp compiles it out); the header must still parse. */
#pragma once
#include "windows.h"
typedef BYTE BOOLEAN;
#define PropertyStandardQuery 0
#define StorageDeviceSeekPenaltyProperty 3
typedef struct _STORAGE_PROPERTY_QUERY { DWORD PropertyId; DWORD QueryType; } STORAGE_PROPERTY_QUERY;
typedef struct _DEVICE_SEEK_PENALTY_DESCRIPTOR { DWORD Version; DWORD Size; BOOLEAN IncursSeekPenalty; } DEVICE_SEEK_PENALTY_DESCRIPTOR;
