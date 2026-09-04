/* probe-parent.c -- reserve the span in a child before its loader runs.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#include <windows.h>
#include <stdio.h>
#define BASE 0x00100000u
#define END  0x0071D830u
int main(int argc, char **argv)
{
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = BASE; size_t busy = 0; void *p;
    char cmd[MAX_PATH];
    if (argc < 2) { printf("usage: probe-parent <child.exe>\n"); return 2; }
    snprintf(cmd, sizeof cmd, "%s", argv[1]);
    if (!CreateProcessA(argv[1], cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        printf("CreateProcess('%s') failed %lu\n", argv[1], GetLastError()); return 1;
    }
    while (a < END && VirtualQueryEx(pi.hProcess, (LPCVOID)a, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State != MEM_FREE) { busy += mbi.RegionSize; printf("  0x%08lX..0x%08lX %s %s\n", (unsigned long)(uintptr_t)mbi.BaseAddress, (unsigned long)((uintptr_t)mbi.BaseAddress+mbi.RegionSize), mbi.State==MEM_COMMIT?"COMMIT ":"RESERVE", mbi.Type==MEM_IMAGE?"IMAGE":mbi.Type==MEM_MAPPED?"MAPPED":"PRIVATE"); }
        if (!mbi.RegionSize) break;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    printf("[parent] suspended child: %zu KB busy of %u KB span\n",
           busy / 1024, (END - BASE) / 1024);
    p = VirtualAllocEx(pi.hProcess, (LPVOID)BASE, END - BASE,
                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    printf("[parent] VirtualAllocEx = %p err=%lu  -> two-stage loader %s\n",
           p, p ? 0UL : GetLastError(), p ? "VIABLE" : "NOT viable");
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
