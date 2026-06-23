#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define NVS_NAMESPACE      "wifi_cred"

static esp_netif_t      *s_sta_netif  = NULL;
static EventGroupHandle_t s_evt_group = NULL;
static int               s_retry_num  = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_CONNECT_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "재연결 %d/%d", s_retry_num, WIFI_CONNECT_RETRY);
        } else {
            if (s_evt_group) {
                xEventGroupSetBits(s_evt_group, WIFI_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        if (s_evt_group) {
            xEventGroupSetBits(s_evt_group, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_ap_info_t *ap_list, uint16_t *ap_count)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  /* blocking */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "스캔 실패: 0x%x", err);
        return err;
    }

    uint16_t num = WIFI_SCAN_MAX_AP;
    wifi_ap_record_t records[WIFI_SCAN_MAX_AP];
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&num, records));

    *ap_count = num;
    for (uint16_t i = 0; i < num; i++) {
        strncpy(ap_list[i].ssid, (char *)records[i].ssid, 32);
        ap_list[i].ssid[32]      = '\0';
        ap_list[i].rssi          = records[i].rssi;
        ap_list[i].auth_required = (records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
    }
    ESP_LOGI(TAG, "스캔 완료: %d 개 발견", num);
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    /* 재설정으로 다른 AP 에 다시 연결할 때 깔끔히 끊고 시작한다.
     * 아직 연결돼 있지 않으면 무시되는 호출이라 안전하다. */
    esp_wifi_disconnect();

    s_evt_group = xEventGroupCreate();
    s_retry_num = 0;

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "연결 시도: %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_evt_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);
    vEventGroupDelete(s_evt_group);
    s_evt_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi 연결 실패: %s", ssid);
    return ESP_FAIL;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t len)
{
    if (!s_sta_netif) return ESP_ERR_INVALID_STATE;

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
    }
    return err;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", password ? password : "");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "자격증명 저장: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                        char *password, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_str(h, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        nvs_get_str(h, "pass", password, &pass_len);
    }
    nvs_close(h);
    return err;
}
