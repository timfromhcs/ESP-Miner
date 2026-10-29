/*
 * Host tests for the network recovery policy.
 *
 * These cover the transitions that cannot be reproduced on a bench without a
 * router that misbehaves in a specific way, which is why the defects they guard
 * against survived a hardware validation cycle:
 *
 *   - a fallback address must never become permanent (the DHCP client must be
 *     allowed to finish its cycle);
 *   - a genuine lease must clear the fallback latch;
 *   - the replayed lease must carry the gateway and netmask that lease provided;
 *   - a resolver we installed ourselves must not be persisted as though a DHCP
 *     server had issued it.
 */

#include "net_recovery_policy.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL: %s (got \"%s\", want \"%s\")\n", what, got ? got : "(null)", want);
    }
}

/* ---------------------------------------------------------------- ipv4 parsing */

static void test_ipv4_usable(void)
{
    printf("== ipv4 validation ==\n");
    check(net_ipv4_is_usable("192.168.178.66"), "plain unicast is usable");
    check(net_ipv4_is_usable("10.0.0.1"), "10/8 is usable");
    check(net_ipv4_is_usable("8.8.8.8"), "8.8.8.8 is usable");

    check(!net_ipv4_is_usable(NULL), "NULL is not usable");
    check(!net_ipv4_is_usable(""), "empty is not usable");
    check(!net_ipv4_is_usable("0.0.0.0"), "0.0.0.0 is not usable");
    check(!net_ipv4_is_usable("255.255.255.255"), "broadcast is not usable");
    check(!net_ipv4_is_usable("224.0.0.1"), "multicast is not usable");
    check(!net_ipv4_is_usable("239.1.1.1"), "multicast range is not usable");

    check(!net_ipv4_is_usable("192.168.178"), "three octets rejected");
    check(!net_ipv4_is_usable("192.168.178.256"), "octet > 255 rejected");
    check(!net_ipv4_is_usable("192.168.178.1.1"), "five octets rejected");
    check(!net_ipv4_is_usable("192.168.178.1 "), "trailing space rejected");
    check(!net_ipv4_is_usable(" 192.168.178.1"), "leading space rejected");
    check(!net_ipv4_is_usable("a.b.c.d"), "non-numeric rejected");
    check(!net_ipv4_is_usable("192.168.178.1234"), "four-digit octet rejected");
    check(!net_ipv4_is_usable("192.168.178.-1"), "negative octet rejected");
    check(!net_ipv4_is_usable("192..168.1.1"), "empty octet rejected");
}

static void test_ipv4_mask(void)
{
    printf("== ipv4 netmask validation ==\n");
    check(net_ipv4_is_mask("255.255.255.0"), "/24 accepted");
    check(net_ipv4_is_mask("255.255.0.0"), "/16 accepted");
    check(net_ipv4_is_mask("255.255.255.255"), "/32 accepted");
    check(net_ipv4_is_mask("255.255.255.252"), "/30 accepted");
    check(net_ipv4_is_mask("255.0.0.0"), "/8 accepted");
    check(net_ipv4_is_mask("252.0.0.0"), "/6 accepted");

    check(!net_ipv4_is_mask("0.0.0.0"), "/0 rejected as a configuration error");
    check(!net_ipv4_is_mask("255.0.255.0"), "non-contiguous mask rejected");
    check(!net_ipv4_is_mask("255.255.255.1"), "non-contiguous mask rejected");
    check(!net_ipv4_is_mask("0.255.255.255"), "leading clear bit rejected");
    check(!net_ipv4_is_mask("255.255.255"), "short mask rejected");
    check(!net_ipv4_is_mask(""), "empty mask rejected");
    check(!net_ipv4_is_mask(NULL), "NULL mask rejected");
}

/* ------------------------------------------------------- resolver persistence */

static void test_resolver_must_come_from_the_lease(void)
{
    printf("== lease resolver persistence ==\n");
    net_addr_config_t lease;
    memset(&lease, 0, sizeof(lease));
    strcpy(lease.ip, "192.168.178.66");
    strcpy(lease.gw, "192.168.178.1");
    strcpy(lease.mask, "255.255.255.0");
    strcpy(lease.dns, "192.168.178.1");

    net_lease_capture_t with_dns = net_policy_capture_lease(&lease, true);
    check(with_dns.persist_dns, "resolver from a lease is persisted");
    check_str(with_dns.addr.dns, "192.168.178.1", "resolver recorded verbatim");

    /* The same value, but read back after our own fallback installed it. Writing
     * that would persist our guess as though the router had issued it. */
    net_lease_capture_t self_installed = net_policy_capture_lease(&lease, false);
    check(!self_installed.persist_dns, "self-installed resolver is not persisted");

    net_addr_config_t zero_dns;
    memset(&zero_dns, 0, sizeof(zero_dns));
    strcpy(zero_dns.ip, "192.168.178.66");
    strcpy(zero_dns.gw, "192.168.178.1");
    strcpy(zero_dns.mask, "255.255.255.0");
    strcpy(zero_dns.dns, "0.0.0.0");

    net_lease_capture_t any = net_policy_capture_lease(&zero_dns, true);
    check(!any.persist_dns, "0.0.0.0 is rejected as a resolver");
    check(any.persist_ip, "0.0.0.0 resolver does not block the address itself");

    /* A resolver that changed while the address stayed the same must still be
     * written: it used to be nested inside the address-changed check, so a lease
     * with a new resolver silently kept the old one. */
    net_addr_config_t no_gw = lease;
    no_gw.gw[0] = '\0';
    net_lease_capture_t partial = net_policy_capture_lease(&no_gw, true);
    check(!partial.persist_gw, "missing gateway is not persisted (would erase a good value)");
    check(partial.persist_ip, "address still persisted when the gateway is absent");
}

static void test_bad_lease_is_rejected_entirely(void)
{
    printf("== malformed lease rejection ==\n");
    net_addr_config_t bad;
    memset(&bad, 0, sizeof(bad));
    strcpy(bad.ip, "not-an-address");

    net_lease_capture_t cap = net_policy_capture_lease(&bad, true);
    check(!cap.persist_ip, "non-numeric lease address is rejected");
    check(!cap.persist_dns, "nothing is persisted for a malformed lease");

    net_lease_capture_t null_cap = net_policy_capture_lease(NULL, true);
    check(!null_cap.persist_ip, "NULL lease is rejected");

    net_addr_config_t any_ip;
    memset(&any_ip, 0, sizeof(any_ip));
    strcpy(any_ip.ip, "0.0.0.0");
    net_lease_capture_t any_cap = net_policy_capture_lease(&any_ip, true);
    check(!any_cap.persist_ip, "0.0.0.0 lease is rejected");
}

/* --------------------------------------------------------------- the ladder */

static void fill_last_lease(net_recovery_inputs_t *in)
{
    memset(in, 0, sizeof(*in));
    strcpy(in->last_lease.addr.ip, "192.168.178.66");
    strcpy(in->last_lease.addr.gw, "192.168.178.1");
    strcpy(in->last_lease.addr.mask, "255.255.255.0");
    strcpy(in->last_lease.addr.dns, "192.168.178.1");
    in->last_lease.valid = true;
}

static void test_last_lease_replays_in_full(void)
{
    printf("== rung 1 replays the whole lease ==\n");
    net_recovery_inputs_t in;
    fill_last_lease(&in);
    /* A /16 network. Replaying this with a hardcoded /24 - which is what the
     * firmware used to do - produces an address the rest of the network cannot
     * reach, while still reporting itself as connected. */
    strcpy(in.last_lease.addr.mask, "255.255.0.0");
    strcpy(in.last_lease.addr.gw, "10.1.0.254");

    net_fallback_choice_t c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_LAST_LEASE, "rung 1 selected");
    check_str(c.bind.ip, "192.168.178.66", "lease address used");
    check_str(c.bind.gw, "10.1.0.254", "lease gateway used, not a hardcoded one");
    check_str(c.bind.mask, "255.255.0.0", "lease netmask used, not a hardcoded /24");
    check_str(c.bind.dns, "192.168.178.1", "lease resolver used");
}

static void test_incomplete_lease_is_skipped(void)
{
    printf("== rung 1 requires a complete lease ==\n");
    net_recovery_inputs_t in;

    /* Address only. Binding it would require inventing a gateway and a netmask,
     * which is wrong on every network but the one it was guessed from. */
    fill_last_lease(&in);
    in.last_lease.addr.gw[0] = '\0';
    net_fallback_choice_t c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_NONE, "lease without a gateway is skipped");

    fill_last_lease(&in);
    in.last_lease.addr.mask[0] = '\0';
    c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_NONE, "lease without a netmask is skipped");

    fill_last_lease(&in);
    in.last_lease.valid = false;
    c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_NONE, "lease not marked valid is skipped");

    /* But with a complete lease plus a static config, rung 1 still wins because
     * it is the address this unit is already known to hold. */
    fill_last_lease(&in);
    in.use_operator_static = true;
    strcpy(in.operator_static.ip, "10.9.9.9");
    strcpy(in.operator_static.gw, "10.9.9.1");
    c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_LAST_LEASE, "rung 1 takes precedence over a configured address");

    c = net_policy_select_fallback(NULL);
    check(c.rung == NET_FALLBACK_NONE, "NULL inputs yield no fallback");
}

static void test_resolver_preference_order(void)
{
    printf("== fallback resolver preference ==\n");
    net_recovery_inputs_t in;

    /* No lease resolver: fall back to the operator's, not straight to the gateway. */
    fill_last_lease(&in);
    in.last_lease.addr.dns[0] = '\0';
    in.use_operator_static = true;
    strcpy(in.operator_static.dns, "9.9.9.9");
    net_fallback_choice_t c = net_policy_select_fallback(&in);
    check_str(c.bind.dns, "9.9.9.9", "operator resolver used when the lease had none");

    /* No lease resolver and no operator resolver: the gateway is the last resort. */
    fill_last_lease(&in);
    in.last_lease.addr.dns[0] = '\0';
    c = net_policy_select_fallback(&in);
    check_str(c.bind.dns, "192.168.178.1", "gateway used as the final resolver");

    /* A 0.0.0.0 resolver is not a resolver and must not be replayed. */
    fill_last_lease(&in);
    strcpy(in.last_lease.addr.dns, "0.0.0.0");
    c = net_policy_select_fallback(&in);
    check(c.bind.dns[0] != '\0' && strcmp(c.bind.dns, "0.0.0.0") != 0,
          "0.0.0.0 is replaced, not replayed");
}

static void test_operator_static_rung(void)
{
    printf("== rung 2 operator-configured address ==\n");
    net_recovery_inputs_t in;
    memset(&in, 0, sizeof(in));
    strcpy(in.operator_static.ip, "192.168.178.66");
    strcpy(in.operator_static.gw, "192.168.178.1");

    /* Not opted in: nothing is bound. */
    in.use_operator_static = false;
    check(net_policy_select_fallback(&in).rung == NET_FALLBACK_NONE,
          "static address is ignored until it is opted into");

    in.use_operator_static = true;
    net_fallback_choice_t c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_OPERATOR_STATIC, "opted-in static address selected");
    check_str(c.bind.mask, "255.255.255.0", "a missing netmask defaults to /24");
    check_str(c.bind.dns, "192.168.178.1", "a missing resolver defaults to the gateway");

    /* An operator-supplied netmask is honoured rather than overwritten. */
    strcpy(in.operator_static.mask, "255.255.0.0");
    strcpy(in.operator_static.dns, "1.1.1.1");
    c = net_policy_select_fallback(&in);
    check_str(c.bind.mask, "255.255.0.0", "operator netmask honoured");
    check_str(c.bind.dns, "1.1.1.1", "operator resolver honoured");

    /* Opted in but incomplete is still nothing. */
    strcpy(in.operator_static.gw, "");
    check(net_policy_select_fallback(&in).rung == NET_FALLBACK_NONE,
          "static address without a gateway is not bound");

    /* A non-contiguous netmask must not be silently replayed. */
    fill_last_lease(&in);
    in.last_lease.valid = false;
    in.use_operator_static = true;
    strcpy(in.operator_static.ip, "192.168.178.66");
    strcpy(in.operator_static.gw, "192.168.178.1");
    strcpy(in.operator_static.mask, "255.0.255.0");
    c = net_policy_select_fallback(&in);
    check(c.rung == NET_FALLBACK_OPERATOR_STATIC, "static rung still selected with a broken netmask");
    check_str(c.bind.mask, "255.255.255.0", "a broken netmask is replaced with /24");

    /* A malformed address must not be bound even when opted in. */
    memset(&in, 0, sizeof(in));
    in.use_operator_static = true;
    strcpy(in.operator_static.ip, "192.168.178.300");
    strcpy(in.operator_static.gw, "192.168.178.1");
    check(net_policy_select_fallback(&in).rung == NET_FALLBACK_NONE,
          "a malformed configured address is not bound");
}

/* ----------------------------------------------------------- reassert / latch */

static void bind_fallback(net_reassert_state_t *st)
{
    memset(st, 0, sizeof(*st));
    strcpy(st->bound.ip, "192.168.178.66");
    strcpy(st->bound.gw, "192.168.178.1");
    strcpy(st->bound.mask, "255.255.255.0");
    strcpy(st->bound.dns, "192.168.178.1");
    st->have_fallback = true;
    st->fallback_generation = 1;
}

static void test_dhcp_window_is_not_treated_as_a_loss(void)
{
    printf("== reassert does not fight a running DHCP client ==\n");
    net_reassert_state_t st;
    bind_fallback(&st);

    /* Normal DHCP cycle: the interface has no address between DISCOVER and OFFER.
     * Reasserting here would stop the client, which is what makes a fallback
     * permanent for the rest of the boot. */
    net_reassert_decision_t d = net_policy_should_reassert(&st, "0.0.0.0", true);
    check(!d.should_reassert, "no reassert while the DHCP client is running");
    check_str(d.reason, "dhcp_running", "reason is reported as dhcp_running");

    /* Address present: nothing to do. */
    d = net_policy_should_reassert(&st, "192.168.178.66", true);
    check(!d.should_reassert, "no reassert when the address is present");
    check_str(d.reason, "address_present", "reason is reported as address_present");

    /* Address genuinely gone and no client running: this is the real case. */
    d = net_policy_should_reassert(&st, "0.0.0.0", false);
    check(d.should_reassert, "reassert when the address is missing and no client is running");
    check_str(d.bind.ip, "192.168.178.66", "the bound fallback address is what gets re-applied");
    check(d.generation == st.fallback_generation, "decision carries the generation counter");
}

static void test_lease_clears_the_latch(void)
{
    printf("== a genuine lease clears the fallback latch ==\n");
    net_reassert_state_t st;
    bind_fallback(&st);

    net_policy_lease_wins(&st);
    check(!st.have_fallback, "latch cleared by a genuine lease");
    check(st.fallback_generation == 2, "generation bumped so in-flight work is identifiable");
    check_str(st.bound.ip, "", "bound address discarded");

    /* With the latch cleared the watchdog must stay idle for the rest of the boot,
     * which is what restores DHCP recovery. */
    net_reassert_decision_t d = net_policy_should_reassert(&st, "0.0.0.0", false);
    check(!d.should_reassert, "no reassert after the latch is cleared");
    check_str(d.reason, "no_fallback", "reason is reported as no_fallback");

    /* A second lease must not keep bumping the counter - otherwise every renewal
     * looks like a state change. */
    net_policy_lease_wins(&st);
    check(st.fallback_generation == 2, "clearing an already-clear latch does not bump the counter");

    net_reassert_decision_t null_state = net_policy_should_reassert(NULL, "0.0.0.0", false);
    check(!null_state.should_reassert, "NULL state is handled");
    net_policy_lease_wins(NULL); /* must not crash */
}

/* ------------------------------------------------- station reconnect backoff */

/* A router reboot produces a run of NO_AP_FOUND / BEACON_TIMEOUT disconnects and
 * then, while its WPA stack is still coming up, a 4-way-handshake timeout. The
 * retry counter is only reset by a lease, so by then it is well past 3. The old
 * inline policy treated "handshake-type reason with >= 3 retries" as a permanent
 * credential failure and stopped reconnecting for the rest of the boot - the
 * unit stayed offline after the router came back until it was power-cycled. */
static void test_wifi_retry_never_gives_up(void)
{
    printf("== wifi reconnect never gives up ==\n");

    int all_positive = 1;
    int all_bounded = 1;
    for (int attempt = 1; attempt <= 100000; attempt++) {
        for (uint32_t jitter = 0; jitter < 4000; jitter += 997) {
            uint32_t plain = net_policy_wifi_retry_delay_ms(attempt, false, jitter);
            uint32_t auth = net_policy_wifi_retry_delay_ms(attempt, true, jitter);
            if (plain == 0 || auth == 0) all_positive = 0;
            if (plain > NET_WIFI_RETRY_CAP_MS || auth > NET_WIFI_AUTH_RETRY_CAP_MS) all_bounded = 0;
        }
    }
    check(all_positive, "every attempt schedules another connect");
    check(all_bounded, "every delay is capped");

    /* Router-reboot trace: ten "AP gone", then the handshake timeout. */
    uint32_t d = net_policy_wifi_retry_delay_ms(11, true, 0);
    check(d > 0 && d <= NET_WIFI_AUTH_RETRY_CAP_MS, "handshake timeout after a long outage keeps retrying");

    /* A genuinely wrong password is still not hammered: once the auth-type
     * reason has repeated, retries slow to the auth cadence. */
    check(net_policy_wifi_retry_delay_ms(3, true, 0) >= NET_WIFI_AUTH_RETRY_MIN_MS, "repeated auth failure slows down");
    check(net_policy_wifi_retry_delay_ms(1, true, 0) < NET_WIFI_AUTH_RETRY_MIN_MS, "first auth-type failure retries quickly");
    check(net_policy_wifi_retry_delay_ms(3, false, 0) < NET_WIFI_AUTH_RETRY_MIN_MS, "plain failure not slowed by auth cadence");

    /* Unchanged schedule for ordinary disconnects (was inline in connect.c). */
    check(net_policy_wifi_retry_delay_ms(1, false, 0) == 1800, "attempt 1 = 1000 + 800");
    check(net_policy_wifi_retry_delay_ms(2, false, 699) == 1000 + 1600 + 699, "attempt 2 with max jitter");
    check(net_policy_wifi_retry_delay_ms(2, false, 700) == 1000 + 1600, "jitter is taken modulo 700");
    check(net_policy_wifi_retry_delay_ms(9, false, 0) == NET_WIFI_RETRY_CAP_MS, "cap reached");
    check(net_policy_wifi_retry_delay_ms(0, false, 0) == 1000, "attempt 0 clamps to base");
    check(net_policy_wifi_retry_delay_ms(-5, false, 0) == 1000, "negative attempt clamps to base");
    check(net_policy_wifi_retry_delay_ms(2147483647, false, 0xFFFFFFFFu) == NET_WIFI_RETRY_CAP_MS, "INT_MAX attempt does not overflow");
}

int main(void)
{
    printf("net_recovery_policy host tests\n");
    test_wifi_retry_never_gives_up();
    test_ipv4_usable();
    test_ipv4_mask();
    test_resolver_must_come_from_the_lease();
    test_bad_lease_is_rejected_entirely();
    test_last_lease_replays_in_full();
    test_incomplete_lease_is_skipped();
    test_resolver_preference_order();
    test_operator_static_rung();
    test_dhcp_window_is_not_treated_as_a_loss();
    test_lease_clears_the_latch();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("TESTS FAILED\n");
    return 1;
}
