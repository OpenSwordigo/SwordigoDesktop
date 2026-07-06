#ifndef SRE_LUA_SETJMP_H
#define SRE_LUA_SETJMP_H

#include "../sre_setjmp.h"

typedef sre_jmp_buf jmp_buf;

#define setjmp(buf) sre_setjmp((buf))
#define longjmp(buf, val) sre_longjmp((buf), (val))
#define _setjmp(buf) sre_setjmp((buf))
#define _longjmp(buf, val) sre_longjmp((buf), (val))

#endif
