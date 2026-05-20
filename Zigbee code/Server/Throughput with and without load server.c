/*
 * Zigbee HA_on_off_switch — Throughput-mätning med load
 *
 * Knapp-tryck startar ett 30-sekunders throughput-test.
 * Två parallella spår:
 * Throughput-task: Write Attribute stop-and-wait, mäter bekräftad throughput
 * Load-task: Write Attribute utan ACK-väntan, genererar bakgrundstrafik
 *
 * Justera LOAD_INTERVAL_MS för lastintensitet:
 * 100 ms → ~10 paket/s (lätt)
 * 50 ms → ~20 paket/s (medel)
 * 20 ms → ~50 paket/s (hög)
 * 10 ms → ~100 paket/s (maxstress)
 *
 * Sätt LOAD_INTERVAL_MS till 0 för att stänga av load (ren throughput-mätning).
 */
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zb_switch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"
#include "string.h"
#if defined ZB_ED_ROLE
#error Define ZB_COORDINATOR_ROLE in idf.py menuconfig.
#endif
static const char *TAG = "ESP_ZB_THROUGHPUT";
/* ===== Konfiguration ===== */
#define TEST_DURATION_SEC 120
#define PAYLOAD_SIZE_BYTES 30
#define LOAD_INTERVAL_MS 0 /* 0 = ingen load */

typedef struct {
    esp_zb_ieee_addr_t ieee_addr;
    uint8_t endpoint;
    uint16_t short_addr;
} light_bulb_device_params_t;
static light_bulb_device_params_t s_light = {0};
static bool s_light_found = false;
static bool s_test_running = false;
static switch_func_pair_t button_func_pair[] = {
    {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}};
static volatile uint32_t s_acked_packets = 0;
static volatile uint32_t s_sent_packets = 0;

static SemaphoreHandle_t s_ack_sem = NULL;
static void send_write_attr(uint16_t value) {
    esp_zb_zcl_write_attr_cmd_t write_cmd = {
        .zcl_basic_cmd =
            {
                .src_endpoint = HA_ONOFF_SWITCH_ENDPOINT,
                .dst_endpoint = s_light.endpoint,
                .dst_addr_u.addr_short = s_light.short_addr,
            },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
    };
    esp_zb_zcl_attribute_t attr = {
        .id = 0x4001,
        .data.type = ESP_ZB_ZCL_ATTR_TYPE_U16,
        .data.value = &value,
    };
    write_cmd.attr_number = 1;
    write_cmd.attr_field = &attr;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_write_attr_cmd_req(&write_cmd);
    esp_zb_lock_release();
}
static void load_task(void *arg) {
#if LOAD_INTERVAL_MS == 0
    vTaskDelete(NULL);
    return;
#endif
    uint32_t load_count = 0;
    uint16_t val = 0x8000; 
    ESP_LOGI(TAG, "Load startar: %d ms interval (~%d paket/s)", LOAD_INTERVAL_MS,
             1000 / LOAD_INTERVAL_MS);
    while (s_test_running) {
        vTaskDelay(pdMS_TO_TICKS(LOAD_INTERVAL_MS));
        if (!s_light_found) continue;
        send_write_attr(val++);
        load_count++;
        if (load_count % (10000 / LOAD_INTERVAL_MS) == 0) {
            ESP_LOGI(TAG, "Load: %lu paket skickade", load_count);
        }
    }
    ESP_LOGI(TAG, "Load klar: %lu paket totalt", load_count);
    vTaskDelete(NULL);
}

static void throughput_task(void *arg) {
    s_acked_packets = 0;
    s_sent_packets = 0;
    int64_t test_start = esp_timer_get_time();
    int64_t interval_start = test_start;
    uint32_t interval_acked = 0;
    uint16_t seq = 0;
#if LOAD_INTERVAL_MS > 0
    ESP_LOGI(TAG, "Throughput-test startar (%d s) med load (%d ms interval)...", TEST_DURATION_SEC,
             LOAD_INTERVAL_MS);
#else
    ESP_LOGI(TAG, "Throughput-test startar (%d s) utan load...", TEST_DURATION_SEC);
#endif
    printf("\n--- Throughput (Mbit/s) ---\n");
    while ((esp_timer_get_time() - test_start) < ((int64_t)TEST_DURATION_SEC * 1000000LL)) {
        send_write_attr(seq++);
        s_sent_packets++;
        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_acked_packets++;
            interval_acked++;
        }
        int64_t now = esp_timer_get_time();
        if ((now - interval_start) >= 1000000LL) {
            double elapsed_s = (now - interval_start) / 1000000.0;
            double mbps = (interval_acked * PAYLOAD_SIZE_BYTES * 8.0) / elapsed_s / 1000000.0;
            printf("%.4f Mbit/s (%lu paket/s)\n", mbps, (uint32_t)(interval_acked / elapsed_s));
            interval_acked = 0;
            interval_start = now;
        }
    }
    int64_t total_us = esp_timer_get_time() - test_start;
    double total_s = total_us / 1000000.0;
    double avg_mbps = (s_acked_packets * PAYLOAD_SIZE_BYTES * 8.0) / total_s / 1000000.0;
    uint32_t lost = (s_sent_packets > s_acked_packets) ? s_sent_packets - s_acked_packets : 0;
    printf("\n===== Zigbee Throughput =====\n");
#if LOAD_INTERVAL_MS > 0
    printf("Load interval : %d ms (~%d paket/s)\n", LOAD_INTERVAL_MS, 1000 / LOAD_INTERVAL_MS);
#else

    printf("Load : ingen\n");
#endif
    printf("Duration : %.1f s\n", total_s);
    printf("Skickade pkt : %lu\n", s_sent_packets);
    printf("Bekräftade pkt: %lu\n", s_acked_packets);
    printf("Tappade pkt : %lu (%.1f%%)\n", lost,
           s_sent_packets > 0 ? (lost * 100.0 / s_sent_packets) : 0.0);
    printf("Avg throughput: %.4f Mbit/s\n", avg_mbps);
    printf("=============================\n\n");
    s_test_running = false;
    vTaskDelete(NULL);
}
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,

                                   const void *message)

{
    if (callback_id == ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_ack_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
    return ESP_OK;
}
static void zb_buttons_handler(switch_func_pair_t *button_func_pair) {
    if (button_func_pair->func == SWITCH_ONOFF_TOGGLE_CONTROL) {
        if (!s_light_found) {
            ESP_LOGW(TAG, "Inget ljus hittat ännu");
            return;
        }
        if (s_test_running) {
            ESP_LOGI(TAG, "Test körs redan");
            return;
        }
        s_test_running = true;
        xTaskCreate(load_task, "zb_load", 4096, NULL, 3, NULL);
        xTaskCreate(throughput_task, "tp_task", 4096, NULL, 4, NULL);
    }
}
static esp_err_t deferred_driver_init(void) {
    ESP_RETURN_ON_FALSE(
        switch_driver_init(button_func_pair, PAIR_SIZE(button_func_pair), zb_buttons_handler),
        ESP_FAIL, TAG, "Failed to initialize switch driver");
    return ESP_OK;
}
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,

                        , TAG, "Failed to start Zigbee bdb commissioning");
}
static void bind_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx) {
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Bound successfully!");
        if (user_ctx) {
            light_bulb_device_params_t *light = (light_bulb_device_params_t *)user_ctx;
            s_light.endpoint = light->endpoint;
            s_light.short_addr = light->short_addr;
            s_light_found = true;
            ESP_LOGI(TAG, "Light addr(0x%x) endpoint(%d)", light->short_addr, light->endpoint);
            free(light);
        }
    }
}
static void user_find_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr,

                         uint8_t endpoint, void *user_ctx)

{
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Found light");
        esp_zb_zdo_bind_req_param_t bind_req;
        light_bulb_device_params_t *light = malloc(sizeof(light_bulb_device_params_t));
        light->endpoint = endpoint;
        light->short_addr = addr;
        esp_zb_ieee_address_by_short(light->short_addr, light->ieee_addr);
        esp_zb_get_long_address(bind_req.src_address);
        bind_req.src_endp = HA_ONOFF_SWITCH_ENDPOINT;
        bind_req.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        bind_req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
        memcpy(bind_req.dst_address_u.addr_long, light->ieee_addr, sizeof(esp_zb_ieee_addr_t));
        bind_req.dst_endp = endpoint;
        bind_req.req_dst_addr = esp_zb_get_short_address();
        esp_zb_zdo_device_bind_req(&bind_req, bind_cb, light);
    }
}
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Initialize Zigbee stack");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err_status == ESP_OK) {
                deferred_driver_init();
                if (esp_zb_bdb_is_factory_new()) {
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
                } else {
                    esp_zb_bdb_open_network(180);
                }
            }
            break;
        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            esp_zb_zdo_signal_device_annce_params_t *dev_annce_params =
                (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
            esp_zb_zdo_match_desc_req_param_t cmd_req;
            cmd_req.dst_nwk_addr = dev_annce_params->device_short_addr;
            cmd_req.addr_of_interest = dev_annce_params->device_short_addr;
            esp_zb_zdo_find_on_off_light(&cmd_req, user_find_cb, NULL);
            break;
        }
        default:
            break;
    }
}
static void esp_zb_task(void *pvParameters) {
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep = esp_zb_on_off_switch_ep_create(HA_ONOFF_SWITCH_ENDPOINT, &switch_cfg);
    esp_zb_device_register(ep);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_start(false);
    esp_zb_stack_main_loop();
}
void app_main(void) {
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    nvs_flash_init();
    esp_zb_platform_config(&config);
    s_ack_sem = xSemaphoreCreateBinary();

    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}