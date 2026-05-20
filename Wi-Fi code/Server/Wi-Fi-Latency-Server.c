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

#define AP_SSID     "ESP32-AP"
#define AP_PASS     "12345678"
#define MY_IPV6     "fe80::1"
#define UDP_PORT    3333

static const char *TAG = "enhet_a";
static esp_netif_t *ap_netif = NULL;
static EventGroupHandle_t wifi_events;
#define CONNECTED_BIT BIT0

static void udp_server_task(void *pvParameters)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Kunde inte skapa socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in6 src;
    memset(&src, 0, sizeof(src));
    src.sin6_family = AF_INET6;
    src.sin6_addr = in6addr_any;
    src.sin6_port = htons(UDP_PORT);
    src.sin6_scope_id = esp_netif_get_netif_impl_index(ap_netif);

    if (bind(sock, (struct sockaddr *)&src, sizeof(src)) < 0) {
        ESP_LOGE(TAG, "bind misslyckades");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP server lyssnar på port %d", UDP_PORT);

    char buf[128];
    while (1) {
        struct sockaddr_in6 from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                          (struct sockaddr *)&from, &fromlen);
        if (len > 0) {
            buf[len] = 0;
            char from_str[40];
            inet6_ntoa_r(from.sin6_addr, from_str, sizeof(from_str));
            ESP_LOGI(TAG, "Mottog från %s: %s", from_str, buf);
            const char *reply = "PONG";
            sendto(sock, reply, strlen(reply), 0,
                  (struct sockaddr *)&from, fromlen);
        }
    }
    close(sock);
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

    int ifindex = esp_netif_get_netif_impl_index(ap_netif);
    ESP_LOGI(TAG, "AP netif index: %d", ifindex);

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
    ESP_LOGI(TAG, "Enhet B ansluten! Startar UDP server...");

    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
}
