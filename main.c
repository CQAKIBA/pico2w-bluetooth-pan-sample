/*
 * Pico 2 W Bluetooth PANU -> phone NAP -> DHCP -> HTTP -> APRS-IS test.
 * PAN, DHCP, HTTP and APRS-IS receive stages confirmed by user (SDK 2.3.1).
 * v0.8: automatically restore the bonded phone from BTstack flash TLV after HCI starts.
 * v0.7 DHCP delay/diagnostics retained to keep the known working path unchanged.
 * v0.6 reconnection additions have not yet been hardware-tested.
 * Requires Pico SDK 2.3.x with bundled BTstack and lwIP.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "bnep_lwip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"

#define PROBE_HOST "example.com"
#define PROBE_PATH "/"
/* Generate a per-device receive-only login name: P + last 3 Bluetooth MAC bytes.
 * NOTE: 24 MAC-derived bits are not guaranteed globally unique. Use a callsign
 * where appropriate. pass -1 is receive-only/unverified, never transmit.
 */
#define APRS_HOST "rotate.aprs2.net"
#define APRS_PORT 14580
/* Approximate geographic coverage of Japan, including Okinawa, Ogasawara,
 * and Minamitorishima. These rectangles also include adjacent countries/sea.
 * Multiple APRS-IS filter terms are combined with OR.
 */
#define APRS_FILTER "a/46/129/30/150 a/31/122/24/133 a/29/134/20/143 a/26/151/23/155"
#define RETRY_MS 5000u           /* PAN/HTTP/APRS retries; no retry count limit */
#define HTTP_TIMEOUT_MS 15000u
#define APRS_CONNECT_TIMEOUT_MS 20000u
#define DHCP_RESTART_MS 30000u
/* Diagnosis: separate remote PAN teardown from reaction to first DHCP frame.
 * Change to 0 for immediate DHCP behavior as in v0.6.
 */
#define DHCP_START_DELAY_MS 2000u
/* aprsc usually sends a comment/keepalive line about every 20s.
 * A 30s gap is suspicious; 60s is treated as a stalled application stream.
 * Other server implementations may not emit regular # aprsc lines, so
 * measure ANY received TCP application bytes, not just # aprsc comments.
 */
#define APRS_SILENCE_WARN_MS 30000u
#define APRS_SILENCE_RECONNECT_MS 60000u
#define STATUS_INTERVAL_MS (60u * 1000u)
#define APRS_PRINT_PACKETS 1   /* Set to 0 to count without serial packet dumps */
#define APRS_LINE_MAX 550
#define BNEP_FRAME_SIZE 1691
#define PAN_PSM 0x000f
/* Several <12s BNEP sessions after a phone BT restart suggest a stale ACL or
 * phone-side tether permission/state problem. Try one fresh ACL after 3 flaps.
 * Rate-limit full ACL resets to avoid continually interrupting phone settings.
 */
#define PAN_SHORT_SESSION_MS 12000u
#define PAN_ACL_RESET_AFTER 3u
#define PAN_ACL_RESET_COOLDOWN_MS 120000u
#define PAN_ACL_RESET_RETRY_MS 10000u
#define POLL_MS 500

static uint8_t sdp_record[220];
static btstack_packet_callback_registration_t hci_events;
static btstack_timer_source_t poll_timer;
static bd_addr_t phone_addr;
static bool have_phone, bt_ready, sdp_busy, bnep_busy, pan_up, dhcp_ok;
static bool phone_restored_at_boot;
static unsigned saved_bond_count;
static bool dhcp_pending, dhcp_started;
static uint32_t dhcp_due_ms;
static bool nap_found, probe_started, probe_finished, probe_ok;
static uint32_t next_sdp_at_ms, connected_at_ms, probe_at_ms, http_next_retry_ms;
static uint32_t next_status_ms, boot_ms;
static uint32_t pan_attempts, pan_connections;
static hci_con_handle_t phone_acl_handle = HCI_CON_HANDLE_INVALID;
static uint32_t pan_short_sessions, pan_short_streak, pan_acl_resets;
static uint32_t last_acl_reset_ms;
static bool ever_acl_reset;

static struct tcp_pcb *http_pcb;
static ip_addr_t resolved_ip;
static struct tcp_pcb *aprs_pcb;
static ip_addr_t aprs_ip;
static bool aprs_ok, aprs_login_seen, aprs_rejected;
static uint32_t aprs_at_ms, aprs_next_retry_ms, aprs_last_rx_ms;
static uint32_t aprs_packets_total, aprs_packets_session, aprs_connections;
static uint32_t aprs_heartbeat_total, aprs_silence_warnings, aprs_watchdog_reconnects;
static uint32_t aprs_max_rx_gap_ms;
static bool aprs_silence_warned;
static char aprs_user[8]; /* P + 6 hex digits + null terminator */
static char aprs_line[APRS_LINE_MAX];
static unsigned aprs_line_len;
static bool aprs_overflow;

typedef enum { APRS_IDLE, APRS_DNS, APRS_CONNECTING, APRS_STREAMING } aprs_stage_t;
static aprs_stage_t aprs_stage;

typedef enum { HTTP_IDLE, HTTP_DNS, HTTP_CONNECTING, HTTP_WAITING, HTTP_DONE } http_stage_t;
static http_stage_t http_stage;

static void status_line(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t secs = (uint32_t)(now - boot_ms) / 1000u;
    struct netif *n = bnep_lwip_get_interface();
    printf("[STATUS] uptime=%lud%02lu:%02lu:%02lu bt=%s phone=%s PAN=%s DHCP=%s HTTP=%s APRS=%s\n",
           (unsigned long)(secs / 86400u), (unsigned long)((secs / 3600u) % 24u),
           (unsigned long)((secs / 60u) % 60u), (unsigned long)(secs % 60u),
           bt_ready ? "ready" : "starting",
           have_phone ? bd_addr_to_str(phone_addr) : "none",
           pan_up ? "UP" : "down", dhcp_ok ? "OK" : "waiting",
           probe_ok ? "PASS" : (probe_finished ? "retrying" : "pending"),
           aprs_stage == APRS_STREAMING ? "STREAMING" : (aprs_stage != APRS_IDLE ? "connecting" : "retry/idle"));
    printf("[STATS] pan_connections=%lu pan_attempts=%lu aprs_connections=%lu APRS_packets_total=%lu APRS_packets_session=%lu\n",
           (unsigned long)pan_connections, (unsigned long)pan_attempts,
           (unsigned long)aprs_connections, (unsigned long)aprs_packets_total,
           (unsigned long)aprs_packets_session);
    printf("[BOOT] selected_phone_source=%s saved_classic_bonds=%u\n",
           phone_restored_at_boot ? "flash" : (have_phone ? "pair/manual" : "none"), saved_bond_count);
    printf("[BT] acl=%s handle=0x%04x short_bnep_streak=%lu acl_resets=%lu\n",
           phone_acl_handle != HCI_CON_HANDLE_INVALID ? "connected" : "unknown/down",
           (unsigned)phone_acl_handle, (unsigned long)pan_short_streak,
           (unsigned long)pan_acl_resets);
    printf("[MON] aprsc_heartbeats=%lu silence_warnings=%lu watchdog_reconnects=%lu max_rx_gap=%lus\n",
           (unsigned long)aprs_heartbeat_total, (unsigned long)aprs_silence_warnings,
           (unsigned long)aprs_watchdog_reconnects, (unsigned long)(aprs_max_rx_gap_ms / 1000u));
    if (aprs_stage == APRS_STREAMING) {
        printf("[MON] last_APRS_TCP_data=%lus ago\n",
               (unsigned long)((uint32_t)(now - aprs_last_rx_ms) / 1000u));
    }
    if (dhcp_ok) {
        printf("[IP] addr=%s\n", ip4addr_ntoa(netif_ip4_addr(n)));
        printf("[IP] gateway=%s\n", ip4addr_ntoa(netif_ip4_gw(n)));
        printf("[IP] netmask=%s\n", ip4addr_ntoa(netif_ip4_netmask(n)));
        printf("[IP] DNS0=%s\n", ipaddr_ntoa(dns_getserver(0)));
    }
}

static void abort_http(void) {
    if (http_pcb) {
        struct tcp_pcb *p = http_pcb;
        http_pcb = NULL;
        tcp_arg(p, NULL);
        tcp_err(p, NULL);
        tcp_recv(p, NULL);
        tcp_abort(p);
    }
    http_stage = HTTP_IDLE;
}

static void http_failed(const char *why) {
    printf("[HTTP] FAIL: %s\n", why);
    abort_http();
    probe_finished = true;
    probe_ok = false;
    http_next_retry_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
}

static void http_tcp_error(void *arg, err_t err) {
    (void)arg;
    http_pcb = NULL; /* lwIP already freed this PCB. */
    printf("[HTTP] TCP error %d\n", (int)err);
    http_stage = HTTP_IDLE;
    probe_finished = true;
    probe_ok = false;
    http_next_retry_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
}

static err_t http_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        http_pcb = NULL;
        tcp_close(pcb);
        if (!probe_finished) http_failed("remote closed without response");
        return ERR_OK;
    }
    if (err != ERR_OK) { pbuf_free(p); return err; }
    char first[128];
    u16_t len = p->tot_len < sizeof(first)-1 ? p->tot_len : sizeof(first)-1;
    pbuf_copy_partial(p, first, len, 0);
    first[len] = 0;
    char *newline = strchr(first, '\n');
    if (newline) *newline = 0;
    printf("[HTTP] received %u bytes, first line: %s\n", (unsigned)p->tot_len, first);
    bool is_http = len >= 5 && memcmp(first, "HTTP/", 5) == 0;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    probe_ok = is_http;
    probe_finished = true;
    printf("[RESULT] INTERNET %s (DNS + TCP + HTTP response)\n", is_http ? "PASS" : "UNEXPECTED RESPONSE");
    http_pcb = NULL;
    tcp_arg(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_abort(pcb);
    http_stage = is_http ? HTTP_DONE : HTTP_IDLE;
    if (!is_http) http_next_retry_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
    return ERR_ABRT;
}

static err_t http_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { http_failed("TCP connect callback failed"); return err; }
    static const char request[] = "GET " PROBE_PATH " HTTP/1.0\r\nHost: " PROBE_HOST "\r\nConnection: close\r\n\r\n";
    printf("[HTTP] TCP connected; sending GET http://" PROBE_HOST PROBE_PATH "\n");
    tcp_recv(pcb, http_receive);
    err = tcp_write(pcb, request, sizeof(request)-1, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) err = tcp_output(pcb);
    if (err != ERR_OK) {
        http_failed("tcp_write/tcp_output failed");
        return ERR_ABRT; /* http_failed aborted pcb */
    }
    http_stage = HTTP_WAITING;
    return ERR_OK;
}

static void http_connect_to_ip(const ip_addr_t *addr) {
    printf("[DNS] " PROBE_HOST " -> %s\n", ipaddr_ntoa(addr));
    http_pcb = tcp_new();
    if (!http_pcb) { http_failed("tcp_new out of memory"); return; }
    tcp_err(http_pcb, http_tcp_error);
    http_stage = HTTP_CONNECTING;
    err_t err = tcp_connect(http_pcb, addr, 80, http_connected);
    if (err != ERR_OK) http_failed("tcp_connect returned error");
}

static void dns_resolved(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name; (void)arg;
    if (!pan_up || !dhcp_ok || http_stage != HTTP_DNS) return;
    if (!addr) { http_failed("DNS lookup failed"); return; }
    http_connect_to_ip(addr);
}

static void start_http(void) {
    if (!pan_up || !dhcp_ok || http_pcb) return;
    probe_started = true;
    probe_finished = probe_ok = false;
    probe_at_ms = to_ms_since_boot(get_absolute_time());
    http_stage = HTTP_DNS;
    printf("[HTTP] Resolving " PROBE_HOST "...\n");
    err_t err = dns_gethostbyname(PROBE_HOST, &resolved_ip, dns_resolved, NULL);
    if (err == ERR_OK) http_connect_to_ip(&resolved_ip);
    else if (err != ERR_INPROGRESS) http_failed("dns_gethostbyname returned error");
}

/* APRS-IS streams are lines on a TCP byte stream, not on TCP packet boundaries. */
static void abort_aprs(void) {
    if (aprs_pcb) {
        struct tcp_pcb *p = aprs_pcb;
        aprs_pcb = NULL;
        tcp_arg(p, NULL);
        tcp_err(p, NULL);
        tcp_recv(p, NULL);
        tcp_abort(p);
    }
    aprs_stage = APRS_IDLE;
}

static void schedule_aprs_retry(void) {
    aprs_silence_warned = false;
    aprs_stage = APRS_IDLE;
    aprs_login_seen = false;
    aprs_ok = false;
    aprs_rejected = false;
    aprs_next_retry_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
}

static void aprs_failed(const char *why) {
    printf("[APRS] Disconnected: %s; retry in %u sec (total packets=%lu)\n",
           why, RETRY_MS / 1000u, (unsigned long)aprs_packets_total);
    abort_aprs();
    schedule_aprs_retry();
}

static void aprs_tcp_error(void *arg, err_t err) {
    (void)arg;
    aprs_pcb = NULL; /* lwIP has already freed this PCB: do not tcp_abort it. */
    printf("[APRS] TCP error %d; retry in %u sec\n", (int)err, RETRY_MS / 1000u);
    schedule_aprs_retry();
}

static void aprs_on_line(const char *line) {
    if (line[0] == '#') {
        if (strncmp(line, "# aprsc ", 8) == 0) ++aprs_heartbeat_total;
        printf("[APRS] %s\n", line);
        if (strncmp(line, "# logresp ", 10) == 0) {
            if (strstr(line, "unverified") || strstr(line, "verified")) {
                aprs_login_seen = true;
                printf("[APRS] Server login response received (receive-only).\n");
            } else {
                aprs_rejected = true;
                printf("[APRS] Unexpected/rejected login response; reconnect scheduled.\n");
            }
        }
        return;
    }
    if (!line[0]) return;
    ++aprs_packets_total;
    ++aprs_packets_session;
#if APRS_PRINT_PACKETS
    printf("[APRS RX %lu] %s\n", (unsigned long)aprs_packets_total, line);
#endif
    if (aprs_login_seen && !aprs_ok) {
        aprs_ok = true;
        printf("[RESULT] APRS-IS PASS (receiving live packets continuously)\n");
    }
}

static err_t aprs_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        aprs_pcb = NULL;
        tcp_arg(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_recv(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
        printf("[APRS] Server closed TCP; reconnect in %u sec\n", RETRY_MS / 1000u);
        schedule_aprs_retry();
        return ERR_OK;
    }
    if (err != ERR_OK) { pbuf_free(p); return err; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t gap_ms = (uint32_t)(now - aprs_last_rx_ms);
    if (gap_ms > aprs_max_rx_gap_ms) aprs_max_rx_gap_ms = gap_ms;
    if (aprs_silence_warned) {
        printf("[APRS] RX resumed after %lus without data\n",
               (unsigned long)(gap_ms / 1000u));
    }
    aprs_last_rx_ms = now;
    aprs_silence_warned = false;
    for (struct pbuf *q = p; q && !aprs_rejected; q = q->next) {
        const char *bytes = (const char *)q->payload;
        for (u16_t i = 0; i < q->len && !aprs_rejected; ++i) {
            char c = bytes[i];
            if (c == '\r') continue;
            if (c == '\n') {
                if (aprs_overflow) {
                    printf("[APRS] Overlong line skipped\n");
                } else if (aprs_line_len) {
                    aprs_line[aprs_line_len] = 0;
                    aprs_on_line(aprs_line);
                }
                aprs_line_len = 0;
                aprs_overflow = false;
            } else if (!aprs_overflow) {
                if (aprs_line_len + 1 < sizeof(aprs_line))
                    aprs_line[aprs_line_len++] = c;
                else
                    aprs_overflow = true;
            }
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (aprs_rejected) {
        /* We must return ERR_ABRT if tcp_abort occurs inside tcp_recv callback. */
        aprs_failed("server did not accept login");
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t aprs_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        aprs_failed("TCP connect callback error");
        return ERR_ABRT;
    }
    char login[256];
    int n = snprintf(login, sizeof(login),
                     "user %s pass -1 vers Pico2W-PANU 0.6 filter " APRS_FILTER "\r\n",
                     aprs_user);
    if (n < 0 || (size_t)n >= sizeof(login)) {
        aprs_failed("login line too long");
        return ERR_ABRT;
    }
    printf("[APRS] TCP connected; receive-only login user=%s filter=Japan\n", aprs_user);
    tcp_recv(pcb, aprs_receive);
    err = tcp_write(pcb, login, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) err = tcp_output(pcb);
    if (err != ERR_OK) {
        aprs_failed("tcp_write/tcp_output failed");
        return ERR_ABRT;
    }
    aprs_stage = APRS_STREAMING;
    aprs_last_rx_ms = to_ms_since_boot(get_absolute_time());
    aprs_silence_warned = false;
    ++aprs_connections;
    /* Probes help detect a phone switching off its data path without a BNEP close. */
    ip_set_option(pcb, SOF_KEEPALIVE);
    pcb->keep_idle = 90000u;
    pcb->keep_intvl = 20000u;
    pcb->keep_cnt = 3u;
    printf("[APRS] Streaming without packet/time limit (TCP keepalive enabled).\n");
    return ERR_OK;
}

static void aprs_connect_to_ip(const ip_addr_t *ip) {
    printf("[APRS] DNS " APRS_HOST " -> %s\n", ipaddr_ntoa(ip));
    aprs_pcb = tcp_new();
    if (!aprs_pcb) { aprs_failed("tcp_new out of memory"); return; }
    tcp_err(aprs_pcb, aprs_tcp_error);
    aprs_stage = APRS_CONNECTING;
    err_t err = tcp_connect(aprs_pcb, ip, APRS_PORT, aprs_connected);
    if (err != ERR_OK) aprs_failed("tcp_connect returned error");
}

static void aprs_dns_resolved(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name; (void)arg;
    if (!pan_up || !dhcp_ok || aprs_stage != APRS_DNS) return;
    if (!ip) { aprs_failed("DNS lookup failed"); return; }
    aprs_connect_to_ip(ip);
}

static void start_aprs(void) {
    if (!pan_up || !dhcp_ok || !probe_ok || aprs_stage != APRS_IDLE || aprs_pcb) return;
    aprs_login_seen = aprs_ok = aprs_rejected = false;
    aprs_packets_session = 0;
    aprs_line_len = 0;
    aprs_overflow = false;
    aprs_at_ms = to_ms_since_boot(get_absolute_time());
    aprs_last_rx_ms = aprs_at_ms;
    aprs_stage = APRS_DNS;
    printf("[APRS] Resolving " APRS_HOST " for TCP/%d...\n", APRS_PORT);
    err_t err = dns_gethostbyname(APRS_HOST, &aprs_ip, aprs_dns_resolved, NULL);
    if (err == ERR_OK) aprs_connect_to_ip(&aprs_ip);
    else if (err != ERR_INPROGRESS) aprs_failed("dns_gethostbyname error");
}

static void sdp_callback(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
            /* Query was restricted to NAP UUID; any returned service record qualifies. */
            nap_found = true;
            break;
        case SDP_EVENT_QUERY_COMPLETE: {
            sdp_busy = false;
            uint8_t status = sdp_event_query_complete_get_status(packet);
            printf("[SDP] complete status=0x%02x NAP=%s\n", status, nap_found ? "found" : "not found");
            if (status || !nap_found || !have_phone) {
                next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
                return;
            }
            printf("[PAN] Connecting PANU -> NAP at %s\n", bd_addr_to_str(phone_addr));
            bnep_busy = true;
            uint8_t ret = bnep_lwip_connect(phone_addr, PAN_PSM,
                                            BLUETOOTH_SERVICE_CLASS_PANU,
                                            BLUETOOTH_SERVICE_CLASS_NAP);
            if (ret) {
                printf("[PAN] bnep_lwip_connect failed 0x%02x\n", ret);
                bnep_busy = false;
                next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
            }
            break;
        }
        default: break;
    }
}

static void note_phone(bd_addr_t address) {
    memcpy(phone_addr, address, sizeof(bd_addr_t));
    have_phone = true;
    phone_restored_at_boot = false;
    next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + 1200;
    printf("[PAIR] Smartphone address %s; preparing NAP lookup\n", bd_addr_to_str(phone_addr));
}

/* SDK 2.3.1 already backs the Classic Link Key DB with flash TLV storage.
 * Only the address is needed for a fresh outgoing SDP/BNEP connection;
 * authentication itself stays entirely inside BTstack. Never print keys.
 *
 * Important: wait for HCI_STATE_WORKING, as in the official BTstack
 * example/gap_link_keys.c. Some ports select their DB at startup.
 *
 * Simple one-phone demo policy: use the FIRST stored Classic bond. If
 * several devices were paired, `c XX:XX:XX:XX:XX:XX` overrides selection.
 */
static void restore_bonded_phone(void) {
    btstack_link_key_iterator_t it;
    bd_addr_t addr;
    link_key_t link_key;
    link_key_type_t key_type;
    saved_bond_count = 0;

    if (!gap_link_key_iterator_init(&it)) {
        printf("[BOOT] Link-key iterator unavailable; waiting for pairing or c MAC\n");
        return;
    }
    while (gap_link_key_iterator_get_next(&it, addr, link_key, &key_type)) {
        ++saved_bond_count;
        printf("[BOOT] Saved Classic bond #%u: %s (type=%u)\n",
               saved_bond_count, bd_addr_to_str(addr), (unsigned)key_type);
        if (!have_phone) {
            memcpy(phone_addr, addr, sizeof(bd_addr_t));
            have_phone = true;
            phone_restored_at_boot = true;
            next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + 1500u;
        }
        memset(link_key, 0, sizeof(link_key));
    }
    gap_link_key_iterator_done(&it);

    if (phone_restored_at_boot) {
        printf("[BOOT] Auto-reconnecting bonded phone %s; no pairing action needed\n",
               bd_addr_to_str(phone_addr));
        if (saved_bond_count > 1) {
            printf("[BOOT] Multiple saved bonds: using first. Set phone via 'c MAC' if needed\n");
        }
    } else if (!saved_bond_count) {
        printf("[BOOT] No stored Classic bonds; pair once from the smartphone\n");
    }
}

static void bt_event(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    bd_addr_t addr;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bt_ready = true;
                gap_local_bd_addr(addr);
                printf("[READY] Pico Bluetooth Classic address %s\n", bd_addr_to_str(addr));
                snprintf(aprs_user, sizeof(aprs_user), "P%02X%02X%02X",
                         addr[3], addr[4], addr[5]);
                printf("[READY] APRS receive-only login: %s (pass -1)\n", aprs_user);
                printf("[READY] Pair with 'Pico2W-PANU' using your phone's Bluetooth settings.\n");
                printf("[READY] Enable Bluetooth tethering on the phone.\n");
                restore_bonded_phone();
            }
            break;
        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t status = hci_event_connection_complete_get_status(packet);
            hci_event_connection_complete_get_bd_addr(packet, addr);
            if (status == 0) {
                /* This demo pairs with one phone at a time. */
                if (!have_phone || memcmp(addr, phone_addr, 6) == 0) {
                    phone_acl_handle = hci_event_connection_complete_get_connection_handle(packet);
                }
                printf("[HCI] ACL connected: %s handle=0x%04x link_type=%u\n",
                       bd_addr_to_str(addr),
                       (unsigned)hci_event_connection_complete_get_connection_handle(packet),
                       (unsigned)hci_event_connection_complete_get_link_type(packet));
            } else {
                printf("[HCI] ACL connection failed: %s status=0x%02x\n",
                       bd_addr_to_str(addr), status);
            }
            break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            printf("[HCI] Authentication complete: handle=0x%04x status=0x%02x\n",
                   (unsigned)hci_event_authentication_complete_get_connection_handle(packet),
                   hci_event_authentication_complete_get_status(packet));
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            printf("[HCI] Encryption change: handle=0x%04x status=0x%02x enabled=%u\n",
                   (unsigned)hci_event_encryption_change_get_connection_handle(packet),
                   hci_event_encryption_change_get_status(packet),
                   (unsigned)hci_event_encryption_change_get_encryption_enabled(packet));
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            printf("[HCI] ACL disconnected: handle=0x%04x reason=0x%02x\n",
                   (unsigned)handle, reason);
            if (phone_acl_handle == handle) phone_acl_handle = HCI_CON_HANDLE_INVALID;
            /* BNEP normally emits CHANNEL_CLOSED separately through bnep_lwip. */
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("[PAIR] Legacy PIN requested by %s (using 0000)\n", bd_addr_to_str(addr));
            gap_pin_code_response(addr, "0000");
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            printf("[PAIR] SSP Just Works confirmation for %s\n", bd_addr_to_str(addr));
            gap_ssp_confirmation_response(addr);
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            hci_event_simple_pairing_complete_get_bd_addr(packet, addr);
            if (hci_event_simple_pairing_complete_get_status(packet) == 0) {
                printf("[PAIR] Secure Simple Pairing completed\n");
                note_phone(addr);
            } else {
                printf("[PAIR] failed status=0x%02x\n", hci_event_simple_pairing_complete_get_status(packet));
            }
            break;
        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            /* Bluetooth HCI event: 6-byte address at packet[2], little-endian. */
            reverse_bytes(&packet[2], addr, 6);
            printf("[PAIR] Link key created\n");
            note_phone(addr);
            break;
        case BNEP_EVENT_CHANNEL_OPENED: {
            uint8_t status = bnep_event_channel_opened_get_status(packet);
            bnep_busy = false;
            if (status) {
                printf("[PAN] Connection failed status=0x%02x\n", status);
                next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
                break;
            }
            pan_up = true;
            ++pan_connections;
            dhcp_ok = false;
            probe_started = probe_finished = probe_ok = false;
            abort_aprs();
            aprs_login_seen = aprs_ok = false;
            aprs_next_retry_ms = 0;
            printf("[PAN] BNEP connected: src=%04x dst=%04x\n",
                   bnep_event_channel_opened_get_source_uuid(packet),
                   bnep_event_channel_opened_get_destination_uuid(packet));
            struct netif *n = bnep_lwip_get_interface();
            /* Do not reuse the former phone tether's IPv4 address. */
            ip4_addr_t zero = {0};
            netif_set_addr(n, &zero, &zero, &zero);
            printf("[NETIF] on BNEP open: up=%u link=%u IP=%s\n",
                   (unsigned)netif_is_up(n), (unsigned)netif_is_link_up(n),
                   ip4addr_ntoa(netif_ip4_addr(n)));
            connected_at_ms = to_ms_since_boot(get_absolute_time());
            dhcp_pending = true;
            dhcp_started = false;
            dhcp_due_ms = connected_at_ms + DHCP_START_DELAY_MS;
            printf("[DIAG] BNEP is UP; intentionally waiting %ums before first DHCP frame\n",
                   (unsigned)DHCP_START_DELAY_MS);
            break;
        }
        case BNEP_EVENT_CHANNEL_CLOSED: {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            uint32_t lifetime = pan_up ? (uint32_t)(now - connected_at_ms) : 0;
            printf("[PAN] BNEP disconnected (session=%lums, DHCP=%s, dhcp_started=%u)\n",
                   (unsigned long)lifetime, dhcp_ok ? "OK" : "not acquired",
                   (unsigned)dhcp_started);
            if (!dhcp_started && lifetime < DHCP_START_DELAY_MS)
                printf("[DIAG] BNEP closed BEFORE first DHCP request was started\n");
            dhcp_pending = false;
            dhcp_started = false;
            if (pan_up && lifetime < PAN_SHORT_SESSION_MS) {
                ++pan_short_sessions;
                ++pan_short_streak;
            } else {
                pan_short_streak = 0;
            }
            abort_http();
            abort_aprs();
            schedule_aprs_retry();
            struct netif *n = bnep_lwip_get_interface();
            dhcp_stop(n);
            /* bnep_lwip already set netif DOWN before notifying the app.
             * Clearing address avoids carrying a stale DHCP lease across PANs.
             */
            ip4_addr_t zero = {0};
            netif_set_addr(n, &zero, &zero, &zero);
            pan_up = dhcp_ok = false;
            bnep_busy = sdp_busy = false;
            probe_started = probe_finished = probe_ok = false;
            next_sdp_at_ms = now + RETRY_MS;
            if (pan_short_streak >= PAN_ACL_RESET_AFTER) {
                pan_short_streak = 0;
                if (phone_acl_handle != HCI_CON_HANDLE_INVALID &&
                    (!ever_acl_reset ||
                     (uint32_t)(now - last_acl_reset_ms) >= PAN_ACL_RESET_COOLDOWN_MS)) {
                    printf("[PAN] 3 short sessions; resetting the ACL link for a clean retry\n");
                    uint8_t result = gap_disconnect(phone_acl_handle);
                    printf("[HCI] gap_disconnect returned 0x%02x\n", result);
                    if (result == 0) {
                        ever_acl_reset = true;
                        last_acl_reset_ms = now;
                        ++pan_acl_resets;
                        next_sdp_at_ms = now + PAN_ACL_RESET_RETRY_MS;
                    }
                } else {
                    printf("[PAN] short-session streak; ACL reset not available/cooldown\n");
                }
            }
            break;
        }
        case BNEP_EVENT_CHANNEL_TIMEOUT:
            printf("[PAN] BNEP setup timeout; retry scheduled\n");
            if (!pan_up) {
                bnep_busy = false;
                next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + RETRY_MS;
            }
            break;
        default: break;
    }
}

/* Check only an established APRS TCP session; DNS/TCP setup has its own timer.
 * This is a two-stage network-health heuristic, not a claim that every
 * APRS-IS server guarantees 20s heartbeats.
 */
static void aprs_watchdog_tick(uint32_t now) {
    if (aprs_stage != APRS_STREAMING) return;
    uint32_t silence_ms = (uint32_t)(now - aprs_last_rx_ms);
    if (silence_ms >= APRS_SILENCE_WARN_MS && !aprs_silence_warned) {
        aprs_silence_warned = true;
        ++aprs_silence_warnings;
        printf("[APRS] WARN: no incoming TCP data for %lus; connection may be stalled\n",
               (unsigned long)(silence_ms / 1000u));
    }
    if (silence_ms >= APRS_SILENCE_RECONNECT_MS) {
        if (silence_ms > aprs_max_rx_gap_ms) aprs_max_rx_gap_ms = silence_ms;
        ++aprs_watchdog_reconnects;
        aprs_failed("no incoming TCP data for 60s (watchdog)");
    }
}

static void tick(btstack_timer_source_t *timer) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - next_status_ms) >= 0) {
        if (pan_up || have_phone) status_line();
        next_status_ms = now + STATUS_INTERVAL_MS;
    }
    if (pan_up) {
        struct netif *n = bnep_lwip_get_interface();
        if (dhcp_pending && (int32_t)(now - dhcp_due_ms) >= 0) {
            dhcp_pending = false;
            dhcp_started = true;
            printf("[DIAG] Starting DHCP after %lums of stable BNEP\n",
                   (unsigned long)(now - connected_at_ms));
            err_t e = dhcp_start(n);
            printf("[DHCP] dhcp_start returned %d\n", (int)e);
        }
        if (dhcp_ok && !dhcp_supplied_address(n)) {
            printf("[DHCP] Lease lost; restarting network tests\n");
            dhcp_ok = false;
            abort_http();
            abort_aprs();
            schedule_aprs_retry();
            probe_started = probe_finished = probe_ok = false;
            connected_at_ms = now;
        }
        if (!dhcp_ok && dhcp_supplied_address(n)) {
            dhcp_ok = true;
            printf("[DHCP] Lease acquired.\n");
            status_line();
            start_http();
        }
        if (!dhcp_pending && !dhcp_ok && (uint32_t)(now - connected_at_ms) > DHCP_RESTART_MS) {
            printf("[DHCP] No lease after %us; restarting DHCP client\n",
                   DHCP_RESTART_MS / 1000u);
            dhcp_stop(n);
            dhcp_started = true;
            ip4_addr_t any = {0};
            netif_set_addr(n, &any, &any, &any);
            err_t e = dhcp_start(n);
            if (e != ERR_OK) printf("[DHCP] dhcp_start error %d\n", (int)e);
            connected_at_ms = now;
        }
        if (http_stage != HTTP_IDLE && http_stage != HTTP_DONE &&
            (uint32_t)(now - probe_at_ms) > HTTP_TIMEOUT_MS) {
            http_failed("HTTP/DNS timeout");
        }
        if (dhcp_ok && !probe_ok && http_stage == HTTP_IDLE &&
            (int32_t)(now - http_next_retry_ms) >= 0) {
            start_http();
        }
        if (dhcp_ok && probe_ok) {
            if (aprs_stage != APRS_IDLE && aprs_stage != APRS_STREAMING &&
                (uint32_t)(now - aprs_at_ms) > APRS_CONNECT_TIMEOUT_MS) {
                aprs_failed("APRS DNS/TCP timeout");
            }
            aprs_watchdog_tick(now);
            if (aprs_stage == APRS_IDLE &&
                (int32_t)(now - aprs_next_retry_ms) >= 0) {
                start_aprs();
            }
        }
    } else if (bt_ready && have_phone && !sdp_busy && !bnep_busy &&
               (int32_t)(now - next_sdp_at_ms) >= 0) {
        ++pan_attempts;
        printf("[SDP] Checking NAP service at %s (attempt %lu; unlimited)\n",
               bd_addr_to_str(phone_addr), (unsigned long)pan_attempts);
        nap_found = false;
        sdp_busy = true;
        next_sdp_at_ms = now + RETRY_MS;
        sdp_client_query_uuid16(sdp_callback, phone_addr, BLUETOOTH_SERVICE_CLASS_NAP);
    }
    btstack_run_loop_set_timer(timer, POLL_MS);
    btstack_run_loop_add_timer(timer);
}

static void command(char *line) {
    if (line[0] == 's') { status_line(); return; }
    if (line[0] == 't') {
        if (!dhcp_ok) { printf("[CMD] HTTP test needs DHCP first\n"); return; }
        abort_aprs();
        aprs_next_retry_ms = 0;
        abort_http();
        start_http();
        return;
    }
    if (line[0] == 'a') {
        if (!probe_ok) { printf("[CMD] APRS retry needs HTTP PASS first\n"); return; }
        abort_aprs();
        aprs_next_retry_ms = 0;
        start_aprs();
        return;
    }
    if (line[0] == 'r') {
        next_sdp_at_ms = to_ms_since_boot(get_absolute_time());
        printf("[CMD] Retry requested (only if PAN disconnected)\n");
        return;
    }
    if (line[0] == 'x') {
        if (phone_acl_handle == HCI_CON_HANDLE_INVALID) {
            printf("[CMD] no known ACL to disconnect\n");
        } else {
            printf("[CMD] forcibly disconnecting phone ACL handle=0x%04x\n", (unsigned)phone_acl_handle);
            printf("[CMD] gap_disconnect returned 0x%02x\n", gap_disconnect(phone_acl_handle));
            next_sdp_at_ms = to_ms_since_boot(get_absolute_time()) + PAN_ACL_RESET_RETRY_MS;
        }
        return;
    }
    if (line[0] == 'c' && line[1] == ' ') {
        bd_addr_t addr;
        if (sscanf_bd_addr(line + 2, addr)) note_phone(addr);
        else printf("[CMD] Invalid Bluetooth address\n");
        return;
    }
    printf("[CMD] s=status, t=HTTP+APRS retest, a=APRS retest, r=retry, x=ACL reset, c MAC=phone\n");
}

int main(void) {
    stdio_init_all();
    boot_ms = to_ms_since_boot(get_absolute_time());
    sleep_ms(1800);
    printf("\n=== Pico 2 W BT PANU -> HTTP -> APRS-IS continuous receive (v0.8 auto-bond reboot) ===\n");
    printf("[INIT] Bluetooth Classic / BNEP / lwIP, no WiFi\n");
    if (cyw43_arch_init() != PICO_OK) {
        printf("[FATAL] cyw43_arch_init failed\n");
        while (true) sleep_ms(1000);
    }

    /* CYW43_LWIP=0: Bluetooth lwIP netif is owned by bnep_lwip, not Wi-Fi. */
    cyw43_arch_lwip_begin();
    /* CYW43_LWIP=0 disables SDK lwIP initialization; BNEP adapter needs it. */
    lwip_init();
    l2cap_init();
    sdp_init();
    bnep_init();
    bnep_lwip_init();
    /* The stock bnep_lwip_init() defaults to 192.168.7.1 (NAP server).
       Clear it: this firmware is a DHCP client (PANU). */
    ip4_addr_t any = {0};
    netif_set_addr(bnep_lwip_get_interface(), &any, &any, &any);
    memset(sdp_record, 0, sizeof(sdp_record));
    uint16_t protocols[] = { 0x0800, 0x0806, 0 };
    pan_create_panu_sdp_record(sdp_record, sdp_create_service_record_handle(),
                               protocols, NULL, NULL, BNEP_SECURITY_NONE);
    sdp_register_service(sdp_record);
    bnep_lwip_register_service(BLUETOOTH_SERVICE_CLASS_PANU, BNEP_FRAME_SIZE);
    bnep_lwip_register_packet_handler(bt_event);
    hci_events.callback = bt_event;
    hci_add_event_handler(&hci_events);
    gap_set_local_name("Pico2W-PANU");
    gap_set_class_of_device(0x020300);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_discoverable_control(1);
    gap_connectable_control(1);
    next_status_ms = to_ms_since_boot(get_absolute_time()) + STATUS_INTERVAL_MS;
    btstack_run_loop_set_timer_handler(&poll_timer, tick);
    btstack_run_loop_set_timer(&poll_timer, POLL_MS);
    btstack_run_loop_add_timer(&poll_timer);
    hci_power_control(HCI_POWER_ON);
    cyw43_arch_lwip_end();

    char line[64];
    unsigned index = 0;
    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            if (ch == '\r' || ch == '\n') {
                if (index) {
                    line[index] = 0;
                    cyw43_arch_lwip_begin();
                    command(line);
                    cyw43_arch_lwip_end();
                    index = 0;
                }
            } else if (ch >= 32 && ch <= 126 && index + 1 < sizeof(line)) {
                line[index++] = (char)ch;
            }
        }
        sleep_ms(10);
    }
}