/* probe-reserve.c -- can a minimal process claim the span as its first action?
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Separates "structurally impossible" from "we fragmented it ourselves before
 * asking". No CreateProcess, no VirtualAllocEx: nothing that looks like process
 * hollowing to an AV engine.
 */
#include <windows.h>
#include <stdio.h>
#define BASE 0x00100000u
#define END  0x0071D830u
int main(void)
{
    void *p = VirtualAlloc((LPVOID)BASE, END - BASE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    printf("VirtualAlloc(0x%08X, %u KB) = %p  err=%lu\n",
           BASE, (END - BASE) / 1024, p, p ? 0UL : GetLastError());
    if (p) { memset(p, 0xCC, 0x1000); printf("wrote to it OK: span is claimable\n"); }
    return p ? 0 : 1;
}
