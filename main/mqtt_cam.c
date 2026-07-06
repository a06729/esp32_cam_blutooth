/*
 * ESP32-S3 MQTT 카메라 클라이언트
 *
 *   서버 → ESP32 : esp32cam/cmd   (payload "capture" → 한 장 찍어 전송)
 *   ESP32 → 서버 : esp32cam/image (JPEG 바이너리)
 *   ESP32 → 서버 : esp32cam/status("online" / LWT "offline")
 *
 * ⚠️ 브로커 주소는 코드에 하드코딩하지 않습니다.
 *    `idf.py menuconfig` → "ESP32-CAM 애플리케이션 설정" → "MQTT 브로커 주소"
 *    에서 설정하면 sdkconfig(=.gitignore 대상)에 저장됩니다. (.env 와 동일한 개념)
 *    설정값은 CONFIG_MQTT_BROKER_URI 매크로로 자동 생성됩니다.
 */
#include "mqtt_cam.h"

#include <string.h>
#include <stdio.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"   /* esp-mqtt 컴포넌트: esp_mqtt_client_* */
#include "ble_prov.h"      /* ble_prov_get_device_id(): WiFi MAC 기반 기기 고유 ID */

/* ====================== 사용자 설정 ====================== */
#ifdef CONFIG_MQTT_BROKER_URI
#define MQTT_BROKER_URI   CONFIG_MQTT_BROKER_URI
#else
#define MQTT_BROKER_URI   "mqtt://192.168.0.57:1883"
#endif
/* ======================================================== */

/* 토픽은 시작 시 device_key(MAC)를 붙여 동적으로 생성한다.
 * 형식: esp32cam/{device_key}/cmd|image|status */
static char s_topic_cmd[48];
static char s_topic_image[48];
static char s_topic_status[48];
static char s_topic_uart[48]; 

static const char *TAG = "mqtt_cam";
static esp_mqtt_client_handle_t s_client = NULL;

/* 카메라로 한 장 찍어 이미지 토픽으로 publish */
static void publish_capture(void)
{
    camera_fb_t *fb = esp_camera_fb_get();

    if (fb) {
        esp_camera_fb_return(fb);
    }

    fb = esp_camera_fb_get();

    if (!fb) {
        ESP_LOGE(TAG, "프레임 캡처 실패");
        esp_mqtt_client_publish(s_client, s_topic_status, "capture_failed", 0, 1, 0);
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_client, s_topic_image,
                                         (const char *)fb->buf, fb->len, 0, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "이미지 publish 실패 (len=%u) — out 버퍼 크기를 확인하세요", fb->len);
    } else {
        ESP_LOGI(TAG, "이미지 전송: %u bytes → %s", fb->len, s_topic_image);
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
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 1, 1 /*retain*/);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "브로커 연결 끊김 — 자동 재연결");
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(s_topic_cmd) && strncmp(event->topic, s_topic_cmd, event->topic_len) == 0) {
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

void mqtt_cam_publish_uart(const char *data,int len){
    if(s_client == NULL){
        ESP_LOGW(TAG,"MQTT 미시작 — UART 데이터 전송 건너뜀");
    }
    int msg_id = esp_mqtt_client_publish(
        s_client, s_topic_uart, data, len,1,0
    );

    if (msg_id < 0) {
        ESP_LOGE(TAG, "UART→MQTT publish 실패");
    }else{
        ESP_LOGI(TAG, "UART→MQTT: %.*s → %s", len, data, s_topic_uart);
    }

}

esp_err_t mqtt_cam_start(void)
{
    /* WiFi MAC 기반 기기 고유 ID 로 per-device 토픽 생성.
     * ble_prov_init() 이 app_main 에서 먼저 호출되므로 이 시점에 유효한 값이 있다. */
    const char *dev_id = ble_prov_get_device_id();
    snprintf(s_topic_cmd,    sizeof(s_topic_cmd),    "esp32cam/%s/cmd",    dev_id);
    snprintf(s_topic_image,  sizeof(s_topic_image),  "esp32cam/%s/image",  dev_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "esp32cam/%s/status", dev_id);
    snprintf(s_topic_uart, sizeof(s_topic_uart),"esp32cam/%s/status",dev_id);

    ESP_LOGI(TAG, "device_key=%s  토픽: %s / %s / %s",
             dev_id, s_topic_cmd, s_topic_image, s_topic_status);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = dev_id,   /* MQTT client_id = device_key(MAC) */
        /* LWT: 비정상 종료 시 브로커가 대신 offline 을 알림 */
        .session.last_will = {
            .topic   = s_topic_status,
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
