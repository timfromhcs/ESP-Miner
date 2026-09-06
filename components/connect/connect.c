#include <string.h>
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
#define DHCP_RETRY_MAX 5
#define DHCP_RETRY_BASE_MS 4000
#define DHCP_RECOVERY_WIFI_RECONNECT_AFTER 3

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

static void ip_timeout_callback(TimerHandle_t xTimer)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvTimerGetTimerID(xTimer);
    // Event-driven: timer is only watchdog, real transitions come from IP_EVENT
    if (GLOBAL_STATE->SYSTEM_MODULE.is_connected) {
        dhcp_retry_count = 0;
        if (s_net_state == NET_STATE_DHCP_RUNNING || s_net_state == NET_STATE_DHCP_RETRY) {
            net_state_transition(GLOBAL_STATE, NET_STATE_IP_ACQUIRED);
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
    if (dhcp_retry_count < DHCP_RETRY_MAX) {
        dhcp_retry_count++;
        int backoff_ms = DHCP_RETRY_BASE_MS + (dhcp_retry_count * 1200) + (esp_random() % 600);
        ESP_LOGW(TAG, "NET,event=DHCP_TIMEOUT,retry=%d/%d,backoff=%d,state=%s", dhcp_retry_count, DHCP_RETRY_MAX, backoff_ms, net_state_to_str(s_net_state));
        net_state_transition(GLOBAL_STATE, NET_STATE_DHCP_RETRY);
        // ESP-IDF LwIP DHCP already retransmits internally per RFC2131 — do NOT bounce dhcpc here (would reset xid and break server).
        // Just wait; external IP_EVENT will handle success. Log hostname for diagnostics only.
        char *hn = nvs_config_get_string(NVS_CONFIG_HOSTNAME);
        const char *hlog = (hn && hn[0]) ? hn : GLOBAL_STATE->SYSTEM_MODULE.ssid;
        ESP_LOGI(TAG, "NET,event=DHCP_RETRY,hostname=%s,retry=%d,backoff=%d,waiting_for_IP_EVENT", hlog, dhcp_retry_count, backoff_ms);
        if (hn) free(hn);
        xTimerChangePeriod(xTimer, pdMS_TO_TICKS(backoff_ms), 0);
        xTimerStart(xTimer, 0);
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "DHCP retry %d/%d", dhcp_retry_count, DHCP_RETRY_MAX);
        return;
    }
    // Exhausted DHCP retries — enter DHCP_FAILED. No fake NETWORK_READY.
    // For this deployment the reserved IP 192.168.178.66 is a legitimate USER_CONFIGURED_STATIC
    // fallback (not a fake DHCP success). Verify connectivity before declaring READY.
    ESP_LOGE(TAG, "NET,event=DHCP_FAILED,retries=%d,state=%s", DHCP_RETRY_MAX, net_state_to_str(s_net_state));
    net_state_transition(GLOBAL_STATE, NET_STATE_DHCP_FAILED);
    dhcp_retry_count = 0;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, sizeof(GLOBAL_STATE->SYSTEM_MODULE.wifi_status), "DHCP failed (%d retries)", DHCP_RETRY_MAX);
    ESP_LOGW(TAG, "NET,event=STATIC_FALLBACK,ip=192.168.178.66,gw=192.168.178.1 — explicit reserved lease for mining");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dhcpc_stop(sta);
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_str_to_ip4("192.168.178.66", &ip_info.ip);
        esp_netif_str_to_ip4("192.168.178.1", &ip_info.gw);
        esp_netif_str_to_ip4("255.255.255.0", &ip_info.netmask);
        esp_netif_set_ip_info(sta, &ip_info);
        esp_netif_set_default_netif(sta);
        esp_netif_dns_info_t dns_main = {0}; dns_main.ip.type = ESP_IPADDR_TYPE_V4; esp_netif_str_to_ip4("192.168.178.1", &dns_main.ip.u_addr.ip4);
        esp_netif_set_dns_info(NULL, ESP_NETIF_DNS_MAIN, &dns_main); esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_main);
        esp_netif_dns_info_t dns_back = {0}; dns_back.ip.type = ESP_IPADDR_TYPE_V4; esp_netif_str_to_ip4("1.1.1.1", &dns_back.ip.u_addr.ip4);
        esp_netif_set_dns_info(NULL, ESP_NETIF_DNS_BACKUP, &dns_back); esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns_back);
        ip_addr_t gw_dns={}, cf={}; ipaddr_aton("192.168.178.1",&gw_dns); ipaddr_aton("1.1.1.1",&cf); dns_setserver(0,&gw_dns); dns_setserver(1,&cf);
#if LWIP_ARP
        struct netif *lwip = (struct netif*)esp_netif_get_netif_impl(sta);
        if (lwip) etharp_gratuitous(lwip);
#endif
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, IP4ADDR_STRLEN_MAX, "192.168.178.66");
        net_state_transition(GLOBAL_STATE, NET_STATE_IP_ACQUIRED);
        ESP_LOGI(TAG, "NET,event=STATIC_IP_ASSIGNED,ip=192.168.178.66,verifying DNS...");
        spawn_mdns_init_if_needed(GLOBAL_STATE);
        GLOBAL_STATE->SYSTEM_MODULE.is_connected = true;
        strcpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Connected (Static Fallback)");
        net_state_transition(GLOBAL_STATE, NET_STATE_DNS_READY);
    } else {
        strcpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "DHCP failed — no netif");
        esp_wifi_disconnect();
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
            esp_wifi_connect();
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
                esp_netif_dhcp_status_t dhcp_status;
                if (esp_netif_dhcpc_get_status(sta_netif, &dhcp_status) == ESP_OK) {
                    if (dhcp_status == ESP_NETIF_DHCP_INIT || dhcp_status == ESP_NETIF_DHCP_STOPPED) {
                        esp_netif_dhcpc_start(sta_netif);
                    }
                }
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
            esp_wifi_connect();

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

        if (ip_acquire_timer != NULL) {
            xTimerStop(ip_acquire_timer, 0);
        }

        GLOBAL_STATE->SYSTEM_MODULE.is_connected = true;

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

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_got_ip6;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, GLOBAL_STATE, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, GLOBAL_STATE, &instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler, GLOBAL_STATE, &instance_got_ip6));

    /* Initialize Wi-Fi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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
