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

#define WIFI_SSID          "ESP32-AP"
#define WIFI_PASS          "12345678"
#define MY_IPV6            "fe80::2"
#define TARGET_IPV6        "fe80::1"
#define THROUGHPUT_PORT    5002
#define TEST_DURATION_SEC  120

static const char *TAG = "enhet_b";
static esp_netif_t *sta_netif = NULL;
static EventGroupHandle_t wifi_events;
#define CONNECTED_BIT BIT0

static void throughput_task(void *pvParameters)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket fel"); vTaskDelete(NULL); return; }

    int ifindex = esp_netif_get_netif_impl_index(sta_netif);

    struct sockaddr_in6 dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin6_family = AF_INET6;
    inet6_aton(TARGET_IPV6, &dest.sin6_addr);
    dest.sin6_port = htons(THROUGHPUT_PORT);
    dest.sin6_scope_id = ifindex;

    char target_str[40];
    inet6_ntoa_r(dest.sin6_addr, target_str, sizeof(target_str));
    ESP_LOGI(TAG, "Skickar till: %s port %d", target_str, THROUGHPUT_PORT);

    uint8_t buf[1400];
    memset(buf, 0xAB, sizeof(buf));

    uint64_t total_bytes = 0;
    uint64_t interval_bytes = 0;
    uint64_t total_packets_sent = 0;
    uint64_t total_packets_failed = 0;
    int64_t last_report = esp_timer_get_time();
    int64_t test_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Startar throughput-test i %d sekunder...", TEST_DURATION_SEC);

    while ((esp_timer_get_time() - test_start) < (TEST_DURATION_SEC * 1000000LL)) {
        int sent = sendto(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&dest, sizeof(dest));
        if (sent > 0) {
            total_bytes += sent;
            interval_bytes += sent;
            total_packets_sent++;
        } else {
            if (errno == ENOMEM || errno == EAGAIN) {
                vTaskDelay(1);
            }
            total_packets_failed++;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report >= 1000000LL) {
            float mbps = (interval_bytes * 8.0f) / 1000000.0f;
            ESP_LOGI(TAG, "Throughput: %.2f Mbps (%.2f MB/s)",
                     mbps, interval_bytes / 1000000.0f);
            interval_bytes = 0;
            last_report = now;
        }
    }

    close(sock);

    float duration = TEST_DURATION_SEC;
    float avg_mbps = (total_bytes * 8.0f) / (duration * 1000000.0f);
    uint64_t total_packets = total_packets_sent + total_packets_failed;
    float loss_pct = total_packets > 0 ?
                     (total_packets_failed * 100.0f) / total_packets : 0;

    ESP_LOGI(TAG, "--- Throughput statistik ---");
    ESP_LOGI(TAG, "Paket skickade:    %llu", total_packets_sent);
    ESP_LOGI(TAG, "Paket misslyckade: %llu", total_packets_failed);
    ESP_LOGI(TAG, "Totalt:            %llu paket (%.1f%% förlust)",
             total_packets, loss_pct);
    ESP_LOGI(TAG, "Total data:        %.2f MB", total_bytes / 1000000.0f);
    ESP_LOGI(TAG, "Genomsnitt:        %.2f Mbps", avg_mbps);

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
    ESP_LOGI(TAG, "Ansluten! Startar throughput-test om 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    xTaskCreate(throughput_task, "throughput", 4096, NULL, 5, NULL);
}
