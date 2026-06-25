#pragma once
#include "esp_err.h"

/*
 * BLE 프로비저닝 모듈 (앱의 커스텀 128비트 GATT 프로토콜에 맞춤)
 *
 * GATT 서비스 (128비트 UUID, ble_prov.c 상단에서 정의):
 *   WIFI_LIST_CHAR   WRITE | NOTIFY - 앱이 "SCAN" write로 스캔 요청 → ESP32가 결과를 notify
 *   SSID_CHAR        WRITE          - 앱이 WiFi 이름(SSID)을 write
 *   PASS_CHAR        WRITE          - 앱이 WiFi 비밀번호를 write
 *   STATUS_CHAR      READ | NOTIFY  - ESP32가 연결 결과 문자열을 notify
 *   DEVICE_ID_CHAR   READ           - 앱이 기기 고유 ID(MAC)를 read → device_key 로 사용
 *
 * 앱 동작 흐름:
 *   1) BLE 연결 → WIFI_LIST_CHAR 구독 → "SCAN" write → ESP32가 스캔해서 목록 전송
 *      목록 포맷: 한 줄당 네트워크 1개 JSON, 줄 끝 '\n', 마지막에 "END\n"
 *        {"ssid":"...","rssi":-65,"secure":true}\n ... END\n
 *   2) SSID_CHAR 에 SSID write
 *   3) PASS_CHAR 에 비밀번호 write  → 이때 WiFi 연결 시도
 *   4) STATUS_CHAR 로 결과 문자열 notify (대문자: "CONNECTING"/"CONNECTED"/"FAILED")
 */

/* BLE 장치가 연결되었을 때 호출 */
typedef void (*ble_on_connected_cb_t)(void);

/* 앱이 WIFI_LIST_CHAR 에 "SCAN" 을 write 하면 호출 (WiFi 스캔 트리거용) */
typedef void (*ble_on_scan_request_cb_t)(void);

/* 앱에서 SSID+비밀번호가 모두 수신되면 호출 (PASS write 시점) */
typedef void (*ble_on_credentials_cb_t)(const char *ssid, const char *password);

esp_err_t ble_prov_init(void);
esp_err_t ble_prov_start(ble_on_connected_cb_t    on_connected,
                         ble_on_scan_request_cb_t on_scan_request,
                         ble_on_credentials_cb_t  on_credentials);
void      ble_prov_stop(void);

/* 이 기기의 고유 ID(MAC 문자열, 콜론 없는 12자리 대문자 16진수)를 반환.
 * 서버 등록 시 device_key 로 쓰거나, MQTT/HTTP 보고에 재사용한다. */
const char *ble_prov_get_device_id(void);

/* WiFi 목록 JSON을 앱에 알림 전송 */
void ble_prov_notify_wifi_list(const char *json);

/* 연결 상태 문자열을 앱에 알림 전송 ("connecting"/"connected"/"failed" 등) */
void ble_prov_notify_status(const char *status);
