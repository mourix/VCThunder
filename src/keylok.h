/* keylok.h -- the parallel-port key Ultimate Arctic Thunder asks for.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The 2004 snow.exe talks to a KEYLOK II (Microcomputer Applications, Inc.)
 * through a driver device, and to a watchdog process that clears a marker in
 * the game's own memory. Both are cabinet hardware this host does not have,
 * and both are stood in for the way every other cabinet device here is: the
 * host answers at the Win32 boundary. The model below is the device half and
 * is deliberately free of windows.h, so tests/keylok_test.c drives it with no
 * game and no handle; src/arctic_keylok.c is the boundary half.
 *
 * Every number here was read out of the game's own code, never out of a
 * key.
 */
#ifndef VCT_KEYLOK_H
#define VCT_KEYLOK_H

#include <stdint.h>

/* KEYLOK II's variable memory: 56 words, addressed by word. */
#define KEYLOK_WORDS 56

/* The commands the game uses, as the manual numbers them. */
enum {
    KEYLOK_CHECK        = 1,    /* challenge; the answer is the next call    */
    KEYLOK_READAUTH     = 2,
    KEYLOK_GETSN        = 3,
    KEYLOK_GETVARWORD   = 4,
    KEYLOK_WRITEAUTH    = 5,
    KEYLOK_WRITEVARWORD = 6,
    KEYLOK_TERMINATE    = 0xffff
};

typedef struct {
    uint16_t mem[KEYLOK_WORDS];
    uint32_t serial;
    uint32_t challenge;         /* the one just issued, 0 when none is open  */
    uint32_t seed;              /* so a challenge differs call to call       */
    unsigned checks, answered, refused;
} keylok_t;

/* A key as the cabinet's was programmed, ready for the first call. */
void keylok_reset(keylok_t *k, uint32_t serial);

/* One KFUNC(command, arg2, arg3, arg4). The game masks every argument to 16
 * bits before the driver sees it, so this takes them already masked and
 * returns the driver's 32-bit answer. */
uint32_t keylok_call(keylok_t *k, unsigned cmd, unsigned a2, unsigned a3,
                     unsigned a4);

/* The response the issued challenge demands, for the test to check the model
 * against the transform the game performs. 0 when no challenge is open. */
int keylok_expected(const keylok_t *k, unsigned *cmd, unsigned *a2,
                    unsigned *a3);

#endif
