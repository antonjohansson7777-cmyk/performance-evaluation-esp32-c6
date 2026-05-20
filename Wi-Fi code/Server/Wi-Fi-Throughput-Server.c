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

#define AP_SSID           "ESP32-AP"
#define AP_PASS           "12345678"
#define MY_IPV6           "fe80::1"
#define THROUGHPUT_PORT   5002
#define TEST_DURATION_SEC 120

static const char *TAG = "enhet_a";
static esp_netif_t *ap_netif = NULL;
static EventGroupHandle_t wifi_events;
#define CONNECTED_BIT BIT0

static void throughput_server_task(void *pvParameters)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket fel"); vTaskDelete(NULL); return; }

    struct sockaddr_in6 src;
    memset(&src, 0, sizeof(src));
    src.sin6_family = AF_INET6;
    src.sin6_addr = in6addr_any;
    src.sin6_port = htons(THROUGHPUT_PORT);
    bind(sock, (struct sockaddr *)&src, sizeof(src));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Throughput-server lyssnar på port %d", THROUGHPUT_PORT);

    uint8_t buf[1400];
    uint64_t total_bytes = 0;
    uint64_t interval_bytes = 0;
    uint64_t total_packets = 0;
    uint64_t interval_packets = 0;
    int64_t last_report = esp_timer_get_time();
    int64_t last_data_time = esp_timer_get_time();
    int64_t test_start = esp_timer_get_time();
    bool receiving = false;

    while (1) {
        struct sockaddr_in6 from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
        if (len > 0) {
            if (!receiving) {
                ESP_LOGI(TAG, "Tar emot data...");
                receiving = true;
                test_start = esp_timer_get_time();
                last_report = test_start;
                last_data_time = test_start;
            }
            total_bytes += len;
            interval_bytes += len;
            total_packets++;
            interval_packets++;
            last_data_time = esp_timer_get_time();
        }

        int64_t now = esp_timer_get_time();
        if (receiving && now - last_report >= 1000000LL) {
            if (interval_bytes > 0) {
                float mbps = (interval_bytes * 8.0f) / 1000000.0f;
                ESP_LOGI(TAG, "Mottaget: %.2f Mbps (%.2f MB/s) %llu paket/s",
                         mbps, interval_bytes / 1000000.0f, interval_packets);
            }
            interval_bytes = 0;
            interval_packets = 0;
            last_report = now;
        }

        if (receiving && (esp_timer_get_time() - last_data_time) > 3000000LL) {
            break;
        }
    }

    close(sock);

    if (total_bytes > 0) {
        float duration = (last_data_time - test_start) / 1000000.0f;
        float avg_mbps = (total_bytes * 8.0f) / (duration * 1000000.0f);

        ESP_LOGI(TAG, "--- Throughput statistik (mottaget) ---");
        ESP_LOGI(TAG, "Paket mottagna: %llu", total_packets);
        ESP_LOGI(TAG, "Total data:     %.2f MB", total_bytes / 1000000.0f);
        ESP_LOGI(TAG, "Tid:            %.1f s", duration);
        ESP_LOGI(TAG, "Genomsnitt:     %.2f Mbps", avg_mbps);
    }

    vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Enhet B anslöt!");
        xEventGroupSetBits(wifi_events, CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *ev = (ip_event_got_ip6_t *)data;
        char addr[40];
        inet6_ntoa_r(ev->ip6_info.ip, addr, sizeof(addr));
        ESP_LOGI(TAG, "IPv6 redo: %s", addr);
    }
}

static void wifi_init_ap(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, event_handler, NULL);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .password       = AP_PASS,
            .ssid_len       = strlen(AP_SSID),
            .channel        = 1,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_AP,
        WIFI_PROTOCOL_11B |
        WIFI_PROTOCOL_11G |
        WIFI_PROTOCOL_11N |
        WIFI_PROTOCOL_11AX
    ));

    esp_netif_create_ip6_linklocal(ap_netif);

    esp_ip6_addr_t my_addr;
    inet6_aton(MY_IPV6, &my_addr);
    esp_netif_add_ip6_address(ap_netif, my_addr, ESP_IP6_ADDR_IS_LINK_LOCAL);

    ESP_LOGI(TAG, "AP startad: %s (Wi-Fi 6/ax)", AP_SSID);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_ap();

    ESP_LOGI(TAG, "Väntar på att enhet B ansluter...");
    xEventGroupWaitBits(wifi_events, CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Enhet B ansluten! Startar throughput-server...");

    xTaskCreate(throughput_server_task, "throughput_server", 4096, NULL, 5, NULL);
}
