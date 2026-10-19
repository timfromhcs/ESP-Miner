#include "net_recovery_policy.h"

#include <string.h>

/* Parse a dotted quad without pulling in inet_pton: this file is compiled for the
 * host test suite as well as for the firmware, and the policy only needs "is this
 * a syntactically valid, unicast IPv4 address", not a 32-bit value. */
static bool parse_quad(const char *s, unsigned out[4])
{
    if (s == NULL) {
        return false;
    }
    unsigned idx = 0;
    const char *p = s;
    while (idx < 4) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        unsigned v = 0;
        unsigned digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned) (*p - '0');
            digits++;
            if (digits > 3) {
                return false;
            }
            p++;
        }
        if (v > 255) {
            return false;
        }
        out[idx++] = v;
        if (idx == 4) {
            break;
        }
        if (*p != '.') {
            return false;
        }
        p++;
    }
    return *p == '\0';
}

/* Bounded copy that always terminates. Returns false (and empties the
 * destination) when the source is absent, empty or too long to fit - so a
 * caller never has to reason about a truncated address. */
static bool copy_if_set(char *dst, size_t cap, const char *src)
{
    dst[0] = '\0';
    if (src == NULL) {
        return false;
    }
    size_t len = strlen(src);
    if (len == 0 || len >= cap) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static bool copy_addr(char *dst, size_t cap, const char *src)
{
    return copy_if_set(dst, cap, src) ? true : (dst[0] = '\0', false);
}

bool net_addr_set(char *dst, const char *src)
{
    return dst != NULL && copy_if_set(dst, NET_ADDR_STR_LEN, src);
}

bool net_ipv4_is_usable(const char *ip)
{
    unsigned q[4];
    if (!parse_quad(ip, q)) {
        return false;
    }
    /* 0.0.0.0 is "no address". 224/4 is multicast. Neither is a usable local
     * address, and binding either is worse than having no address at all: the
     * interface looks configured and nothing can be sent. */
    if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0) {
        return false;
    }
    if (q[0] >= 224) {
        return false;
    }
    return true;
}

bool net_ipv4_is_mask(const char *mask)
{
    unsigned q[4];
    if (!parse_quad(mask, q)) {
        return false;
    }
    /* A contiguous netmask is a run of set bits followed by a run of clear bits.
     * /0 is excluded: a host address with no mask is a configuration error, not a
     * wide-open network, and accepting it hides a typo behind "it sort of works". */
    bool seen_zero = false;
    unsigned bits = 0;
    for (unsigned i = 0; i < 4; i++) {
        for (int b = 7; b >= 0; b--) {
            if ((q[i] >> b) & 1u) {
                if (seen_zero) {
                    return false;
                }
                bits++;
            } else {
                seen_zero = true;
            }
        }
    }
    return bits >= 1;
}

bool net_dns_is_usable(const char *dns)
{
    return net_ipv4_is_usable(dns);
}

net_lease_capture_t net_policy_capture_lease(const net_addr_config_t *lease, bool dns_came_from_lease)
{
    net_lease_capture_t out;
    memset(&out, 0, sizeof(out));

    if (lease == NULL) {
        return out;
    }
    if (!net_ipv4_is_usable(lease->ip)) {
        return out;
    }

    copy_if_set(out.addr.ip, sizeof(out.addr.ip), lease->ip);
    copy_if_set(out.addr.gw, sizeof(out.addr.gw), lease->gw);
    copy_if_set(out.addr.mask, sizeof(out.addr.mask), lease->mask);
    copy_if_set(out.addr.dns, sizeof(out.addr.dns), lease->dns);

    out.persist_ip = true;
    /* The gateway and netmask come from the lease's own options. Only write them
     * when they are actually present: an empty write would erase a previously good
     * value and downgrade the next outage to a rung that cannot be used. */
    out.persist_gw = net_ipv4_is_usable(out.addr.gw);
    out.persist_mask = net_ipv4_is_mask(out.addr.mask);
    /* Only a resolver a DHCP server handed out. 0.0.0.0 and a resolver installed
     * by our own fallback are both rejected: persisting either turns a single
     * network outage into a permanent one, because the fallback then replays a
     * resolver that never worked. */
    out.persist_dns = dns_came_from_lease && net_dns_is_usable(out.addr.dns);

    return out;
}

net_fallback_choice_t net_policy_select_fallback(const net_recovery_inputs_t *in)
{
    net_fallback_choice_t choice;
    memset(&choice, 0, sizeof(choice));
    choice.rung = NET_FALLBACK_NONE;

    if (in == NULL) {
        return choice;
    }

    /* Rung 1: the last real lease, replayed in full.
     *
     * Only attempted when the gateway and netmask came from that same lease. An
     * IP alone is not enough: with no gateway and no netmask the only way to bind
     * it is to invent them, and an invented pair is wrong on every network except
     * the one it was guessed from - which presents as "connected" with nothing
     * reachable. Skipping the rung and continuing the DHCP retry is strictly
     * better than binding an address that cannot work. */
    if (in->last_lease.valid && net_ipv4_is_usable(in->last_lease.addr.ip)
        && net_ipv4_is_usable(in->last_lease.addr.gw)
        && net_ipv4_is_mask(in->last_lease.addr.mask)) {
        choice.rung = NET_FALLBACK_LAST_LEASE;
        choice.bind = in->last_lease.addr;

        /* Resolver order: the one the lease handed out, then the operator's
         * configured resolver, then the gateway. An address with no resolver binds
         * fine and then fails every lookup, which is how this path first presented
         * as "connected but no shares". */
        if (!net_dns_is_usable(choice.bind.dns)) {
            if (!copy_addr(choice.bind.dns, sizeof(choice.bind.dns), in->operator_static.dns)) {
                copy_addr(choice.bind.dns, sizeof(choice.bind.dns), choice.bind.gw);
            }
        }
        return choice;
    }

    /* Rung 2: the operator-configured address. Opt-in, and only once DHCP has
     * genuinely failed. */
    if (in->use_operator_static && net_ipv4_is_usable(in->operator_static.ip)
        && net_ipv4_is_usable(in->operator_static.gw)) {
        choice.rung = NET_FALLBACK_OPERATOR_STATIC;
        choice.bind = in->operator_static;
        if (!net_ipv4_is_mask(choice.bind.mask)) {
            copy_addr(choice.bind.mask, sizeof(choice.bind.mask), "255.255.255.0");
        }
        if (!net_dns_is_usable(choice.bind.dns)) {
            copy_addr(choice.bind.dns, sizeof(choice.bind.dns), choice.bind.gw);
        }
        return choice;
    }

    return choice;
}

net_reassert_decision_t net_policy_should_reassert(const net_reassert_state_t *state,
                                                   const char *current_ip,
                                                   bool dhcp_running)
{
    net_reassert_decision_t d;
    memset(&d, 0, sizeof(d));
    d.generation = state != NULL ? state->fallback_generation : 0;

    if (state == NULL || !state->have_fallback) {
        d.reason = "no_fallback";
        return d;
    }

    if (net_ipv4_is_usable(current_ip)) {
        d.reason = "address_present";
        return d;
    }

    /* This is the check that was missing. Between DISCOVER and OFFER the interface
     * legitimately has no address, and that window occurs on every DHCP cycle. If
     * the client is running we let it finish: stopping it here is what makes a
     * fallback address permanent for the rest of the boot. */
    if (dhcp_running) {
        d.reason = "dhcp_running";
        return d;
    }

    d.should_reassert = true;
    d.bind = state->bound;
    d.reason = "address_missing";
    return d;
}

void net_policy_lease_wins(net_reassert_state_t *state)
{
    if (state == NULL) {
        return;
    }
    if (state->have_fallback) {
        state->fallback_generation++;
    }
    state->have_fallback = false;
    memset(&state->bound, 0, sizeof(state->bound));
}
