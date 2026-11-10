#ifndef TYPES_H
#define TYPES_H

/* Use compiler-provided builtins — correct for target ABI */
typedef __UINT8_TYPE__   uint8_t;
typedef __INT8_TYPE__    int8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __INT16_TYPE__   int16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __INT32_TYPE__   int32_t;
//typedef __UINT64_TYPE__  uint64_t;
typedef __INT64_TYPE__   int64_t;
typedef __SIZE_TYPE__    size_t;
typedef __UINTPTR_TYPE__ uintptr_t;

/* OS/2 semantic types (always 32-bit) */
typedef uint32_t  ULONG;
typedef int32_t   LONG;
typedef uint16_t  USHORT;
typedef int16_t   SHORT;
typedef uint8_t   UCHAR;
typedef void*     PVOID;
typedef ULONG*    PULONG;
typedef USHORT*   PUSHORT;
typedef uint32_t  APIRET;
typedef void      VOID;
#define APIENTRY

#define NO_ERROR 0

typedef int bool;
#define true  1
#define false 0

#ifndef NULL
#define NULL ((void*)0)
#endif

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

#endif

