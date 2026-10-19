#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/err.h"
#include "lwip/lwip_napt.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "ping/ping_sock.h"
#include "lwip/netif.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_defaults.h"
#include "netif/etharp.h"
#include "nvs_flash.h"
#include "esp_wifi_types_generic.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "connect.h"
#include "net_recovery_policy.h"
#include "global_state.h"
#include "nvs_config.h"
#include "esp_app_desc.h"

/* Access points retained from a UI-initiated scan. The scan buffer is static, so
 * this is a hard cap that the driver's reported count must be clamped to - the
 * driver will happily report far more than this. */
#define MAX_AP_COUNT 20

/* Upper bound for the per-outage mesh survey. The records live in heap there, so
 * this bounds an allocation rather than a fixed buffer, but it still has to be
 * enforced before the array is sized. */
#define MESH_SURVEY_MAX_RECORDS 32

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""

#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID

#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static const char * TAG = "connect";

static TimerHandle_t ip_acquire_timer = NULL;

static bool is_scanning = false;
static uint16_t ap_number = 0;
static wifi_ap_record_t ap_info[MAX_AP_COUNT];
static int s_retry_num = 0;
static int clients_connected_to_ap = 0;
static bool mdns_initialized = false;
static bool mdns_init_in_progress = false;

static const char *get_wifi_reason_string(int reason);
static void wifi_softap_on(void);
static void wifi_softap_off(void);
/* Used from the gateway-probe callback, which runs before this is defined. */
static void ensure_config_ap(void);
static void event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data);

esp_err_t wifi_apply_hostname(const char *hostname)
{
    /* A per-device build may pin the name that goes out in DHCP option 12. Some
     * servers drop a DISCOVER whose option 12 is empty or that they cannot parse,
     * and that is indistinguishable from "the network is down" on the device: the
     * radio is associated, the client is running, and no offer ever comes back. */
    if (ESP_MINER_DHCP_HOSTNAME[0] != '\0') {
        hostname = ESP_MINER_DHCP_HOSTNAME;
    }

    if (hostname == NULL || hostname[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *esp_netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (esp_netif_sta == NULL) {
        ESP_LOGW(TAG, "STA netif not ready; hostname will apply on next Wi-Fi start");
        return ESP_ERR_ESP_NETIF_IF_NOT_READY;
    }

    esp_err_t err = esp_netif_set_hostname(esp_netif_sta, hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Set Wi-Fi hostname to: %s", hostname);

    // Bounce the DHCP client so the new hostname is sent in option 12 on the
    // next DISCOVER/REQUEST. This keeps the Wi-Fi link up — no AP flap.
    err = esp_netif_dhcpc_stop(esp_netif_sta);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcpc_start(esp_netif_sta);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static void initialize_mdns_if_needed(GlobalState *GLOBAL_STATE);
static char* generate_unique_hostname(const char *base);
static char* check_and_resolve_hostname_conflict(const char *hostname, const char *current_ip);

static void mdns_init_task(void *pvParameters) {
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    initialize_mdns_if_needed(GLOBAL_STATE);
    mdns_init_in_progress = false;
    vTaskDelete(NULL);
}

static void spawn_mdns_init_if_needed(GlobalState *GLOBAL_STATE) {
    if (mdns_initialized || mdns_init_in_progress) {
        return;
    }
    mdns_init_in_progress = true;
    BaseType_t ret = xTaskCreate(mdns_init_task, "mdns_init", 4096, GLOBAL_STATE, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create mDNS init task");
        mdns_init_in_progress = false;
    }
}

static void initialize_mdns_if_needed(GlobalState *GLOBAL_STATE) {
    if (mdns_initialized) {
        return;
    }

    char * hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME);
    if (hostname == NULL) {
        ESP_LOGW(TAG, "Hostname not configured, skipping mDNS setup");
        return;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS/Avahi initialization failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "Device will not be discoverable via mDNS/Bonjour/Avahi");
    } else {
        ESP_LOGI(TAG, "mDNS/Avahi initialized successfully - device discoverable on network");

        /* Get current IP */
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif == NULL) {
            netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        }
        char current_ip[16];
        if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No active network interface for mDNS init, using 0.0.0.0");
            strlcpy(current_ip, "0.0.0.0", sizeof(current_ip));
        } else {
            snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&ip_info.ip));
        }

        /* Check for hostname conflicts */
        char *final_hostname = check_and_resolve_hostname_conflict(hostname, current_ip);
        if (final_hostname == NULL) {
            ESP_LOGE(TAG, "Failed to resolve hostname conflicts, skipping mDNS hostname setup");
            free(hostname);
            return;
        }

        /* If conflict resolution changed the hostname, persist to NVS */
        if (strcmp(final_hostname, hostname) != 0) {
            nvs_config_set_string(NVS_CONFIG_HOSTNAME, final_hostname);
            ESP_LOGI(TAG, "Hostname conflict resolved, updated NVS to: %s", final_hostname);
        }

        /* Set mDNS hostname */
        err = mdns_hostname_set(final_hostname);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS hostname setup failed: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "Device hostname not set for mDNS discovery");
        } else {
            ESP_LOGI(TAG, "mDNS hostname set to: %s.local", final_hostname);
            ESP_LOGI(TAG, "Access device at: http://%s.local", final_hostname);
            strlcpy(GLOBAL_STATE->SYSTEM_MODULE.mdns_hostname, final_hostname, sizeof(GLOBAL_STATE->SYSTEM_MODULE.mdns_hostname));
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.full_hostname, sizeof(GLOBAL_STATE->SYSTEM_MODULE.full_hostname), "%s.local", final_hostname);
        }

        free(final_hostname);

        /* Set mDNS instance name */
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char mac_suffix[6];
        snprintf(mac_suffix, sizeof(mac_suffix), "%02X%02X", mac[4], mac[5]);

        char instance_name[64];
        snprintf(instance_name, sizeof(instance_name), "Bitaxe %s %s (%s)",
                 GLOBAL_STATE->DEVICE_CONFIG.family.name,
                 GLOBAL_STATE->DEVICE_CONFIG.board_version,
                 mac_suffix);
        
        /* Add HTTP service */
        err = mdns_service_add(instance_name, "_http", "_tcp", 80, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS HTTP service registration failed: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "HTTP service not advertised via mDNS");
        } else {
            ESP_LOGI(TAG, "mDNS HTTP service registered: _http._tcp port 80");
            ESP_LOGI(TAG, "Discover with: avahi-browse _http._tcp");
            ESP_LOGI(TAG, "mDNS instance: %s", instance_name);

            /* Add AxeOS subtype for DNS-SD discovery */
            err = mdns_service_subtype_add_for_host(instance_name, "_http", "_tcp", NULL, "_axeos");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mDNS AxeOS subtype registration failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "mDNS AxeOS subtype registered: _axeos._sub._http._tcp");
                ESP_LOGI(TAG, "Discover AxeOS devices with: avahi-browse _axeos._sub._http._tcp");
            }

            /* Add TXT records for device identification */
            err = mdns_service_txt_item_set_for_host(instance_name, "_http", "_tcp", NULL, "board", GLOBAL_STATE->DEVICE_CONFIG.board_version);
            if (err != ESP_OK) ESP_LOGW(TAG, "mDNS TXT 'board' failed: %s", esp_err_to_name(err));
            err = mdns_service_txt_item_set_for_host(instance_name, "_http", "_tcp", NULL, "family", GLOBAL_STATE->DEVICE_CONFIG.family.name);
            if (err != ESP_OK) ESP_LOGW(TAG, "mDNS TXT 'family' failed: %s", esp_err_to_name(err));
            err = mdns_service_txt_item_set_for_host(instance_name, "_http", "_tcp", NULL, "asic", GLOBAL_STATE->DEVICE_CONFIG.family.asic.name);
            if (err != ESP_OK) ESP_LOGW(TAG, "mDNS TXT 'asic' failed: %s", esp_err_to_name(err));
            char asic_count_str[4];
            snprintf(asic_count_str, sizeof(asic_count_str), "%u", GLOBAL_STATE->DEVICE_CONFIG.family.asic_count);
            err = mdns_service_txt_item_set_for_host(instance_name, "_http", "_tcp", NULL, "asic_count", asic_count_str);
            if (err != ESP_OK) ESP_LOGW(TAG, "mDNS TXT 'asic_count' failed: %s", esp_err_to_name(err));
            const esp_app_desc_t *app_desc = esp_app_get_description();
            err = mdns_service_txt_item_set_for_host(instance_name, "_http", "_tcp", NULL, "fw_version", app_desc->version);
            if (err != ESP_OK) ESP_LOGW(TAG, "mDNS TXT 'fw_version' failed: %s", esp_err_to_name(err));
            ESP_LOGI(TAG, "mDNS TXT records added: board=%s, family=%s, asic=%s, asic_count=%s, fw_version=%s",
                     GLOBAL_STATE->DEVICE_CONFIG.board_version,
                     GLOBAL_STATE->DEVICE_CONFIG.family.name,
                     GLOBAL_STATE->DEVICE_CONFIG.family.asic.name,
                     asic_count_str,
                     app_desc->version);
        }

        ESP_LOGI(TAG, "mDNS/Avahi setup complete - device ready for network discovery");
        mdns_initialized = true;
    }
    free(hostname);
}

esp_err_t update_mdns_hostname(const char *new_hostname, GlobalState *GLOBAL_STATE) {
    if (new_hostname == NULL || strlen(new_hostname) == 0) {
        ESP_LOGW(TAG, "Invalid hostname provided for mDNS update");
        return ESP_ERR_INVALID_ARG;
    }

    /* Get current IP for conflict checking */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    }
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "No active network interface for mDNS hostname update");
        ip_info.ip.addr = 0;
    }
    char current_ip[16];
    if (ip_info.ip.addr == 0) {
        strlcpy(current_ip, "0.0.0.0", sizeof(current_ip));
    } else {
        snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&ip_info.ip));
    }

    /* Check for hostname conflicts and resolve if needed */
    char *resolved_hostname = check_and_resolve_hostname_conflict(new_hostname, current_ip);
    if (resolved_hostname == NULL) {
        ESP_LOGE(TAG, "Failed to resolve hostname conflicts");
        return ESP_ERR_NO_MEM;
    }

    /* If the hostname was resolved to a different one, update NVS */
    if (strcmp(resolved_hostname, new_hostname) != 0) {
        nvs_config_set_string(NVS_CONFIG_HOSTNAME, resolved_hostname);
        ESP_LOGI(TAG, "Hostname conflict resolved, updated NVS to: %s", resolved_hostname);
    }

    esp_err_t err = mdns_hostname_set(resolved_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update mDNS hostname to: %s, error: %s", resolved_hostname, esp_err_to_name(err));
        free(resolved_hostname);
        return err;
    }

    ESP_LOGI(TAG, "mDNS hostname updated to: %s", resolved_hostname);
    if (GLOBAL_STATE != NULL) {
        strlcpy(GLOBAL_STATE->SYSTEM_MODULE.mdns_hostname, resolved_hostname, sizeof(GLOBAL_STATE->SYSTEM_MODULE.mdns_hostname));
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.full_hostname, sizeof(GLOBAL_STATE->SYSTEM_MODULE.full_hostname), "%s.local", resolved_hostname);
    }
    free(resolved_hostname);
    return ESP_OK;
}

esp_err_t get_wifi_current_rssi(int8_t *rssi)
{
    wifi_ap_record_t current_ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&current_ap_info);

    if (err == ESP_OK) {
        *rssi = current_ap_info.rssi;
        return ERR_OK;
    }

    return err;
}

// Function to scan for available WiFi networks
esp_err_t wifi_scan(wifi_ap_record_simple_t *ap_records, uint16_t *ap_count)
{
    if (is_scanning) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting Wi-Fi scan!");
    is_scanning = true;

    wifi_ap_record_t current_ap_info;
    if (esp_wifi_sta_get_ap_info(&current_ap_info) != ESP_OK) {
        ESP_LOGI(TAG, "Forcing disconnect so that we can scan!");
        esp_wifi_disconnect();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

     wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan start failed with error: %s", esp_err_to_name(err));
        is_scanning = false;
        return err;
    }

    uint16_t retries_remaining = 10;
    while (is_scanning) {
        retries_remaining--;
        if (retries_remaining == 0) {
            is_scanning = false;
            return ESP_FAIL;
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    ESP_LOGD(TAG, "Wi-Fi networks found: %d", ap_number);
    if (ap_number == 0) {
        ESP_LOGW(TAG, "No Wi-Fi networks found");
    }

    *ap_count = ap_number;
    memset(ap_records, 0, (*ap_count) * sizeof(wifi_ap_record_simple_t));
    for (int i = 0; i < ap_number; i++) {
        memcpy(ap_records[i].ssid, ap_info[i].ssid, sizeof(ap_records[i].ssid));
        ap_records[i].rssi = ap_info[i].rssi;
        ap_records[i].authmode = ap_info[i].authmode;
    }

    ESP_LOGD(TAG, "Finished Wi-Fi scan!");

    return ESP_OK;
}

// Explicit network state model — single source of truth (see docs/NETWORKING.md)
typedef enum {
    NET_STATE_OFF = 0,
    NET_STATE_WIFI_INIT,
    NET_STATE_WIFI_CONNECTING,
    NET_STATE_WIFI_CONNECTED_NO_IP,
    NET_STATE_DHCP_RUNNING,
    NET_STATE_DHCP_RETRY,
    NET_STATE_IP_ACQUIRED,
    NET_STATE_ROUTE_CHECK,
    NET_STATE_DNS_READY,
    NET_STATE_INTERNET_READY,
    NET_STATE_STRATUM_CONNECTING,
    NET_STATE_STRATUM_READY,
    NET_STATE_MINING_READY,
    NET_STATE_WIFI_AUTH_FAILED,
    NET_STATE_DHCP_FAILED,
    NET_STATE_DNS_FAILED,
    NET_STATE_ROUTE_FAILED,
    NET_STATE_STRATUM_FAILED,
    NET_STATE_WIFI_RECOVERY,
    NET_STATE_DHCP_RECOVERY,
    NET_STATE_MAX
} net_state_t;

static net_state_t s_net_state = NET_STATE_OFF;
static uint32_t s_network_generation = 0;
static uint32_t s_dhcp_generation = 0;
static uint32_t s_stratum_generation = 0;
static int dhcp_retry_count = 0;

/* True while the current address came from the fallback ladder rather than from a
 * DHCP server.
 *
 * Without this an address *we* chose gets written to NVS as the remembered lease,
 * and the next outage replays it as rung 1 - so a fallback becomes permanent and
 * silently outranks the address the operator actually configured. That is how a
 * unit ends up stuck on an address nobody chose, with no log line saying so. */
static bool s_addr_is_fallback = false;

/* Used by the DHCP retry ladder; defined further down. */
static void wifi_connect_pinned(void);
/* Retry budget kept deliberately short. Each rung of the ladder is attempted as
 * soon as there is any chance of it succeeding, and DHCP keeps running in the
 * background throughout, so waiting longer only extends the outage. */
/* DHCP retry policy.
 *
 * No maximum: the loop is endless by design, because a miner that stops asking
 * stays offline forever, and the only thing that should end the retry is the router
 * answering. The backoff is bounded so a unit that has been offline for hours still
 * recovers within seconds of the network returning. */
#define DHCP_RETRY_BASE_MS 2500
#define DHCP_BACKOFF_CAP_MS 12000

/* Escalation ladder: each step changes *how* the client asks rather than only how
 * often. Bouncing the client produces a fresh DISCOVER with a new transaction id; a
 * Wi-Fi re-association re-registers the station with a mesh DHCP relay. Both are
 * needed because the two failure modes are distinct and indistinguishable from the
 * outside. */
#define DHCP_ESCALATION_CYCLE 4

/* First retry timeout after which the fallback ladder is consulted. The last
 * real lease costs nothing to try — no negotiation, no timeout, and it is an
 * address this unit is already known to hold — so there is no reason to make the
 * operator idling through a full retry budget first. */
#define DHCP_LADDER_AFTER_RETRY 1

/* Re-assert a bound fallback address if it disappears.
 *
 * A static address is not stable on its own: esp-netif clears it when the DHCP
 * client times out, the default STA_CONNECTED handler overwrites it via
 * esp_netif_set_old_ip_info(), and the IP lost timer zeroes it once the netif has
 * been down long enough. So a fallback address is re-applied when it vanishes.
 *
 * The state and the decision live in net_recovery_policy, which is pure and
 * host-tested. `have_fallback` is a latch describing *now* - it is cleared the
 * moment a genuine lease arrives, by net_policy_lease_wins() below. That clearing
 * is the whole point: while it stayed set for the rest of the boot, this task
 * kept stopping the DHCP client on every window in which the interface
 * legitimately had no address (between DISCOVER and OFFER, and on every renewal),
 * so a unit that fell back once could never obtain a lease again. The policy
 * refuses to act while the client is running, which is what keeps the fallback
 * from becoming a one-way door. */
static net_reassert_state_t s_reassert;

/* One esp_netif_get_ip_info() per second, which is a struct copy. Real work
 * happens only when the address is actually missing. */
static void ip_reassert_check(void)
{
    if (!s_reassert.have_fallback) {
        return;
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL || !esp_netif_is_netif_up(sta)) {
        return;
    }

    esp_netif_ip_info_t cur = {0};
    char cur_s[IP4ADDR_STRLEN_MAX] = "";
    if (esp_netif_get_ip_info(sta, &cur) == ESP_OK && cur.ip.addr != 0) {
        snprintf(cur_s, sizeof(cur_s), IPSTR, IP2STR(&cur.ip));
    }

    /* Whether the client is running is part of the decision, not an afterthought:
     * reasserting while it runs would stop it and destroy the recovery path. */
    /* esp_netif_dhcp_status_t only distinguishes INIT / STARTED / STOPPED in
     * ESP-IDF v6 - there is no BOUND state - so "still running" is simply
     * "not stopped". INIT counts as running: the client has been created and is
     * ours to leave alone. */
    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STOPPED;
    bool dhcp_running = (esp_netif_dhcpc_get_status(sta, &dhcp_status) == ESP_OK)
                        && (dhcp_status != ESP_NETIF_DHCP_STOPPED);

    net_reassert_decision_t d = net_policy_should_reassert(&s_reassert, cur_s, dhcp_running);
    if (!d.should_reassert) {
        ESP_LOGD(TAG, "NET,event=IP_FALLBACK_WATCH,reason=%s,gen=%lu",
                 d.reason, (unsigned long) d.generation);
        return;
    }

    ESP_LOGW(TAG, "NET,event=IP_FALLBACK_LOST,ip=%s,reasserting=1,gen=%lu",
             s_reassert.bound.ip, (unsigned long) s_reassert.fallback_generation);
    esp_netif_ip_info_t want = {0};
    esp_netif_str_to_ip4(s_reassert.bound.ip, &want.ip);
    esp_netif_str_to_ip4(s_reassert.bound.gw, &want.gw);
    esp_netif_str_to_ip4(s_reassert.bound.mask, &want.netmask);

    /* The policy only returns true when no client is running, so this cannot
     * interrupt an in-flight negotiation. */
    esp_err_t stop_err = esp_netif_dhcpc_stop(sta);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "NET,event=IP_FALLBACK_LOST,ip=%s,reassert=DHCP_STOP_FAILED,err=%s",
                 s_reassert.bound.ip, esp_err_to_name(stop_err));
        return;
    }

    struct netif *gn = (struct netif *) esp_netif_get_netif_impl(sta);
    if (gn != NULL && !(gn->flags & NETIF_FLAG_UP)) {
        netif_set_up(gn);
        netif_set_link_up(gn);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_err_t err = esp_netif_set_ip_info(sta, &want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NET,event=IP_FALLBACK_LOST,ip=%s,reassert=FAILED,err=%s",
                 s_reassert.bound.ip, esp_err_to_name(err));
        return;
    }

    if (s_reassert.bound.dns[0] != '\0') {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(s_reassert.bound.dns, &dns.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
            esp_netif_set_dns_info(NULL, ESP_NETIF_DNS_MAIN, &dns);
        }
    }

    ESP_LOGW(TAG, "NET,event=IP_FALLBACK_REASSERTED,ip=%s,gw=%s,mask=%s",
             s_reassert.bound.ip, s_reassert.bound.gw, s_reassert.bound.mask);
}

static void ip_reassert_task(void * arg)
{
    (void) arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ip_reassert_check();
    }
}

/* How often a unit that came up on a fallback address asks DHCP for a real lease
 * again.
 *
 * Starting a client with no server available clears the interface address, which
 * is why the fallback address stays bound and only the OFFER is requested. The
 * address is therefore at risk for the few hundred milliseconds a negotiation
 * takes; a lease arriving in that window supersedes the fallback outright and
 * never touches it, and a negotiation that fails simply leaves the existing
 * address in place. That is the safe direction: a failed probe costs one
 * renegotiation, whereas the previous behaviour - re-running the ladder - could
 * never leave the static address once it had bound it. */
#define DHCP_PROBE_INTERVAL_MS 300000
static uint32_t s_dhcp_probe_due_ms = 0;

static const char *net_state_to_str(net_state_t s) {
    switch (s) {
        case NET_STATE_OFF: return "OFF";
        case NET_STATE_WIFI_INIT: return "WIFI_INIT";
        case NET_STATE_WIFI_CONNECTING: return "WIFI_CONNECTING";
        case NET_STATE_WIFI_CONNECTED_NO_IP: return "WIFI_CONNECTED_NO_IP";
        case NET_STATE_DHCP_RUNNING: return "DHCP_RUNNING";
        case NET_STATE_DHCP_RETRY: return "DHCP_RETRY";
        case NET_STATE_IP_ACQUIRED: return "IP_ACQUIRED";
        case NET_STATE_ROUTE_CHECK: return "ROUTE_CHECK";
        case NET_STATE_DNS_READY: return "DNS_READY";
        case NET_STATE_INTERNET_READY: return "INTERNET_READY";
        case NET_STATE_STRATUM_CONNECTING: return "STRATUM_CONNECTING";
        case NET_STATE_STRATUM_READY: return "STRATUM_READY";
        case NET_STATE_MINING_READY: return "MINING_READY";
        case NET_STATE_WIFI_AUTH_FAILED: return "WIFI_AUTH_FAILED";
        case NET_STATE_DHCP_FAILED: return "DHCP_FAILED";
        case NET_STATE_DNS_FAILED: return "DNS_FAILED";
        case NET_STATE_ROUTE_FAILED: return "ROUTE_FAILED";
        case NET_STATE_STRATUM_FAILED: return "STRATUM_FAILED";
        case NET_STATE_WIFI_RECOVERY: return "WIFI_RECOVERY";
        case NET_STATE_DHCP_RECOVERY: return "DHCP_RECOVERY";
        default: return "UNKNOWN";
    }
}

static void net_state_transition(GlobalState *gs, net_state_t new_state) {
    if (s_net_state == new_state) return;
    net_state_t old = s_net_state;
    s_net_state = new_state;
    if (new_state == NET_STATE_IP_ACQUIRED) {
        s_network_generation++;
        s_dhcp_generation++;
    }
    if (new_state == NET_STATE_DHCP_FAILED) {
        s_dhcp_generation++;
    }
    ESP_LOGI(TAG, "NET,event=STATE_TRANSITION,from=%s,to=%s,gen=%lu,dhcp_gen=%lu,retry=%d,ts=%lld",
        net_state_to_str(old), net_state_to_str(new_state),
        (unsigned long)s_network_generation, (unsigned long)s_dhcp_generation,
        dhcp_retry_count, (long long)esp_timer_get_time()/1000);
    // Single source of truth — UI/API/Stratum must read s_net_state, not infer.
    (void)gs;
    // is_connected only true when we have a real DHCP lease, not fake static
    // kept compatible: actual flag set in IP_EVENT handlers
}

/**
 * @brief Bind a specific IPv4 address to the station and bring it fully online.
 *
 * Shared by both fallback sources so the ordering fix, the read-back checks, DNS,
 * gratuitous ARP and the state-machine advance can only ever be right in one
 * place. `source` is only used for logging.
 */
static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void) args;
    uint32_t sent = 0;
    uint32_t recv = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    if (recv == 0) {
        ESP_LOGE(TAG, "NET,event=GATEWAY_PROBE,result=UNREACHABLE,sent=%lu,received=0",
                 (unsigned long) sent);
        /* An address is bound but the gateway does not answer. The unit is
         * associated and has an IP and still cannot reach anything, so the local
         * web-login fallback is the only route left to it - bring it up. Without
         * this a unit in this state is unreachable by every means at once. */
        ensure_config_ap();
    } else {
        ESP_LOGW(TAG, "NET,event=GATEWAY_PROBE,result=OK,sent=%lu,received=%lu",
                 (unsigned long) sent, (unsigned long) recv);
    }
}

/**
 * @brief Reachability probe that reports the real errno.
 *
 * esp_ping only reports how many packets were handed to the socket, and lwIP does
 * not implement getsockopt(SO_ERROR), so a route or ARP failure shows up as
 * "sent=0, received=0" with no way to tell why. A TCP connect to a port on the
 * gateway exercises the identical path - route lookup, ARP, then transmit - and
 * leaves the errno intact, which is what actually separates "no route to host"
 * from "packets going out and being dropped".
 *
 * Runs on its own task: connect() can block for a full retransmit window and the
 * fallback path must not stall behind it.
 */
static void gateway_tcp_probe_task(void * arg)
{
    char * gw_s = (char *) arg;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    if (inet_aton(gw_s, &dst.sin_addr) != 1) {
        ESP_LOGE(TAG, "NET,event=GATEWAY_TCP,target=%s,result=BAD_ADDRESS", gw_s);
        goto done;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "NET,event=GATEWAY_TCP,target=%s,result=SOCKET_FAILED,errno=%d(%s)",
                 gw_s, errno, strerror(errno));
        goto done;
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    errno = 0;
    int r = connect(s, (struct sockaddr *) &dst, sizeof(dst));
    if (r == 0) {
        ESP_LOGW(TAG, "NET,event=GATEWAY_TCP,target=%s:80,result=CONNECTED", gw_s);
    } else {
        ESP_LOGE(TAG, "NET,event=GATEWAY_TCP,target=%s:80,result=FAILED,errno=%d(%s)",
                 gw_s, errno, strerror(errno));
    }
    close(s);

done:
    free(gw_s);
    vTaskDelete(NULL);
}

static void gateway_tcp_probe(const char * gw_s)
{
    char * target = strdup(gw_s);
    if (target == NULL) {
        return;
    }
    xTaskCreate(gateway_tcp_probe_task, "gw_probe", 3072, target, 3, NULL);
}

static bool apply_static_address(GlobalState * GLOBAL_STATE, const char * ip_s, const char * gw_s,
                                const char * mask_s, const char * dns_s, const char * source)
{
    bool ok = false;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (sta == NULL || ip_s == NULL || ip_s[0] == '\0' || gw_s == NULL || gw_s[0] == '\0') {
        ESP_LOGW(TAG, "NET,event=IP_FALLBACK_SKIPPED,source=%s,reason=incomplete", source);
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(ip_s, &ip_info.ip) != ESP_OK
        || esp_netif_str_to_ip4(gw_s, &ip_info.gw) != ESP_OK) {
        ESP_LOGE(TAG, "NET,event=IP_FALLBACK_INVALID,source=%s,ip=%s,gw=%s", source, ip_s, gw_s);
        return false;
    }
    if (mask_s != NULL && mask_s[0] != '\0') {
        esp_netif_str_to_ip4(mask_s, &ip_info.netmask);
    }

/* Order is load-bearing, and getting it wrong produces a unit that looks
     * configured but can never send or receive anything.
     *
     * esp_netif_dhcpc_stop() leaves the netif administratively DOWN, and
     * esp_netif_set_ip_info() only pushes the address into the live TCP/IP stack
     * "if the interface is up" - otherwise it merely updates esp-netif's own copy.
     * So: stop the client first (set_ip_info refuses to run against a running
     * client), then bring the interface back UP, and only then assign. Assigning
     * while the netif is down leaves lwIP with ip_addr == 0, which answers no ARP
     * and yields EHOSTUNREACH for every destination, including the gateway.
     *
     * The read-back further down asks esp-netif, which is NOT sufficient evidence:
     * that copy is updated whether or not the value reached the stack. */
    esp_netif_dhcpc_stop(sta);

    /* Bring the interface up BEFORE assigning. This is the step that makes the
     * address actually reach the stack. */
    {
        struct netif *gn = (struct netif *) esp_netif_get_netif_impl(sta);
        if (gn != NULL && !(gn->flags & NETIF_FLAG_UP)) {
            netif_set_up(gn);
            netif_set_link_up(gn);
            ESP_LOGW(TAG, "NET,event=NETIF_RAISE,netif_up=%d",
                     (gn->flags & NETIF_FLAG_UP) ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    /* Give esp-netif a consistent picture: the client is stopped, the interface is
     * up, and the address is ours. */
    esp_netif_set_ip_info(sta, &ip_info);
    esp_netif_set_default_netif(sta);

    if (dns_s != NULL && dns_s[0] != '\0') {
        esp_netif_dns_info_t dns_main = {0};
        dns_main.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(dns_s, &dns_main.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_main);
            esp_netif_set_dns_info(NULL, ESP_NETIF_DNS_MAIN, &dns_main);
        }
        esp_netif_dns_info_t dns_fallback = {0};
        dns_fallback.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(gw_s, &dns_fallback.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(NULL, ESP_NETIF_DNS_FALLBACK, &dns_fallback);
        }
    }

    /* Read the address back rather than trusting the setter, and assert the
     * interface is up: with the netif down the address never reaches LwIP, so a
     * read-back of esp-netif's own copy is not sufficient evidence. */
    esp_netif_ip_info_t check = {0};
    char check_s[IP4ADDR_STRLEN_MAX];
    if (esp_netif_get_ip_info(sta, &check) != ESP_OK || check.ip.addr == 0) {
        ESP_LOGE(TAG, "NET,event=IP_FALLBACK_FAILED,source=%s,reason=ip_not_bound,ip=%s", source, ip_s);
        return false;
    }
    snprintf(check_s, sizeof(check_s), IPSTR, IP2STR(&check.ip));
    if (!esp_netif_is_netif_up(sta)) {
        ESP_LOGE(TAG, "NET,event=IP_FALLBACK_FAILED,source=%s,reason=netif_down", source);
        return false;
    }

#if LWIP_ARP
    {
        struct netif *g = (struct netif *) esp_netif_get_netif_impl(sta);
        if (g != NULL) {
            etharp_gratuitous(g);
        }
    }
#endif

    /* Verify the result rather than trusting the setters. Without this the only
     * symptom of a silently-ignored DNS or route assignment is a stratum task
     * retrying "DNS resolution failed" forever with nothing in the log explaining
     * why. The ARP table is included because it is the only direct evidence of
     * whether traffic is actually flowing: an empty table after a gratuitous ARP
     * plus several seconds means nothing is reaching the wire at all. */
    {
        esp_netif_ip_info_t got_ip = {0};
        esp_netif_get_ip_info(sta, &got_ip);
        esp_netif_dns_info_t got_dns = {0};
        esp_err_t dns_err = esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &got_dns);
        char ip_s2[IP4ADDR_STRLEN_MAX], gw_s2[IP4ADDR_STRLEN_MAX];
        char dns_s2[IP4ADDR_STRLEN_MAX] = "(unset)";
        if (dns_err == ESP_OK && !ip4_addr_isany_val(got_dns.ip.u_addr.ip4)) {
            snprintf(dns_s2, sizeof(dns_s2), IPSTR, IP2STR(&got_dns.ip.u_addr.ip4));
        }
        snprintf(ip_s2, sizeof(ip_s2), IPSTR, IP2STR(&got_ip.ip));
        snprintf(gw_s2, sizeof(gw_s2), IPSTR, IP2STR(&got_ip.gw));
        char mask_s2[IP4ADDR_STRLEN_MAX] = "(unset)";
        if (!ip4_addr_isany_val(got_ip.netmask)) {
            snprintf(mask_s2, sizeof(mask_s2), IPSTR, IP2STR(&got_ip.netmask));
        }

        ESP_LOGW(TAG, "NET,event=IP_FALLBACK_VERIFY,source=%s,ip=%s,gw=%s,mask=%s,dns=%s,dns_get=%s,espnetif_up=%d",
                 source, ip_s2, gw_s2, mask_s2, dns_s, dns_s2, esp_netif_is_netif_up(sta) ? 1 : 0);
    }

    /* Publish the bound address to the re-assert watchdog. Every field is a
     * validated, non-empty string at this point. */
    snprintf(s_reassert.bound.ip, sizeof(s_reassert.bound.ip), "%s", check_s);
    snprintf(s_reassert.bound.gw, sizeof(s_reassert.bound.gw), "%s", gw_s);
    snprintf(s_reassert.bound.mask, sizeof(s_reassert.bound.mask), "%s",
             (mask_s && mask_s[0]) ? mask_s : "255.255.255.0");
    snprintf(s_reassert.bound.dns, sizeof(s_reassert.bound.dns), "%s", dns_s ? dns_s : "");
    s_reassert.have_fallback = true;
    s_reassert.fallback_generation++;

    /* Do NOT restart the DHCP client here. Once a fallback address is ours,
     * restarting the client hands it straight back to the DHCP machine, and three
     * separate mechanisms then take it away again:
     *   - the client's own timeout makes esp-netif clear the stored IP to 0.0.0.0
     *   - the default STA_CONNECTED handler calls esp_netif_set_old_ip_info(),
     *     documented to overwrite the previous address
     *   - the "IP lost timer" zeroes it once the netif has been down long enough
     * The symptom was a unit that came up for a few seconds and then vanished,
     * which reads exactly like a router fault and is not one. A fallback address is
     * therefore re-asserted by us; see ip_reassert_check(). A real lease still
     * wins, because it calls esp_netif_set_ip_info() itself. */

    /* Probe the gateway from the device itself. This is the only way to tell
     * "the interface is configured" from "traffic actually flows": if the device
     * cannot reach its own gateway, no amount of address configuration will make
     * it reachable, and the cause is on the access point rather than here. */
    {
        static const esp_ping_callbacks_t cbs = {
            .on_ping_success = NULL,
            .on_ping_timeout = NULL,
            .on_ping_end = on_ping_end,
            .cb_args = NULL,
        };
        esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
        if (ipaddr_aton(gw_s, &pc.target_addr) != 1) {
            ESP_LOGE(TAG, "NET,event=GATEWAY_PROBE,target=%s,result=BAD_ADDRESS", gw_s);
        } else {
            pc.count = 3;
            pc.interval_ms = 500;
            pc.timeout_ms = 1000;
            pc.task_stack_size = 4096;
            esp_ping_handle_t ph = NULL;
            esp_err_t perr = esp_ping_new_session(&pc, &cbs, &ph);
            if (perr == ESP_OK) {
                esp_ping_start(ph);
            } else {
                ESP_LOGE(TAG, "NET,event=GATEWAY_PROBE,target=%s,result=PROBE_UNAVAILABLE,err=%s",
                         gw_s, esp_err_to_name(perr));
            }
        }
        gateway_tcp_probe(gw_s);
    }

    /* End-to-end reachability is not asserted here on purpose: the stratum connect
     * that follows proves it, and its failure is already logged distinctly. */
    ESP_LOGW(TAG, "NET,event=IP_FALLBACK_APPLIED,source=%s,ip=%s,gw=%s,netmask=%s",
             source, check_s, gw_s, (mask_s && mask_s[0]) ? mask_s : "(default)");

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, IP4ADDR_STRLEN_MAX, "%s", check_s);
    spawn_mdns_init_if_needed(GLOBAL_STATE);
    GLOBAL_STATE->SYSTEM_MODULE.is_connected = true;
    strcpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Connected (no DHCP)");
    net_state_transition(GLOBAL_STATE, NET_STATE_IP_ACQUIRED);
    net_state_transition(GLOBAL_STATE, NET_STATE_DNS_READY);
    ok = true;
    return ok;
}

/**
 * @brief Recovery ladder used once DHCP has genuinely failed.
 *
 * Order, cheapest and safest first:
 *   1. the last address the DHCP server really handed out - it is a lease this
 *      unit is already known to hold, so it cannot collide with a stranger;
 *   2. the operator-configured static address, if one was set for this device.
 *
 * Whichever succeeds, the caller stops. If neither does, the caller returns to
 * the reconnect/DHCP loop, so the ladder is re-attempted on every cycle and the
 * unit comes up as soon as the router answers again.
 */
static bool try_ip_fallback_ladder(GlobalState * GLOBAL_STATE)
{
    /* Which rung to take, and whether it is usable at all, is decided by
     * net_recovery_policy (components/connect/net_recovery_policy.c). That file is
     * pure logic and is differential-tested on the host; this function is only
     * responsible for reading NVS, handing the answer to apply_static_address(),
     * and freeing what it read. */
    net_recovery_inputs_t in;
    memset(&in, 0, sizeof(in));

    char *last_ip = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_IP);
    char *last_dns = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_DNS);
    char *last_gw = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_GW);
    char *last_mask = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_MASK);
    char *static_gw = nvs_config_get_string(NVS_CONFIG_STATIC_GATEWAY);
    char *static_mask = nvs_config_get_string(NVS_CONFIG_STATIC_SUBNET);
    char *static_dns = nvs_config_get_string(NVS_CONFIG_STATIC_DNS);
    char *static_ip = nvs_config_get_string(NVS_CONFIG_STATIC_IP);

    in.use_operator_static = nvs_config_get_bool(NVS_CONFIG_USE_STATIC_FALLBACK);

    /* The remembered lease is only usable if the address is there. The gateway and
     * netmask are checked by the policy: an address alone cannot be replayed
     * without inventing them, and an invented pair is wrong on every network but
     * the one it was guessed from. */
    /* net_addr_config_t fields are fixed-size arrays, not pointers: copy through a
     * bounded helper rather than assigning, and treat NULL as "unset" so an empty
     * NVS string stays an empty string instead of becoming a wild pointer. */
    net_addr_set(in.last_lease.addr.ip, last_ip);
    net_addr_set(in.last_lease.addr.gw, last_gw);
    net_addr_set(in.last_lease.addr.mask, last_mask);
    net_addr_set(in.last_lease.addr.dns, last_dns);
    in.last_lease.valid = net_ipv4_is_usable(in.last_lease.addr.ip);

    net_addr_set(in.operator_static.ip, static_ip);
    net_addr_set(in.operator_static.gw, static_gw);
    net_addr_set(in.operator_static.mask, static_mask);
    net_addr_set(in.operator_static.dns, static_dns);

    net_fallback_choice_t choice = net_policy_select_fallback(&in);

    bool ok = false;
    if (choice.rung != NET_FALLBACK_NONE) {
        const char *source = (choice.rung == NET_FALLBACK_LAST_LEASE) ? "last-lease" : "static-config";
        ESP_LOGW(TAG, "NET,event=IP_FALLBACK_ATTEMPT,rung=%s,ip=%s,gw=%s,mask=%s,dns=%s",
                 source, choice.bind.ip, choice.bind.gw, choice.bind.mask, choice.bind.dns);
        /* Set *before* the address is applied: apply_static_address() makes LwIP report
         * IP_ACQUIRED synchronously, so a flag set afterwards is always one event
         * too late and the address we chose gets remembered as a lease. */
        s_addr_is_fallback = true;
        ok = apply_static_address(GLOBAL_STATE, choice.bind.ip, choice.bind.gw,
                                  choice.bind.mask, choice.bind.dns, source);
        s_addr_is_fallback = ok;
    }

    if (!ok) {
        ESP_LOGW(TAG, "NET,event=IP_FALLBACK_NONE,last_lease=%s,lease_complete=%d,static_configured=%d",
                 in.last_lease.valid ? in.last_lease.addr.ip : "(none)",
                 (net_ipv4_is_usable(in.last_lease.addr.gw) && net_ipv4_is_mask(in.last_lease.addr.mask)) ? 1 : 0,
                 in.use_operator_static ? 1 : 0);
    }

    free(last_ip);
    free(last_dns);
    free(last_gw);
    free(last_mask);
    free(static_ip);
    free(static_gw);
    free(static_mask);
    free(static_dns);
    return ok;
}

/**
 * @brief Schedule the next attempt to win back a genuine DHCP lease.
 *
 * Only armed when we came up on a fallback address. This is a liveness mechanism
 * for the migration path, not a rebind: the address stays bound and DHCP is
 * simply asked to offer again.
 */
static void arm_dhcp_probe(void)
{
    s_dhcp_probe_due_ms = (uint32_t) (esp_timer_get_time() / 1000) + (DHCP_PROBE_INTERVAL_MS / 1000);
    ESP_LOGI(TAG, "NET,event=DHCP_REPROBE_ARMED,in=%ds", DHCP_PROBE_INTERVAL_MS / 1000);
}

/* Survey the visible mesh once per outage and log what is out there.
 *
 * Every node in a mesh broadcasts the same SSID, so this is the only way to see
 * that the station landed on a repeater instead of the router: a station that
 * associates but never appears in the router's own device list is talking to a
 * node that does not relay it. The scan runs while disconnected, because
 * esp_wifi_scan_start() does not complete reliably on an associated station and a
 * silent scan would hide exactly what we are looking for.
 *
 * This is a diagnostic, not a control path: nothing here changes what the firmware
 * does next. It therefore runs on its own task rather than inside the DHCP retry
 * timer callback, which runs in the timer service task - a blocking scan plus two
 * delays in there stalls every other software timer in the system. */

/* One survey at a time: the scan disconnects the station, so two overlapping ones
 * would fight over the same radio. The task clears the flag before deleting
 * itself, so a completed survey and a failed xTaskCreate both leave it correct. */
static atomic_bool s_mesh_survey_running;

/* Cleared when a lease is actually bound, so a later outage is surveyed again. */
static bool s_mesh_survey_done_this_outage = false;

/* Has a real DHCP lease ever been bound this boot? Distinguishes "no address yet"
 * from "on a fallback address", and is what keeps the config AP from being the
 * normal way to reach a unit that is perfectly well connected. */
static bool s_ever_had_lease = false;

/* Remember a lease that a DHCP server really handed out.
 *
 * Only writes what changed: a server that keeps reissuing the same reserved
 * address must not cause an NVS write per renewal, and NVS writes are finite. */
static void capture_lease_to_nvs(const net_lease_capture_t *cap)
{
    if (cap->persist_ip) {
        char *known = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_IP);
        if (known == NULL || strcmp(known, cap->addr.ip) != 0) {
            nvs_config_set_string(NVS_CONFIG_LAST_DHCP_IP, cap->addr.ip);
        }
        free(known);
    }
    if (cap->persist_gw) {
        char *known = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_GW);
        if (known == NULL || strcmp(known, cap->addr.gw) != 0) {
            nvs_config_set_string(NVS_CONFIG_LAST_DHCP_GW, cap->addr.gw);
        }
        free(known);
    }
    if (cap->persist_mask) {
        char *known = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_MASK);
        if (known == NULL || strcmp(known, cap->addr.mask) != 0) {
            nvs_config_set_string(NVS_CONFIG_LAST_DHCP_MASK, cap->addr.mask);
        }
        free(known);
    }
    if (cap->persist_dns) {
        char *known = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_DNS);
        if (known == NULL || strcmp(known, cap->addr.dns) != 0) {
            nvs_config_set_string(NVS_CONFIG_LAST_DHCP_DNS, cap->addr.dns);
        }
        free(known);
    }
}

/* Bring up the local configuration AP so a unit that cannot reach the LAN can
 * still be reached - at http://192.168.4.1 - from any phone or laptop nearby.
 *
 * This has to do all three parts, not just the mode. Switching the driver to
 * APSTA alone comes up with no AP netif, no DHCP server and no usable SSID: a
 * client can associate and then gets nothing. That is not a fallback, it only
 * looks like one - which is exactly how this device ended up associated, holding
 * an address, and unreachable by every route at the same time.
 *
 * Deliberately independent of GlobalState: the AP SSID is derived from the AP MAC
 * and nothing else, and this has to be callable from the gateway-probe callback,
 * which has no business carrying a global pointer. */
static void ensure_config_ap(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
        ESP_LOGI(TAG, "NET,event=CONFIG_AP_NETIF,created=%d", ap_netif != NULL);
    }
    if (ap_netif == NULL) {
        return;
    }

    wifi_softap_on();

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    wifi_config_t ap_cfg = {0};
    ap_cfg.ap.ssid_len = snprintf((char *) ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
                                  "Bitaxe_%02X%02X", mac[4], mac[5]);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    esp_err_t cerr = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ESP_LOGI(TAG, "NET,event=CONFIG_AP_READY,ssid=%s,err=%s",
             (const char *) ap_cfg.ap.ssid, esp_err_to_name(cerr));
}

static void mesh_survey_task(void * arg)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)arg;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(400));

    wifi_scan_config_t sc = {
        .ssid = (uint8_t *) GLOBAL_STATE->SYSTEM_MODULE.ssid,
        .bssid = NULL, .channel = 0, .show_hidden = false,
    };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        /* Clamped before the buffer is sized and before it is filled. The count
         * comes from the driver and can exceed any array we choose here. */
        if (n > MESH_SURVEY_MAX_RECORDS) {
            n = MESH_SURVEY_MAX_RECORDS;
        }
        /* n == 0 is a result, not an allocation failure: malloc(0) may return NULL,
         * and reporting that as NO_MEMORY would blame the heap for an empty scan
         * and hide the thing this survey exists to show. */
        if (n == 0) {
            ESP_LOGW(TAG, "NET,event=MESH_SCAN_DONE,nodes=0,ssid_seen=no_match");
        } else {
            wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n);
            if (recs == NULL) {
                ESP_LOGE(TAG, "NET,event=MESH_SCAN_DONE,result=NO_MEMORY,nodes=%d", n);
            } else if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                for (uint16_t i = 0; i < n; i++) {
                    ESP_LOGW(TAG, "NET,event=MESH_NODE,bssid=%02x:%02x:%02x:%02x:%02x:%02x,"
                                  "channel=%d,rssi=%d,auth=%d",
                             recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                             recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                             recs[i].primary, recs[i].rssi, recs[i].authmode);
                }
                ESP_LOGW(TAG, "NET,event=MESH_SCAN_DONE,nodes=%d", n);
                free(recs);
            } else {
                ESP_LOGE(TAG, "NET,event=MESH_SCAN_DONE,result=RECORDS_FAILED");
                free(recs);
            }
        }
    } else {
        ESP_LOGE(TAG, "NET,event=MESH_SCAN_DONE,result=START_FAILED");
    }

    wifi_connect_pinned();
    atomic_store(&s_mesh_survey_running, false);
    vTaskDelete(NULL);
}

static void mesh_survey_task_create(GlobalState * GLOBAL_STATE)
{
    /* One at a time. The survey disconnects the station, so two overlapping ones
     * would fight each other over the same radio. */
    if (atomic_exchange(&s_mesh_survey_running, true)) {
        return;
    }
    if (xTaskCreate(mesh_survey_task, "mesh_survey", 4096, GLOBAL_STATE, 3, NULL) != pdPASS) {
        atomic_store(&s_mesh_survey_running, false);
        ESP_LOGE(TAG, "NET,event=MESH_SCAN_DONE,result=TASK_CREATE_FAILED");
    }
}

static void ip_timeout_callback(TimerHandle_t xTimer)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvTimerGetTimerID(xTimer);
    // Event-driven: timer is only watchdog, real transitions come from IP_EVENT
    if (GLOBAL_STATE->SYSTEM_MODULE.is_connected) {
        dhcp_retry_count = 0;
        if (s_net_state == NET_STATE_DHCP_RUNNING || s_net_state == NET_STATE_DHCP_RETRY) {
            net_state_transition(GLOBAL_STATE, NET_STATE_IP_ACQUIRED);
        }
        /* Online on a fallback address: periodically ask DHCP for a real lease
         * again. It does NOT re-run the fallback ladder - that only ever re-bound
         * the same static address, so a unit that fell back once could never
         * migrate back to a lease for the rest of the boot. Instead the client is
         * nudged to re-offer, and the interface keeps the fallback address until a
         * lease actually arrives and supersedes it via IP_EVENT_STA_GOT_IP. */
        if (s_dhcp_probe_due_ms != 0) {
            uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
            if ((int32_t) (now_ms - s_dhcp_probe_due_ms) >= 0) {
                s_dhcp_probe_due_ms = 0;
                esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (sta != NULL) {
                    esp_err_t start_err = esp_netif_dhcpc_start(sta);
                    ESP_LOGW(TAG, "NET,event=DHCP_REPROBE,action=REQUEST_LEASE,ip=%s,err=%s",
                             s_reassert.bound.ip, esp_err_to_name(start_err));
                }
                arm_dhcp_probe();
            }
            xTimerChangePeriod(xTimer, pdMS_TO_TICKS(30000), 0);
            xTimerStart(xTimer, 0);
        }
        return;
    }
    esp_netif_t *sta_check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_check != NULL) {
        esp_netif_ip_info_t ip_chk = {0};
        if (esp_netif_get_ip_info(sta_check, &ip_chk) == ESP_OK && ip_chk.ip.addr != 0) {
            // Race: IP arrived, event will handle, just reset
            dhcp_retry_count = 0;
            net_state_transition(GLOBAL_STATE, NET_STATE_IP_ACQUIRED);
            return;
        }
    }
    // No IP yet — this is DHCP timeout, NOT a signal to fake an address
// No IP yet — this is a DHCP timeout, NOT a signal to fake an address.
    //
    // There is deliberately no retry ceiling. A miner that gives up after three
    // attempts stays offline until something restarts it, which is the worst
    // possible behaviour for a device whose whole job is to hash unattended. The
    // loop runs forever with a bounded backoff, and each escalation step changes
    // *how* we ask rather than only asking again:
    //
    //   step 0-1  plain retry, lwIP's own RFC2131 retransmits
    //   step 2    bounce the DHCP client: a fresh xid, so the server treats the
    //             request as a new client rather than a stuck lease
    //   step 3+   alternate a full Wi-Fi re-association, which is the only thing
    //             that reliably re-registers a station with a mesh DHCP relay
    //
    // The backoff is capped rather than growing without limit, so a unit that has
    // been offline for hours still recovers within seconds of the network coming
    // back.
    {
        dhcp_retry_count++;
        int step = (dhcp_retry_count - 1) % DHCP_ESCALATION_CYCLE;
        int backoff_ms = DHCP_RETRY_BASE_MS + (step * 1500) + (esp_random() % 1500);
        if (backoff_ms > DHCP_BACKOFF_CAP_MS) {
            backoff_ms = DHCP_BACKOFF_CAP_MS;
        }

        ESP_LOGW(TAG, "NET,event=DHCP_TIMEOUT,attempt=%d,step=%d,backoff=%d,state=%s",
                 dhcp_retry_count, step, backoff_ms, net_state_to_str(s_net_state));
        net_state_transition(GLOBAL_STATE, NET_STATE_DHCP_RETRY);

        /* Report the live client state alongside the retry count. A client stuck
         * in INIT was never started, which looks identical from the outside but
         * has a completely different cause than a server that never answers. */
        esp_netif_t *dbg = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dhcp_status_t st_now = ESP_NETIF_DHCP_INIT;
        if (dbg != NULL && esp_netif_dhcpc_get_status(dbg, &st_now) == ESP_OK) {
            ESP_LOGW(TAG, "NET,event=DHCP_CLIENT_STATUS,status=%d,link_up=%d", (int) st_now,
                     esp_netif_is_netif_up(dbg) ? 1 : 0);
        }

        /* Survey the mesh once per outage, but only as a genuine last resort.
         *
         * The survey disconnects the station to run a blocking scan, so running it
         * while the recovery ladder below could still produce an address destroys
         * the very path that would have brought the unit up: the reconnect fires
         * WIFI_EVENT_STA_CONNECTED, which starts the DHCP client, and esp-netif then
         * clears the static fallback address that had just been bound. The unit
         * oscillates between "has an address" and "has none" and never settles - it
         * looks alive in the log but is unreachable and never mines.
         *
         * It is a diagnostic, not a control path, so it runs only once the ladder
         * has produced nothing at all. It also has to happen here - the ladder
         * returns as soon as any address binds - but not in this callback: the
         * survey disconnects, waits, runs a blocking scan and reconnects, and this
         * runs in the FreeRTOS timer service task. Blocking there stalls every
         * software timer, and the record array it used to hold was far larger than
         * that task's 2 KB stack. */

        /* Consult the ladder early. Rung 1 (the last real lease) costs nothing to
         * attempt and cannot collide with a stranger, so there is no value in
         * making the operator wait out a retry budget first. */
        if (dhcp_retry_count >= DHCP_LADDER_AFTER_RETRY && try_ip_fallback_ladder(GLOBAL_STATE)) {
            dhcp_retry_count = 0;
            arm_dhcp_probe();
            return;
        }

        /* No rung produced an address, so there is nothing left to protect. Once
         * per outage: the flag is cleared when a lease is actually bound. */
        if (!s_mesh_survey_done_this_outage) {
            s_mesh_survey_done_this_outage = true;
            mesh_survey_task_create(GLOBAL_STATE);
        }

        /* Local web-login fallback.
         *
         * The configuration access point used to come up only when the SSID was
         * empty or after ten *association* failures. Neither can happen here: this
         * unit associates perfectly and then never gets a lease, so s_retry_num
         * stays at zero forever and the one interface that would still reach the
         * unit - the open AP at 192.168.4.1 - is never brought up. The device is
         * then unreachable by every route at once: no LAN, no web UI, no API.
         *
         * Being stuck without an address is exactly the situation the config AP
         * exists for, so it is enabled on the DHCP path too. A genuine lease
         * turns it off again below. */
        if (ESP_MINER_CONFIG_AP_STUCK == 1 || (ESP_MINER_CONFIG_AP_STUCK == 2 && !s_ever_had_lease)) {
            wifi_softap_on();
        }

        /* Escalation: change *how* we ask, not just how often. */
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (step == 2 && sta != NULL) {
            /* A stuck client keeps its transaction id, and a server that already gave
             * up on that xid will not answer it again. Restarting the client issues a
             * new DISCOVER with a fresh xid, which the relay forwards as a new client
             * request instead of re-offering into a dead transaction. */
            ESP_LOGW(TAG, "NET,event=DHCP_CLIENT_BOUNCE,reason=no_offer");
            esp_netif_dhcpc_stop(sta);
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_netif_dhcpc_start(sta);
        }

        if (step >= 3 && (step % 2) == 1) {
            /* Re-association is the only reliable way to get a client re-registered
             * with a mesh DHCP relay. ESP-IDF keeps the configured SSID/BSSID, so this
             * reconnects to the same network - it is not a channel change. */
            ESP_LOGW(TAG, "NET,event=DHCP_FORCE_REASSOCIATE,attempt=%d", dhcp_retry_count);
            GLOBAL_STATE->SYSTEM_MODULE.is_connected = false;
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(300));
            wifi_connect_pinned();
        }

        char *hn = nvs_config_get_string(NVS_CONFIG_HOSTNAME);
        const char *hlog = (hn && hn[0]) ? hn : GLOBAL_STATE->SYSTEM_MODULE.ssid;
        ESP_LOGI(TAG, "NET,event=DHCP_RETRY,hostname=%s,attempt=%d,step=%d,backoff=%d,waiting_for_IP_EVENT",
                 hlog, dhcp_retry_count, step, backoff_ms);
        free(hn);
        xTimerChangePeriod(xTimer, pdMS_TO_TICKS(backoff_ms), 0);
        xTimerStart(xTimer, 0);
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status),
                 "DHCP retry %d (step %d)", dhcp_retry_count, step);
        return;
    }
}

/* Connect, honouring an optional BSSID pin.
 *
 * In a mesh every node broadcasts the same SSID, so the driver picks whichever is
 * loudest. That is not necessarily the node with a working backhaul: a client can
 * associate successfully with a repeater that relays nothing, and the result looks
 * exactly like a dead network - association succeeds, DHCP never yields an offer,
 * nothing reaches the gateway, and the unit never appears in the router's own device
 * list.
 *
 * The pin has to be re-applied immediately before every attempt.
 * esp_wifi_set_config() accepts sta.bssid and reports ESP_OK, but the driver clears
 * it again on each reconnection, so a pin written once at init is silently gone by
 * the next attempt.
 *
 * If the pinned node refuses the request for reasons outside our control, the pin is
 * dropped and an unpinned connect is attempted, so a stale or unreachable BSSID in
 * the configuration can never brick the unit. */
static void wifi_connect_pinned(void)
{
    char * pin = nvs_config_get_string(NVS_CONFIG_WIFI_BSSID);
    uint8_t mac[6];
    bool have = false;

    if (pin != NULL && pin[0] != '\0'
        && sscanf(pin, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            if (memcmp(cfg.sta.bssid, mac, 6) != 0 || !cfg.sta.bssid_set) {
                memcpy(cfg.sta.bssid, mac, 6);
                /* Both halves are needed: the address to match and bssid_set to say
                 * the address is meant. Setting only the address is what made this
                 * pin a no-op - it looked applied and changed nothing. */
                cfg.sta.bssid_set = true;
                cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                esp_wifi_set_config(WIFI_IF_STA, &cfg);
            }
            have = true;
        }
    }
    free(pin);

    /* Read the BSSID back after writing it. esp_wifi_set_config() returning ESP_OK
     * is not evidence that the pin is in effect - it has accepted and silently
     * dropped the field before, which left a unit that logged "pinned" on every
     * boot and connected to the wrong access point anyway. Log what the driver
     * actually holds, so a pin that does not take is visible instead of assumed. */
    if (have) {
        wifi_config_t check = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &check) == ESP_OK) {
            ESP_LOGW(TAG, "NET,event=WIFI_BSSID_EFFECTIVE,bssid=%02x:%02x:%02x:%02x:%02x:%02x,"
                     "expected=%02x:%02x:%02x:%02x:%02x:%02x",
                     check.sta.bssid[0], check.sta.bssid[1], check.sta.bssid[2],
                     check.sta.bssid[3], check.sta.bssid[4], check.sta.bssid[5],
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && have) {
        ESP_LOGW(TAG, "NET,event=WIFI_PIN_REJECTED,err=%s,retrying_unpinned",
                 esp_err_to_name(err));
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            uint8_t zero[6] = {0};
            memcpy(cfg.sta.bssid, zero, 6);
            cfg.sta.bssid_set = false;
            esp_wifi_set_config(WIFI_IF_STA, &cfg);
        }
        esp_wifi_connect();
    }
}

static void event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)arg;
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_SCAN_DONE) {
            /* Clamped before anything reads or writes ap_info. The count comes
             * from the driver and reflects however many SSIDs are visible, which
             * on a normal urban network is far more than MAX_AP_COUNT - the
             * unclamped call wrote past the end of a static array and corrupted
             * every static declared after it, including the network state
             * machine's own variables. */
            esp_wifi_scan_get_ap_num(&ap_number);
            if (ap_number > MAX_AP_COUNT) {
                ESP_LOGW(TAG, "Wi-Fi scan found %u APs, reporting the first %d",
                         (unsigned) ap_number, MAX_AP_COUNT);
                ap_number = MAX_AP_COUNT;
            }
            ESP_LOGI(TAG, "Wi-Fi Scan Done");
            if (esp_wifi_scan_get_ap_records(&ap_number, ap_info) != ESP_OK) {
                ESP_LOGI(TAG, "Failed esp_wifi_scan_get_ap_records");
                ap_number = 0;
            }
            is_scanning = false;
        }

        if (is_scanning) {
            ESP_LOGI(TAG, "Still scanning, ignore wifi event.");
            return;
        }

        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "NET,event=WIFI_CONNECTING");
            net_state_transition(GLOBAL_STATE, NET_STATE_WIFI_CONNECTING);
            strcpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Connecting...");
            wifi_connect_pinned();
        }

        if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "NET,event=WIFI_CONNECTED,reason=assoc_success");
            net_state_transition(GLOBAL_STATE, NET_STATE_WIFI_CONNECTED_NO_IP);
            ESP_LOGI(TAG, "NET,event=DHCP_START");
            net_state_transition(GLOBAL_STATE, NET_STATE_DHCP_RUNNING);
            strcpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Acquiring IP...");
            dhcp_retry_count = 0;

            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta_netif != NULL) {
                // Do NOT clear the IP here. esp_netif_set_ip_info() requires the DHCP
                // client to be stopped first, and wiping a still-valid lease forces a
                // full renegotiation on every re-association (roaming, AP reboot),
                // which drops the stratum socket and the web UI for no benefit.
                // Bouncing stop->start here would reset the DHCP xid and is exactly
                // what makes some routers hand out a conflicting lease, so the
                // client is only ever started, never restarted.
                esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
                if (esp_netif_dhcpc_get_status(sta_netif, &dhcp_status) == ESP_OK) {
                    ESP_LOGD(TAG, "NET,event=DHCP_STATUS,status=%d", (int)dhcp_status);
                    // INIT is the state a freshly booted client sits in ("not yet
                    // started"), so it must be started here too - testing only for
                    // STOPPED means the client never starts on a cold boot and no
                    // lease is ever requested. STARTED is left alone: that is the
                    // re-association case, and restarting it would reset the DHCP
                    // xid for no reason.
                    if (dhcp_status == ESP_NETIF_DHCP_INIT || dhcp_status == ESP_NETIF_DHCP_STOPPED) {
                        esp_err_t start_err = esp_netif_dhcpc_start(sta_netif);
                        ESP_LOGI(TAG, "NET,event=DHCP_CLIENT_START,err=%s", esp_err_to_name(start_err));
                    } else {
                        ESP_LOGI(TAG, "NET,event=DHCP_CLIENT_ALREADY_RUNNING,status=%d", (int)dhcp_status);
                    }
                } else {
                    ESP_LOGE(TAG, "NET,event=DHCP_STATUS_FAILED");
                }
                // Diagnostics only, and only while we are waiting for a lease.
                esp_log_level_set("dhcp", ESP_LOG_DEBUG);
            }

            if (ip_acquire_timer == NULL) {
                ip_acquire_timer = xTimerCreate("ip_acquire_timer", pdMS_TO_TICKS(8000), pdFALSE, (void *)GLOBAL_STATE, ip_timeout_callback);
            }
            if (ip_acquire_timer != NULL) {
                xTimerChangePeriod(ip_acquire_timer, pdMS_TO_TICKS(8000), 0);
                xTimerStart(ip_acquire_timer, 0);
            }            
        }

        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
            if (event->reason == WIFI_REASON_ROAMING) {
                ESP_LOGI(TAG, "NET,event=WIFI_ROAMING");
                return;
            }

            ESP_LOGI(TAG, "NET,event=WIFI_DISCONNECTED,reason=%d (%s),rssi=%d", event->reason, get_wifi_reason_string(event->reason), event->rssi);
            net_state_transition(GLOBAL_STATE, NET_STATE_WIFI_RECOVERY);
            if (clients_connected_to_ap > 0) {
                ESP_LOGI(TAG, "Client(s) connected to AP, not retrying...");
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "Config AP connected!");
                return;
            }

            GLOBAL_STATE->SYSTEM_MODULE.is_connected = false;
            memset(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, 0, sizeof(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str));
            // Invalidate stratum on WiFi loss — socket is no longer valid (LwIP requires close)
            s_stratum_generation++;
            if (GLOBAL_STATE->transport) {
                ESP_LOGW(TAG, "NET,event=SOCKET_INVALIDATE,reason=WIFI_LOST,gen=%lu", (unsigned long)s_stratum_generation);
            }

            // Differentiate permanent auth failure vs transient RF
            bool is_auth_failure = (event->reason == WIFI_REASON_AUTH_FAIL || event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || event->reason == WIFI_REASON_AUTH_EXPIRE || event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
            if (is_auth_failure && s_retry_num >= 3) {
                ESP_LOGE(TAG, "NET,event=WIFI_AUTH_FAILED,reason=%d,retries=%d — stop retrying permanent failure", event->reason, s_retry_num);
                net_state_transition(GLOBAL_STATE, NET_STATE_WIFI_AUTH_FAILED);
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "WiFi auth failed (%s)", get_wifi_reason_string(event->reason));
                if (ip_acquire_timer) xTimerStop(ip_acquire_timer, 0);
                return;
            }
            // Only bring up SoftAP if no SSID is configured (initial setup) or after persistent failures (> 10 retries)
            if (strlen(GLOBAL_STATE->SYSTEM_MODULE.ssid) == 0 || s_retry_num > 10) {
                wifi_softap_on();
            }

            snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "%s (Error %d, retry #%d)", get_wifi_reason_string(event->reason), event->reason, s_retry_num);
            ESP_LOGI(TAG, "Wi-Fi status: %s", GLOBAL_STATE->SYSTEM_MODULE.wifi_status);

            s_retry_num++;
            // Exponential backoff with jitter for WiFi reconnect
            int backoff_ms = 1000 + (s_retry_num * 800) + (esp_random() % 700);
            if (backoff_ms > 8000) backoff_ms = 8000;
            ESP_LOGI(TAG, "NET,event=WIFI_RETRY,attempt=%d,backoff=%d", s_retry_num, backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            wifi_connect_pinned();

            if (ip_acquire_timer != NULL) {
                xTimerStop(ip_acquire_timer, 0);
            }            
        }
        
        if (event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "Configuration Access Point enabled");
            GLOBAL_STATE->SYSTEM_MODULE.ap_enabled = true;
        }
                
        if (event_id == WIFI_EVENT_AP_STOP) {
            ESP_LOGI(TAG, "Configuration Access Point disabled");
            GLOBAL_STATE->SYSTEM_MODULE.ap_enabled = false;
        }

        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            clients_connected_to_ap += 1;
        }
        
        if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            clients_connected_to_ap -= 1;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t * event = (ip_event_got_ip_t *) event_data;
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, IP4ADDR_STRLEN_MAX, IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "NET,event=IP_ACQUIRED,ip=%s,gen=%lu", GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, (unsigned long)(s_network_generation+1));
        net_state_transition(GLOBAL_STATE, NET_STATE_IP_ACQUIRED);
        s_retry_num = 0;
        dhcp_retry_count = 0;

        /* Whether the configured resolver came from this lease has to be decided
         * before the latch is cleared, because the answer is "no" whenever we are
         * still holding a fallback of our own. */
        bool had_fallback = s_reassert.have_fallback;
        bool dns_from_lease = !had_fallback;

        /* A genuine lease supersedes any fallback. Clearing the latch here is what
         * restores DHCP recovery: while it stayed set, the 1 Hz watchdog kept
         * stopping the client and the unit could never come back to a real lease
         * for the rest of the boot. */
        if (had_fallback) {
            ESP_LOGW(TAG, "NET,event=IP_FALLBACK_SUPERSEDED,by=DHCP,ip=%s,prev_fallback_gen=%lu",
                     GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str,
                     (unsigned long) s_reassert.fallback_generation);
        }
        net_policy_lease_wins(&s_reassert);
        s_dhcp_probe_due_ms = 0;

        /* Remember this lease, in full, so a future DHCP outage can replay it.
         *
         * What gets written is decided by net_policy_capture_lease(), which
         * enforces two rules the previous code broke: a field is only written when
         * the lease actually supplied it, and a resolver is only written when a
         * DHCP server handed it out. The second matters because esp_netif keeps
         * whatever resolver is configured, including the one our own fallback
         * installed - persisting that would make a single outage permanent.
         *
         * The address comes from event->ip_info, not from the shared
         * SYSTEM_MODULE buffer, so a concurrent write there cannot persist an
         * empty string as "last good lease". */
        {
            net_addr_config_t lease;
            memset(&lease, 0, sizeof(lease));
            snprintf(lease.ip, sizeof(lease.ip), IPSTR, IP2STR(&event->ip_info.ip));
            if (!ip4_addr_isany_val(event->ip_info.gw)) {
                snprintf(lease.gw, sizeof(lease.gw), IPSTR, IP2STR(&event->ip_info.gw));
            }
            if (!ip4_addr_isany_val(event->ip_info.netmask)) {
                snprintf(lease.mask, sizeof(lease.mask), IPSTR, IP2STR(&event->ip_info.netmask));
            }

            /* dns_from_lease was decided above, before the latch was cleared. */
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_dns_info_t dns_info = {0};
            if (sta != NULL && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK
                && !ip4_addr_isany_val(dns_info.ip.u_addr.ip4)) {
                snprintf(lease.dns, sizeof(lease.dns), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            }

            net_lease_capture_t cap = net_policy_capture_lease(&lease, dns_from_lease);
            /* The outage is over: a mesh survey may be useful again next time. */
            s_mesh_survey_done_this_outage = false;

            /* Nothing was learned if the address in hand is one we chose. The
             * address still has to reach LwIP (that happens above, in the netif
             * handler) - it just must not be remembered as a lease. */
            if (s_addr_is_fallback) {
                ESP_LOGW(TAG, "NET,event=LAST_LEASE_SKIPPED,reason=fallback_address,ip=%s", cap.addr.ip);
                s_addr_is_fallback = false;
            } else {
                s_ever_had_lease = true;
                /* A real lease means the unit is reachable over the LAN; the local
                 * web-login fallback has done its job and can go away. */
                wifi_softap_off();
                capture_lease_to_nvs(&cap);
                ESP_LOGI(TAG, "NET,event=LAST_LEASE_SAVED,ip=%s,gw=%s,mask=%s,dns=%s,dns_from_lease=%d",
                         cap.addr.ip, cap.persist_gw ? cap.addr.gw : "(kept)",
                         cap.persist_mask ? cap.addr.mask : "(kept)",
                         cap.persist_dns ? cap.addr.dns : "(kept)", dns_from_lease ? 1 : 0);
            }
        }

        if (ip_acquire_timer != NULL) {
            xTimerStop(ip_acquire_timer, 0);
        }

        GLOBAL_STATE->SYSTEM_MODULE.is_connected = true;

        // Lease acquired — stop the verbose DHCP tracing again so the log ring
        // buffer is not permanently consumed by RFC2131 chatter.
        esp_log_level_set("dhcp", ESP_LOG_INFO);

        ESP_LOGI(TAG, "Connected to SSID: %s", GLOBAL_STATE->SYSTEM_MODULE.ssid);
        strcpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Connected!");

        wifi_softap_off();
        
        // Route/DNS readiness — single source truth, DNS will be validated on next Stratum resolve
        net_state_transition(GLOBAL_STATE, NET_STATE_DNS_READY);
        // Create IPv6 link-local address after WiFi connection
        esp_netif_t *netif = event->esp_netif;
        esp_err_t ipv6_err = esp_netif_create_ip6_linklocal(netif);
        if (ipv6_err != ESP_OK) {
            ESP_LOGE(TAG, "NET,event=IPV6_FAILED,err=%s", esp_err_to_name(ipv6_err));
        }

        spawn_mdns_init_if_needed(GLOBAL_STATE);
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "NET,event=IP_LOST,gen=%lu,state=%s", (unsigned long)s_network_generation, net_state_to_str(s_net_state));
        GLOBAL_STATE->SYSTEM_MODULE.is_connected = false;
        memset(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, 0, sizeof(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str));
        s_stratum_generation++;
        net_state_transition(GLOBAL_STATE, NET_STATE_DHCP_RECOVERY);
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "IP lost, renewing...");
        esp_log_level_set("dhcp", ESP_LOG_DEBUG);

        /* A lease was lost, so the fallback latch is no longer describing the
         * present. Clearing it here means that if the recovery below fails and the
         * ladder binds a fallback, the watchdog can heal that one; and if a fresh
         * lease arrives it supersedes cleanly. */
        if (s_reassert.have_fallback) {
            ESP_LOGW(TAG, "NET,event=IP_FALLBACK_CLEARED,reason=LEASE_LOST,gen=%lu",
                     (unsigned long) s_reassert.fallback_generation);
            net_policy_lease_wins(&s_reassert);
        }
        s_dhcp_probe_due_ms = 0;
        dhcp_retry_count = 0;

        /* Bounce the client for renewal. Both rungs are explicitly tolerated: a
         * stopped client still has to be started, and a running one must not be
         * restarted, because restarting resets the transaction id and that is what
         * makes some routers hand out a conflicting lease. */
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_err_t stop_err = esp_netif_dhcpc_stop(sta);
            if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                ESP_LOGW(TAG, "NET,event=DHCP_STOP_FAILED,err=%s", esp_err_to_name(stop_err));
            }
            esp_err_t start_err = esp_netif_dhcpc_start(sta);
            ESP_LOGW(TAG, "NET,event=DHCP_RENEW_START,stop=%s,start=%s",
                     esp_err_to_name(stop_err), esp_err_to_name(start_err));
        }

        /* Bring the watchdog back to the DHCP retry period so recovery is driven
         * from here rather than waiting out a long stale period. */
        if (ip_acquire_timer) {
            xTimerChangePeriod(ip_acquire_timer, pdMS_TO_TICKS(8000), 0);
            xTimerStart(ip_acquire_timer, 0);
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t * event = (ip_event_got_ip6_t *) event_data;
        
        // Convert IPv6 address to string
        char ipv6_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &event->ip6_info.ip, ipv6_str, sizeof(ipv6_str));
        
        // Check if it's a link-local address (fe80::/10)
        if ((event->ip6_info.ip.addr[0] & 0xFFC0) == 0xFE80) {
            // For link-local addresses, append zone identifier using netif index
            int netif_index = esp_netif_get_netif_impl_index(event->esp_netif);
            if (netif_index >= 0) {
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str,
                        sizeof(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str),
                        "%s%%%d", ipv6_str, netif_index);
                ESP_LOGI(TAG, "IPv6 Link-Local Address: %s", GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str);
            } else {
                strncpy(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str, ipv6_str,
                       sizeof(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str) - 1);
                GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str[sizeof(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str) - 1] = '\0';
                ESP_LOGW(TAG, "IPv6 Link-Local Address: %s (could not get interface index)", ipv6_str);
            }
        } else {
            // Global or ULA address - no zone identifier needed
            strncpy(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str, ipv6_str,
                   sizeof(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str) - 1);
            GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str[sizeof(GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str) - 1] = '\0';
            ESP_LOGI(TAG, "IPv6 Address: %s", GLOBAL_STATE->SYSTEM_MODULE.ipv6_addr_str);
        }

        spawn_mdns_init_if_needed(GLOBAL_STATE);
    }
}

esp_netif_t * wifi_init_softap(GlobalState * GLOBAL_STATE)
{
    esp_netif_t * esp_netif_ap = esp_netif_create_default_wifi_ap();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    // Format the last 4 bytes of the MAC address as a hexadecimal string
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.ap_ssid, sizeof(GLOBAL_STATE->SYSTEM_MODULE.ap_ssid), "Bitaxe_%02X%02X", mac[4], mac[5]);

    wifi_config_t wifi_ap_config = { 0 };
    wifi_ap_config.ap.ssid_len = strlen(GLOBAL_STATE->SYSTEM_MODULE.ap_ssid);
    memcpy(wifi_ap_config.ap.ssid, GLOBAL_STATE->SYSTEM_MODULE.ap_ssid, wifi_ap_config.ap.ssid_len);
    wifi_ap_config.ap.channel = 1;
    wifi_ap_config.ap.max_connection = 10;
    wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_ap_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    return esp_netif_ap;
}

static bool is_wifi_operation_allowed(esp_err_t err)
{
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_STOP_STATE) {
        ESP_LOGI(TAG, "WiFi not initialized or stopped, skipping operation");
        return false;
    }
    return true;
}

void toggle_wifi_softap(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (is_wifi_operation_allowed(err)) {
        ESP_ERROR_CHECK(err);
    
        if (mode == WIFI_MODE_APSTA) {
            wifi_softap_off();
        } else {
            wifi_softap_on();
        }
    }
}

static void wifi_softap_off(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA) {
        return;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (is_wifi_operation_allowed(err)) {
        ESP_ERROR_CHECK(err);
    }
}



static char* generate_unique_hostname(const char *base) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char suffix[6];
    snprintf(suffix, sizeof(suffix), "-%02x%02x", mac[4], mac[5]);
    char *new_hostname = malloc(strlen(base) + strlen(suffix) + 1);
    if (new_hostname == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for unique hostname");
        return NULL;
    }
    strcpy(new_hostname, base);
    strcat(new_hostname, suffix);
    return new_hostname;
}

static char* check_and_resolve_hostname_conflict(const char *hostname, const char *current_ip) {
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_generic(hostname, NULL, NULL, MDNS_TYPE_A, MDNS_QUERY_MULTICAST, 1000, 1, &results);
    if (err != ESP_OK || !results || !results->addr) {
        // No A record found, no conflict
        if (results) mdns_query_results_free(results);
        return strdup(hostname);
    }

    mdns_ip_addr_t *a = results->addr;
    char ip_str[INET6_ADDRSTRLEN];
    if (a->addr.type == IPADDR_TYPE_V4) {
        esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip_str, sizeof(ip_str));
    } else {
        inet_ntop(AF_INET6, &a->addr.u_addr.ip6, ip_str, sizeof(ip_str));
    }

    if (strcmp(ip_str, current_ip) != 0) {
        char *new_hostname = generate_unique_hostname(hostname);
        if (new_hostname == NULL) {
            ESP_LOGW(TAG, "mDNS conflict detected for '%s' but could not generate unique name, keeping original", hostname);
            mdns_query_results_free(results);
            return strdup(hostname);
        }
        ESP_LOGI(TAG, "mDNS conflict detected for '%s' at %s, renaming to '%s'", hostname, ip_str, new_hostname);
        mdns_query_results_free(results);
        return new_hostname;
    }

    mdns_query_results_free(results);
    return strdup(hostname);
}

static void wifi_softap_on(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (is_wifi_operation_allowed(err)) {
        ESP_ERROR_CHECK(err);
    }
}

/* Initialize wifi station */
esp_netif_t * wifi_init_sta(const char * wifi_ssid, const char * wifi_pass)
{
    esp_netif_t * esp_netif_sta = esp_netif_create_default_wifi_sta();

    /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
    * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
    * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
    * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
    */
    wifi_auth_mode_t authmode;

    if (strlen(wifi_pass) == 0) {
        ESP_LOGI(TAG, "No Wi-Fi password provided, using open network");
        authmode = WIFI_AUTH_OPEN;
    } else {
        ESP_LOGI(TAG, "Wi-Fi Password provided, using WPA2");
        authmode = WIFI_AUTH_WPA2_PSK;
    }

    // On some new-ish Wi-Fi 7 routers, the Wi-Fi connection would fail, which leaves users in a bit of 
    // a pickle. This will drop down to WPA2, instead of having a connection failure due to a vague 
    // failed WPA3 handshake error.
    wifi_config_t wifi_sta_config = {
        .sta =
            {
                .threshold.authmode = authmode,
                .btm_enabled = 0,
                .rm_enabled = 0,
                .scan_method = WIFI_FAST_SCAN,
                .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                .pmf_cfg =
                    {
                        .capable = true,
                        .required = false
                    },
                .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
                .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
                .disable_wpa3_compatible_mode = 1,
                .failure_retry_cnt = 3,
        },
    };

    size_t ssid_len = strlen(wifi_ssid);
    if (ssid_len > 32) ssid_len = 32;
    memcpy(wifi_sta_config.sta.ssid, wifi_ssid, ssid_len);
    if (ssid_len < 32) {
        wifi_sta_config.sta.ssid[ssid_len] = '\0';
    }

    /* Seed the BSSID pin here; wifi_connect_pinned() re-applies it before every
     * connect attempt, because the driver clears sta.bssid on reconnection. */
    {
        char * pin = nvs_config_get_string(NVS_CONFIG_WIFI_BSSID);
        if (pin != NULL && pin[0] != '\0') {
            uint8_t mac[6];
            if (sscanf(pin, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                       &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
                memcpy(wifi_sta_config.sta.bssid, mac, 6);
                /* bssid_set is what actually arms the pin.
                 *
                 * In ESP-IDF 6.x a bssid alone is inert: esp_wifi_set_config()
                 * accepts it, reports ESP_OK, and the station still associates with
                 * whichever node it likes best. sta.bssid_set is the flag that tells
                 * the driver to honour it - "it needs to be 1 only when users need
                 * to check the MAC address of the AP" (esp_wifi_types_generic.h).
                 * Without it the unit logged "pinned" on every boot and landed on
                 * the repeater anyway, which is the exact failure this pin exists
                 * to prevent. */
                wifi_sta_config.sta.bssid_set = true;
                /* A pin and FAST_SCAN do not work together: the scan cache is
                 * populated independently of the pin and the driver then connects
                 * to whichever cached node matches the SSID, ignoring the BSSID
                 * that was just written. */
                wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                ESP_LOGW(TAG, "NET,event=WIFI_BSSID_PINNED,bssid=%s,scan=all_channel", pin);
            } else {
                ESP_LOGE(TAG, "NET,event=WIFI_BSSID_INVALID,value=%s", pin);
            }
        }
        free(pin);
    }

    if (authmode != WIFI_AUTH_OPEN) {
        strncpy((char *) wifi_sta_config.sta.password, wifi_pass, sizeof(wifi_sta_config.sta.password));
        wifi_sta_config.sta.password[sizeof(wifi_sta_config.sta.password) - 1] = '\0';
    }
    // strncpy((char *) wifi_sta_config.sta.password, wifi_pass, 63);
    // wifi_sta_config.sta.password[63] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    /* Station MAC override.
     *
     * This has to be set on WIFI_IF_STA *before* esp_wifi_start(), which is what
     * wifi_init() does next, so that the address is in place for the very first
     * association and not just for the second boot onwards. A per-device release
     * sets it when the unit has to be recognised by a reservation, an ACL or a
     * bridge rule: the efuse MAC is fixed but is not something the operator can
     * pre-register, and a unit whose address changes identity is a unit that
     * silently never gets a lease.
     *
     * A malformed value is logged and ignored - falling back to the efuse MAC is
     * always better than advertising a half-parsed address. */
    if (ESP_MINER_STA_MAC[0] != '\0') {
        uint8_t want[6];
        if (sscanf(ESP_MINER_STA_MAC, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                   &want[0], &want[1], &want[2], &want[3], &want[4], &want[5]) == 6) {
            esp_err_t merr = esp_wifi_set_mac(WIFI_IF_STA, want);
            if (merr == ESP_OK) {
                ESP_LOGW(TAG, "NET,event=WIFI_MAC_PINNED,mac=%s", ESP_MINER_STA_MAC);
            } else {
                ESP_LOGE(TAG, "NET,event=WIFI_MAC_PIN_FAILED,mac=%s,err=%s",
                         ESP_MINER_STA_MAC, esp_err_to_name(merr));
            }
        } else {
            ESP_LOGE(TAG, "NET,event=WIFI_MAC_PIN_INVALID,value=%s", ESP_MINER_STA_MAC);
        }
    }

    // IPv6 link-local address will be created after WiFi connection
    // DHCP client for IPv4 will be started automatically by esp_netif_action_connected upon association

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    return esp_netif_sta;
}

void wifi_init(GlobalState * GLOBAL_STATE)
{
    net_state_transition(GLOBAL_STATE, NET_STATE_WIFI_INIT);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // The DHCP client log level stays at its default here. It is raised to DEBUG
    // only while we are waiting for a lease (WIFI_EVENT_STA_CONNECTED) and
    // restored in the IP_EVENT_STA_GOT_IP handler, so the 512 KB log ring buffer
    // is not permanently flooded with RFC2131 chatter.

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_lost_ip;
    esp_event_handler_instance_t instance_got_ip6;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, GLOBAL_STATE, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, GLOBAL_STATE, &instance_got_ip));
    /* This one was implemented but never registered, so the entire DHCP-loss
     * recovery path was dead code: esp-netif posted the event to nobody. Losing a
     * lease therefore left the unit believing it was still online until the 8 s
     * watchdog noticed, and the stratum socket was never invalidated at the point
     * the loss actually happened. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &event_handler, GLOBAL_STATE, &instance_lost_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler, GLOBAL_STATE, &instance_got_ip6));

    /* Initialize Wi-Fi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // NOTE: the STA MAC is the efuse factory MAC and must NOT be overridden.
    // The router-side DHCP reservation for this device is keyed on that MAC,
    // so replacing it would hand out a different lease and lose the reserved
    // address. Lease conflicts are handled by the DHCP client itself via
    // CONFIG_LWIP_DHCP_DOES_ARP_CHECK (see docs/NETWORKING.md).

    /* Pin the station to 20 MHz. Must happen after esp_wifi_init() and before
     * esp_wifi_start(); the driver defaults a station to HT40, and on a mesh that
     * auto-selects, a station can end up negotiated onto a channel pairing where
     * association succeeds but frames never get delivered. Espressif's own guidance
     * is to force HT20 in crowded environments, and HT20 also leaves more of the
     * spectrum to the neighbouring cells. */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);

    /* Regulatory domain. Without this the radio runs in the world domain, with
     * different channel limits and a different power ceiling than the network is
     * allowed to use here, which costs range and roaming. Override at build time
     * for other regions. */
    {
        const char * cc = ESP_MINER_COUNTRY_CODE;
        if (cc != NULL && cc[0] != '\0') {
            esp_err_t cerr = esp_wifi_set_country_code(cc, true);
            ESP_LOGI(TAG, "NET,event=WIFI_COUNTRY,code=%s,err=%s", cc, esp_err_to_name(cerr));
        }
    }

    /* main.c has already loaded this and validated it. wifi_init overwrote the
     * pointer without freeing it and then called strlen() on the result, which is
     * NULL whenever the allocation failed. Take ownership of the existing value
     * and only replace it when NVS actually has one. */
    {
        char *ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);
        if (ssid != NULL && ssid[0] != '\0') {
            free(GLOBAL_STATE->SYSTEM_MODULE.ssid);
            GLOBAL_STATE->SYSTEM_MODULE.ssid = ssid;
        } else {
            free(ssid);
            if (GLOBAL_STATE->SYSTEM_MODULE.ssid == NULL) {
                GLOBAL_STATE->SYSTEM_MODULE.ssid = strdup("");
            }
        }
    }

    /* Only initialize and enable SoftAP if no SSID is configured */
    if (strlen(GLOBAL_STATE->SYSTEM_MODULE.ssid) == 0) {
        wifi_softap_on();
        wifi_init_softap(GLOBAL_STATE);
    } else {
        /* Start in clean STA-only mode for immediate connection without AP coexistence */
        esp_wifi_set_mode(WIFI_MODE_STA);
    }

    /* Skip connection if SSID is null */
    if (strlen(GLOBAL_STATE->SYSTEM_MODULE.ssid) == 0) {
        ESP_LOGI(TAG, "No WiFi SSID provided, skipping connection");

        /* Start WiFi */
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Disable power savings for best performance */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        return;
    } else {

        char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS);

        /* Initialize STA */
        ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
        esp_netif_t * esp_netif_sta = wifi_init_sta(GLOBAL_STATE->SYSTEM_MODULE.ssid, wifi_pass);

        free(wifi_pass);

        /* Disable power savings for best performance */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        char * hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME);

        /* esp_netif_set_hostname() copies from the pointer with no way to report a
         * NULL source, so an unset key must not be passed straight through. */
        if (hostname != NULL && hostname[0] != '\0') {
            esp_err_t err = esp_netif_set_hostname(esp_netif_sta, hostname);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "ESP_WIFI setting hostname to: %s", hostname);
            }
        } else {
            ESP_LOGW(TAG, "ESP_WIFI no hostname configured, leaving the driver default");
        }

        free(hostname);

    /* Start Wi-Fi */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Keeps a fallback address bound for the life of the firmware. No-op unless a
     * fallback was applied and the address has since gone missing; it exists because
     * esp-netif drops a static address whenever DHCP times out or the interface is
     * re-associated, which otherwise leaves the unit up for a few seconds and then
     * unreachable again. */
    xTaskCreate(ip_reassert_task, "ip_reassert", 3072, NULL, 3, NULL);
}
}

typedef struct {
    int reason;
    const char *description;
} wifi_reason_desc_t;

static const wifi_reason_desc_t wifi_reasons[] = {
    {WIFI_REASON_UNSPECIFIED,                        "Unspecified reason"},
    {WIFI_REASON_AUTH_EXPIRE,                        "Authentication expired"},
    {WIFI_REASON_AUTH_LEAVE,                         "Deauthentication due to leaving"},
    {WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY,         "Disassociated due to inactivity"},
    {WIFI_REASON_ASSOC_TOOMANY,                      "Too many associated stations"},
    {WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA,      "Class 2 frame from non-authenticated STA"},
    {WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA,     "Class 3 frame from non-associated STA"},
    {WIFI_REASON_ASSOC_LEAVE,                        "Deassociated due to leaving"},
    {WIFI_REASON_ASSOC_NOT_AUTHED,                   "Association but not authenticated"},
    {WIFI_REASON_DISASSOC_PWRCAP_BAD,                "Disassociated due to poor power capability"},
    {WIFI_REASON_DISASSOC_SUPCHAN_BAD,               "Disassociated due to unsupported channel"},
    {WIFI_REASON_BSS_TRANSITION_DISASSOC,            "Disassociated due to BSS transition"},
    {WIFI_REASON_IE_INVALID,                         "Invalid Information Element"},
    {WIFI_REASON_MIC_FAILURE,                        "MIC failure detected"},
    {WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT,             "Incorrect password entered"}, // 4-way handshake timeout
    {WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT,           "Group key update timeout"},
    {WIFI_REASON_IE_IN_4WAY_DIFFERS,                 "IE differs in 4-way handshake"},
    {WIFI_REASON_GROUP_CIPHER_INVALID,               "Invalid group cipher"},
    {WIFI_REASON_PAIRWISE_CIPHER_INVALID,            "Invalid pairwise cipher"},
    {WIFI_REASON_AKMP_INVALID,                       "Invalid AKMP"},
    {WIFI_REASON_UNSUPP_RSN_IE_VERSION,              "Unsupported RSN IE version"},
    {WIFI_REASON_INVALID_RSN_IE_CAP,                 "Invalid RSN IE capabilities"},
    {WIFI_REASON_802_1X_AUTH_FAILED,                 "802.1X authentication failed"},
    {WIFI_REASON_CIPHER_SUITE_REJECTED,              "Cipher suite rejected"},
    {WIFI_REASON_TDLS_PEER_UNREACHABLE,              "TDLS peer unreachable"},
    {WIFI_REASON_TDLS_UNSPECIFIED,                   "TDLS unspecified error"},
    {WIFI_REASON_SSP_REQUESTED_DISASSOC,             "SSP requested disassociation"},
    {WIFI_REASON_NO_SSP_ROAMING_AGREEMENT,           "No SSP roaming agreement"},
    {WIFI_REASON_BAD_CIPHER_OR_AKM,                  "Bad cipher or AKM"},
    {WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION,       "Not authorized in this location"},
    {WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS,        "Service change precludes TS"},
    {WIFI_REASON_UNSPECIFIED_QOS,                    "Unspecified QoS reason"},
    {WIFI_REASON_NOT_ENOUGH_BANDWIDTH,               "Not enough bandwidth"},
    {WIFI_REASON_MISSING_ACKS,                       "Missing ACKs"},
    {WIFI_REASON_EXCEEDED_TXOP,                      "Exceeded TXOP"},
    {WIFI_REASON_STA_LEAVING,                        "Station leaving"},
    {WIFI_REASON_END_BA,                             "End of Block Ack"},
    {WIFI_REASON_UNKNOWN_BA,                         "Unknown Block Ack"},
    {WIFI_REASON_TIMEOUT,                            "Timeout occured"},
    {WIFI_REASON_PEER_INITIATED,                     "Peer-initiated disassociation"},
    {WIFI_REASON_AP_INITIATED,                       "Access Point-initiated disassociation"},
    {WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT,      "Invalid FT action frame count"},
    {WIFI_REASON_INVALID_PMKID,                      "Invalid PMKID"},
    {WIFI_REASON_INVALID_MDE,                        "Invalid MDE"},
    {WIFI_REASON_INVALID_FTE,                        "Invalid FTE"},
    {WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED, "Transmission link establishment failed"},
    {WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED,        "Alternative channel occupied"},
    {WIFI_REASON_BEACON_TIMEOUT,                     "Beacon timeout"},
    {WIFI_REASON_NO_AP_FOUND,                        "No access point found"},
    {WIFI_REASON_AUTH_FAIL,                          "Authentication failed"},
    {WIFI_REASON_ASSOC_FAIL,                         "Association failed"},
    {WIFI_REASON_HANDSHAKE_TIMEOUT,                  "Handshake timeout"},
    {WIFI_REASON_CONNECTION_FAIL,                    "Connection failed"},
    {WIFI_REASON_AP_TSF_RESET,                       "Access point TSF reset"},
    {WIFI_REASON_ROAMING,                            "Roaming in progress"},
    {WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG,       "Association comeback time too long"},
    {WIFI_REASON_SA_QUERY_TIMEOUT,                   "SA query timeout"},
    {WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY,  "No access point found with compatible security"},
    {WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,  "No access point found in auth mode threshold"},
    {WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD,      "No access point found in RSSI threshold"},
    {0,                                               NULL},
};

static const char *get_wifi_reason_string(int reason) {
    for (int i = 0; wifi_reasons[i].reason != 0; i++) {
        if (wifi_reasons[i].reason == reason) {
            return wifi_reasons[i].description;
        }
    }
    return "Unknown error";
}

bool wifi_is_connected(void)
{
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}
