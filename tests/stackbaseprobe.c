/* Is gs:0x8 the thread's stack base, the way it is on Windows?
 *
 * Blink reads NT_TIB.StackBase as __readgsqword(0x8) for its main thread stack
 * estimate. On this runtime PE code runs with gsbase on the pthread TSD, where
 * that slot is macOS's errno pointer, so the estimate covers every thread's
 * stack and a Chromium 74 renderer takes every thread for the main thread
 * (patches/0002). With WINE_EXPOSE_STACK_BASE=1 the two reads must agree.
 *
 * Checks the main thread and a second thread, before and after a call into
 * unix code, since the dispatchers are what put the value there.
 *
 * Exit code 0 when every read agrees, 1 when any differs. Writes to a file
 * because a second wine attached to an existing wineserver has nowhere to print.
 *
 * build: x86_64-w64-mingw32-gcc -O2 -o stackbaseprobe.exe stackbaseprobe.c
 * run:   WINE_EXPOSE_STACK_BASE=1 wine stackbaseprobe.exe   (expect 0)
 *        wine stackbaseprobe.exe                            (expect 1)
 *        copied to ZFGameBrowser.exe, wine ZFGameBrowser.exe (expect 0, patches/0004)
 */
#include <windows.h>
#include <stdio.h>

static FILE *out;
static int mismatches;

static void *gs_stack_base(void)
{
    void *v;
    __asm__ volatile ("movq %%gs:0x08, %0" : "=r" (v));
    return v;
}

static void report(const char *who)
{
    void *through_gs = gs_stack_base();
    void *through_teb = ((NT_TIB *)NtCurrentTeb())->StackBase;

    if (through_gs != through_teb) mismatches++;
    fprintf(out, "%-28s gs:0x8=%p TEB->StackBase=%p %s\n", who, through_gs, through_teb,
            through_gs == through_teb ? "same" : "DIFFERENT");
}

static DWORD WINAPI thread_proc(void *arg)
{
    (void)arg;
    report("second thread, at start");
    Sleep(10);  /* a round trip through the syscall dispatcher */
    report("second thread, after Sleep");
    return 0;
}

int main(void)
{
    HANDLE thread;

    out = fopen("stackbaseprobe.txt", "w");
    if (!out) return 2;

    report("main thread, at start");
    Sleep(10);
    report("main thread, after Sleep");

    thread = CreateThread(NULL, 0, thread_proc, NULL, 0, NULL);
    if (!thread) return 2;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    report("main thread, after join");

    fprintf(out, "mismatches: %d\n", mismatches);
    fclose(out);
    return mismatches ? 1 : 0;
}
