

#ifndef __CONFIG__H__
#define __CONFIG__H__

#define     DEBUG

#if defined(DEBUG)
//#   define     USE_MALLOC
#endif

#if defined(WIN32) || defined(_WIN32) || defined(WINDOWS)
#   if defined(DEBUG)
#      define _CRT_SECURE_NO_WARNINGS     1
#      define _CRT_NONSTDC_NO_DEPRECATE   1
#      define _CRTDBG_MAP_ALLOC 
#      include <crtdbg.h>
#   endif 

#   define localtime_r(tm, tmt) localtime_s(tmt, tm);
#   define WINDOWS
#else

#   define UNIX
#   define USE_CONSOLE_COLOR

#endif


#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <assert.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>


#ifndef bool 
typedef enum {
    false,
    true,
}bool;
#endif 


#ifndef inline
#define inline
#endif


#ifndef va_copy 
#   ifdef __va_copy 
#       define va_copy(DEST,SRC)  __va_copy((DEST),(SRC)) 
#   else 
#       define va_copy(DEST, SRC) memcpy((&DEST), (&SRC), sizeof(va_list)) 
#   endif 
#endif


#endif
