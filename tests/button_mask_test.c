/* button_mask_test.c -- the 128-button mask, against the 32-bit code it replaced.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Widening host_state_t.buttons to uint32_t[4] rewrote io_board.c's pad_down(),
 * which is on the play path of every user of this project, and that rewrite
 * cannot be exercised without hardware. So it is pinned here against THE CODE
 * IT REPLACED. No game data, no device, no profile: io.h's accessors are inline
 * and pure.
 */
#include <stdio.h>
#include <string.h>

#include "../src/io.h"

static int s_failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        s_failures++;
    }
}

/* Exactly the expression io_board.c carried before D4. */
static int old_pad_down(unsigned buttons, unsigned ordinal)
{
    return ordinal >= 1 && ordinal <= 32 &&
           (buttons & (1u << (ordinal - 1))) != 0;
}

/* Case 1 is the whole point: for every ordinal 1..32, against every one of a
 * set of masks, the new accessor must answer exactly what
 *
 *     ordinal >= 1 && ordinal <= 32 && (buttons & (1u << (ordinal - 1)))
 *
 * answered. A difference there is a control that stops working on a wheel
 * with 32 buttons or fewer, a failure nobody would attribute to this
 * accessor. */
int main(void)
{
    /* Chosen to cover both ends, both edges of the word, a lone bit, an
     * alternating pattern and the two saturating cases. */
    static const unsigned k_masks[] = {
        0x00000000u, 0xFFFFFFFFu, 0x00000001u, 0x80000000u,
        0xAAAAAAAAu, 0x55555555u, 0x00008000u, 0x0000FFFFu
    };
    uint32_t mask[IO_BUTTON_WORDS];
    unsigned m, i;
    int b;

    printf("button mask tests\n");

    /* 1. THE REGRESSION THAT MATTERS: identical to the old code, 1..32. */
    for (m = 0; m < sizeof k_masks / sizeof k_masks[0]; m++) {
        memset(mask, 0, sizeof mask);
        mask[0] = k_masks[m];
        for (i = 1; i <= 32; i++)
            check(io_button_down(mask, i) == old_pad_down(k_masks[m], i),
                  "io_button_down disagrees with the pre-D4 expression");
    }

    /* 2. Zero is unbound, and out of range is DOWN=false rather than a read
     *    off the end of the array. An ordinal comes out of the user's ini. */
    memset(mask, 0xFF, sizeof mask);
    check(!io_button_down(mask, 0), "ordinal 0 must be unbound");
    check(!io_button_down(mask, IO_MAX_BUTTONS + 1), "past the end must be false");
    check(!io_button_down(mask, 0xFFFFFFFFu), "a huge ordinal must be false");
    check(io_button_down(mask, IO_MAX_BUTTONS), "the last button must be readable");

    /* 3. Every ordinal in range round-trips through set/down, one at a time,
     *    and sets exactly one bit. This is what D4 bought. */
    for (i = 1; i <= IO_MAX_BUTTONS; i++) {
        unsigned n = 0;
        memset(mask, 0, sizeof mask);
        io_button_set(mask, i - 1);          /* set is ZERO-based */
        check(io_button_down(mask, i), "set then down must agree");
        for (b = 1; b <= IO_MAX_BUTTONS; b++)
            if (io_button_down(mask, (unsigned)b))
                n++;
        check(n == 1, "one set button must light exactly one ordinal");
    }

    /* 4. io_button_set must refuse an out-of-range index rather than write
     *    past the array; nothing may be set by it. */
    memset(mask, 0, sizeof mask);
    io_button_set(mask, IO_MAX_BUTTONS);
    io_button_set(mask, IO_MAX_BUTTONS + 1000);
    check(!io_buttons_any(mask), "an out-of-range set must set nothing");

    /* 5. io_buttons_any sees a button in EVERY word, not just the first. The
     *    launcher arms its button capture on this, so a word it cannot see is
     *    a binding that can never be captured. */
    for (i = 0; i < IO_BUTTON_WORDS; i++) {
        memset(mask, 0, sizeof mask);
        mask[i] = 1u;
        check(io_buttons_any(mask), "io_buttons_any must see every word");
    }
    memset(mask, 0, sizeof mask);
    check(!io_buttons_any(mask), "an empty mask must report nothing pressed");

    /* 6. The 33rd button, which is the whole reason for the accessor: a Pro DD
     *    reports 92 and every one of them must be reachable. */
    memset(mask, 0, sizeof mask);
    io_button_set(mask, 32);                  /* ordinal 33 */
    check(io_button_down(mask, 33), "button 33 must be readable");
    check(!old_pad_down(mask[0], 33), "and the old code could not read it");
    io_button_set(mask, 91);                  /* ordinal 92, the Pro DD's top */
    check(io_button_down(mask, 92), "button 92 must be readable");

    /* ---- Phase 4: one action, several devices --------------------------
     *
     * io_pad_down() replaced an io_button_down() call on every user's play
     * path, and the control is the same one case 1 uses: THE CODE IT REPLACED.
     * A binding of one unlabelled reference is what every file written before
     * Phase 4 resolves to, and it must answer exactly what the single mask and
     * single ordinal answered. */
    {
        uint32_t buttons[IO_MAX_SLOTS][IO_BUTTON_WORDS];
        io_pad_bind_t bind;

        /* 7. THE ADDITIVE CASE. One reference, no label, slot 0: identical to
         *    io_button_down() on the primary's mask, for every ordinal and
         *    every mask; including the ordinals that are out of range. */
        for (m = 0; m < sizeof k_masks / sizeof k_masks[0]; m++) {
            memset(buttons, 0, sizeof buttons);
            buttons[0][0] = k_masks[m];
            /* Every other slot holds the COMPLEMENT, so a binding that reads
             * the wrong slot cannot agree by accident. */
            memset(buttons[1], 0xFF, sizeof buttons[0]);
            memset(buttons[2], 0xFF, sizeof buttons[0]);
            memset(buttons[3], 0xFF, sizeof buttons[0]);
            for (i = 1; i <= 32; i++) {
                memset(&bind, 0, sizeof bind);
                bind.ref[0].ordinal = i;
                bind.ref[0].resolved = 1;
                bind.n = 1;
                check(io_pad_down(buttons, &bind) ==
                      old_pad_down(k_masks[m], i),
                      "one unlabelled reference must answer what the single "
                      "mask and single ordinal answered");
            }
        }

        /* 8. A reference reads ITS OWN SLOT and no other. */
        for (i = 0; i < IO_MAX_SLOTS; i++) {
            unsigned s;
            memset(buttons, 0, sizeof buttons);
            io_button_set(buttons[i], 0);            /* ordinal 1 of slot i */
            for (s = 0; s < IO_MAX_SLOTS; s++) {
                memset(&bind, 0, sizeof bind);
                bind.ref[0].slot = (unsigned char)s;
                bind.ref[0].ordinal = 1;
                bind.ref[0].resolved = 1;
                bind.ref[0].label[0] = s ? 'a' : '\0';
                bind.n = 1;
                check(io_pad_down(buttons, &bind) == (s == i),
                      "a reference must read its own slot and no other");
            }
        }

        /* 9. ANY of its references. `boost = 9, pad:1` is down when either is. */
        memset(&bind, 0, sizeof bind);
        bind.ref[0].ordinal = 9;  bind.ref[0].resolved = 1;
        bind.ref[1].ordinal = 1;  bind.ref[1].resolved = 1;
        bind.ref[1].slot = 1;     bind.ref[1].label[0] = 'a';
        bind.n = 2;
        memset(buttons, 0, sizeof buttons);
        check(!io_pad_down(buttons, &bind), "neither pressed is not down");
        io_button_set(buttons[0], 8);                 /* the wheel's button 9 */
        check(io_pad_down(buttons, &bind), "the first half is enough");
        memset(buttons, 0, sizeof buttons);
        io_button_set(buttons[1], 0);                 /* the shifter's 1 */
        check(io_pad_down(buttons, &bind), "and so is the second");
        io_button_set(buttons[0], 8);
        check(io_pad_down(buttons, &bind), "and both together is still down");

        /* 10. AN UNRESOLVED LABEL IS NOT SLOT 0, and this is the case that
         *     would be a real defect rather than a cosmetic one: a shifter
         *     that is not plugged in must leave its gear UNBOUND, never
         *     silently become that button number on the wheel. */
        memset(buttons, 0xFF, sizeof buttons);
        memset(&bind, 0, sizeof bind);
        bind.ref[0].label[0] = 'a';
        bind.ref[0].ordinal = 1;
        bind.ref[0].resolved = 0;
        bind.n = 1;
        check(!io_pad_down(buttons, &bind),
              "a label that resolved to nothing must read as unbound, with "
              "every button on every device held down");
        bind.ref[0].resolved = 1;
        check(io_pad_down(buttons, &bind),
              "and the same reference resolved must read");

        /* 11. io_pad_bound is about the FILE, not about the hand: it is what
         *     keeps the shifter latch out of the way when neither paddle is
         *     bound, and it must not be fooled by a reference of ordinal 0. */
        memset(&bind, 0, sizeof bind);
        check(!io_pad_bound(&bind), "an empty binding is not bound");
        bind.ref[0].ordinal = 0; bind.ref[0].resolved = 1; bind.n = 1;
        check(!io_pad_bound(&bind), "a reference to button 0 is not bound");
        bind.ref[1].ordinal = 5; bind.ref[1].resolved = 1; bind.n = 2;
        check(io_pad_bound(&bind), "and one real reference is");

        /* 12. `n` is the bound, not the array: a struct that claims more
         *     references than it has must not read past them. Nothing is set
         *     beyond ref[0], so a reader running off the end would find the
         *     zeroes and answer the same, which is why ordinal 1 of slot 0
         *     is held down and only the FIRST reference names it. */
        memset(buttons, 0, sizeof buttons);
        io_button_set(buttons[0], 0);
        memset(&bind, 0, sizeof bind);
        bind.ref[0].ordinal = 1; bind.ref[0].resolved = 1;
        bind.n = IO_MAX_PAD_REFS + 4;            /* a lie, and it is clamped */
        check(io_pad_down(buttons, &bind),
              "an n past IO_MAX_PAD_REFS is clamped rather than walked");
    }

    if (s_failures) {
        printf("button mask tests: %d FAILED\n", s_failures);
        return 1;
    }
    printf("button mask tests: all passed\n");
    return 0;
}
