/*
 * ESP32-S3 + OV2640 MJPEG 스트리밍 + BLE WiFi 프로비저닝
 *
 * 동작 순서:
 *   1) NVS 에 저장된 WiFi 자격증명 확인 → 있으면 바로 연결
 *   2) 저장된 정보가 없거나 연결 실패 → BLE 광고 시작 ("ESP32-CAM")
 *   3) 앱이 BLE 연결 → WiFi 목록을 앱으로 전송 (0xFF01 알림)
 *   4) 앱에서 SSID/비밀번호 전송 → WiFi 연결 시도 (0xFF02 쓰기)
 *   5) 연결 성공: IP 주소를 앱으로 알림 (0xFF03) → BLE 종료
 *   6) HTTP 스트리밍 서버 시작
 *        http://<IP>/        : 뷰어 페이지
 *        http://<IP>/stream  : MJPEG 스트림
 *        http://<IP>/jpg     : 정지 이미지
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "camera_module.h"
#include "http_server.h"
#include "wifi_manager.h"
#include "ble_prov.h"

static const char *TAG = "main";

/* WiFi 연결 완료 시 app_main task 를 깨우는 세마포어 */
static SemaphoreHandle_t s_prov_done = NULL;

/* ------------------------------------------------------------------ */
/* BLE 콜백: 앱이 연결되면 WiFi 스캔 후 목록 전송                       */
/* ------------------------------------------------------------------ */
static void scan_and_notify_task(void *arg)
{
    wifi_ap_info_t ap_list[WIFI_SCAN_MAX_AP];
    uint16_t ap_count = 0;

    if (wifi_manager_scan(ap_list, &ap_count) != ESP_OK) {
        ble_prov_notify_wifi_list("[]");
        vTaskDelete(NULL);
        return;
    }

    /* JSON 직렬화: [{"ssid":"...","rssi":-65,"auth":1}, ...] */
    char json[WIFI_SCAN_MAX_AP * 60 + 8];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "[");
    for (uint16_t i = 0; i < ap_count; i++) {
        if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        ap_list[i].ssid, ap_list[i].rssi, ap_list[i].auth_required);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]");

    ESP_LOGI(TAG, "WiFi 목록 전송: %d 개", ap_count);
    ble_prov_notify_wifi_list(json);
    vTaskDelete(NULL);
}

static void on_ble_connected(void)
{
    /* BLE 콜백 컨텍스트에서 바로 스캔하면 안 되므로 별도 태스크 생성 */
    xTaskCreate(scan_and_notify_task, "wifi_scan", 4096, NULL, 5, NULL);
}

/* ------------------------------------------------------------------ */
/* BLE 콜백: 앱에서 자격증명 수신 후 WiFi 연결 시도                      */
/* ------------------------------------------------------------------ */
typedef struct {
    char ssid[33];
    char pass[65];
} cred_t;

static void connect_wifi_task(void *arg)
{
    cred_t *cred = (cred_t *)arg;

    /* ⚠️ 아래 상태 문자열은 앱의 WifiStatus 값과 일치해야 합니다. 앱 코드 확인! */
    ble_prov_notify_status("connecting");

    esp_err_t err = wifi_manager_connect(cred->ssid, cred->pass);
    if (err == ESP_OK) {
        wifi_manager_save_credentials(cred->ssid, cred->pass);

        char ip[16] = { 0 };
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "WiFi 연결 성공, IP=%s", ip);

        /* 앱이 IP 가 필요하면 "connected:192.168.x.x" 형태로 보낼 수도 있음.
         * 앱이 단순 "connected" 만 기대하면 ip 부분은 빼세요. */
        char status[40];
        snprintf(status, sizeof(status), "connected:%s", ip);
        ble_prov_notify_status(status);

        /* 알림이 앱에 전달될 시간 확보 */
        vTaskDelay(pdMS_TO_TICKS(800));

        /* app_main 을 깨워 HTTP 서버 시작 */
        xSemaphoreGive(s_prov_done);
    } else {
        ble_prov_notify_status("failed");
        ESP_LOGW(TAG, "연결 실패 — 앱에서 다시 시도하세요");
    }

    free(cred);
    vTaskDelete(NULL);
}

static void on_ble_credentials(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "자격증명 수신 SSID=%s", ssid);

    cred_t *cred = malloc(sizeof(cred_t));
    if (!cred) return;
    strncpy(cred->ssid, ssid, sizeof(cred->ssid) - 1);
    strncpy(cred->pass, password, sizeof(cred->pass) - 1);

    xTaskCreate(connect_wifi_task, "wifi_conn", 4096, cred, 5, NULL);
}

/* ------------------------------------------------------------------ */
/* app_main                                                             */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    /* NVS 초기화 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 전역 네트워크/이벤트 루프 초기화 (WiFi + BLE 공용) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 디버그 로그 */
    esp_log_level_set("camera", ESP_LOG_DEBUG);
    esp_log_level_set("sccb",   ESP_LOG_DEBUG);

    /* WiFi 드라이버 초기화 (연결은 아직 하지 않음)
     * 카메라 초기화는 WiFi 연결 후로 미룸 — 카메라 배선 문제가
     * BLE 프로비저닝을 막지 않도록 하기 위함 */
    ESP_ERROR_CHECK(wifi_manager_init());

    /* ---- NVS 에서 자격증명 불러와 자동 연결 시도 ---- */
    char saved_ssid[33] = { 0 };
    char saved_pass[65] = { 0 };
    bool wifi_connected = false;

    if (wifi_manager_load_credentials(saved_ssid, sizeof(saved_ssid),
                                      saved_pass, sizeof(saved_pass)) == ESP_OK
        && saved_ssid[0] != '\0') {
        ESP_LOGI(TAG, "저장된 WiFi 사용: %s", saved_ssid);
        wifi_connected = (wifi_manager_connect(saved_ssid, saved_pass) == ESP_OK);
        if (!wifi_connected) {
            ESP_LOGW(TAG, "저장된 자격증명으로 연결 실패 → BLE 프로비저닝 시작");
        }
    }

    /* ---- 연결 실패 또는 저장 정보 없음 → BLE 프로비저닝 ---- */
    if (!wifi_connected) {
        s_prov_done = xSemaphoreCreateBinary();

        ESP_ERROR_CHECK(ble_prov_init());
        ESP_ERROR_CHECK(ble_prov_start(on_ble_connected, on_ble_credentials));

        ESP_LOGI(TAG, "BLE 프로비저닝 대기 중 — 앱에서 'ESP32-CAM' 에 연결하세요");

        /* 자격증명 수신 후 WiFi 연결 성공까지 대기 */
        xSemaphoreTake(s_prov_done, portMAX_DELAY);

        /* BLE 종료 후 HTTP 서버 시작 */
        ble_prov_stop();
        vSemaphoreDelete(s_prov_done);
    }

    /* ---- WiFi 연결 완료 → 카메라 초기화 ---- */
    char ip[16] = { 0 };
    wifi_manager_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "WiFi 연결됨. IP: %s", ip);

    if (camera_module_init() != ESP_OK) {
        ESP_LOGE(TAG, "카메라 초기화 실패 — 스트림은 동작하지 않지만 서버는 계속 실행");
    }

    /* ---- HTTP 스트리밍 서버 시작 ---- */
    httpd_handle_t server = http_server_start();
    if (!server) {
        ESP_LOGE(TAG, "HTTP 서버 시작 실패");
        return;
    }

    ESP_LOGI(TAG, "준비 완료 — http://%s/ 에서 스트림을 확인하세요", ip);
}
