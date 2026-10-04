#ifndef WEBSOCKET_H_
#define WEBSOCKET_H_

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

void websocket_set_log_task_handle(TaskHandle_t task_handle);

#define MESSAGE_QUEUE_SIZE (128)
#define MAX_WEBSOCKET_CLIENTS (10)
/*
 * Ringpuffer fuer die Boot- und Laufzeitlogs, damit Logs einen Reset
 * ueberleben und ueber den WebSocket scrollbar bleiben.
 *
 * 128 KB statt der ursprünglichen 512 KB: der Puffer wird bei jedem Kaltstart
 * vollstaendig genullt und per esp_cache_msync in die PSRAM-Cache-Lines
 * geschrieben (log_buffer.c). Das war bei 512 KB messbare Boot-Zeit fuer
 * Speicher, der nie gelesen wird. 128 KB reichen fuer die komplette
 * Bootsequenz um ein Vielfaches und bleiben damit sinnvoll Puffer fuer die
 * WebSocket-Ansicht.
 *
 * WICHTIG: Der Puffer liegt per EXT_RAM_NOINIT_ATTR in PSRAM. Ohne
 * CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y faellt das Attribut auf
 * __NOINIT_ATTR zurueck, und 128 KB landen im internen DRAM. Die
 * main/CMakeLists.txt prueft das beim Build.
 */
#define LOG_BUFFER_SIZE  (128 * 1024)  /* 128 KB */

typedef enum {
    WS_TYPE_LOGS,
    WS_TYPE_API,
    WS_TYPE_MAX
} WebSocketClientType;

esp_err_t websocket_add_client(int fd, WebSocketClientType type);
void websocket_remove_client(int fd);
void websocket_broadcast(WebSocketClientType type, httpd_ws_frame_t *pkt);
void websocket_send_to_client(int fd, httpd_ws_frame_t *pkt);
int websocket_get_active_client_count(WebSocketClientType type);
void websocket_init(httpd_handle_t server);
esp_err_t websocket_pre_handshake(httpd_req_t *req);
esp_err_t websocket_post_handshake(httpd_req_t *req);
esp_err_t websocket_handler(httpd_req_t *req);

void websocket_close_fn(httpd_handle_t hd, int sockfd);

/**
 * Notifies the WebSocket task that new log data is available in the ring buffer.
 * Non-blocking, safe to call from log hook.
 */
void websocket_log_notify(void);

#endif /* WEBSOCKET_H_ */
