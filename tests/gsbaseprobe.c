/* Does unix code still run with gsbase on the pthread TSD after a signal moved
 * it to the TEB, and what does a trip into unix code cost?
 *
 * patches/0005 stops the dispatchers from calling _thread_set_tsd_base on every
 * entry into unix code. They call it only after leave_handler has put gsbase on
 * the TEB, which happens when a signal handler returns to PE code. If that
 * bookkeeping is wrong, unix code runs with gsbase on the TEB, where libsystem
 * finds TEB->Tib.StackBase instead of its errno pointer, so a failing open()
 * reports the wrong error or crashes.
 *
 * Each worker loops: take an access violation in PE code (a signal, resumed by
 * a vectored handler through NtContinue), check gs:0x30 is still the TEB, then
 * fail to open a file that does not exist and check the error is
 * ERROR_FILE_NOT_FOUND. Meanwhile the main thread suspends each worker, reads
 * its context and resumes it, which reaches the worker as a signal wherever it
 * happens to be.
 *
 * The suspends also show an older problem that 0005 does not change: now and
 * then a worker ends early, with "nested exception on signal stack" at
 * _sigtramp in the log, and GetThreadContext fails. It happens on the engine
 * without 0005 as well, so it is counted and printed but does not fail the run.
 *
 * Then times, on one thread, QueryPerformanceCounter (one NT syscall per call),
 * gethostname (one ws2_32 unix call per call) and an access violation resumed
 * by a vectored handler (one signal per call).
 *
 * Exit code 0 when gs:0x30 and every open error were right, 1 when any was
 * wrong. Writes to a file because a second wine attached to an existing
 * wineserver has nowhere to print.
 *
 * build: x86_64-w64-mingw32-gcc -O2 -o gsbaseprobe.exe gsbaseprobe.c -lws2_32
 * run:   wine gsbaseprobe.exe   (expect 0; gsbaseprobe.txt has the timings)
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#define WORKERS 4
#define ROUNDS  2000

static FILE *out;
static volatile LONG faults, bad_gs, bad_error;
static volatile LONG done[WORKERS];
static volatile DWORD last_error;

static LONG CALLBACK skip_fault(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Rip += 2;  /* movl (%rax),%eax */
    InterlockedIncrement(&faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void fault(void)
{
    __asm__ volatile ("xorl %%eax,%%eax\n\tmovl (%%rax),%%eax" ::: "rax", "memory");
}

static void *gs_teb(void)
{
    void *v;
    __asm__ volatile ("movq %%gs:0x30, %0" : "=r" (v));
    return v;
}

static void check_round(void *teb)
{
    HANDLE file;

    fault();
    if (gs_teb() != teb) InterlockedIncrement(&bad_gs);
    file = CreateFileA("gsbaseprobe-missing.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        last_error = GetLastError();
        InterlockedIncrement(&bad_error);
    }
}

static DWORD WINAPI worker(void *arg)
{
    int id = (int)(INT_PTR)arg;
    void *teb = NtCurrentTeb();

    for (int i = 0; i < ROUNDS; i++)
    {
        check_round(teb);
        done[id]++;
    }
    return 0;
}

static double per_call_ns(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq, int n)
{
    return (double)(end.QuadPart - start.QuadPart) * 1e9 / freq.QuadPart / n;
}

int main(void)
{
    HANDLE threads[WORKERS];
    LARGE_INTEGER freq, start, end, now;
    CONTEXT ctx;
    WSADATA wsa;
    char host[256];
    int suspends = 0, context_failures = 0, bad_hostname = 0, n;

    out = fopen("gsbaseprobe.txt", "w");
    if (!out) return 2;
    if (!AddVectoredExceptionHandler(1, skip_fault)) return 2;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 2;

    for (int i = 0; i < WORKERS; i++)
        if (!(threads[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL))) return 2;
    while (WaitForMultipleObjects(WORKERS, threads, TRUE, 0) == WAIT_TIMEOUT)
    {
        for (int i = 0; i < WORKERS; i++)
        {
            if (SuspendThread(threads[i]) == (DWORD)-1) continue;
            ctx.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(threads[i], &ctx)) context_failures++;
            ResumeThread(threads[i]);
            suspends++;
        }
        Sleep(1);
    }
    fprintf(out, "workers: %d x %d rounds, %ld faults, %d suspends, GetThreadContext failed %d\n",
            WORKERS, ROUNDS, faults, suspends, context_failures);
    for (int i = 0; i < WORKERS; i++)
    {
        DWORD code = 0;

        GetExitCodeThread(threads[i], &code);
        if (done[i] != ROUNDS || code)
            fprintf(out, "worker %d ended early: %ld of %d rounds, exit code %#lx\n", i, done[i], ROUNDS, code);
        CloseHandle(threads[i]);
    }

    QueryPerformanceFrequency(&freq);

    n = 1000000;
    QueryPerformanceCounter(&start);
    for (int i = 0; i < n; i++) QueryPerformanceCounter(&now);
    QueryPerformanceCounter(&end);
    fprintf(out, "QueryPerformanceCounter: %7.1f ns\n", per_call_ns(start, end, freq, n));

    n = 200000;
    QueryPerformanceCounter(&start);
    for (int i = 0; i < n; i++)
        if (gethostname(host, sizeof(host))) bad_hostname++;
    QueryPerformanceCounter(&end);
    fprintf(out, "gethostname:             %7.1f ns (%d failed)\n", per_call_ns(start, end, freq, n), bad_hostname);

    n = 20000;
    QueryPerformanceCounter(&start);
    for (int i = 0; i < n; i++) fault();
    QueryPerformanceCounter(&end);
    fprintf(out, "access violation:        %7.1f ns\n", per_call_ns(start, end, freq, n));

    /* the main thread too, now that it has taken signals of its own */
    for (int i = 0; i < ROUNDS; i++) check_round(NtCurrentTeb());

    fprintf(out, "wrong gs:0x30: %ld, wrong open error: %ld (last %lu)\n", bad_gs, bad_error, last_error);
    fclose(out);
    WSACleanup();
    return bad_gs || bad_error || bad_hostname ? 1 : 0;
}
