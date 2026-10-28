/* keylok_test.c -- the key's model against the game's own 21 checks.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * There is no key here and no game. What there is, is every test Ultimate
 * Arctic Thunder's snow.exe performs on a key's answers, written out as the
 * game performs it: the boot sequence it runs before anything is drawn, the
 * reads it compares against each other, the serial it reads twice, and the
 * challenge. If the model satisfies all of them the game has nothing left to
 * object to -- and when one of them is wrong, the message names the site.
 *
 * The transform between the two challenge calls is written here a second
 * time, from the game's code rather than from src/keylok.c, so the two have
 * to agree for the test to pass. A test that imported the model's own
 * response_to() would pass on any transform at all.
 */
#include <stdio.h>
#include <string.h>

#include "../src/keylok.h"

static int s_failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        s_failures++;
    }
}

/* snow.exe 0x44ef66..0x44efd2, read off the instruction stream. */
static unsigned game_rol16(unsigned v, unsigned n)
{
    v &= 0xffffu;
    n &= 15u;
    return n ? ((v << n) | (v >> (16 - n))) & 0xffffu : v;
}

static void game_transform(unsigned answer, unsigned *cmd, unsigned *a2,
                           unsigned *a3)
{
    unsigned h = (answer >> 16) & 0xffffu, l = answer & 0xffffu;

    *cmd = (game_rol16(l, h & 7u) ^ h ^ 0x601u) & 0xffffu;
    *a2  = game_rol16(h, l & 15u);
    *a3  = (h ^ l) & 0xffffu;
}

/* snow.exe 0x44ef40: the challenge, and what the game does with the answer.
 * Neither half matching suspends the game's own thread. */
static int game_challenge_passes(keylok_t *k)
{
    unsigned cmd, a2, a3;
    unsigned r1 = keylok_call(k, KEYLOK_CHECK, 0xaaaa, 0x9c88, 0x162a), r2;

    game_transform(r1, &cmd, &a2, &a3);
    r2 = keylok_call(k, cmd, a2, a3, 0);
    return (r2 & 0xffffu) == 0xb2e0u || ((r2 >> 16) & 0xffffu) == 0x7fe3u;
}

/* snow.exe 0x43f7d0: the boot sequence, in its own order. */
static void game_boot(keylok_t *k, unsigned random_word)
{
    keylok_call(k, KEYLOK_WRITEVARWORD, 0, 'P', 0x78);
    keylok_call(k, KEYLOK_WRITEVARWORD, 2, 'l', 0x69);
    keylok_call(k, KEYLOK_WRITEVARWORD, 4, 'a', 0x6e);
    keylok_call(k, KEYLOK_WRITEVARWORD, 6, 'y', 0x68);
    keylok_call(k, KEYLOK_WRITEVARWORD, 0x32, random_word, 0xfafa);
    keylok_call(k, KEYLOK_GETVARWORD, 0x32, 0xecaf, 0x1256);
}

static unsigned word(keylok_t *k, unsigned addr)
{
    return keylok_call(k, KEYLOK_GETVARWORD, addr, 0xecaf, 0x1256) & 0xffffu;
}

int main(void)
{
    keylok_t k;
    unsigned cmd, a2, a3, i;

    printf("keylok: the key's model against snow.exe's own checks\n");

    /* 1. The boot sequence, then the four reads at 0x414d80 that decide
     *    whether the game sets its lockout. */
    keylok_reset(&k, 0x0056435Au);
    game_boot(&k, 0x1234);
    check(word(&k, 0) == 'P', "0x414d90: word 0 reads back 'P'");
    check(word(&k, 2) == 'l', "0x414db5: word 2 reads back 'l'");
    check(word(&k, 4) == 'a', "0x414dec: word 4 reads back 'a'");
    check(word(&k, 6) == 'y', "0x414e20: word 6 reads back 'y'");
    check(word(&k, 6) == 0x79, "0x43ff30: word 6 is 0x79");

    /* 2. The same four before the boot sequence has run: a key arrives
     *    programmed, so a read that happens first must still answer. */
    keylok_reset(&k, 0x0056435Au);
    check(word(&k, 0) == 'P' && word(&k, 2) == 'l' && word(&k, 4) == 'a' &&
          word(&k, 6) == 'y', "a key reads 'Play' before the game writes it");

    /* 3. 0x43f87b and 0x43fb22: word 4 read twice, exit(0) if they differ.
     *    Run it after a write to 50 with a different value each time, which
     *    is what the game interleaves. */
    keylok_reset(&k, 0x0056435Au);
    game_boot(&k, 0x1234);
    for (i = 0; i < 8; i++) {
        unsigned first, second;

        keylok_call(&k, KEYLOK_WRITEVARWORD, 0x32, i * 7919u, 0xfafa);
        first = word(&k, 4);
        second = word(&k, 4);
        check(first == second, "0x43f896: word 4 reads the same twice");
    }

    /* 4. 0x4401f0 and 0x440938: the serial, read twice with different
     *    arguments. A difference adds rand() to the game-state index. */
    keylok_reset(&k, 0x0056435Au);
    check((keylok_call(&k, KEYLOK_GETSN, 0x4d, 0, 0xface) & 0xffffu) ==
          (keylok_call(&k, KEYLOK_GETSN, 0x78, 0xbbbb, 0xface) & 0xffffu),
          "0x440938: the serial is the same both times");

    /* 5. The challenge, from a fresh key and from a used one; and enough
     *    times to walk the challenge generator, because a challenge whose
     *    transform collided with a command number would be answered as a
     *    command. */
    keylok_reset(&k, 0x0056435Au);
    for (i = 0; i < 2000; i++)
        check(game_challenge_passes(&k), "0x44efee: the challenge is answered");
    check(k.answered == 2000 && k.refused == 0,
          "every challenge was answered and none refused");

    /* 6. A key must refuse a response that is not the one its challenge
     *    demands -- otherwise the model is a rubber stamp and case 5 proves
     *    nothing. */
    keylok_reset(&k, 0x0056435Au);
    keylok_call(&k, KEYLOK_CHECK, 0xaaaa, 0x9c88, 0x162a);
    check(keylok_expected(&k, &cmd, &a2, &a3), "a challenge is open");
    check(keylok_call(&k, cmd ^ 1u, a2, a3, 0) == 0,
          "a wrong command word is refused");
    keylok_call(&k, KEYLOK_CHECK, 0xaaaa, 0x9c88, 0x162a);
    keylok_expected(&k, &cmd, &a2, &a3);
    check(keylok_call(&k, cmd, a2 ^ 0x100u, a3, 0) == 0,
          "a wrong arg2 is refused");
    keylok_call(&k, KEYLOK_CHECK, 0xaaaa, 0x9c88, 0x162a);
    keylok_expected(&k, &cmd, &a2, &a3);
    check(keylok_call(&k, cmd, a2, a3 ^ 0x4000u, 0) == 0,
          "a wrong arg3 is refused");
    check(k.refused == 3 && k.answered == 0, "all three were counted refused");

    /* 7. The answer must never be the sentinel the client reads as "the
     *    device went away" (snow.exe 0x401155), for any call it makes. */
    keylok_reset(&k, 0x0056435Au);
    check(keylok_call(&k, KEYLOK_GETSN, 0, 0, 0) != 0xffa00000u,
          "the serial is not the device-lost sentinel");
    for (i = 0; i < KEYLOK_WORDS; i++) {
        keylok_call(&k, KEYLOK_WRITEVARWORD, i, 0xffff, 0);
        check(keylok_call(&k, KEYLOK_GETVARWORD, i, 0, 0) != 0xffa00000u,
              "a word is not the device-lost sentinel");
    }

    /* 8. Addresses the key does not have. KEYLOK II has 56 words. */
    keylok_reset(&k, 0x0056435Au);
    check(keylok_call(&k, KEYLOK_WRITEVARWORD, KEYLOK_WORDS, 0x1234, 0) == 0,
          "a write past the last word is refused");
    check(keylok_call(&k, KEYLOK_GETVARWORD, KEYLOK_WORDS, 0, 0) == 0,
          "a read past the last word is refused");
    check(keylok_call(&k, KEYLOK_GETVARWORD, 0xffff, 0, 0) == 0,
          "a read far past the last word is refused");

    printf(s_failures ? "keylok: %d FAILURE(S)\n" : "keylok: all checks pass\n",
           s_failures);
    return s_failures ? 1 : 0;
}
