/* mmsystem.h - Win32 compatibility shim (Linux): timeGetTime lives in shim_kern; the waveOut
 * backend is only referenced by audio_out.cpp's Windows path (the Linux build uses SDL audio). */
#pragma once
#include "windows.h"
typedef struct tWAVEFORMATEX { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define WAVE_MAPPER ((UINT)-1)
#define CALLBACK_EVENT 0x50000
#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define WHDR_DONE 1
typedef struct wavehdr_tag { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded; DWORD_PTR dwUser; DWORD dwFlags, dwLoops; struct wavehdr_tag *lpNext; DWORD_PTR reserved; } WAVEHDR;
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEXTENSIBLE_dummy;
UINT waveOutGetNumDevs(void);
UINT waveOutGetDevCapsA(UINT, void *, UINT);
UINT waveOutOpen(HWAVEOUT *, UINT, const WAVEFORMATEX *, DWORD_PTR, DWORD_PTR, DWORD);
UINT waveOutPrepareHeader(HWAVEOUT, WAVEHDR *, UINT);
UINT waveOutWrite(HWAVEOUT, WAVEHDR *, UINT);
UINT waveOutUnprepareHeader(HWAVEOUT, WAVEHDR *, UINT);
UINT waveOutReset(HWAVEOUT);
UINT waveOutClose(HWAVEOUT);
DWORD timeGetTime(void);
UINT timeBeginPeriod(UINT);
UINT timeEndPeriod(UINT);
