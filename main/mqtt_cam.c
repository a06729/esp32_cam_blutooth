/*
 * ESP32-S3 MQTT 카메라 클라이언트
 *
 *   서버 → ESP32 : esp32cam/cmd   (payload "capture" → 한 장 찍어 전송)
 *   ESP32 → 서버 : esp32cam/image (JPEG 바이너리)
 *   ESP32 → 서버 : esp32cam/status("online" / LWT "offline")
 *
 * ⚠️ MQTT_BROKER_URI 를 FastAPI 서버(=브로커)가 떠 있는 PC 의 IP 로 바꾸세요.
 *    예) "mqtt://192.168.0.10:1883"
 */
#include "mqtt_cam.h"

#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"   /* esp-mqtt 컴포넌트: esp_mqtt_client_* */

/* ====================== 사용자 설정 ====================== */
#define MQTT_BROKER_URI   "mqtt://192.168.0.57:1883"   /* ← 서버 PC(맥) LAN IP */

#define TOPIC_CMD     "esp32cam/cmd"
#define TOPIC_IMAGE   "esp32cam/image"
#define TOPIC_STATUS  "esp32cam/status"
/* ======================================================== */

static const char *TAG = "mqtt_cam";
static esp_mqtt_client_handle_t s_client = NULL;

/* 카메라로 한 장 찍어 이미지 토픽으로 publish */
static void publish_capture(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "프레임 캡처 실패");
        esp_mqtt_client_publish(s_client, TOPIC_STATUS, "capture_failed", 0, 1, 0);
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_client, TOPIC_IMAGE,
                                         (const char *)fb->buf, fb->len, 0, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "이미지 publish 실패 (len=%u) — out 버퍼 크기를 확인하세요", fb->len);
    } else {
        ESP_LOGI(TAG, "이미지 전송: %u bytes", fb->len);
    }
    esp_camera_fb_return(fb);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "브로커 연결됨");
        esp_mqtt_client_subscribe(s_client, TOPIC_CMD, 1);
        esp_mqtt_client_publish(s_client, TOPIC_STATUS, "online", 0, 1, 1 /*retain*/);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "브로커 연결 끊김 — 자동 재연결");
        break;

    case MQTT_EVENT_DATA:
        /* 명령 토픽 처리 
           TOPIC_CMD 값이 오는지 체크
        */
        if (event->topic_len == (int)strlen(TOPIC_CMD) && strncmp(event->topic, TOPIC_CMD, event->topic_len) == 0) {
            ESP_LOGI(TAG, "명령 수신: %.*s", event->data_len, event->data);
            if (event->data_len == 7 && strncmp(event->data, "capture", 7) == 0) {
                publish_capture();
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 오류 발생");
        break;

    default:
        break;
    }
}

esp_err_t mqtt_cam_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        /* LWT: 비정상 종료 시 브로커가 대신 offline 을 알림 */
        .session.last_will = {
            .topic   = TOPIC_STATUS,
            .msg     = "offline",
            .msg_len = 0,
            .qos     = 1,
            .retain  = 1,
        },
        /* JPEG 한 장(수십 KB)을 한 번에 publish 하려면 out 버퍼를 키워야 한다.
         * VGA/quality12 기준 보통 < 50KB. 여유 있게 64KB. */
        .buffer.size     = 4096,
        .buffer.out_size = 64 * 1024,
        .task.stack_size = 6144,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT 클라이언트 초기화 실패");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 클라이언트 시작 실패: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "MQTT 시작 — 브로커 %s", MQTT_BROKER_URI);
    return ESP_OK;
}
