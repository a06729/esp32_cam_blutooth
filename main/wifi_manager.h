#pragma once
#include "esp_err.h"
#include "esp_wifi.h"

#define WIFI_SCAN_MAX_AP   10
#define WIFI_CONNECT_RETRY 10

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_required;  /* 1 = 비밀번호 필요, 0 = 개방 */
} wifi_ap_info_t;

/* WiFi 드라이버 초기화 (netif/event_loop는 호출 전에 완료되어 있어야 함) */
esp_err_t wifi_manager_init(void);

/* WiFi 스캔. ap_list 에 결과 채우고 ap_count 에 개수 반환 */
esp_err_t wifi_manager_scan(wifi_ap_info_t *ap_list, uint16_t *ap_count);

/* 지정한 SSID/비밀번호로 WiFi 연결 (blocking, 타임아웃 30 s) */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/* 현재 할당된 IP 주소를 ip_str 에 복사 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);

/* NVS 에 WiFi 자격증명 저장/불러오기 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                        char *password, size_t pass_len);
