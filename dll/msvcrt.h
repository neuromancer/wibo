#pragma once

#include "types.h"

typedef int (_CC_CDECL *MSVCRT_SOPEN)(LPCSTR filename, int oflag, int shflag, int pmode);
typedef int (_CC_CDECL *MSVCRT_STAT)(LPCSTR path, void *buffer);
typedef LPSTR (_CC_CDECL *MSVCRT_STRDUP)(LPCSTR string);

namespace msvcrt {

LPSTR CDECL _mktemp(LPSTR templateName);
LPSTR CDECL _tempnam(LPCSTR dir, LPCSTR prefix);
LPSTR CDECL _strdup(LPCSTR string);
int CDECL _sopen(LPCSTR filename, int oflag, int shflag, int pmode);
int CDECL _stat(LPCSTR path, void *buffer);

} // namespace msvcrt
