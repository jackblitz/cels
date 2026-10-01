/**
 * win32_compat.c
 * Compatibility symbols for static linking of libwinpthread on MinGW-w64.
 *
 * When statically linking libwinpthread (-lwinpthread) into executables and DLLs,
 * MinGW-w64 libwinpthread expects __intrinsic_setjmpex and __ms_vsnprintf.
 * Providing these lightweight shims allows full static embedding of libwinpthread
 * into the executable with zero external runtime DLL dependencies.
 */

#if defined(_WIN32) && defined(__GNUC__)

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

int _setjmpex(void *env, void *frame);
int __intrinsic_setjmpex(void *env, void *frame)
{
    return _setjmpex(env, frame);
}

int __ms_vsnprintf(char *buffer, size_t count, const char *format, va_list argptr)
{
    return _vsnprintf(buffer, count, format, argptr);
}

#endif /* defined(_WIN32) && defined(__GNUC__) */
