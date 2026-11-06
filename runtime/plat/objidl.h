/* objidl.h - Win32 compatibility shim (Linux): the IStream-over-HGLOBAL surface launcher.cpp uses. */
#pragma once
#include "windows.h"

class IStream {
public:
    virtual ULONG Release() = 0;
    virtual HRESULT Read(void *dst, ULONG n, ULONG *got) = 0;
    virtual HRESULT Seek(LARGE_INTEGER d, DWORD from, ULARGE_INTEGER *np) = 0;
    virtual HRESULT Stat(void *st, DWORD) = 0;
};
HRESULT CreateStreamOnHGlobal(HGLOBAL glob, BOOL delete_on_release, IStream **out);
