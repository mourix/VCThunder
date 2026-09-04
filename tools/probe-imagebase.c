/* probe-imagebase.c -- put the span inside our OWN PE image.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The Windows loader creates a single reservation of SizeOfImage at ImageBase
 * BEFORE the kernel places thread stacks, so if our image spans
 * 0x100000..0x71e000 the game's range is ours by construction. Our code is
 * pushed above the game's .stack via --section-start.
 */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;
    void *lo = (void *)0x00100000u;
    printf("main() lives at %p\n", (void *)main);
    if (VirtualQuery(lo, &mbi, sizeof mbi) != sizeof mbi) { printf("query failed\n"); return 1; }
    printf("0x00100000: state=%lx protect=%lx type=%lx alloc_base=%p size=%zu KB\n",
           mbi.State, mbi.Protect, mbi.Type, mbi.AllocationBase, mbi.RegionSize / 1024);
    if (!VirtualProtect(lo, 0x61d000, PAGE_EXECUTE_READWRITE, &old)) {
        printf("VirtualProtect failed err=%lu\n", GetLastError()); return 1;
    }
    printf("VirtualProtect(0x100000, 6.1MB, RWX) OK (was %lx)\n", old);
    memset((char *)lo + 0x6b0, 0xCC, 0x1000);
    *(volatile unsigned char *)lo = 0x4D;          /* clobber the 'MZ' */
    printf("wrote over the header page and into 0x1006b0: still alive\n");
    printf("SUCCESS: image-base method works\n");
    return 0;
}
