#pragma once
#include "esp_err.h"

/* MQTT 카메라 클라이언트 시작.
 * WiFi 가 연결되고 카메라가 초기화된 뒤에 호출해야 한다.
 * 브로커로 접속해 명령 토픽을 구독하고, 'capture' 명령을 받으면
 * 카메라로 한 장 찍어 이미지 토픽으로 publish 한다. */
esp_err_t mqtt_cam_start(void);

void mqtt_cam_publish_uart(const char *data, int len);
