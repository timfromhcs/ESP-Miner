#include <string.h>
#include <stdlib.h>
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
#include "netif/etharp.h"
#include "nvs_flash.h"
#include "esp_wifi_types_generic.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "connect.h"
#include "global_state.h"
#include "nvs_config.h"
#include "esp_app_desc.h"

// Maximum number of access points to scan
#define MAX_AP_COUNT 20

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
static void event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data);

esp_err_t wifi_apply_hostname(const char *hostname)
{
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
 * been down long enough. Rather than restart DHCP and fight all three, this
 * re-applies the bind whenever the address is gone.
 *
 * One esp_netif_get_ip_info() per second, which is a struct copy. Real work happens
 * only when the address is actually missing, and a genuine lease still supersedes
 * this because the lease calls esp_netif_set_ip_info() itself. */
static bool s_have_fallback = false;
static char s_fb_ip[IP4ADDR_STRLEN_MAX];
static char s_fb_gw[IP4ADDR_STRLEN_MAX];
static char s_fb_mask[IP4ADDR_STRLEN_MAX];
static char s_fb_dns[IP4ADDR_STRLEN_MAX];

static void ip_reassert_check(void)
{
    if (!s_have_fallback) {
        return;
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL || !esp_netif_is_netif_up(sta)) {
        return;
    }

    esp_netif_ip_info_t cur = {0};
    if (esp_netif_get_ip_info(sta, &cur) == ESP_OK && cur.ip.addr != 0) {
        return;
    }

    ESP_LOGW(TAG, "NET,event=IP_FALLBACK_LOST,ip=%s,reasserting=1", s_fb_ip);
    esp_netif_ip_info_t want = {0};
    esp_netif_str_to_ip4(s_fb_ip, &want.ip);
    esp_netif_str_to_ip4(s_fb_gw, &want.gw);
    esp_netif_str_to_ip4(s_fb_mask, &want.netmask);

    /* DHCP must stay stopped: esp_netif_set_ip_info() rejects the call with
     * ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED while a client is running. */
    esp_netif_dhcpc_stop(sta);
    struct netif *gn = (struct netif *) esp_netif_get_netif_impl(sta);
    if (gn != NULL && !(gn->flags & NETIF_FLAG_UP)) {
        netif_set_up(gn);
        netif_set_link_up(gn);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_err_t err = esp_netif_set_ip_info(sta, &want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NET,event=IP_FALLBACK_LOST,ip=%s,reassert=FAILED,err=%s",
                 s_fb_ip, esp_err_to_name(err));
        return;
    }

    if (s_fb_dns[0] != '\0') {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(s_fb_dns, &dns.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
            esp_netif_set_dns_info(NULL, ESP_NETIF_DNS_MAIN, &dns);
        }
    }

    ESP_LOGW(TAG, "NET,event=IP_FALLBACK_REASSERTED,ip=%s,gw=%s,mask=%s",
             s_fb_ip, s_fb_gw, s_fb_mask);
}

static void ip_reassert_task(void * arg)
{
    (void) arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ip_reassert_check();
    }
}

/* While online on a fallback address, re-check it periodically so a dropped link
 * is healed. This deliberately does NOT restart the DHCP client: starting it with
 * no server available clears the interface address, which takes the unit offline -
 * exactly the outage the fallback exists to prevent. A DHCP client is also only
 * consulted for rungs that came *from* DHCP in the first place. Migrating back to
 * a router-issued lease therefore happens naturally whenever the lease is lost and
 * DHCP is restarted (i.e. on reconnect or reboot), which is the safe moment. */
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

    snprintf(s_fb_ip, sizeof(s_fb_ip), "%s", check_s);
    snprintf(s_fb_gw, sizeof(s_fb_gw), "%s", gw_s);
    snprintf(s_fb_mask, sizeof(s_fb_mask), "%s", (mask_s && mask_s[0]) ? mask_s : "255.255.255.0");
    snprintf(s_fb_dns, sizeof(s_fb_dns), "%s", dns_s ? dns_s : "");
    s_have_fallback = true;

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
    char *last_ip = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_IP);
    char *last_dns = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_DNS);
    char *static_gw = nvs_config_get_string(NVS_CONFIG_STATIC_GATEWAY);
    char *static_mask = nvs_config_get_string(NVS_CONFIG_STATIC_SUBNET);
    char *static_dns = nvs_config_get_string(NVS_CONFIG_STATIC_DNS);
    char *static_ip = nvs_config_get_string(NVS_CONFIG_STATIC_IP);
    bool use_static = nvs_config_get_bool(NVS_CONFIG_USE_STATIC_FALLBACK);

    bool ok = false;
    /* The gateway is the router, which on a normal network is also the resolver. */
    const char *fallback_gw = "192.168.178.1";

    /* Rung 1: the last real lease, with the DNS server that lease handed out.
     * Tried even without opting into the static fallback, because it is the safer
     * of the two addresses. DNS is essential here - an address with no resolver
     * binds fine and then fails every name lookup, which is how this path first
     * presented as "connected but no shares". */
    if (last_ip != NULL && last_ip[0] != '\0') {
        const char *dns = NULL;
        if (last_dns != NULL && last_dns[0] != '\0') {
            dns = last_dns;
        } else if (static_dns != NULL && static_dns[0] != '\0') {
            dns = static_dns;
        } else {
            dns = fallback_gw;
        }
        ok = apply_static_address(GLOBAL_STATE, last_ip, fallback_gw, "255.255.255.0", dns, "last-lease");
    }

    if (!ok && use_static && static_ip != NULL && static_ip[0] != '\0') {
        const char *dns = (static_dns != NULL && static_dns[0] != '\0') ? static_dns : static_gw;
        ok = apply_static_address(GLOBAL_STATE, static_ip, static_gw, static_mask, dns, "static-config");
    }

    if (!ok) {
        ESP_LOGW(TAG, "NET,event=IP_FALLBACK_NONE,last_lease=%s,static_configured=%d",
                 (last_ip && last_ip[0]) ? last_ip : "(none)", use_static ? 1 : 0);
    }

    free(last_ip);
    free(last_dns);
    free(static_ip);
    free(static_gw);
    free(static_mask);
    free(static_dns);
    return ok;
}

/**
 * @brief Schedule the periodic re-assertion of the fallback address.
 *
 * Only armed when we came up on a fallback. Re-binding is safe and idempotent, so
 * this heals a dropped link without ever risking the address we are holding.
 */
static void arm_dhcp_probe(void)
{
    s_dhcp_probe_due_ms = (uint32_t) (esp_timer_get_time() / 1000) + (DHCP_PROBE_INTERVAL_MS / 1000);
    ESP_LOGI(TAG, "NET,event=FALLBACK_REASSERT_ARMED,in=%ds", DHCP_PROBE_INTERVAL_MS / 1000);
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
        /* Online on a fallback address: periodically re-assert it. Re-binding is
         * idempotent and cannot lose connectivity, unlike restarting DHCP. */
        if (s_dhcp_probe_due_ms != 0) {
            uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
            if ((int32_t) (now_ms - s_dhcp_probe_due_ms) >= 0) {
                ESP_LOGI(TAG, "NET,event=FALLBACK_REASSERT,action=REBIND");
                s_dhcp_probe_due_ms = 0;
                if (try_ip_fallback_ladder(GLOBAL_STATE)) {
                    arm_dhcp_probe();
                    xTimerChangePeriod(xTimer, pdMS_TO_TICKS(30000), 0);
                } else {
                    /* Nothing left to fall back to: fall through to the DHCP
                     * retry path so the normal recovery loop takes over. */
                    dhcp_retry_count = 0;
                    GLOBAL_STATE->SYSTEM_MODULE.is_connected = false;
                }
                xTimerStart(xTimer, 0);
                return;
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

        /* Survey the mesh once per outage, before the ladder can return early.
         *
         * Every node in a mesh broadcasts the same SSID, so this is the only way to
         * see that the station landed on a repeater instead of the router: a
         * station that associates but never appears in the router's own device list
         * is talking to a node that does not relay it. It has to run here, because
         * the ladder below returns as soon as any address binds.
         *
         * The scan runs while disconnected, since esp_wifi_scan_start() does not
         * complete reliably on an associated station, and a silent scan would hide
         * exactly what we are looking for. Once per outage, so its cost is
         * irrelevant next to the outage it diagnoses. */
        if (dhcp_retry_count == 1) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(400));
            wifi_scan_config_t sc = {
                .ssid = (uint8_t *) GLOBAL_STATE->SYSTEM_MODULE.ssid,
                .bssid = NULL, .channel = 0, .show_hidden = false,
            };
            if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
                uint16_t n = 0;
                esp_wifi_scan_get_ap_num(&n);
                if (n > 32) { n = 32; }
                wifi_ap_record_t recs[32];
                if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                    for (uint16_t i = 0; i < n; i++) {
                        ESP_LOGW(TAG, "NET,event=MESH_NODE,bssid=%02x:%02x:%02x:%02x:%02x:%02x,"
                                      "channel=%d,rssi=%d,auth=%d",
                                 recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                                 recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                                 recs[i].primary, recs[i].rssi, recs[i].authmode);
                    }
                    ESP_LOGW(TAG, "NET,event=MESH_SCAN_DONE,nodes=%d", n);
                }
            }
            wifi_connect_pinned();
        }

        /* Consult the ladder early. Rung 1 (the last real lease) costs nothing to
         * attempt and cannot collide with a stranger, so there is no value in
         * making the operator wait out a retry budget first. */
        if (dhcp_retry_count >= DHCP_LADDER_AFTER_RETRY && try_ip_fallback_ladder(GLOBAL_STATE)) {
            dhcp_retry_count = 0;
            arm_dhcp_probe();
            return;
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
            if (memcmp(cfg.sta.bssid, mac, 6) != 0) {
                memcpy(cfg.sta.bssid, mac, 6);
                esp_wifi_set_config(WIFI_IF_STA, &cfg);
            }
            have = true;
        }
    }
    free(pin);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && have) {
        ESP_LOGW(TAG, "NET,event=WIFI_PIN_REJECTED,err=%s,retrying_unpinned",
                 esp_err_to_name(err));
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            uint8_t zero[6] = {0};
            memcpy(cfg.sta.bssid, zero, 6);
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
            esp_wifi_scan_get_ap_num(&ap_number);
            ESP_LOGI(TAG, "Wi-Fi Scan Done");
            if (esp_wifi_scan_get_ap_records(&ap_number, ap_info) != ESP_OK) {
                ESP_LOGI(TAG, "Failed esp_wifi_scan_get_ap_records");
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
        /* A real lease supersedes any fallback: stop probing so we do not keep
         * poking the DHCP client for an address we already hold legitimately. */
        s_dhcp_probe_due_ms = 0;

        /* Remember this lease so a future DHCP outage has a safe address to fall
         * back onto. Only written when it actually changed: a DHCP server that
         * keeps reissuing the same reserved address must not cause an NVS write
         * per renewal. The resolver handed out with the lease is remembered too -
         * an address with no DNS server binds fine and then fails every name
         * lookup, which is precisely how the first fallback attempt presented. */
        {
            char *known = nvs_config_get_string(NVS_CONFIG_LAST_DHCP_IP);
            if (known == NULL || strcmp(known, GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str) != 0) {
                nvs_config_set_string(NVS_CONFIG_LAST_DHCP_IP, GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str);

                /* Read back the resolver this lease provided, via esp-netif: that
                 * is the value that demonstrably worked. */
                char dns_s[IP4ADDR_STRLEN_MAX] = "";
                esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_dns_info_t dns_info = {0};
                if (sta != NULL && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
                    snprintf(dns_s, sizeof(dns_s), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
                }
                if (dns_s[0] != '\0') {
                    nvs_config_set_string(NVS_CONFIG_LAST_DHCP_DNS, dns_s);
                }
                ESP_LOGI(TAG, "NET,event=LAST_LEASE_SAVED,ip=%s,dns=%s",
                         GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, dns_s[0] ? dns_s : "(none)");
            }
            free(known);
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
        // Invalidate stratum — will reconnect on next IP
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "IP lost, renewing...");
        // Bounce DHCP client for renewal (minimal recovery)
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_dhcpc_stop(sta);
esp_netif_dhcpc_start(sta);
        }
        if (ip_acquire_timer) {
            xTimerChangePeriod(ip_acquire_timer, pdMS_TO_TICKS(DHCP_RETRY_BASE_MS), 0);
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
                ESP_LOGW(TAG, "NET,event=WIFI_BSSID_PINNED,bssid=%s", pin);
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
    esp_event_handler_instance_t instance_got_ip6;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, GLOBAL_STATE, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, GLOBAL_STATE, &instance_got_ip));
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

    GLOBAL_STATE->SYSTEM_MODULE.ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);

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

        /* Set Hostname */
        esp_err_t err = esp_netif_set_hostname(esp_netif_sta, hostname);
        if (err != ERR_OK) {
            ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "ESP_WIFI setting hostname to: %s", hostname);
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
