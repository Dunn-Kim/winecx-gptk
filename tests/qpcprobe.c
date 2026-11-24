/* Does QueryPerformanceCounter still agree with NtQueryPerformanceCounter now
 * that it can skip the syscall?
 *
 * patches/0007 lets RtlQueryPerformanceCounter read rdtsc itself under Rosetta 2,
 * where rdtsc counts the nanoseconds of mach_continuous_time, the clock that
 * NtQueryPerformanceCounter reads in unix code. KUSER_SHARED_DATA says when:
 * QpcBypassEnabled with QpcFrequency 1000000000. Both must keep 100 ns units.
 *
 * Checks that the frequency is 10 MHz, that calls alternating between the two
 * never go backwards, and that 500 ms of Sleep reads as about 500 ms. Then times
 * one QueryPerformanceCounter call.
 *
 * Exit code 0 when every check passed, 1 when any failed. Writes to a file
 * because a second wine attached to an existing wineserver has nowhere to print.
 *
 * build: x86_64-w64-mingw32-gcc -O2 -o qpcprobe.exe qpcprobe.c
 * run:   wine qpcprobe.exe   (expect 0; qpcprobe.txt says whether the bypass is on)
 */
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *nt_qpc_fn)(LARGE_INTEGER *, LARGE_INTEGER *);

int main(void)
{
    const volatile BYTE *bypass = (const BYTE *)0x7ffe03c6;          /* KUSER_SHARED_DATA.QpcBypassEnabled */
    const volatile LONGLONG *qpc_freq = (const LONGLONG *)0x7ffe0300; /* KUSER_SHARED_DATA.QpcFrequency */
    nt_qpc_fn nt_qpc = (nt_qpc_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryPerformanceCounter");
    LARGE_INTEGER freq, a, b, start, end;
    ULONGLONG tick0, tick1;
    LONGLONG prev = 0;
    int backwards = 0, failures = 0, n = 1000000;
    double ratio;
    FILE *out = fopen("qpcprobe.txt", "w");

    if (!out || !nt_qpc) return 2;
    fprintf(out, "KUSER_SHARED_DATA: QpcBypassEnabled %#x, QpcFrequency %lld\n", *bypass, *qpc_freq);

    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart != 10000000) failures++;
    fprintf(out, "QueryPerformanceFrequency: %lld\n", freq.QuadPart);

    for (int i = 0; i < n; i++)
    {
        QueryPerformanceCounter(&a);
        nt_qpc(&b, NULL);
        if (a.QuadPart < prev || b.QuadPart < a.QuadPart) backwards++;
        prev = b.QuadPart;
    }
    if (backwards) failures++;
    fprintf(out, "alternating calls: %d of %d went backwards\n", backwards, n);

    QueryPerformanceCounter(&a);
    tick0 = GetTickCount64();
    Sleep(500);
    QueryPerformanceCounter(&b);
    tick1 = GetTickCount64();
    ratio = (double)(b.QuadPart - a.QuadPart) / 10000.0 / (double)(tick1 - tick0);
    if (ratio < 0.9 || ratio > 1.1) failures++;
    fprintf(out, "500 ms sleep: %.1f ms by counter, %llu ms by tick count (ratio %.3f)\n",
            (b.QuadPart - a.QuadPart) / 10000.0, tick1 - tick0, ratio);

    QueryPerformanceCounter(&start);
    for (int i = 0; i < n; i++) QueryPerformanceCounter(&a);
    QueryPerformanceCounter(&end);
    fprintf(out, "QueryPerformanceCounter: %.1f ns\n", (double)(end.QuadPart - start.QuadPart) * 100.0 / n);

    fprintf(out, "failures: %d\n", failures);
    fclose(out);
    return failures ? 1 : 0;
}
