#include "sdkconfig.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"


#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt_device.h"


static const char *TAG = "mqtt";

#define DEVICE_ID       CONFIG_DEVICE_ID
#define DEVICE_TYPE     CONFIG_DEVICE_TYPE


static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

static int64_t s_time_offset = 0; // 单位：秒，真实时间-esp_timer_get_time()

// 在WiFi联网并NTP同步后调用，设置真实时间戳
void mqtt_device_set_time_offset(time_t real_time)
{
    int64_t now = esp_timer_get_time() / 1000000ULL;
    s_time_offset = (int64_t)real_time - now;
}


static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}



esp_err_t mqtt_device_start(void)
{
    ESP_LOGI(TAG, "Starting mqtt_device_start, broker: %s, user: %s", CONFIG_BROKER_URL, CONFIG_MQTT_USERNAME);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        // .credentials.username = CONFIG_MQTT_USERNAME,
        // .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to init mqtt client");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Registering MQTT event handler...");
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "Starting MQTT client (async connect)...");
    esp_mqtt_client_start(s_mqtt_client);
    ESP_LOGI(TAG, "MQTT client start command issued.");
    return ESP_OK;
}


esp_err_t mqtt_device_publish_sensor(const char* sensor_id, const char* sensor_type, float mg, float ppb)
{
    if (!s_mqtt_client || !s_mqtt_connected) {
        ESP_LOGE(TAG, "MQTT client not connected");
        return ESP_FAIL;
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "air-quality/hcho/%s/data", DEVICE_ID);
    char payload[256];
    int64_t now = esp_timer_get_time() / 1000000ULL;
    uint32_t ts = (uint32_t)(now + s_time_offset);
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"device_type\":\"%s\",\"sensor_id\":\"%s\",\"sensor_type\":\"%s\",\"timestamp\":%lu,\"data\":{\"formaldehyde\":%.3f,\"ppb\":%.1f}}",
        DEVICE_ID, DEVICE_TYPE, sensor_id, sensor_type, ts, mg, ppb);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Publish sensor: topic=%s, payload=%s, msg_id=%d", topic, payload, msg_id);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_device_publish_air_quality(const air_quality_data_t *data)
{
    if (!s_mqtt_client || !s_mqtt_connected || !data) return ESP_FAIL;
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"dart_mg\":%.3f,\"dart_ppb\":%.1f,\"winsen_mg\":%.3f,\"winsen_ppb\":%.1f}",
        data->dart_hcho_mg, data->dart_hcho_ppb, data->winsen_hcho_mg, data->winsen_hcho_ppb);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, "air_quality/all", payload, 0, 1, 0);
    ESP_LOGI(TAG, "Publish all: %s, msg_id=%d", payload, msg_id);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}
