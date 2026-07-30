/* Small CRT name bridge used only by the MinGW Windows profile. */
#ifndef FLUXA_WINDOWS_COMPAT_H
#define FLUXA_WINDOWS_COMPAT_H

#if defined(_WIN32)
#  include <string.h>
#  include <direct.h>
/*
 * winnt.h exposes an enum member named TokenType. Include the Win32 headers
 * before Fluxa's lexer and rename only that SDK symbol while they are parsed.
 */
#  define TokenType Win32TokenType
#  include <windows.h>
#  include <shellapi.h>
#  undef TokenType
#  include <pthread.h>
#  define strdup  _strdup
#  define strtok_r strtok_s

/* MinGW's mkdir has the Windows one-argument signature. */
#  define mkdir(path, ...) _mkdir(path)
#endif

#endif
