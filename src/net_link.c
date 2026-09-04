/* net_link.c -- the arcade link: a synthetic ether0 and four Winsock entries.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Both titles speak ordinary IPv4 UDP, from the game thread, inside the frame,
 * above a Winsock boundary this host patches onto real ws2_32.dll. So nothing
 * in this file carries a packet, parses a message or knows an opcode. It
 * supplies the four things that were missing, and they are all small:
 *
 *   1. ether0 itself. Both link stacks ask ETS for the device by name before
 *      they open a socket, and its whole surface is three calls: name to
 *      handle, read a 0x3a byte configuration block, write it back. None of
 *      them touches a packet, which is why a shim can answer them and an
 *      NE2000 is not needed.
 *   2. SO_BROADCAST, without which Windows answers WSAEACCES to the broadcast
 *      both titles send in their FIRST packet and the link never discovers.
 *   3. A wildcard bind. The game's idea of its own address comes from the
 *      configuration block, never from bind, so rewriting sin_addr and leaving
 *      the port alone is invisible to it.
 *   4. A receive budget, because Hydro drains until WSAEWOULDBLOCK with no
 *      upper bound, inside the frame.
 *
 * The fifth thing is the single machine case, and it is one substitution: in
 * `local` mode this device ADOPTS the address the game wrote into it, so two
 * instances have the distinct identities the protocols require, and
 * 192.168.200.X is carried on 127.0.0.X. See net_local_map() below.
 *
 * The instrumentation is cheap and stays: the resolved identity once, every ETS
 * answer once, every bind with BOTH addresses, and a per-interval census of
 * datagrams in, out, fanned out and refused. Read those lines before believing
 * a link formed. tests/net_link_loopback_test.c covers the same ground with no
 * game at all, so a regression here fails `make check`.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

/* off is what every build before this one did, and it stays the default. */
typedef enum { LINK_OFF = 0, LINK_LAN, LINK_LOCAL } link_mode_t;

static link_mode_t s_mode;

/* The real ws2_32 exports our four overrides call through. Resolved in
 * net_link_preload(), because win32_bind()'s resolver would hand back the
 * overrides themselves. */
static SOCKET (WINAPI *s_socket)(int, int, int);
static int (WINAPI *s_bind)(SOCKET, const struct sockaddr *, int);
static int (WINAPI *s_sendto)(SOCKET, const char *, int, int,
                              const struct sockaddr *, int);
static int (WINAPI *s_recvfrom)(SOCKET, char *, int, int,
                                struct sockaddr *, int *);
static int (WINAPI *s_setsockopt)(SOCKET, int, int, const char *, int);
static void (WINAPI *s_wsa_set_last_error)(int);
static int (WINAPI *s_wsa_ioctl)(SOCKET, DWORD, void *, DWORD, void *, DWORD,
                                 DWORD *, void *, void *);

/* Windows reports an ICMP port-unreachable provoked by a DATAGRAM WE SENT as
 * WSAECONNRESET on the next receive on the sending socket. Both titles' drain
 * loops treat anything that is not WSAEWOULDBLOCK as a failure of the link, so
 * one absent unit would look like a broken one: `local` mode addresses every
 * unit the protocols allow whether or not an instance is running as it, and
 * `lan` unicasts to a peer that has quit. Turning the report off is what a UDP
 * server on Windows does; it is not specific to this project. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

typedef ULONG (WINAPI *get_adapters_info_fn)(PIP_ADAPTER_INFO, PULONG);

/* This machine's own adapter, read once before the game image lands on our PE
 * headers. Network byte order, as they sit in a sockaddr_in. */
static uint32_t s_host_addr, s_host_mask;
static uint8_t  s_host_mac[6];
static char     s_host_adapter[MAX_ADAPTER_DESCRIPTION_LENGTH + 8];

/* The device. The handle is this object's address: the games test it against
 * 0, hand it straight back to us, and read nothing out of it. */
static uint8_t s_device;

/* The 0x3a byte configuration block ETS hands back. Both titles
 * copy 0xe dwords and a word out of it, so it must be at least 0x3a bytes and
 * everything in it that they do not read is honestly zero. */
#define ETS_CFG_BYTES 0x3a
#define ETS_CFG_IP    0x18
#define ETS_CFG_MASK  0x1c
#define ETS_CFG_MAC   0x34
static uint8_t s_cfg[0x40];

/* The address this instance believes it has: the host adapter's in `lan`, and
 * the one the game itself wrote in `local`. 0 until the device is configured. */
static uint32_t s_identity;

/* Per interval, so a first run says what it did rather than that it ran. */
static struct {
    unsigned handles, cfgs, configures;
    unsigned sockets, binds;
    unsigned sent, fanned, send_errors;
    unsigned received, budget_refusals;
} s_n;

static unsigned s_rx_this_frame;
static unsigned s_rx_budget = 64;

/* ---- small helpers ------------------------------------------------------ */

/* sin_port is network order. Swapped here rather than through ntohs so this
 * file adds no import to the host: docs/building.md's rule about the DLL's
 * import table is not only about dinput8. */
static unsigned port_of(const struct sockaddr_in *in)
{
    return (unsigned)(((in->sin_port >> 8) | (in->sin_port << 8)) & 0xffffu);
}

/* a.b.c.d as it sits in sockaddr_in.sin_addr: first octet in the low byte. */
static uint32_t ipv4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    return a | (b << 8) | (c << 16) | ((uint32_t)d << 24);
}

static const char *ip_str(uint32_t addr, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(addr & 0xff),
             (unsigned)((addr >> 8) & 0xff), (unsigned)((addr >> 16) & 0xff),
             (unsigned)((addr >> 24) & 0xff));
    return out;
}

/* "192.168.200.11" -> the same 4 bytes in the same order. 0 on anything that
 * is not four octets, which is a refusal and not an address. */
static uint32_t ip_parse(const char *s)
{
    unsigned o[4];
    int i;

    if (!s)
        return 0;
    for (i = 0; i < 4; i++) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255u)
                return 0;
        }
        if (!digits)
            return 0;
        o[i] = v;
        if (i < 3 && *s++ != '.')
            return 0;
    }
    return *s ? 0 : ipv4(o[0], o[1], o[2], o[3]);
}

static uint32_t cfg_at(unsigned off)
{
    uint32_t v;
    memcpy(&v, s_cfg + off, sizeof v);
    return v;
}

static void cfg_set(uint32_t addr, uint32_t mask)
{
    memcpy(s_cfg + ETS_CFG_IP, &addr, sizeof addr);
    memcpy(s_cfg + ETS_CFG_MASK, &mask, sizeof mask);
    memcpy(s_cfg + ETS_CFG_MAC, s_host_mac, sizeof s_host_mac);
}

int net_link_enabled(void)
{
    return s_mode != LINK_OFF;
}

/* ---- this machine's own address ----------------------------------------- */

/* The block the games read has to carry a REAL address, because in `lan` mode
 * it is the address peers will answer to and the one each machine uses to
 * ignore its own broadcasts. Renumbering a host NIC to 192.168.200.x is not
 * this program's business and is not needed: both protocols learn every peer's
 * address from the packets.
 *
 * GetAdaptersInfo rather than a connected socket's getsockname, because the
 * MAC and the mask are in the same answer and neither is available from a
 * socket. Resolved through GetProcAddress so the host gains no import: this
 * runs before game_map(), which is the last moment LoadLibrary works at all. */
static int resolve_host_address(void)
{
    HMODULE iph = LoadLibraryA("iphlpapi.dll");
    get_adapters_info_fn get_info;
    IP_ADAPTER_INFO *buf = NULL, *a, *chosen = NULL;
    ULONG len = 0;
    ULONG rc;

    if (!iph) {
        LOGE("link: iphlpapi.dll did not load (%lu); this machine's own "
             "address cannot be read", GetLastError());
        return 0;
    }
    get_info = (get_adapters_info_fn)(void *)
        GetProcAddress(iph, "GetAdaptersInfo");
    if (!get_info) {
        LOGE("link: iphlpapi.dll has no GetAdaptersInfo");
        return 0;
    }

    rc = get_info(NULL, &len);
    if (rc != ERROR_BUFFER_OVERFLOW && rc != ERROR_SUCCESS) {
        LOGE("link: GetAdaptersInfo sizing failed (%lu)", rc);
        return 0;
    }
    if (!len)
        len = 16u * 1024u;
    buf = (IP_ADAPTER_INFO *)HeapAlloc(GetProcessHeap(), 0, len);
    if (!buf) {
        LOGE("link: out of memory reading the adapter list");
        return 0;
    }
    rc = get_info(buf, &len);
    if (rc != ERROR_SUCCESS) {
        LOGE("link: GetAdaptersInfo failed (%lu)", rc);
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    /* First adapter with a real address; one with a gateway wins, because a
     * machine with a virtual switch or a VPN has several and only one of them
     * is on the broadcast domain a second cabinet would be on. */
    for (a = buf; a; a = a->Next) {
        uint32_t addr = ip_parse(a->IpAddressList.IpAddress.String);
        if (a->Type == MIB_IF_TYPE_LOOPBACK || !addr)
            continue;
        if (!chosen)
            chosen = a;
        if (ip_parse(a->GatewayList.IpAddress.String)) {
            chosen = a;
            break;
        }
    }
    if (chosen) {
        s_host_addr = ip_parse(chosen->IpAddressList.IpAddress.String);
        s_host_mask = ip_parse(chosen->IpAddressList.IpMask.String);
        if (chosen->AddressLength == sizeof s_host_mac)
            memcpy(s_host_mac, chosen->Address, sizeof s_host_mac);
        snprintf(s_host_adapter, sizeof s_host_adapter, "%s",
                 chosen->Description);
    }
    HeapFree(GetProcessHeap(), 0, buf);

    if (!s_host_addr) {
        LOGW("link: no adapter with an IPv4 address; falling back to "
             "127.0.0.1. Two machines cannot see each other over that, and "
             "`link = lan` will look exactly like a dead link.");
        s_host_addr = ipv4(127, 0, 0, 1);
        s_host_mask = ipv4(255, 0, 0, 0);
        snprintf(s_host_adapter, sizeof s_host_adapter, "%s", "(none)");
    }
    return 1;
}

/* ---- the synthetic ether0 ----------------------------------------------- */

/* All three run on the game's stack, under cdecl, exactly as the ETS entry
 * points they replace did, and under the same -O1 rule. */

static void *__cdecl ov_ets_get_device_handle(const char *name)
{
    int match = name && _stricmp(name, "ether0") == 0;

    if (!s_n.handles++)
        LOGI("link: ether0 requested as '%s': %s", name ? name : "(null)",
             match ? "answering with a synthetic device" :
                     "REFUSED, this host has only ether0");
    else
        LOGT("link: device '%s' -> %s", name ? name : "(null)",
             match ? "ether0" : "none");
    return match ? (void *)&s_device : NULL;
}

static void *__cdecl ov_ets_get_device_cfg(void *handle)
{
    char a[16], m[16];

    if (handle != (void *)&s_device) {
        LOGW("link: configuration asked for on handle %p, which is not "
             "ether0; answering 0 as ETS would", handle);
        return NULL;
    }
    if (!s_n.cfgs++)
        LOGI("link: ether0 configuration read: address %s mask %s, "
             "MAC %02X:%02X:%02X:%02X:%02X:%02X (%s)",
             ip_str(cfg_at(ETS_CFG_IP), a, sizeof a),
             ip_str(cfg_at(ETS_CFG_MASK), m, sizeof m),
             s_host_mac[0], s_host_mac[1], s_host_mac[2],
             s_host_mac[3], s_host_mac[4], s_host_mac[5], s_host_adapter);
    else
        LOGT("link: ether0 configuration read (%u)", s_n.cfgs);
    return s_cfg;
}

static int __cdecl ov_ets_configure_device(void *handle, const void *cfg,
                                           int flags)
{
    uint32_t wanted;
    char a[16], b[16];

    (void)flags;
    if (handle != (void *)&s_device || !cfg) {
        LOGW("link: configure called on handle %p, which is not ether0", handle);
        return 1;                       /* nonzero is the failure Offroad tests */
    }
    memcpy(&wanted, (const uint8_t *)cfg + ETS_CFG_IP, sizeof wanted);
    s_n.configures++;

    if (s_mode == LINK_LOCAL) {
        /* The identity is ours to invent, and this is where the game tells us
         * which one it wants: 192.168.200.<unit + 10>, from the operator
         * adjustment that is the single source of this cabinet's unit id.
         * Adopting it is what gives two instances on one machine the distinct
         * identities both protocols require; Hydro drops any message whose
         * source address equals its own, so two instances sharing one address
         * would ignore each other and look exactly like a dead link. */
        char c[16];

        cfg_set(wanted, ipv4(255, 255, 255, 0));
        s_identity = wanted;
        LOGI("link: ether0 adopts the address this instance's game chose, "
             "%s (was %s). local mode: this identity is carried on %s.",
             ip_str(wanted, a, sizeof a), ip_str(s_host_addr, b, sizeof b),
             ip_str(ipv4(127, 0, 0, (wanted >> 24) & 0xff), c, sizeof c));
    } else {
        /* Two machines on a LAN: keep this machine's real address, because it
         * is the one a peer's reply has to reach. The game's write is accepted
         * and not acted on, which is what the games' own protocol expects. */
        s_identity = s_host_addr;
        LOGI("link: the game asked ether0 for %s; keeping this machine's own "
             "address %s, which is what its peers must answer to. Both "
             "protocols learn peer addresses from the packets.",
             ip_str(wanted, a, sizeof a), ip_str(s_host_addr, b, sizeof b));
    }
    return 0;                           /* 0 is success; Offroad tests it */
}

/* ---- the Winsock overrides ---------------------------------------------- */

/* 192.168.200.X, the subnet both titles derive from the unit adjustment. */
static int is_link_subnet(uint32_t addr)
{
    return (addr & 0x00ffffffu) == ipv4(192, 168, 200, 0);
}

static int is_broadcast(uint32_t addr)
{
    return addr == 0xffffffffu ||
           (is_link_subnet(addr) && (addr >> 24) == 255u);
}

/* The single machine case, and it is one substitution: 192.168.200.X becomes
 * 127.0.0.X. Ports are untouched, so two instances never contend for one
 * address:port pair and no SO_REUSEADDR is needed; the second instance simply
 * binds a different loopback address. That is why this is a map rather than a
 * shared receive port: sharing one would make discovery depend on Windows
 * delivering a single broadcast to several sockets, which is a thing to check
 * rather than to assume, and this needs neither. */
static uint32_t net_local_map(uint32_t addr)
{
    return ipv4(127, 0, 0, (addr >> 24) & 0xff);
}

static SOCKET __stdcall ov_socket(int af, int type, int protocol)
{
    SOCKET s = s_socket(af, type, protocol);
    BOOL on = TRUE;

    if (s == INVALID_SOCKET || s_mode == LINK_OFF)
        return s;
    s_n.sockets++;
    if (s_wsa_ioctl) {
        DWORD returned = 0;
        BOOL off = FALSE;

        if (s_wsa_ioctl(s, SIO_UDP_CONNRESET, &off, (DWORD)sizeof off, NULL, 0,
                        &returned, NULL, NULL) != 0)
            LOGW("link: SIO_UDP_CONNRESET refused on socket %llu (%d); a "
                 "datagram to a unit that is not running can then be reported "
                 "as WSAECONNRESET and read as a dead link",
                 (unsigned long long)s, (int)GetLastError());
    }
    /* Where the socket is created is the only place this has to happen, and
     * it costs nothing on a socket that never broadcasts. */
    if (s_setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on,
                     (int)sizeof on) != 0)
        LOGW("link: SO_BROADCAST refused on socket %llu; the first discovery "
             "packet will fail with WSAEACCES and the game will report no "
             "peers", (unsigned long long)s);
    else
        LOGI("link: socket %llu created (af %d type %d proto %d), "
             "SO_BROADCAST set", (unsigned long long)s, af, type, protocol);
    return s;
}

static int __stdcall ov_bind(SOCKET s, const struct sockaddr *name, int namelen)
{
    struct sockaddr_in in;
    char was[16], now[16];
    uint32_t addr, replacement;
    int rc;

    if (s_mode == LINK_OFF || !name || namelen < (int)sizeof in ||
        name->sa_family != AF_INET)
        return s_bind(s, name, namelen);

    memcpy(&in, name, sizeof in);
    addr = in.sin_addr.s_addr;
    /* In `local` the address IS the identity, so bind it and let two
     * instances be told apart by the kernel; in `lan` bind the wildcard, so a
     * limited broadcast arrives on the socket the game bound to its own
     * unicast address. The game never reads either back: its own idea of its
     * address comes from the device block. */
    replacement = (s_mode == LINK_LOCAL && is_link_subnet(addr))
                      ? net_local_map(addr) : INADDR_ANY;
    in.sin_addr.s_addr = replacement;

    rc = s_bind(s, (const struct sockaddr *)&in, (int)sizeof in);
    s_n.binds++;
    if (rc != 0) {
        LOGE("link: bind of socket %llu to %s:%u (the game asked for %s) "
             "failed, WSA error %d. Two instances with the SAME unit id in "
             "their operator settings is what this looks like.",
             (unsigned long long)s, ip_str(replacement, now, sizeof now),
             port_of(&in), ip_str(addr, was, sizeof was),
             (int)GetLastError());
    } else {
        LOGI("link: socket %llu bound %s:%u (the game asked for %s:%u)",
             (unsigned long long)s, ip_str(replacement, now, sizeof now),
             port_of(&in), ip_str(addr, was, sizeof was),
             port_of(&in));
    }
    return rc;
}

static int __stdcall ov_sendto(SOCKET s, const char *buf, int len, int flags,
                               const struct sockaddr *to, int tolen)
{
    struct sockaddr_in in;
    uint32_t addr;
    char a[16], b[16];
    int rc;

    if (s_mode == LINK_OFF || !to || tolen < (int)sizeof in ||
        to->sa_family != AF_INET)
        return s_sendto(s, buf, len, flags, to, tolen);

    memcpy(&in, to, sizeof in);
    addr = in.sin_addr.s_addr;

    if (s_mode != LINK_LOCAL) {
        rc = s_sendto(s, buf, len, flags, to, tolen);
        if (rc < 0)
            s_n.send_errors++;
        else
            s_n.sent++;
        LOGT("link: %d bytes -> %s:%u (rc %d)", len,
             ip_str(addr, a, sizeof a), port_of(&in), rc);
        return rc;
    }

    /* One machine. A broadcast has to become one datagram per unit, because
     * nothing on this box is listening on a broadcast address any more: the
     * four unit addresses are the whole membership both protocols allow, and
     * .10 is included because a unit adjustment of 0 is what an unconfigured
     * cabinet has. Our own address is skipped, which is what the wire would
     * have done for us. */
    if (is_broadcast(addr)) {
        unsigned octet;
        int last = 0;

        for (octet = 10; octet <= 14; octet++) {
            uint32_t peer = ipv4(192, 168, 200, octet);

            if (peer == s_identity)
                continue;
            in.sin_addr.s_addr = net_local_map(peer);
            last = s_sendto(s, buf, len, flags,
                            (const struct sockaddr *)&in, (int)sizeof in);
            if (last < 0)
                s_n.send_errors++;
            else
                s_n.fanned++;
        }
        LOGT("link: %d bytes broadcast -> 127.0.0.10..14 minus self (rc %d)",
             len, last);
        return last < 0 ? last : len;
    }

    in.sin_addr.s_addr = is_link_subnet(addr) ? net_local_map(addr) : addr;
    rc = s_sendto(s, buf, len, flags, (const struct sockaddr *)&in,
                  (int)sizeof in);
    if (rc < 0)
        s_n.send_errors++;
    else
        s_n.sent++;
    LOGT("link: %d bytes -> %s:%u (was %s, rc %d)", len,
         ip_str(in.sin_addr.s_addr, a, sizeof a),
         port_of(&in), ip_str(addr, b, sizeof b), rc);
    return rc;
}

static int __stdcall ov_recvfrom(SOCKET s, char *buf, int len, int flags,
                                 struct sockaddr *from, int *fromlen)
{
    int rc;

    /* Hydro's _netdriver_Recieve drains until WSAEWOULDBLOCK with no upper
     * bound of its own, inside the frame, so a peer that can produce
     * datagrams faster than this machine consumes them stops the frame rather
     * than the link. Since this host owns the entry point, the bound costs no
     * game code: WSAEWOULDBLOCK is the answer the loop already handles. */
    if (s_mode != LINK_OFF && s_rx_this_frame >= s_rx_budget) {
        s_n.budget_refusals++;
        s_wsa_set_last_error(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    rc = s_recvfrom(s, buf, len, flags, from, fromlen);
    if (rc >= 0 && s_mode != LINK_OFF) {
        s_rx_this_frame++;
        s_n.received++;
        LOGT("link: %d bytes received on socket %llu", rc,
             (unsigned long long)s);
    }
    return rc;
}

void *net_link_override(const char *base)
{
    /* With the link off these are not ours: let the entry points bind to the
     * real ws2_32 exports, so the trace says what actually runs. A
     * passthrough that logs as a shim is an instrument reporting something it
     * never did, and the mode is settled by net_link_preload(), which runs
     * before win32_bind(). */
    if (!base || !s_socket || s_mode == LINK_OFF)
        return NULL;
    if (strcmp(base, "socket") == 0)   return (void *)ov_socket;
    if (strcmp(base, "bind") == 0)     return (void *)ov_bind;
    if (strcmp(base, "sendto") == 0)   return (void *)ov_sendto;
    if (strcmp(base, "recvfrom") == 0) return (void *)ov_recvfrom;
    return NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

void net_link_frame(void)
{
    s_rx_this_frame = 0;
}

void net_link_report(unsigned frames)
{
    if (s_mode == LINK_OFF || !(s_n.sent || s_n.fanned || s_n.received ||
                                s_n.send_errors || s_n.budget_refusals))
        return;
    LOGI("link: over %u frames: %u sent, %u fanned out, %u received, "
         "%u send errors, %u receives refused by the %u/frame budget",
         frames, s_n.sent, s_n.fanned, s_n.received, s_n.send_errors,
         s_n.budget_refusals, s_rx_budget);
    s_n.sent = s_n.fanned = s_n.received = 0;
    s_n.send_errors = s_n.budget_refusals = 0;
}

int net_link_preload(void)
{
    HMODULE ws2 = LoadLibraryA("ws2_32.dll");

    if (_stricmp(g_cfg.link, "lan") == 0)
        s_mode = LINK_LAN;
    else if (_stricmp(g_cfg.link, "local") == 0)
        s_mode = LINK_LOCAL;
    else
        s_mode = LINK_OFF;
    if (g_cfg.link_rx_budget)
        s_rx_budget = g_cfg.link_rx_budget;

    /* Resolved whatever the mode is: the overrides are bound by win32_bind()
     * either way and a null passthrough would be a fault, not a no-op. */
    if (!ws2) {
        LOGE("link: ws2_32.dll did not load (%lu)", GetLastError());
        s_mode = LINK_OFF;
        return 0;
    }
    s_socket = (SOCKET (WINAPI *)(int, int, int))(void *)
        GetProcAddress(ws2, "socket");
    s_bind = (int (WINAPI *)(SOCKET, const struct sockaddr *, int))(void *)
        GetProcAddress(ws2, "bind");
    s_sendto = (int (WINAPI *)(SOCKET, const char *, int, int,
                               const struct sockaddr *, int))(void *)
        GetProcAddress(ws2, "sendto");
    s_recvfrom = (int (WINAPI *)(SOCKET, char *, int, int,
                                 struct sockaddr *, int *))(void *)
        GetProcAddress(ws2, "recvfrom");
    s_setsockopt = (int (WINAPI *)(SOCKET, int, int, const char *, int))(void *)
        GetProcAddress(ws2, "setsockopt");
    s_wsa_set_last_error = (void (WINAPI *)(int))(void *)
        GetProcAddress(ws2, "WSASetLastError");
    /* Optional: without it a missing unit is reported as a dead link rather
     * than as an absent one, which is worse but not fatal. */
    s_wsa_ioctl = (int (WINAPI *)(SOCKET, DWORD, void *, DWORD, void *, DWORD,
                                  DWORD *, void *, void *))(void *)
        GetProcAddress(ws2, "WSAIoctl");
    if (!s_wsa_ioctl)
        LOGW("link: ws2_32.dll has no WSAIoctl; SIO_UDP_CONNRESET cannot be "
             "cleared");
    if (!s_socket || !s_bind || !s_sendto || !s_recvfrom || !s_setsockopt ||
        !s_wsa_set_last_error) {
        LOGE("link: ws2_32.dll is missing one of socket/bind/sendto/recvfrom/"
             "setsockopt/WSASetLastError; the link stays off");
        s_socket = NULL;
        s_mode = LINK_OFF;
        return 0;
    }

    if (s_mode == LINK_OFF) {
        LOGI("link: [link] link = off. No ether0, and Offroad is held in its "
             "supported standalone state, which is what every build before "
             "this one did.");
        return 1;
    }
    if (!resolve_host_address()) {
        LOGE("link: this machine's own address could not be read; the link "
             "stays off rather than answering ether0 with a guess");
        s_mode = LINK_OFF;
        return 0;
    }
    cfg_set(s_host_addr, s_host_mask);
    return 1;
}

int net_link_install(void)
{
    char a[16], m[16];

    if (s_mode == LINK_OFF)
        return 0;
    if (!game_require("link", "_EtsTCPGetDeviceHandle / GetDeviceCfg / "
                      "ConfigureDevice", G->ets_tcp_get_device_handle &&
                      G->ets_tcp_get_device_cfg &&
                      G->ets_tcp_configure_device)) {
        s_mode = LINK_OFF;
        return 0;
    }
    patch_jmp(G->ets_tcp_get_device_handle, (void *)ov_ets_get_device_handle);
    patch_jmp(G->ets_tcp_get_device_cfg, (void *)ov_ets_get_device_cfg);
    patch_jmp(G->ets_tcp_configure_device, (void *)ov_ets_configure_device);

    LOGI("link: %s mode. ether0 is synthetic (3 ETS calls at 0x%08X/0x%08X/"
         "0x%08X); this machine is %s mask %s on '%s'; receive budget %u "
         "datagrams per frame.",
         s_mode == LINK_LOCAL ? "local" : "lan",
         G->ets_tcp_get_device_handle, G->ets_tcp_get_device_cfg,
         G->ets_tcp_configure_device,
         ip_str(s_host_addr, a, sizeof a), ip_str(s_host_mask, m, sizeof m),
         s_host_adapter, s_rx_budget);
    LOGI("link: whether the link runs at all is still the GAME'S operator "
         "setting, in its own service menu, and so is this cabinet's unit id: "
         "this key selects a transport, not a cabinet number. Read the 'link:' "
         "lines below and the per-interval census before believing a link "
         "formed.");
    if (s_mode == LINK_LOCAL)
        LOGI("link: local mode carries 192.168.200.X on 127.0.0.X with the "
             "ports untouched, so a second instance needs its own unit id in "
             "its own operator settings, which means its own --save "
             "directory, and its own --log file.");
    return 1;
}
