#ifndef CONNECT_NET_RECOVERY_POLICY_H
#define CONNECT_NET_RECOVERY_POLICY_H

/*
 * Pure network-recovery policy.
 *
 * Everything in here is a decision function with no ESP-IDF dependency: no
 * esp_netif, no NVS, no FreeRTOS. That is deliberate. The previous version of this
 * logic lived inside connect.c, where every branch could only be reached by owning
 * real hardware and waiting for a real router to misbehave - which is exactly why a
 * unit that fell back once could never get a DHCP lease again, and why that went
 * unnoticed. The decisions are now stated here, in one place, and exercised by
 * simulation/test_net_recovery_policy.c on the host.
 *
 * connect.c owns the hardware; this file owns the policy.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough for "255.255.255.255" plus NUL. */
#define NET_ADDR_STR_LEN 16

/* Which rung of the recovery ladder produced a bind. Reported in logs and in the
 * API so an operator can tell "DHCP is broken" from "we are on a fallback". */
typedef enum {
    NET_FALLBACK_NONE = 0,
    NET_FALLBACK_LAST_LEASE,
    NET_FALLBACK_OPERATOR_STATIC
} net_fallback_rung_t;

typedef struct {
    char ip[NET_ADDR_STR_LEN];
    char gw[NET_ADDR_STR_LEN];
    char mask[NET_ADDR_STR_LEN];
    char dns[NET_ADDR_STR_LEN];
} net_addr_config_t;

/* The last address the DHCP server really handed out, with the gateway, netmask
 * and resolver it came with. All four are remembered together: replaying the IP
 * with a hardcoded /24 and a hardcoded gateway produces an address that is
 * unreachable on any network that is not that one exact network. */
typedef struct {
    net_addr_config_t addr;
    bool valid; /* ip is set, and gw and mask are known */
} net_last_lease_t;

typedef struct {
    net_last_lease_t last_lease;
    net_addr_config_t operator_static; /* ip/gw/mask/dns as the operator set them */
    bool use_operator_static;
} net_recovery_inputs_t;

typedef struct {
    net_fallback_rung_t rung;
    net_addr_config_t bind; /* fully resolved: every field is always present */
} net_fallback_choice_t;

/* Bounded copy into one of the fixed-size address fields above. A NULL, empty or
 * over-long source leaves the field empty rather than truncated, so a caller can
 * never mistake half an address for a whole one. Returns false when nothing was
 * copied. */
bool net_addr_set(char *dst, const char *src);

/* A resolver we installed ourselves must not be mistaken for one a DHCP server
 * handed out, and neither must 0.0.0.0. Re-persisting either one turns a
 * self-healable outage into a permanent one. */
bool net_dns_is_usable(const char *dns);

/* A dotted-quad IPv4 that is neither unspecified nor multicast. `gw` and `mask`
 * must pass this; `ip` additionally must not be multicast. */
bool net_ipv4_is_usable(const char *ip);
bool net_ipv4_is_mask(const char *mask);

/* Record a real lease. Returns which fields are worth writing to NVS, so a server
 * that keeps reissuing the same address does not cause a write per renewal.
 *
 * `dns_grew_lease` is true only when the resolver came out of a lease; esp-netif
 * returns whatever is currently configured for the netif, which after a fallback
 * is a resolver *we* installed. Passing that back would persist our own guess as
 * though a router had issued it.
 */
typedef struct {
    bool persist_ip;
    bool persist_gw;
    bool persist_mask;
    bool persist_dns;
    net_addr_config_t addr;
} net_lease_capture_t;

net_lease_capture_t net_policy_capture_lease(const net_addr_config_t *lease, bool dns_came_from_lease);

/* Choose a bind after DHCP has genuinely failed. Returns NET_FALLBACK_NONE when
 * nothing usable is configured, in which case the caller keeps the DHCP client
 * retrying rather than binding an address it cannot justify. */
net_fallback_choice_t net_policy_select_fallback(const net_recovery_inputs_t *in);

/* Reassert state. `have_fallback` is a latch that is cleared the moment a genuine
 * lease arrives, so it describes *now*, not "ever".
 *
 * `dhcp_running` must reflect the live client. While a client is running, a missing
 * address is the normal state between DISCOVER and OFFER, and stopping the client
 * to rebind a fallback address is what turns a transient DHCP gap into a permanent
 * loss of DHCP for the rest of the boot.
 */
typedef struct {
    bool have_fallback;
    net_addr_config_t bound;
    uint32_t fallback_generation; /* bumped on every bind and every clear */
} net_reassert_state_t;

typedef struct {
    bool should_reassert;
    net_addr_config_t bind;
    uint32_t generation;
    const char *reason;
} net_reassert_decision_t;

net_reassert_decision_t net_policy_should_reassert(const net_reassert_state_t *state,
                                                   const char *current_ip,
                                                   bool dhcp_running);

/* Clear the latch. Called when a genuine lease arrives. */
void net_policy_lease_wins(net_reassert_state_t *state);

/* Station reconnect cadence.
 *
 * There is no "give up" result. Handshake-type disconnect reasons
 * (AUTH_FAIL, 4WAY_HANDSHAKE_TIMEOUT, HANDSHAKE_TIMEOUT, AUTH_EXPIRE) are what a
 * wrong password produces, but also what a router produces while it is rebooting
 * and its WPA stack is not up yet. A miner that stops reconnecting on them stays
 * offline after the router returns until someone power-cycles it. Repeated
 * auth-type failures therefore only slow the cadence, so a wrong password is not
 * hammered and a returning router is still found.
 *
 * `attempt` counts from 1 and is reset by a lease; `jitter` is any random word. */
#define NET_WIFI_RETRY_CAP_MS        8000u
#define NET_WIFI_AUTH_RETRY_AFTER    3
#define NET_WIFI_AUTH_RETRY_MIN_MS   30000u
#define NET_WIFI_AUTH_RETRY_CAP_MS   (NET_WIFI_AUTH_RETRY_MIN_MS + 5000u)

uint32_t net_policy_wifi_retry_delay_ms(int attempt, bool auth_failure, uint32_t jitter);

#ifdef __cplusplus
}
#endif

#endif /* CONNECT_NET_RECOVERY_POLICY_H */
