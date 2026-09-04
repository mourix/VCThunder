/* stub.c -- freestanding executable stub.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Its image base and .bss reserve the game's fixed low-address span before
 * process mappings are placed. The game later overwrites this module's code, so
 * it has no CRT or TLS callbacks, loads the host DLL first, and never receives
 * control again.
 */
#include <windows.h>

#include "../generated/span.h"

/* Sized so the image extends past VCT_SPAN_END even after this stub's own
 * headers and .text. Uninitialised, so it costs nothing in the file: only
 * VirtualSize grows. Verified at runtime below, because a toolchain change that
 * moved .bss would silently break the guarantee. */
static volatile unsigned char g_span_reservation[0x61E000];

typedef int (*shim_run_fn)(void);

/* ---- freestanding helpers (no CRT) -------------------------------------- */

static void emit(const char *s)
{
    DWORD n = 0, len = 0;
    while (s[len])
        len++;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, len, &n, NULL);
}

static void emit_hex(unsigned long v)
{
    char buf[11];
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = "0123456789ABCDEF"[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    emit(buf);
}

static void die(const char *msg, unsigned long v)
{
    emit("VCThunder: ");
    emit(msg);
    emit(" ");
    emit_hex(v);
    emit("\r\n");
    ExitProcess(1);
}

/* ---- entry -------------------------------------------------------------- */

void stub_entry(void)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    UINT_PTR base;
    HMODULE dll;
    shim_run_fn run;
    char path[MAX_PATH];
    int i, cut = 0;

    /* ASK where we are; do not assume it and read there. Under mandatory ASLR
     * this module is somewhere else entirely and 0x100000 is unmapped, so
     * probing it for an MZ signature faults at the first instruction that
     * matters: measured 0xC0000005 with no message, against the claim that
     * this stub refuses rather than corrupts. GetModuleHandle(NULL) is the
     * loader's own answer and is true at any base. */
    base = (UINT_PTR)GetModuleHandleA(NULL);
    if (!base)
        die("GetModuleHandle(NULL) failed", GetLastError());

    if (base != VCT_IMAGE_BASE)
        die("not loaded at 0x100000, so the game's span is not reserved and "
            "cannot be. Either this image was relocated (mandatory ASLR: "
            "exempt VCThunder.exe under Exploit Protection, or "
            "'Set-ProcessMitigation -Name VCThunder.exe -Disable "
            "ForceRelocateImages'), or it was linked without "
            "-Wl,--image-base,0x100000 -Wl,--disable-dynamicbase "
            "-Wl,--disable-reloc-section. Loaded at", (unsigned long)base);

    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        die("no PE image at 0x100000", VCT_IMAGE_BASE);

    nt = (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);

    if (nt->OptionalHeader.ImageBase != VCT_IMAGE_BASE)
        die("wrong ImageBase", nt->OptionalHeader.ImageBase);

    /* A relocatable image is one mandatory ASLR is entitled to move, and the
     * check above then turns a working host into a refusal on a machine whose
     * policy we do not control. Stripping the relocations removes the choice:
     * measured 2026-08-21, an image with none loads at 0x100000 and runs
     * normally with mandatory ASLR forced on. Enforced at link time too
     * (Makefile); refuse here as well, because this is the invariant the base
     * check exists to protect. */
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED))
        die("this stub carries relocations, so mandatory ASLR may move it; "
            "link with -Wl,--disable-reloc-section. Characteristics =",
            nt->FileHeader.Characteristics);

    if (nt->OptionalHeader.ImageBase + nt->OptionalHeader.SizeOfImage
            < VCT_SPAN_END)
        die("image too small to cover the game's span; enlarge "
            "g_span_reservation. SizeOfImage =", nt->OptionalHeader.SizeOfImage);

    /* The game's CODE begins at RVA 0x6b0, so our PE headers survive only if
     * they fit below it: loader.c depends on that to keep LoadLibrary and
     * host-image inspection working after the game is mapped. */
    if (nt->OptionalHeader.SizeOfHeaders > VCT_CODE_FILE_OFF)
        die("PE headers overlap the game's CODE; SizeOfHeaders =",
            nt->OptionalHeader.SizeOfHeaders);

    /* A TLS directory here would be fatal: Windows dispatches TLS callbacks on
     * every thread creation, and they point into .text, which the game
     * overwrites. -nostdlib should mean there is none; refuse to run if that
     * ever stops being true rather than crash on a later thread. */
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
        die("this stub has a TLS directory; it must be built freestanding "
            "(-nostdlib). Size =",
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size);

    /* Load the real shim from beside the exe, so a portable pack is
     * self-contained. Hand-rolled path surgery: there is no CRT here. */
    if (!GetModuleFileNameA(NULL, path, sizeof path))
        die("GetModuleFileName failed", GetLastError());
    for (i = 0; path[i]; i++)
        if (path[i] == '\\' || path[i] == '/')
            cut = i + 1;
    {
        static const char name[] = "VCThunder.dll";
        for (i = 0; i < (int)sizeof name; i++)
            path[cut + i] = name[i];
    }

    dll = LoadLibraryA(path);
    if (!dll)
        die("LoadLibrary(VCThunder.dll) failed", GetLastError());

    run = (shim_run_fn)(void *)GetProcAddress(dll, "shim_run");
    if (!run)
        die("VCThunder.dll has no shim_run export", 0);

    if ((UINT_PTR)run >= VCT_IMAGE_BASE && (UINT_PTR)run < VCT_SPAN_END)
        die("VCThunder.dll loaded inside the game's span at",
            (unsigned long)(UINT_PTR)run);

    ExitProcess((UINT)run());        /* run() does not return; see the header */
}
