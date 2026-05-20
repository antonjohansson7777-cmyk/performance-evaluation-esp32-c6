#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/ip6_addr.h"
#include "lwip/sockets.h"
#include "esp_timer.h"

#define WIFI_SSID        "ESP32-AP"
#define WIFI_PASS        "12345678"
#define MY_IPV6          "fe80::2"
#define TARGET_IPV6      "fe80::1"
#define UDP_PORT         3333
#define PING_DURATION_SEC 120

static const char *TAG = "enhet_b";
static esp_netif_t *sta_netif = NULL;
static EventGroupHandle_t wifi_events;
#define CONNECTED_BIT BIT0

static void udp_client_task(void *pvParameters)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Kunde inte skapa socket");
        vTaskDelete(NULL);
        return;
    }

    int ifindex = esp_netif_get_netif_impl_index(sta_netif);
    ESP_LOGI(TAG, "Använder netif index: %d", ifindex);

    struct sockaddr_in6 dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin6_family = AF_INET6;
    inet6_aton(TARGET_IPV6, &dest.sin6_addr);
    dest.sin6_port = htons(UDP_PORT);
    dest.sin6_scope_id = ifindex;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[64];
    int seq = 0;
    int received = 0;
    float min_ms = 1e9f;
    float max_ms = 0;
    float total_ms = 0;
    int64_t test_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Startar ping i %d sekunder...", PING_DURATION_SEC);

    while ((esp_timer_get_time() - test_start) < (PING_DURATION_SEC * 1000000LL)) {
        snprintf(buf, sizeof(buf), "PING %d", seq++);

        int64_t start = esp_timer_get_time();

        int sent = sendto(sock, buf, strlen(buf), 0,
                         (struct sockaddr *)&dest, sizeof(dest));
        if (sent < 0) {
            ESP_LOGE(TAG, "sendto misslyckades (errno=%d)", errno);
        } else {
            struct sockaddr_in6 from;
            socklen_t fromlen = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&from, &fromlen);
            if (len > 0) {
                float elapsed = (esp_timer_get_time() - start) / 1000.0f;
                buf[len] = 0;
                char from_str[40];
                inet6_ntoa_r(from.sin6_addr, from_str, sizeof(from_str));
                ESP_LOGI(TAG, "Svar från %s seq=%d: %.2f ms", from_str, seq - 1, elapsed);

                received++;
                total_ms += elapsed;
                if (elapsed < min_ms) min_ms = elapsed;
                if (elapsed > max_ms) max_ms = elapsed;
            } else {
                ESP_LOGW(TAG, "Timeout (seq=%d)", seq - 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    close(sock);

    ESP_LOGI(TAG, "--- %s ping statistik ---", TARGET_IPV6);
    ESP_LOGI(TAG, "%d skickade, %d mottagna, %d%% paketförlust",
             seq, received, (seq - received) * 100 / seq);
    if (received > 0) {
        ESP_LOGI(TAG, "rtt min/avg/max = %.2f/%.2f/%.2f ms",
                 min_ms, total_ms / received, max_ms);
    }

    vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *ev = (ip_event_got_ip6_t *)data;
        char addr[40];
        inet6_ntoa_r(ev->ip6_info.ip, addr, sizeof(addr));
        ESP_LOGI(TAG, "IPv6 redo: %s", addr);
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, event_handler, NULL);

    wifi_config_t wcfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_STA,
        WIFI_PROTOCOL_11B |
        WIFI_PROTOCOL_11G |
        WIFI_PROTOCOL_11N |
        WIFI_PROTOCOL_11AX
    ));

    esp_ip6_addr_t my_addr;
    inet6_aton(MY_IPV6, &my_addr);
    esp_netif_add_ip6_address(sta_netif, my_addr, ESP_IP6_ADDR_IS_LINK_LOCAL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    ESP_LOGI(TAG, "Väntar på IPv6...");
    xEventGroupWaitBits(wifi_events, CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Ansluten! Startar ping om 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
}
