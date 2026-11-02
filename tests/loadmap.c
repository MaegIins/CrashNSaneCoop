// loadmap.dll - test helper (test copies only): run the game's console command "loadmap <level>" once, on the game thread (DR0 on the
// per-frame hero state machine exe+0x511480). Console = exe+0x117F30(); execute = console vtable+0x30(console, char*),
// as the game's own exe+0x4C030 does. Level from loadmap.cmd, e.g. crash3/l301_toadvillage/l301_toadvillage.
// Log <dll dir>\loadmap-<pid>.log
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#define SM_RVA 0x511480
static DWORD64 g_base; static char g_dir[MAX_PATH], g_cmd[300]; static volatile LONG g_done;
static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    CONTEXT *c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 1) || c->Rip != g_base + SM_RVA) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0; c->EFlags |= 0x10000;
    if (!g_done) { g_done = 1; DWORD64 con = ((DWORD64 (*)(void))(g_base + 0x117F30))();
        if (con) { DWORD64 vt = *(DWORD64 *)con; ((void (*)(DWORD64, const char *))(*(DWORD64 *)(vt + 0x30)))(con, g_cmd); g_done = 2; } }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void arm_all(int on) {
    DWORD pid = GetCurrentProcessId(); HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
    if (Thread32First(s, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID); if (!t) continue;
        SuspendThread(t); CONTEXT c; ZeroMemory(&c, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) { c.Dr0 = on ? g_base + SM_RVA : 0; c.Dr7 &= ~(0xFull << 16); c.Dr7 &= ~3ull; if (on) c.Dr7 |= 1; SetThreadContext(t, &c); }
        ResumeThread(t); CloseHandle(t);
    } while (Thread32Next(s, &te));
    CloseHandle(s);
}
static DWORD WINAPI worker(LPVOID x) {
    (void)x; char p[MAX_PATH], lv[256] = ""; g_base = (DWORD64)GetModuleHandleA(NULL);
    sprintf(p, "%sloadmap.cmd", g_dir); FILE *f = fopen(p, "r"); if (!f) return 1; fscanf(f, "%255s", lv); fclose(f);
    sprintf(g_cmd, "loadmap %s", lv);
    sprintf(p, "%sloadmap-%lu.log", g_dir, GetCurrentProcessId()); FILE *lg = fopen(p, "w"); if (!lg) return 1;
    PVOID h = AddVectoredExceptionHandler(1, veh); arm_all(1); for (int i = 0; i < 300 && !g_done; i++) Sleep(10); arm_all(0); Sleep(100); RemoveVectoredExceptionHandler(h);
    fprintf(lg, "'%s' -> %ld (2 = sent to the console, 1 = no console, 0 = game thread not reached)\n", g_cmd, g_done); fclose(lg); return 0;
}
BOOL WINAPI DllMain(HINSTANCE inst, DWORD r, LPVOID x) {
    (void)x; if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(inst); GetModuleFileNameA(inst, g_dir, MAX_PATH); char *s = strrchr(g_dir, 0x5C); if (s) s[1] = 0; HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL); if (t) CloseHandle(t); }
    return TRUE;
}
