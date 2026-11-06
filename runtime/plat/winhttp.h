/* winhttp.h - Win32 compatibility shim (Linux): the launcher's update check. Implemented in
 * shim_misc.cpp over plain sockets when TLS is available is NOT attempted; the check reports
 * "no update" with a single log line. A real implementation can come later. */
#pragma once
#include "windows.h"
/*HINTERNET comes from windows.h (a plain HANDLE here)*/
#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#define WINHTTP_NO_PROXY_NAME nullptr
#define WINHTTP_NO_PROXY_BYPASS nullptr
#define INTERNET_DEFAULT_HTTPS_PORT 443
#define WINHTTP_FLAG_SECURE 0x00800000
#define WINHTTP_NO_REFERER nullptr
#define WINHTTP_DEFAULT_ACCEPT_TYPES nullptr
#define WINHTTP_QUERY_STATUS_CODE 19
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000
HINTERNET WinHttpOpen(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
BOOL WinHttpSetTimeouts(HINTERNET, int, int, int, int);
HINTERNET WinHttpConnect(HINTERNET, LPCWSTR, WORD, DWORD);
HINTERNET WinHttpOpenRequest(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *accept, DWORD);
BOOL WinHttpSendRequest(HINTERNET, LPCWSTR, DWORD, const void *, DWORD, DWORD, DWORD_PTR);
BOOL WinHttpReceiveResponse(HINTERNET, void *);
BOOL WinHttpQueryHeaders(HINTERNET, DWORD, LPCWSTR, void *, DWORD *, DWORD *);
BOOL WinHttpReadData(HINTERNET, void *, DWORD, DWORD *);
BOOL WinHttpCloseHandle(HINTERNET);
