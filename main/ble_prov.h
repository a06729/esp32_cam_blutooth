#pragma once
#include "esp_err.h"

/*
 * BLE 프로비저닝 모듈 (앱의 커스텀 128비트 GATT 프로토콜에 맞춤)
 *
 * GATT 서비스 (128비트 UUID, ble_prov.c 상단에서 정의):
 *   WIFI_LIST_CHAR   READ | NOTIFY  - ESP32가 스캔 결과(JSON)를 앱으로 전송
 *   SSID_CHAR        WRITE          - 앱이 WiFi 이름(SSID)을 write
 *   PASS_CHAR        WRITE          - 앱이 WiFi 비밀번호를 write
 *   STATUS_CHAR      READ | NOTIFY  - ESP32가 연결 결과 문자열을 notify
 *
 * 앱 동작 흐름:
 *   1) BLE 연결 → WIFI_LIST_CHAR 구독 → ESP32가 스캔해서 목록 전송
 *   2) SSID_CHAR 에 SSID write
 *   3) PASS_CHAR 에 비밀번호 write  → 이때 WiFi 연결 시도
 *   4) STATUS_CHAR 로 결과 문자열 notify ("connecting"/"connected"/"failed")
 */

/* BLE 장치가 연결되었을 때 호출 (WiFi 스캔 트리거용) */
typedef void (*ble_on_connected_cb_t)(void);

/* 앱에서 SSID+비밀번호가 모두 수신되면 호출 (PASS write 시점) */
typedef void (*ble_on_credentials_cb_t)(const char *ssid, const char *password);

esp_err_t ble_prov_init(void);
esp_err_t ble_prov_start(ble_on_connected_cb_t  on_connected,
                         ble_on_credentials_cb_t on_credentials);
void      ble_prov_stop(void);

/* WiFi 목록 JSON을 앱에 알림 전송 */
void ble_prov_notify_wifi_list(const char *json);

/* 연결 상태 문자열을 앱에 알림 전송 ("connecting"/"connected"/"failed" 등) */
void ble_prov_notify_status(const char *status);
