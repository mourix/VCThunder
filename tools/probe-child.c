/* probe-child.c -- the "game host": does the span become available at all?
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * It is a separate binary on purpose; a self-spawning one trips AV heuristics.
 */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery((LPCVOID)0x00100000u, &mbi, sizeof mbi);
    printf("[child] 0x00100000 state=%lx type=%lx size=%zu KB\n",
           mbi.State, mbi.Type, mbi.RegionSize / 1024);
    if (mbi.State != MEM_FREE && mbi.RegionSize >= 0x61d830u)
        printf("[child] the parent's reservation survived into my address space\n");
    return 0;
}
