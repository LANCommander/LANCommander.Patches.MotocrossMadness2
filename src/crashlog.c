/* [Debug] CrashLog=1: a crash (access violation and the like) is written to mcm2patch.log with the registers and
 * a call chain: the stack is scanned for addresses that follow a call instruction in a loaded module. The
 * game's own handler (errorlog.txt) still runs afterwards. */
#include <stdio.h>
#include <string.h>
#include "mcm2patch.h"

static const char *module_of(DWORD addr, DWORD *base)
{
    static char name[MAX_PATH];
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && GetModuleFileNameA(m, name, sizeof name)) {
        char *s = strrchr(name, '\\');
        *base = (DWORD)(uintptr_t)m;
        return s ? s + 1 : name;
    }
    return NULL;
}

/* addr follows a call: E8 rel32, FF /2 with any operand (2..7 bytes before) */
static int after_call(DWORD addr)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    int k;
    if (IsBadReadPtr(p - 7, 7)) {
        return 0;
    }
    if (p[-5] == 0xe8) {
        return 1;
    }
    for (k = 2; k <= 7; k++) {
        if (p[-k] == 0xff && ((p[-k + 1] >> 3) & 7) == 2) {
            return 1;
        }
    }
    return 0;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    static int logged;
    DWORD code = ep->ExceptionRecord->ExceptionCode, base = 0, i, n = 0;
    CONTEXT *c = ep->ContextRecord;
    const char *mod;
    DWORD *sp;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO
         && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION) || logged >= 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    logged++;
    mod = module_of(c->Eip, &base);
    mcm_log("crash: %08lx at %08lx (%s+%lx), thread %lu, access %08lx", code, c->Eip, mod ? mod : "?", c->Eip - base,
            GetCurrentThreadId(), ep->ExceptionRecord->NumberParameters > 1 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0);
    mcm_log("crash: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx,
            c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    sp = (DWORD *)(uintptr_t)c->Esp;
    for (i = 0; i < 24; i += 8) {
        DWORD *q = sp - 16 + i;
        if (!IsBadReadPtr(q, 32)) {
            mcm_log("crash: esp%+04ld: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", (long)(i - 16) * 4, q[0], q[1],
                    q[2], q[3], q[4], q[5], q[6], q[7]);
        }
    }
    for (i = 0; i < 4096 && n < 24 && !IsBadReadPtr(sp + i, 4); i++) {
        DWORD v = sp[i];
        if (v > 0x10000 && (mod = module_of(v, &base)) != NULL && after_call(v)) {
            mcm_log("crash:   [esp+%04lx] %08lx %s+%lx", i * 4, v, mod, v - base);
            n++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void mcm_crashlog_install(void)
{
    if (mcm_ini_int("Debug", "CrashLog", 0)) {
        AddVectoredExceptionHandler(1, on_exception);
        mcm_log("crash: crash log on");
    }
}
