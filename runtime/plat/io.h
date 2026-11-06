/* io.h - MSVC CRT compatibility shim (Linux): the fd plumbing diag.cpp uses for the log tee.
 * The return conventions follow MSVC (_dup2 returns 0 on success, not the new fd). */
#pragma once
#include <unistd.h>
#include <fcntl.h>
#include "windows.h"
#define _fileno(f) fileno(f)
#define _close(fd) close(fd)
static inline int tf_shim_dup2(int a, int b) { return dup2(a, b) < 0 ? -1 : 0; }   /* MSVC returns 0 on success */
#define _dup2(a, b) tf_shim_dup2(a, b)
#define _O_WRONLY O_WRONLY
#define _O_BINARY 0
/* HANDLE -> fd: the shim's pipe handles carry their descriptor (shim_kern) */
int tf_shim_handle_fd(HANDLE h);
#define _open_osfhandle(h, flags) tf_shim_open_osfhandle((HANDLE)(h), flags)
int tf_shim_open_osfhandle(HANDLE h, int flags);
