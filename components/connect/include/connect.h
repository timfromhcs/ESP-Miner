#ifndef CONNECT_H_
#define CONNECT_H_

#include "lwip/sys.h"
#include <arpa/inet.h>
#include <lwip/netdb.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct GlobalState GlobalState;

/** Regulatory domain (ISO 3166-1 alpha-2). "DE" unless overridden at build time. */
#ifndef ESP_MINER_COUNTRY_CODE
#define ESP_MINER_COUNTRY_CODE "DE"
#endif

/* Station channel bandwidth: HT20 (20 MHz) rather than the driver default HT40.
 *
 * ESP-IDF leaves a station on HT40 by default, and Espressif's own guidance is to
 * force HT20 in crowded environments. HT40 needs a legal primary channel with a
 * valid secondary; on a mesh that auto-selects, a station can end up negotiated
 * onto a channel pairing where association succeeds but frames never get through.
 * The link then looks healthy while no traffic flows, which is much harder to
 * diagnose than a clean association failure.
 *
 * HT20 also leaves more of the spectrum to neighbouring cells, which is what a
 * miner needs: it spends its whole life sending small packets on one channel.
 */
#ifndef ESP_MINER_WIFI_BW_HT20
#define ESP_MINER_WIFI_BW_HT20 1
#endif

// Structure to hold WiFi scan results
typedef struct {
    char ssid[33];  // 32 chars + null terminator
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_record_simple_t;

void toggle_wifi_softap(void);
void wifi_init(GlobalState * GLOBAL_STATE);
esp_err_t wifi_apply_hostname(const char *hostname);
esp_err_t wifi_scan(wifi_ap_record_simple_t *ap_records, uint16_t *ap_count);
esp_err_t get_wifi_current_rssi(int8_t *rssi);
bool wifi_is_connected(void);
esp_err_t update_mdns_hostname(const char *new_hostname, GlobalState *GLOBAL_STATE);

#endif /* CONNECT_H_ */
