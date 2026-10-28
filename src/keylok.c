/* keylok.c -- the key's own behaviour, with no key and no Windows.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A KEYLOK II answers four 16-bit words with one 32-bit word. The game uses
 * six of its commands, and what it does with each answer is the whole of what
 * this has to get right. It calls the key from 21 sites; three of them
 * matter:
 *
 *   the variable memory   56 words the game writes at boot and reads back
 *                         later; two sites read one word twice and exit(0) if
 *                         the two reads disagree. A store satisfies all of it
 *                         by construction, so nothing here is keyed to what
 *                         the game expects to find -- it finds what it wrote.
 *   the serial number     read twice a session and compared with itself. Any
 *                         stable number does; an unstable one corrupts the
 *                         game state silently.
 *   the challenge         the game transforms the answer to CHECK and sends
 *                         the transform back. The key validates it.
 *
 * The challenge is the only one with a secret in it on real hardware, and the
 * secret is the key's, not the game's: the game only checks that the answer
 * came back. So this issues a challenge, recomputes the response that
 * challenge demands, and refuses anything else -- the protocol, not a
 * recording of one key's answers.
 */
#include "keylok.h"

#include <string.h>

/* The game rotates within 16 bits; x86 shifts take the count mod 32, so a
 * count of 0 must not become a 16-bit shift. */
static uint16_t rol16(unsigned v, unsigned n)
{
    v &= 0xffffu;
    n &= 15u;
    return (uint16_t)(n ? ((v << n) | (v >> (16 - n))) : v);
}

/* snow.exe 0x44ef66..0x44efd2, the transform between the two CHECK calls:
 * from the answer's halves H (high) and L (low) it builds a second call
 *     command = ROL16(L, H & 7) ^ H ^ 0x601
 *     arg2    = ROL16(H, L & 15)
 *     arg3    = H ^ L
 * and sends arg4 = 0. */
static void response_to(uint32_t challenge, unsigned *cmd, unsigned *a2,
                        unsigned *a3)
{
    unsigned h = (challenge >> 16) & 0xffffu, l = challenge & 0xffffu;

    *cmd = (unsigned)(rol16(l, h & 7u) ^ h ^ 0x601u) & 0xffffu;
    *a2  = rol16(h, l & 15u);
    *a3  = (h ^ l) & 0xffffu;
}

int keylok_expected(const keylok_t *k, unsigned *cmd, unsigned *a2,
                    unsigned *a3)
{
    if (!k->challenge)
        return 0;
    response_to(k->challenge, cmd, a2, a3);
    return 1;
}

/* The answer a validated response earns. The game accepts the call when
 * either half matches (snow.exe 0x44efee and 0x44f002), and suspends its own
 * thread when neither does; both halves are set, so the game's two tests
 * agree with each other. */
#define KEYLOK_PASS 0x7fe3b2e0u

/* A challenge whose transform would land on a command number would make the
 * response indistinguishable from an ordinary call, so those are skipped.
 * Everything else is free: the game does not inspect the challenge, it only
 * transforms it. */
static int collides(uint32_t challenge)
{
    unsigned cmd, a2, a3;

    response_to(challenge, &cmd, &a2, &a3);
    return cmd <= KEYLOK_WRITEVARWORD || cmd == KEYLOK_TERMINATE;
}

static uint32_t next_challenge(keylok_t *k)
{
    unsigned guard;

    for (guard = 0; guard < 64; guard++) {
        k->seed = k->seed * 1664525u + 1013904223u;
        if (k->seed && !collides(k->seed))
            return k->seed;
    }
    return 0x5aa5c33cu;         /* checked: transforms to 0x3b3c            */
}

void keylok_reset(keylok_t *k, uint32_t serial)
{
    memset(k, 0, sizeof *k);
    k->serial = serial;
    k->seed = serial ? serial : 1u;
    /* The cabinet's key came programmed. The game rewrites these four words
     * at every boot (snow.exe 0x43f7d0) before it reads them, so the contents
     * only decide what a read sees if one ever runs first. */
    k->mem[0] = 'P';
    k->mem[2] = 'l';
    k->mem[4] = 'a';
    k->mem[6] = 'y';
}

uint32_t keylok_call(keylok_t *k, unsigned cmd, unsigned a2, unsigned a3,
                     unsigned a4)
{
    (void)a4;                   /* the password half of every call           */
    cmd &= 0xffffu;
    a2 &= 0xffffu;
    a3 &= 0xffffu;

    /* An open challenge takes precedence: its response carries a transformed
     * command word, not a command. */
    if (k->challenge) {
        unsigned want, w2, w3;

        response_to(k->challenge, &want, &w2, &w3);
        k->challenge = 0;
        if (cmd == want && a2 == w2 && a3 == w3) {
            k->answered++;
            return KEYLOK_PASS;
        }
        k->refused++;
        return 0;
    }

    switch (cmd) {
    case KEYLOK_CHECK:
        k->checks++;
        k->challenge = next_challenge(k);
        return k->challenge;

    case KEYLOK_GETSN:
        return k->serial;

    case KEYLOK_GETVARWORD:
        return a2 < KEYLOK_WORDS ? k->mem[a2] : 0;

    case KEYLOK_WRITEVARWORD:
        if (a2 < KEYLOK_WORDS)
            k->mem[a2] = (uint16_t)a3;
        return a2 < KEYLOK_WORDS ? a3 : 0;

    /* The game discards both answers (snow.exe 0x44e99a, 0x44e9ba). A key
     * returns the authorisation word; echoing the argument is as true to that
     * as anything can be without a key to read one from. */
    case KEYLOK_READAUTH:
    case KEYLOK_WRITEAUTH:
        return a2;

    case KEYLOK_TERMINATE:      /* the client closes the handle itself       */
        return 0;

    default:
        k->refused++;
        return 0;
    }
}
