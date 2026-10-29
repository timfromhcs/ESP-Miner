#!/usr/bin/env bash
# Argument checks for tools/build_release.sh. Every case must be rejected before
# any build starts, so this needs no ESP-IDF and runs in a second.
set -u
SCRIPT="${BUILD_RELEASE_SCRIPT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build_release.sh}"
fail=0

expect_reject() {
    local what="$1"; shift
    local out rc
    out=$(bash "$SCRIPT" "$@" 2>&1); rc=$?
    if [ "$rc" -ne 2 ]; then
        echo "FAIL: $what (exit $rc, want 2)"; fail=1
    elif ! grep -qiE 'usage|bssid' <<<"$out"; then
        echo "FAIL: $what (no usage/bssid message)"; fail=1
    else
        echo "ok:   $what"
    fi
}

for v in timsminer blackharkminer; do
    expect_reject "$v without any arguments"               "$v"
    expect_reject "$v with only an ip"                     "$v" 192.168.178.61
    expect_reject "$v without the bssid"                   "$v" 192.168.178.61 192.168.178.1 255.255.255.0 192.168.178.1
    expect_reject "$v with an empty bssid"                 "$v" 192.168.178.61 192.168.178.1 255.255.255.0 192.168.178.1 ""
    expect_reject "$v with a short bssid"                  "$v" 192.168.178.61 192.168.178.1 255.255.255.0 192.168.178.1 aa:bb:cc:dd:ee
    expect_reject "$v with a non-hex bssid"                "$v" 192.168.178.61 192.168.178.1 255.255.255.0 192.168.178.1 zz:bb:cc:dd:ee:ff
    expect_reject "$v with dashes instead of colons"       "$v" 192.168.178.61 192.168.178.1 255.255.255.0 192.168.178.1 aa-bb-cc-dd-ee-ff
    expect_reject "$v with a trailing character"           "$v" 192.168.178.61 192.168.178.1 255.255.255.0 192.168.178.1 aa:bb:cc:dd:ee:ff:
done

# Accepted forms must get past argument checking: they would start a build, so
# only the validation line is exercised here.
valid_bssid='^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$'
for good in aa:bb:cc:dd:ee:ff AA:BB:CC:DD:EE:FF 48:5d:35:0f:39:fc; do
    [[ "$good" =~ $valid_bssid ]] && echo "ok:   accepts $good" || { echo "FAIL: rejects $good"; fail=1; }
done
grep -qF '^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$' "$SCRIPT" || { echo "FAIL: script no longer uses the tested pattern"; fail=1; }
grep -q '"none"' "$SCRIPT" || { echo "FAIL: explicit 'none' opt-out missing"; fail=1; }

[ "$fail" -eq 0 ] && echo "ALL TESTS PASSED" || echo "TESTS FAILED"
exit "$fail"
