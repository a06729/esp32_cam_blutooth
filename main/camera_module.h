#pragma once
#include "esp_err.h"

esp_err_t camera_module_init(void);

/* SCCB(I2C) 버스를 스캔해 카메라 센서 응답 여부를 로그로 출력.
 * esp_camera_init() 호출 "전에" 부를 것. (디버깅용) */
void camera_sccb_scan(void);

/* 데이터버스(D0~D7/PCLK/VSYNC/HREF) 진단: 컬러바 테스트패턴을 RGB565 로
 * 받아 원시 바이트를 로그로 출력. camera_module_init() 성공 "후" 호출. (디버깅용) */
void camera_databus_test(void);
