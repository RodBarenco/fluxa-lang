/* Small CRT name bridge used only by the MinGW Windows profile. */
#ifndef FLUXA_WINDOWS_COMPAT_H
#define FLUXA_WINDOWS_COMPAT_H

#if defined(_WIN32)
#  include <string.h>
#  include <direct.h>
/*
 * Win32 exposes names that collide with Fluxa and raylib declarations.
 * Include the SDK before those headers and rename only the conflicting SDK
 * symbols while windows.h is being parsed.
 */
#  define TokenType Win32TokenType
#  define Rectangle Win32Rectangle
#  define CloseWindow Win32CloseWindow
#  define ShowCursor Win32ShowCursor
#  define LoadImage Win32LoadImage
#  define DrawText Win32DrawText
#  define DrawTextEx Win32DrawTextEx
#  include <windows.h>
#  include <shellapi.h>
#  undef TokenType
#  undef Rectangle
#  undef CloseWindow
#  undef ShowCursor
#  undef LoadImage
#  undef DrawText
#  undef DrawTextEx
#  include <pthread.h>
#  define strdup  _strdup
#  define strtok_r strtok_s

/* MinGW's mkdir has the Windows one-argument signature. */
#  define mkdir(path, ...) _mkdir(path)
#endif

#endif
