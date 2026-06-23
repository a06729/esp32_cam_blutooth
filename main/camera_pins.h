/*
 *
 * ESP32-S3 + OV2640 (DVP 8비트 병렬, FIFO 없는 18핀 모듈) 핀 매핑
 *
 * 매크로 이름은 모듈 PCB 실크스크린 표기를 그대로 사용했습니다.
 * 실제 보드의 핀 순서(2열, 18핀):
 *   윗줄 : GND  SCL  SDA   D0  D2  D4  D6  DCLK PWDN
 *   아랫줄: 3.3  VSYNC HREF RST D1  D3  D5  D7   NC
 *
 * GPIO 번호는 ESP32-S3 쪽 배선에 맞춰 수정하세요.
 * USB-JTAG(GPIO19/20), 내장 flash/PSRAM 전용 핀은 피해야 합니다.
 */
#pragma once

/* ---- 전원/그라운드 (GPIO 연결 아님) ----
 * GND : 그라운드
 * 3.3 : 3.3V 전원 입력
 * NC  : 미사용 (No Connect)
 */

/* ---- SCCB (I2C 유사) : 센서 레지스터 설정 ---- */
#define CAM_PIN_SCL     5   /* SCL : SCCB 클럭   (센서 SIOC) */
#define CAM_PIN_SDA     4   /* SDA : SCCB 데이터 (센서 SIOD) */

/* ---- 동기 신호 ---- */
#define CAM_PIN_VSYNC   6   /* VSYNC : 프레임(수직) 동기 */
#define CAM_PIN_HREF    7   /* HREF  : 라인(수평) 유효 신호. 실크 표기 'HRFF' */

/* ---- 픽셀 클럭 (카메라 -> ESP32, 입력) ---- */
#define CAM_PIN_DCLK    13  /* DCLK : 픽셀 클럭 (= PCLK). 데이터 래치 기준 */

/* ---- 8비트 병렬 데이터 버스 (카메라 -> ESP32, 입력) ---- */
#define CAM_PIN_D0      11  /* D0 : 최하위 비트 */
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3      10
#define CAM_PIN_D4      12
#define CAM_PIN_D5      18
#define CAM_PIN_D6      17
#define CAM_PIN_D7      16  /* D7 : 최상위 비트 */

/* ---- 제어 ---- */
#define CAM_PIN_RST     3   /* RST  : 리셋 (active low). 미사용 시 -1 */
#define CAM_PIN_PWDN    14  /* PWDN : 파워다운 (active high). 미사용 시 -1 */

/* ============================================================
 * ⚠️  XCLK 주의
 *   이 모듈의 핀헤더에는 XCLK(마스터 클럭) 핀이 없습니다.
 *   그러나 OV2640 센서는 외부 마스터 클럭(XVCLK)이 반드시 필요하고,
 *   esp32-camera 드라이버가 아래 GPIO에서 XCLK를 생성해 공급합니다.
 *
 *   확인 사항:
 *     - 보드에 XCLK 패드(또는 별도 핀/점퍼)가 있는지
 *     - 혹은 보드에 자체 오실레이터(클럭 발생기)가 있는지
 *   XCLK가 카메라에 전달되지 않으면 센서가 전혀 동작하지 않습니다.
 * ============================================================ */
#define CAM_PIN_XCLK    15  /* XCLK : ESP32 -> 카메라 (LEDC 로 생성, 출력) */
