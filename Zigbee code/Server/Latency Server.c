/*
 * Zigbee HA_on_off_switch — Latency test via Read Attribute
 *
 * Flöde per ping:
 * 1. Switchen skickar Toggle till ljuset
 * 2. Switchen skickar Read Attribute (on/off) till ljuset
 * 3. Ljuset svarar med Read Attribute Response
 * 4. Switchen fångar svaret i ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID
 * 5. RTT = tid från steg 1 till steg 4
 *
 * Ljuset är helt omodifierat.
 */
#include <limits.h>
#include <math.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zb_switch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"
#include "string.h"
#if defined ZB_ED_ROLE
#error Define ZB_COORDINATOR_ROLE in idf.py menuconfig to compile light switch source
code.
#endif
    static const char *TAG = "ESP_ZB_ON_OFF_SWITCH";
typedef struct light_bulb_device_params_s {
    esp_zb_ieee_addr_t ieee_addr;
    uint8_t endpoint;
    uint16_t short_addr;
} light_bulb_device_params_t;
static light_bulb_device_params_t s_light = {0};
static bool s_light_found = false;
static switch_func_pair_t button_func_pair[] = {
    {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}};
typedef struct {
    uint32_t count;
    int64_t min_us;
    int64_t max_us;
    double sum_us;
    double sum_sq_us;
} latency_stats_t;

static latency_stats_t stats;
static int64_t ping_start_us = 0;
static int64_t test_start_us = 0;
static bool test_running = false;
static bool waiting_for_ack = false;
static uint32_t ping_id = 0;
#define TEST_DURATION_US (120LL * 1000000LL)
#define PING_INTERVAL_MS 10
static void stats_init(void) {
    stats.count = 0;
    stats.min_us = INT64_MAX;
    stats.max_us = 0;
    stats.sum_us = 0.0;
    stats.sum_sq_us = 0.0;
}
static void stats_add(int64_t rtt_us) {
    stats.count++;
    if (rtt_us < stats.min_us) stats.min_us = rtt_us;
    if (rtt_us > stats.max_us) stats.max_us = rtt_us;
    stats.sum_us += (double)rtt_us;
    stats.sum_sq_us += (double)rtt_us * (double)rtt_us;
}
static void stats_print(void) {
    if (stats.count == 0) {
        printf("\n[Latency] Inga samples — fick switchen inga svar från ljuset?\n");
        return;
    }
    double avg = stats.sum_us / stats.count;
    double var = (stats.sum_sq_us / stats.count) - (avg * avg);
    double stddev = (var > 0) ? sqrt(var) : 0.0;
    printf("\n===== Zigbee Latency Stats =====\n");
    printf("Samples : %lu\n", stats.count);
    printf("Min : %.2f ms\n", stats.min_us / 1000.0);
    printf("Max : %.2f ms\n", stats.max_us / 1000.0);
    printf("Avg : %.2f ms\n", avg / 1000.0);
    printf("Stddev : %.2f ms\n", stddev / 1000.0);
    printf("================================\n\n");
}
static void send_read_attr(void) {
    uint16_t attr_id = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
    esp_zb_zcl_read_attr_cmd_t read_req = {
        .zcl_basic_cmd =
            {
                .src_endpoint = HA_ONOFF_SWITCH_ENDPOINT,
                .dst_endpoint = s_light.endpoint,
                .dst_addr_u.addr_short = s_light.short_addr,

            },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        .attr_number = 1,
        .attr_field = &attr_id,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_read_attr_cmd_req(&read_req);
    esp_zb_lock_release();
}
static void send_ping(void) {
    if (!test_running || waiting_for_ack || !s_light_found) return;
    if ((esp_timer_get_time() - test_start_us) > TEST_DURATION_US) {
        test_running = false;
        ESP_LOGI(TAG, "Test klart");
        stats_print();
        return;
    }
    ping_id++;
    ping_start_us = esp_timer_get_time();
    waiting_for_ack = true;
    /* Steg 1: Toggle */
    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd.src_endpoint = HA_ONOFF_SWITCH_ENDPOINT,
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
    send_read_attr();
}
static void ping_delay_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
    send_ping();
    vTaskDelete(NULL);
}
static void zb_buttons_handler(switch_func_pair_t *button_func_pair) {
    if (button_func_pair->func == SWITCH_ONOFF_TOGGLE_CONTROL) {
        if (!s_light_found) {
            ESP_LOGW(TAG, "Inget ljus hittat ännu");

            return;
        }
        if (test_running) {
            ESP_LOGI(TAG, "Test körs redan");
            return;
        }
        ESP_LOGI(TAG, "Startar latenstest (2 min)");
        stats_init();
        test_start_us = esp_timer_get_time();
        test_running = true;
        waiting_for_ack = false;
        ping_id = 0;
        send_ping();
    }
}
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,

                                   const void *message)

{
    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        if (test_running && waiting_for_ack) {
            int64_t rtt_us = esp_timer_get_time() - ping_start_us;
            waiting_for_ack = false;
            stats_add(rtt_us);
            printf("Ping %4lu: %7.2f ms\n", ping_id, rtt_us / 1000.0);
            xTaskCreate(ping_delay_task, "ping_delay", 2048, NULL, 4, NULL);
        }
    }
    return ESP_OK;
}
static esp_err_t deferred_driver_init(void) {
    ESP_RETURN_ON_FALSE(
        switch_driver_init(button_func_pair, PAIR_SIZE(button_func_pair), zb_buttons_handler),
        ESP_FAIL, TAG, "Failed to initialize switch driver");
    return ESP_OK;
}
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG,
                        "Failed to start Zigbee bdb commissioning");
}
static void bind_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx) {
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Bound successfully!");
        if (user_ctx) {
            light_bulb_device_params_t *light = (light_bulb_device_params_t *)user_ctx;
            ESP_LOGI(TAG, "Light addr(0x%x) endpoint(%d)", light->short_addr, light->endpoint);

            s_light.endpoint = light->endpoint;
            s_light.short_addr = light->short_addr;
            s_light_found = true;
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
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}