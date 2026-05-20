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

#define WIFI_SSID "ESP32-AP"
#define WIFI_PASS "12345678"
#define MY_IPV6 "fe80::2"

static const char *TAG = "enhet_b";
static esp_netif_t *sta_netif = NULL;
static EventGroupHandle_t wifi_events;

#define CONNECTED_BIT BIT0
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
            .ssid = WIFI_SSID,
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
    ESP_LOGI(TAG, "Ansluten! Idle — mäter nu viloström.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}