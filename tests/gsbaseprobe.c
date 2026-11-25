/* Does unix code still run with gsbase on the pthread TSD after a signal moved
 * it to the TEB, and what does a trip into unix code cost?
 *
 * patches/0005 stops the dispatchers from calling _thread_set_tsd_base on every
 * entry into unix code. They call it only after leave_handler has put gsbase on
 * the TEB, which happens when a signal handler returns to PE code. If that
 * bookkeeping is wrong, unix code runs with gsbase on the TEB. Run with
 * WINE_EXPOSE_STACK_BASE=1: the dispatchers then write the errno pointer to
 * gs:0x8 on every entry into unix code, which with gsbase on the TEB lands on
 * TEB->Tib.StackBase. Without the variable the probe refuses to run, since
 * nothing else it checks can see 0005 break.
 *
 * Each worker loops: take an access violation in PE code (a signal, resumed by
 * a vectored handler through NtContinue), fail to open a file that does not
 * exist and check the error is ERROR_FILE_NOT_FOUND, then check
 * TEB->Tib.StackBase still holds the stack base. Meanwhile the main thread suspends each worker, reads
 * its context and resumes it, which reaches the worker as a signal wherever it
 * happens to be.
 *
 * The suspends also check patches/0006. Under Rosetta, an access violation
 * that coincides with a suspend can reach segv_handler with the context of
 * the suspend signal's entry into _sigtramp; without 0006 the worker is then
 * aborted ("nested exception on signal stack" in the log), ends early with
 * its remaining rounds undone, and GetThreadContext on it fails. Every fault
 * must reach the vectored handler exactly once. GetThreadContext failures are
 * printed but do not fail the run: an occasional ERROR_ACCESS_DENIED on a live
 * worker remains with 0006 too, and is a separate problem.
 *
 * Then times, on one thread, QueryPerformanceCounter (an NT syscall per call
 * unless patches/0007 reads rdtsc instead),
 * gethostname (one ws2_32 unix call per call) and an access violation resumed
 * by a vectored handler (one signal per call).
 *
 * Exit code 0 when every check passed, 1 when any failed. Writes to a file
 * because a second wine attached to an existing wineserver has nowhere to print.
 *
 * build: x86_64-w64-mingw32-gcc -O2 -o gsbaseprobe.exe gsbaseprobe.c -lws2_32
 * run:   WINE_EXPOSE_STACK_BASE=1 wine gsbaseprobe.exe   (expect 0; gsbaseprobe.txt has the timings)
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define WORKERS 4
#define ROUNDS  2000

static FILE *out;
static volatile LONG faults, bad_stack_base, bad_error;
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

static void check_round(void *stack_base)
{
    HANDLE file;

    fault();
    file = CreateFileA("gsbaseprobe-missing.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        last_error = GetLastError();
        InterlockedIncrement(&bad_error);
    }
    if (((NT_TIB *)NtCurrentTeb())->StackBase != stack_base) InterlockedIncrement(&bad_stack_base);
}

static DWORD WINAPI worker(void *arg)
{
    int id = (int)(INT_PTR)arg;
    void *stack_base = ((NT_TIB *)NtCurrentTeb())->StackBase;

    for (int i = 0; i < ROUNDS; i++)
    {
        check_round(stack_base);
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
    int suspends = 0, context_failures = 0, bad_hostname = 0, lost = 0, n;
    DWORD context_error = 0;
    void *main_stack_base = ((NT_TIB *)NtCurrentTeb())->StackBase;

    out = fopen("gsbaseprobe.txt", "w");
    if (!out) return 2;
    if (!GetEnvironmentVariableA("WINE_EXPOSE_STACK_BASE", host, sizeof(host)) || strcmp(host, "1"))
    {
        fprintf(out, "set WINE_EXPOSE_STACK_BASE=1, or 0005 breaking goes unseen\n");
        fclose(out);
        return 2;
    }
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
            if (!GetThreadContext(threads[i], &ctx))
            {
                context_failures++;
                context_error = GetLastError();
            }
            ResumeThread(threads[i]);
            suspends++;
        }
        Sleep(1);
    }
    fprintf(out, "workers: %d x %d rounds, %ld faults, %d suspends, GetThreadContext failed %d (last error %lu)\n",
            WORKERS, ROUNDS, faults, suspends, context_failures, context_error);
    for (int i = 0; i < WORKERS; i++)
    {
        DWORD code = 0;

        GetExitCodeThread(threads[i], &code);
        if (done[i] != ROUNDS || code)
        {
            fprintf(out, "worker %d ended early: %ld of %d rounds, exit code %#lx\n", i, done[i], ROUNDS, code);
            lost++;
        }
        CloseHandle(threads[i]);
    }
    if (faults != WORKERS * ROUNDS) lost++;

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
    for (int i = 0; i < ROUNDS; i++) check_round(main_stack_base);

    fprintf(out, "wrong TEB StackBase: %ld, wrong open error: %ld (last %lu)\n", bad_stack_base, bad_error, last_error);
    fclose(out);
    WSACleanup();
    return bad_stack_base || bad_error || bad_hostname || lost ? 1 : 0;
}
