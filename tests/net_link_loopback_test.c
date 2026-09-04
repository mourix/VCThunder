/* net_link_loopback_test.c -- does the single machine case work on Windows?
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * net_link.c carries 192.168.200.X on 127.0.0.X and leaves the ports alone,
 * which assumes Windows lets one process bind 127.0.0.11 and 127.0.0.12. That
 * is what this checks, over real sockets, because nothing about it can be
 * reasoned out of the two game binaries.
 *
 * It also covers the three things a --dry-run cannot reach, since the game only
 * creates a socket once its operator settings enable the link: the address
 * substitution, the broadcast fan-out including the skip of our own identity,
 * and the receive budget's WSAEWOULDBLOCK.
 *
 * No game data, no dump, no profile: the unit under test is included whole so
 * the test can reach its statics, and the host around it is stubbed.
 * /
 */
#include "../src/net_link.c"

#include <stdarg.h>
#include <stdio.h>

/* ---- the collaborators net_link.c reaches for --------------------------- */

shim_config_t g_cfg;
const game_profile_t *g_game;
log_level_t g_log_level = LOG_ERR;

static int s_show_log;

void log_printf(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    if (!s_show_log)
        return;
    (void)level;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

int game_require(const char *s, const char *w, uint32_t va)
{ (void)s; (void)w; return va != 0; }
void patch_jmp(uint32_t va, const void *target) { (void)va; (void)target; }

/* ---- the test ----------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#define PORT 2537u
#define UNIT(n) ipv4(192, 168, 200, (n))

/* Tell the synthetic device which unit this instance is, exactly as the game
 * does: read the block, put an address in it, write it back. */
static void become(unsigned octet)
{
    uint8_t copy[0x40];
    void *h = ov_ets_get_device_handle("ether0");
    void *cfg = ov_ets_get_device_cfg(h);
    uint32_t addr = UNIT(octet);

    check(h != NULL, "ether0 answers a handle");
    check(cfg != NULL, "ether0 answers a configuration block");
    memcpy(copy, cfg, sizeof copy);
    memcpy(copy + ETS_CFG_IP, &addr, sizeof addr);
    check(ov_ets_configure_device(h, copy, 0) == 0,
          "configure returns 0, which is the success Offroad tests");
    check(cfg_at(ETS_CFG_IP) == addr,
          "local mode adopts the address the game wrote");
    check(s_identity == addr, "the adopted address is this instance's identity");
}

static SOCKET open_unit(unsigned octet)
{
    struct sockaddr_in in;
    SOCKET s;

    u_long nonblocking = 1;

    become(octet);
    s = ov_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(s != INVALID_SOCKET, "socket created");
    if (s == INVALID_SOCKET)
        return s;
    /* Both titles do this themselves, through ioctlsocket, before they drain:
     * without it the reads below would block forever rather than report the
     * WSAEWOULDBLOCK the budget has to imitate. */
    ioctlsocket(s, FIONBIO, &nonblocking);

    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_port = (u_short)((PORT << 8) | (PORT >> 8));
    in.sin_addr.s_addr = UNIT(octet);
    check(ov_bind(s, (const struct sockaddr *)&in, (int)sizeof in) == 0,
          "the game's own 192.168.200.x:port binds");
    return s;
}

static int drain(SOCKET s, char *buf, int len)
{
    return ov_recvfrom(s, buf, len, 0, NULL, NULL);
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    SOCKET a, b;
    struct sockaddr_in to;
    char buf[64];
    int i, got;

    s_show_log = argc > 1 && strcmp(argv[1], "-v") == 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "FAIL: WSAStartup\n");
        return 1;
    }

    /* Off is off: the entry points must bind to the real ws2_32 exports, or
     * the trace would report a shim that does nothing. */
    snprintf(g_cfg.link, sizeof g_cfg.link, "%s", "off");
    check(net_link_preload() == 1, "preload succeeds with the link off");
    check(net_link_override("socket") == NULL,
          "link off owns no Winsock entry point");
    check(net_link_enabled() == 0, "link off reports itself off");

    snprintf(g_cfg.link, sizeof g_cfg.link, "%s", "local");
    g_cfg.link_rx_budget = 4;
    check(net_link_preload() == 1, "preload succeeds in local mode");
    check(net_link_enabled() == 1, "local mode reports itself on");
    check(net_link_override("socket") != NULL && net_link_override("bind") &&
          net_link_override("sendto") && net_link_override("recvfrom"),
          "local mode owns exactly the four Winsock entry points it needs");
    check(net_link_override("connect") == NULL, "and owns nothing else");

    /* Two units, one port, one machine. This is the claim. */
    a = open_unit(11);
    b = open_unit(12);
    if (a == INVALID_SOCKET || b == INVALID_SOCKET) {
        fprintf(stderr, "FAIL: two units could not both bind port %u\n", PORT);
        return 1;
    }

    /* We are unit 12 now, so a broadcast must reach 11 and must not come back
     * to us: Hydro drops a message whose source address is its own, so a
     * fan-out that included ourselves would look like a working link that
     * silently ignores half its traffic. */
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = (u_short)((PORT << 8) | (PORT >> 8));
    to.sin_addr.s_addr = 0xffffffffu;
    check(ov_sendto(b, "broadcast", 9, 0, (const struct sockaddr *)&to,
                    (int)sizeof to) == 9, "a broadcast reports its own length");
    Sleep(50);
    got = drain(a, buf, (int)sizeof buf);
    check(got == 9 && memcmp(buf, "broadcast", 9) == 0,
          "unit 11 receives unit 12's broadcast");
    check(drain(a, buf, (int)sizeof buf) < 0,
          "and receives it exactly once");
    check(drain(b, buf, (int)sizeof buf) < 0,
          "unit 12 does not receive its own broadcast");

    /* Unicast, which is the part two machines never need: both games resolve a
     * single destination to a fabricated address that is not on this box. */
    become(11);
    to.sin_addr.s_addr = UNIT(12);
    check(ov_sendto(a, "unicast", 7, 0, (const struct sockaddr *)&to,
                    (int)sizeof to) == 7, "a unicast is sent");
    Sleep(50);
    got = drain(b, buf, (int)sizeof buf);
    check(got == 7 && memcmp(buf, "unicast", 7) == 0,
          "unit 12 receives a unicast addressed to 192.168.200.12");

    /* The receive budget, which is 4 for this test. Hydro's drain has no
     * upper bound of its own, so the answer that stops it has to be the one
     * its loop already handles. */
    net_link_frame();
    for (i = 0; i < 6; i++)
        ov_sendto(a, "x", 1, 0, (const struct sockaddr *)&to, (int)sizeof to);
    Sleep(50);
    for (i = 0; i < 4; i++)
        check(drain(b, buf, (int)sizeof buf) == 1,
              "the budget passes the datagrams it allows");
    check(drain(b, buf, (int)sizeof buf) < 0 &&
          WSAGetLastError() == WSAEWOULDBLOCK,
          "and answers WSAEWOULDBLOCK once it is spent");
    net_link_frame();
    check(drain(b, buf, (int)sizeof buf) == 1,
          "the next frame resumes the drain rather than dropping it");

    closesocket(a);
    closesocket(b);
    WSACleanup();

    if (failures) {
        fprintf(stderr, "net_link loopback tests: %d FAILED\n", failures);
        return 1;
    }
    printf("net_link loopback tests: all passed\n");
    return 0;
}
