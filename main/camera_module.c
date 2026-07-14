#include "camera_module.h"
#include "camera_pins.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

/* 센서 하드 리셋: PWDN/RST 핀으로 전원 사이클을 돌려
 * SCCB 가 간헐적으로 깨진 상태(데이터 phase NACK 등)를 초기화한다. */
static void camera_power_cycle(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CAM_PIN_PWDN) | (1ULL << CAM_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    gpio_set_level(CAM_PIN_PWDN, 1);   /* 파워다운 진입 (active high) */
    gpio_set_level(CAM_PIN_RST, 0);    /* 리셋 어서트 (active low) */
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(CAM_PIN_PWDN, 0);   /* 파워다운 해제 */
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(CAM_PIN_RST, 1);    /* 리셋 해제 → 센서 재기동 */
    vTaskDelay(pdMS_TO_TICKS(50));     /* 내부 안정화 대기 */
}

/* SCCB(I2C) 버스를 스캔해 카메라 센서가 응답하는지 로그로 확인.
 * OV2640 은 주소 0x30 에서 응답한다.
 * 반드시 esp_camera_init() 호출 "전에" 부를 것 (스캔 후 버스 해제하므로
 * 카메라 드라이버가 같은 핀을 다시 쓸 수 있다).
 * ※ ESP-IDF 6.x 의 새 I2C 드라이버(i2c_master) 사용 — 구형 driver/i2c.h 와
 *    섞으면 부팅 시 드라이버 충돌로 abort 하므로 새 API 로 작성. */
/* SDA/SCL 라인 상태 점검 + I2C 데드락 복구.
 * - 풀업 걸고 읽었을 때 두 선 모두 HIGH 여야 정상 (idle).
 * - LOW 로 붙잡혀 있으면: 카메라 전원 미공급 / 센서 데드락 / 단락.
 * - SDA 가 LOW 면 표준 복구 시퀀스(SCL 9펄스 + STOP)를 시도한다. */
static void sccb_bus_check_and_recover(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CAM_PIN_SDA) | (1ULL << CAM_PIN_SCL),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(10));

    int sda = gpio_get_level(CAM_PIN_SDA);
    int scl = gpio_get_level(CAM_PIN_SCL);
    ESP_LOGI(TAG, "SCCB 라인 상태: SDA=%s  SCL=%s",
             sda ? "HIGH(정상)" : "LOW(비정상!)",
             scl ? "HIGH(정상)" : "LOW(비정상!)");

    if (!scl) {
        ESP_LOGE(TAG, "SCL 이 LOW → 마스터가 복구 불가. 카메라 전원(3.3V) 미공급,"
                      " SCL 선 단락/오배선을 확인하세요");
        return;
    }

    if (!sda) {
        ESP_LOGW(TAG, "SDA 가 LOW → 센서 데드락 가능성. SCL 9펄스 복구 시도...");

        /* SCL 을 오픈드레인 출력으로 9번 토글해 센서가 물고 있는 바이트를 밀어냄 */
        gpio_set_direction(CAM_PIN_SCL, GPIO_MODE_OUTPUT_OD);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(CAM_PIN_SCL, 0);
            esp_rom_delay_us(10);
            gpio_set_level(CAM_PIN_SCL, 1);
            esp_rom_delay_us(10);
        }

        /* STOP 조건 생성: SCL HIGH 상태에서 SDA LOW→HIGH */
        gpio_set_direction(CAM_PIN_SDA, GPIO_MODE_OUTPUT_OD);
        gpio_set_level(CAM_PIN_SDA, 0);
        esp_rom_delay_us(10);
        gpio_set_level(CAM_PIN_SCL, 1);
        esp_rom_delay_us(10);
        gpio_set_level(CAM_PIN_SDA, 1);
        esp_rom_delay_us(10);

        /* 다시 입력으로 되돌리고 결과 확인 */
        gpio_set_direction(CAM_PIN_SDA, GPIO_MODE_INPUT);
        gpio_set_direction(CAM_PIN_SCL, GPIO_MODE_INPUT);
        vTaskDelay(pdMS_TO_TICKS(10));

        sda = gpio_get_level(CAM_PIN_SDA);
        if (sda) {
            ESP_LOGI(TAG, "복구 성공: SDA=HIGH 로 돌아옴");
        } else {
            ESP_LOGE(TAG, "복구 실패: SDA 여전히 LOW → 카메라 전원(3.3V) 미공급"
                          " 또는 SDA 선 단락/오배선");
        }
    }
}

void camera_sccb_scan(void)
{
    /* 스캔 전에 라인 상태 점검 (LOW 로 죽어있으면 스캔은 전부 timeout 남) */
    sccb_bus_check_and_recover();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = CAM_PIN_SDA,
        .scl_io_num        = CAM_PIN_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "SCCB 스캔용 I2C 버스 생성 실패");
        return;
    }

    ESP_LOGI(TAG, "===== SCCB(I2C) 스캔 시작 (SDA=%d SCL=%d) =====",
             CAM_PIN_SDA, CAM_PIN_SCL);

    int found = 0;
    for (uint8_t addr = 1; addr < 0x7F; addr++) {
        esp_err_t ret = i2c_master_probe(bus, addr, 50 /*ms*/);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  >> 응답 발견: 0x%02X%s", addr,
                     (addr == 0x30) ? "  (OV2640 센서!)" : "");
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGE(TAG, "  !! 아무 응답 없음 → 카메라 전원/GND 또는 SDA/SCL 배선 문제");
    } else {
        ESP_LOGI(TAG, "===== 스캔 완료: %d 개 응답 =====", found);
    }

    i2c_del_master_bus(bus);   // 버스 해제 (카메라 드라이버가 쓸 수 있도록)
}

/* ---- 데이터버스 진단용 (임시 테스트) ----
 * OV2640 의 컬러바 테스트패턴을 RGB565 로 받아 원시 바이트를 hex 로 출력.
 * JPEG 이 아니므로 NO-SOI 체크 없이 D0~D7/PCLK/VSYNC/HREF 로 들어오는
 * 데이터를 그대로 볼 수 있다.
 *
 * 판독법:
 *   - "프레임 자체를 못 받음"        → PCLK/VSYNC/HREF 배선 문제
 *   - 전부 00 또는 전부 FF          → 데이터선 미연결/전원 문제
 *   - 실행할 때마다 값이 뒤죽박죽    → 신호 무결성(선 길이/GND/노이즈)
 *   - 규칙적 패턴이지만 비트가 이상  → 데이터선 순서 바뀜
 *
 * 반드시 camera_module_init() "성공 후" 호출할 것. 테스트 후 JPEG 모드로 원복. */
void camera_databus_test(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "TEST: 센서 핸들 없음 (init 실패 상태)");
        return;
    }

    ESP_LOGI(TAG, "===== 데이터버스 테스트 시작 (RGB565 + 컬러바) =====");

    s->set_pixformat(s, PIXFORMAT_RGB565);
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_colorbar(s, 1);              /* 컬러바 테스트패턴 ON */
    vTaskDelay(pdMS_TO_TICKS(300));     /* 설정 반영 대기 */

    /* 모드 전환 직후의 낡은 프레임 버리기 */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) { esp_camera_fb_return(fb); }

    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "TEST: 프레임 자체를 못 받음 → PCLK/VSYNC/HREF 배선 의심");
    } else {
        ESP_LOGI(TAG, "TEST: %u bytes 수신 (%ux%u)", fb->len, fb->width, fb->height);

        /* 프레임 중간쯤 64바이트 출력 (가장자리 블랭킹 회피) */
        const uint8_t *p = fb->buf + fb->len / 2;
        for (int i = 0; i < 64; i += 8) {
            ESP_LOGI(TAG, "  [%02d] %02X %02X %02X %02X %02X %02X %02X %02X",
                     i, p[i], p[i+1], p[i+2], p[i+3],
                        p[i+4], p[i+5], p[i+6], p[i+7]);
        }

        /* 간단 자동 판정: 전체가 같은 값이면 데이터선 미연결 의심 */
        bool all_same = true;
        for (int i = 1; i < 64; i++) {
            if (p[i] != p[0]) { all_same = false; break; }
        }
        if (all_same) {
            ESP_LOGE(TAG, "TEST: 64바이트가 전부 0x%02X → 데이터선(D0~D7) 미연결 의심", p[0]);
        } else {
            ESP_LOGI(TAG, "TEST: 데이터 변화 있음 — 위 hex 가 규칙적 띠 패턴인지 확인");
        }
        esp_camera_fb_return(fb);
    }

    /* 원복: 컬러바 OFF + JPEG 모드로 복귀 */
    s->set_colorbar(s, 0);
    s->set_pixformat(s, PIXFORMAT_JPEG);
    s->set_framesize(s, FRAMESIZE_VGA);   /* 640x480 (init 설정과 일치) */
    ESP_LOGI(TAG, "===== 데이터버스 테스트 종료 (JPEG 모드 원복) =====");
}

esp_err_t camera_module_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RST,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SDA,
        .pin_sccb_scl = CAM_PIN_SCL,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,

        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_DCLK,

        /* OV2640 XVCLK 허용 범위는 6~20MHz (5MHz 이하는 센서 미동작 → probe 실패).
         * 점퍼선 배선에서는 20MHz 가 SDA/SCL 에 crosstalk 노이즈를 유도해
         * SCCB 가 간헐적으로 깨질 수 있어, 스펙 내 최저 안정값인 10MHz 사용. */
        .xclk_freq_hz = 8000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_QVGA,   /* 640x480 */
        .jpeg_quality = 15,

        .fb_count    = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode   = CAMERA_GRAB_LATEST,
    };

    /* SCCB 간헐 실패(접촉/신호 마진) 완화: 실패 시 센서를 전원 사이클로
     * 하드 리셋한 뒤 재시도한다. (최대 3회) */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        camera_power_cycle();          /* 매 시도 전 센서 하드 리셋 */

        err = esp_camera_init(&config);
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW(TAG, "카메라 초기화 성공 (%d번째 시도)", attempt);
            }
            break;
        }

        ESP_LOGW(TAG, "카메라 초기화 실패 0x%x — 재시도 %d/3", err, attempt);
        esp_camera_deinit();           /* 부분 초기화 잔재 정리 (미초기화 상태면 무해) */
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "카메라 초기화 최종 실패 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "센서 PID=0x%x", s->id.PID);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_gainceiling(s, GAINCEILING_2X);
        s->set_brightness(s, -1);
        s->set_ae_level(s, -1);
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);
        s->set_lenc(s, 1);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
    }
    return ESP_OK;
}
