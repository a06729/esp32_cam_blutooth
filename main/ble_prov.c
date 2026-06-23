#include "ble_prov.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdio.h>

/* TAG: ESP-IDF 로그(ESP_LOGI 등)에 찍히는 이 모듈의 이름표. */
static const char *TAG = "ble_prov";

/* ==================================================================
 *  이 파일이 하는 일 (큰 그림)
 * ------------------------------------------------------------------
 *  ESP32 가 "BLE 주변장치(Peripheral)"가 되어, 폰 앱("Central")이
 *  연결해서 WiFi 정보를 주고받을 수 있게 해주는 코드다.
 *
 *  [BLE 용어 3개만 알면 된다]
 *   - GAP  : '연결' 담당. 광고(advertising)를 뿌려서 폰이 나를 발견/연결하게 함.
 *   - GATT : '데이터 교환' 담당. 데이터를 (서비스 > 특성) 구조로 노출한다.
 *   - Service / Characteristic(특성):
 *        Service        = 관련 데이터 묶음 (폴더 같은 것)
 *        Characteristic = 실제 값 한 칸 (파일 같은 것).
 *                         칸마다 read/write/notify 같은 권한이 정해져 있다.
 *
 *  [이 모듈의 GATT 구조] — 서비스 1개 + 특성 4개
 *     WiFi 프로비저닝 서비스 (PROV_SVC_UUID, ...f0)
 *       ├─ WIFI_LIST 특성 (...f4)  폰이 write→스캔 요청 / 칩이 notify→목록 전송
 *       ├─ SSID     특성 (...f1)  폰이 write→ WiFi 이름 전달
 *       ├─ PASS     특성 (...f2)  폰이 write→ 비밀번호 전달
 *       └─ STATUS   특성 (...f3)  칩이 notify→ 연결 결과 전달
 *
 *  [핵심 규칙]
 *   - 폰이 어떤 특성에 write 하면  → 이 파일의 그 특성 'access_cb' 함수가 호출됨.
 *   - 이 파일이 notify 를 보내면   → 폰 쪽 수신 콜백이 호출됨.
 *
 *  [NimBLE 란?] ESP-IDF 가 쓰는 BLE 소프트웨어 스택(라이브러리).
 *   - 별도 FreeRTOS 태스크(ble_host_task)에서 돌아간다.
 *   - 준비되면 sync 콜백(ble_on_sync)이 불리고, 거기서 광고를 시작한다.
 *   - 이후 모든 사건(연결/해제/데이터 수신)은 '콜백'으로 우리에게 전달된다.
 *     즉 이 파일은 대부분 "사건이 생기면 불릴 함수"들의 모음이다.
 * ================================================================== */

/* ==================================================================
 *  ⚠️ 아래 4개 128비트 UUID는 앱 코드와 *정확히* 일치해야 합니다.
 *
 *  앱(TypeScript):
 *    WIFI_SERVICE_UUID = '12345678-1234-5678-1234-56789abcdef0'
 *    SSID_CHAR_UUID    = '12345678-1234-5678-1234-56789abcdef1'  ← 앱 값 확인!
 *    PASS_CHAR_UUID    = '12345678-1234-5678-1234-56789abcdef2'  ← 앱 값 확인!
 *    STATUS_CHAR_UUID  = '12345678-1234-5678-1234-56789abcdef3'  ← 앱 값 확인!
 *    (WiFi 목록 특성 UUID)                                       ← 앱 값 확인!
 *
 *  NimBLE 의 BLE_UUID128_INIT 는 바이트를 *역순(little-endian)* 으로 받습니다.
 *  즉 '...def0' 은 0xf0 가 맨 앞에 옵니다. 끝자리만 f0→f4 로 바뀝니다.
 *  앱 UUID가 다르면 16바이트 전체를 역순으로 바꿔 넣으세요.
 * ================================================================== */
/* UUID = 서비스/특성을 구분하는 고유 ID(주소 같은 것). 128비트(16바이트)다.
 * 이 매크로는 끝 한 바이트(last)만 바꿔서 5개의 UUID를 쉽게 만든다.
 * 바이트가 역순인 이유는 윗 주석 참고(NimBLE 가 little-endian 으로 받음). */
#define UUID128_BASE(last) BLE_UUID128_INIT(                       \
    (last), 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,             \
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12)

/* 서비스 1개 + 특성 4개의 UUID. 끝자리(f0~f4)로 구분된다. */
static const ble_uuid128_t PROV_SVC_UUID   = UUID128_BASE(0xf0);  /* 서비스 */
static const ble_uuid128_t WIFI_LIST_UUID  = UUID128_BASE(0xf4);  /* WiFi 목록 ("SCAN" write / notify) */
static const ble_uuid128_t SSID_UUID       = UUID128_BASE(0xf1);  /* SSID (write) */
static const ble_uuid128_t PASS_UUID       = UUID128_BASE(0xf2);  /* 비밀번호 (write) */
static const ble_uuid128_t STATUS_UUID     = UUID128_BASE(0xf3);  /* 상태 (read/notify) */

#define DEVICE_NAME         "ESP32-CAM"
#define WIFI_LIST_BUF_SIZE  512
#define STATUS_BUF_SIZE     128

/* ---- 모듈 내부 상태 (static = 이 파일 안에서만 보이는 전역 변수) ---- */
static uint16_t s_conn_handle        = BLE_HS_CONN_HANDLE_NONE; /* 현재 연결의 ID. 연결 없으면 NONE */
static uint16_t s_wifi_list_val_hdl  = 0;  /* WIFI_LIST 특성의 핸들(내부 주소). notify 보낼 때 필요 */
static uint16_t s_status_val_hdl     = 0;  /* STATUS 특성의 핸들. notify 보낼 때 필요 */
static uint8_t  s_own_addr_type      = 0;  /* 내 BLE 주소 종류(public/random). sync 때 자동 결정 */

/* 콜백 함수 포인터: 사건이 생기면 'main.c 가 등록해 둔 함수'를 대신 호출한다.
 * (이 파일은 BLE 통신만 담당, '무엇을 할지'는 main.c 가 결정 → 역할 분리) */
static ble_on_connected_cb_t     s_on_connected    = NULL; /* 연결됐을 때 부를 함수 */
static ble_on_scan_request_cb_t  s_on_scan_request = NULL; /* "SCAN" write 받았을 때 부를 함수 */
static ble_on_credentials_cb_t   s_on_credentials  = NULL; /* SSID+PASS 다 받았을 때 부를 함수 */

/* 폰이 'read' 하면 돌려줄 마지막 값을 담아두는 버퍼 */
static char s_wifi_list_buf[WIFI_LIST_BUF_SIZE] = "[]"; /* 마지막 WiFi 목록 JSON */
static char s_status_buf[STATUS_BUF_SIZE]        = "";  /* 마지막 상태 문자열 */

/* SSID 와 PASS 는 따로따로(write 2번) 도착한다. SSID 가 먼저 오면 여기 잠깐
 * 저장해 두고, PASS 가 도착하는 순간 둘을 합쳐 WiFi 연결을 시작한다. */
static char s_pending_ssid[33] = { 0 };

/* ------------------------------------------------------------------ */
/* mbuf → 평문 문자열 추출 헬퍼                                          */
/* ------------------------------------------------------------------ */
/* NimBLE 는 폰이 보낸 데이터를 'os_mbuf'(여러 조각으로 나뉠 수 있는 네트워크
 * 버퍼)로 준다. 다루기 쉬운 평범한 C 문자열(char[])로 복사해 주는 도우미.
 *   ctxt->om     : 수신 데이터가 든 mbuf
 *   OS_MBUF_PKTLEN: 그 안의 전체 바이트 수
 * 버퍼보다 길면 잘라내고(out_size-1), 끝에 '\0'(문자열 끝 표시)을 붙인다. */
static void read_chr_value(struct ble_gatt_access_ctxt *ctxt,
                           char *out, size_t out_size)
{
    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);     /* 받은 바이트 수 */
    if (om_len >= out_size) om_len = out_size - 1;  /* 버퍼 넘침 방지 */
    ble_hs_mbuf_to_flat(ctxt->om, out, om_len, NULL); /* mbuf → out 로 복사 */
    out[om_len] = '\0';                              /* C 문자열로 마무리 */
}

/* ------------------------------------------------------------------ */
/* GATT 접근 콜백                                                      */
/* ------------------------------------------------------------------ */
/* 'access_cb' = 폰이 특정 특성에 read/write 할 때마다 NimBLE 가 자동으로
 * 불러주는 함수. ctxt->op 로 "지금 read 인지 write 인지"를 구분한다.
 *   - READ_CHR  : 폰이 읽으려 함 → os_mbuf_append 로 보낼 값을 채워준다.
 *   - WRITE_CHR : 폰이 썼음     → read_chr_value 로 보낸 값을 꺼내 쓴다.
 * 반환값 0 = 성공(0이 아니면 폰에 에러 응답). */

/* [WIFI_LIST 특성] 한 특성이 read 와 write 둘 다 받는다.
 *  - write: 폰이 "SCAN" 을 보냄 → 스캔을 시작하라는 신호로만 쓰고 내용은 무시
 *  - read : 마지막에 저장해 둔 목록(s_wifi_list_buf)을 돌려줌 */
static int wifi_list_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* 폰에게 보낼 데이터를 응답 버퍼(ctxt->om)에 채운다 */
        os_mbuf_append(ctxt->om, s_wifi_list_buf, strlen(s_wifi_list_buf));
        break;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* 페이로드("SCAN")는 무시하고 스캔만 트리거.
         * 실제 스캔은 main.c 가 등록한 콜백이 별도 태스크에서 수행한다. */
        if (s_on_scan_request) {
            s_on_scan_request();
        }
        break;
    default:
        break;
    }
    return 0;
}

/* [SSID 특성] 폰이 WiFi 이름을 write → 비밀번호가 올 때까지 잠시 보관만 한다. */
static int ssid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        read_chr_value(ctxt, s_pending_ssid, sizeof(s_pending_ssid)); /* 받은 SSID 저장 */
        ESP_LOGI(TAG, "SSID 수신: %s", s_pending_ssid);
    }
    return 0;
}

/* [PASS 특성] 폰이 비밀번호를 write → 이제 SSID+비번 둘 다 모였으므로
 * main.c 의 연결 콜백을 불러 실제 WiFi 연결을 시작시킨다. */
static int pass_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0; /* write 만 처리 */

    char pass[65] = { 0 };
    read_chr_value(ctxt, pass, sizeof(pass));               /* 받은 비밀번호 꺼내기 */
    ESP_LOGI(TAG, "비밀번호 수신 (길이=%d)", (int)strlen(pass));

    /* SSID 가 먼저 와 있어야 정상. 둘 다 있으면 연결 콜백 호출(SSID, 비번 전달) */
    if (s_pending_ssid[0] && s_on_credentials) {
        s_on_credentials(s_pending_ssid, pass);
    } else {
        ESP_LOGW(TAG, "SSID 가 먼저 수신되지 않음 — 연결 생략");
    }
    return 0;
}

/* [STATUS 특성] 폰이 read 하면 마지막 상태 문자열을 돌려준다.
 * (평소엔 read 보다 notify(ble_prov_notify_status)로 결과를 밀어준다) */
static int status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, s_status_buf, strlen(s_status_buf));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* GATT 서비스 테이블                                                   */
/* ------------------------------------------------------------------ */
/* "이 기기는 어떤 서비스/특성을 제공한다"를 표로 정의한 것. ble_prov_init 에서
 * 이 표를 NimBLE 에 등록한다. 각 칸의 의미:
 *   .uuid       : 그 서비스/특성의 고유 ID
 *   .access_cb  : read/write 가 오면 불릴 함수(위에서 만든 콜백들)
 *   .val_handle : 등록 후 NimBLE 가 그 특성의 핸들(주소)을 여기에 채워줌
 *                 → 나중에 notify 보낼 때 이 핸들이 필요(그래서 주소 &로 넘김)
 *   .flags      : 그 특성의 권한(READ/WRITE/NOTIFY 등)을 OR(|)로 조합
 *   { 0 }       : 배열의 '끝' 표시(센티넬). 특성 목록 끝, 서비스 목록 끝에 둔다. */
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,   /* 기본(primary) 서비스 */
        .uuid = &PROV_SVC_UUID.u,            /* WiFi 프로비저닝 서비스(...f0) */
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* WiFi 목록: 폰이 write(스캔요청) + 칩이 notify(목록) + read(마지막값) */
                .uuid       = &WIFI_LIST_UUID.u,
                .access_cb  = wifi_list_access_cb,
                .val_handle = &s_wifi_list_val_hdl,  /* notify 용 핸들 받기 */
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                              BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   /* SSID: 폰이 write 만 함 (응답 있는/없는 write 둘 다 허용) */
                .uuid      = &SSID_UUID.u,
                .access_cb = ssid_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   /* PASS: 폰이 write 만 함 */
                .uuid      = &PASS_UUID.u,
                .access_cb = pass_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   /* STATUS: 칩이 notify(결과) + read(마지막값). 폰은 못 씀 */
                .uuid       = &STATUS_UUID.u,
                .access_cb  = status_access_cb,
                .val_handle = &s_status_val_hdl,     /* notify 용 핸들 받기 */
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }   /* ← 특성 목록의 끝 */
        },
    },
    { 0 }           /* ← 서비스 목록의 끝 */
};

/* ------------------------------------------------------------------ */
/* GAP 이벤트 및 광고                                                   */
/* ------------------------------------------------------------------ */
/* gap_event_handler 를 아래에서 정의하지만 ble_start_advertising 에서 먼저
 * 참조하므로, 컴파일러에게 "이런 함수가 있다"고 미리 알리는 선언(프로토타입). */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* 광고(advertising) 시작 = "나 여기 있어요!"라고 주변에 신호를 뿌려
 * 폰이 검색/연결할 수 있게 하는 것. */
static void ble_start_advertising(void)
{
    /* 1) 광고 패킷에 담을 정보 구성 */
    struct ble_hs_adv_fields fields = { 0 };
    /* 일반 발견 가능 + BR/EDR(구형 블루투스) 미지원 표시 */
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;                       /* 송신 출력 포함 */
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name                  = (const uint8_t *)DEVICE_NAME; /* 기기 이름 "ESP32-CAM" */
    fields.name_len              = strlen(DEVICE_NAME);
    fields.name_is_complete      = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        /* 광고 패킷은 최대 31바이트 — 이름이 너무 길면 여기서 실패 */
        ESP_LOGE(TAG, "광고 데이터 설정 실패: %d (광고 패킷이 31바이트 초과인지 확인)", rc);
        return;
    }

    /* 2) 광고 방식 설정 */
    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  /* 누구나 연결 가능(undirected) */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  /* 일반 발견 가능 모드 */

    /* 3) 광고 시작. 앞으로 GAP 사건은 gap_event_handler 로 들어온다.
     *    BLE_HS_FOREVER = 시간 제한 없이 계속 광고 */
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {  /* EALREADY = 이미 광고 중(무시) */
        ESP_LOGE(TAG, "광고 시작 실패: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE 광고 시작 — 폰에서 '%s' 검색하세요", DEVICE_NAME);
    }
}

/* GAP 사건 처리기: 연결/해제/MTU협상 같은 '연결 수준' 사건이 여기로 들어온다. */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:  /* 폰이 연결을 시도함 */
        if (event->connect.status == 0) {           /* 0 = 연결 성공 */
            s_conn_handle = event->connect.conn_handle; /* 연결 ID 저장(notify 때 사용) */
            ESP_LOGI(TAG, "BLE 연결됨 (handle=%d)", s_conn_handle);
            if (s_on_connected) {
                s_on_connected();                    /* main.c 에 "연결됨" 알림 */
            }
        } else {                                     /* 연결 실패 */
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_start_advertising();                 /* 다시 광고해 재시도 대기 */
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:  /* 연결이 끊김 */
        ESP_LOGI(TAG, "BLE 연결 해제 (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_start_advertising();    /* 끊기면 다시 광고 → 다음 연결을 받을 수 있게 */
        break;

    case BLE_GAP_EVENT_MTU:  /* 한 번에 보낼 수 있는 최대 바이트(MTU) 협상 결과 */
        ESP_LOGI(TAG, "MTU 협상: %d bytes", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* sync 콜백: BLE 스택(호스트+컨트롤러)이 준비를 마치면 NimBLE 가 한 번 불러준다.
 * "이제 BLE 써도 된다"는 신호 → 여기서 주소를 정하고 광고를 시작한다. */
static void ble_on_sync(void)
{
    /* 컨트롤러에 사용할 BLE 주소가 등록되어 있는지 보장 (없으면 랜덤 생성) */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 실패: %d", rc);
        return;
    }

    /* 사용할 주소 타입 자동 추론 (public 없으면 random) → s_own_addr_type 에 저장 */
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "주소 타입 추론 실패: %d", rc);
        return;
    }

    /* 디버그용: 내 BLE MAC 주소를 로그로 출력 (바이트가 역순 저장이라 거꾸로 찍음) */
    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE 주소 타입=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             s_own_addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    ble_start_advertising();  /* 준비 끝 → 광고 시작 */
}

/* NimBLE 호스트가 도는 전용 FreeRTOS 태스크. nimble_port_run() 은 내부적으로
 * 무한 루프라서, 이벤트를 계속 처리하며 여기서 빠져나오지 않는다(BLE 종료 전까지). */
static void ble_host_task(void *param)
{
    nimble_port_run();              /* BLE 이벤트 루프(블로킹) */
    nimble_port_freertos_deinit();  /* 루프가 끝나면 태스크 정리 */
}

/* ------------------------------------------------------------------ */
/* 공개 API (다른 파일, 주로 main.c 가 호출하는 함수들)                  */
/* ------------------------------------------------------------------ */
/* ble_prov_init: BLE 를 '준비'만 한다(아직 광고는 시작 안 함).
 * NimBLE 켜기 → 기본 서비스(GAP/GATT) 등록 → 우리 서비스 표 등록 → sync 콜백 지정. */
esp_err_t ble_prov_init(void)
{
    esp_err_t err = nimble_port_init();   /* NimBLE 스택 초기화 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 실패: %d", err);
        return err;
    }

    ble_svc_gap_init();                       /* GAP 기본 서비스 */
    ble_svc_gatt_init();                      /* GATT 기본 서비스 */
    ble_svc_gap_device_name_set(DEVICE_NAME); /* 기기 이름 설정 */

    /* 우리가 만든 서비스 표(s_gatt_svcs)를 등록.
     * count_cfg = 필요한 자원 계산, add_svcs = 실제 등록 (2단계) */
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count_cfg 실패: %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs 실패: %d", rc); return ESP_FAIL; }

    ble_hs_cfg.sync_cb = ble_on_sync;  /* 준비 끝나면 부를 콜백 등록 */
    return ESP_OK;
}

/* ble_prov_start: 콜백들을 기억해 두고, BLE 호스트 태스크를 띄워 '실제 가동'.
 * 이 태스크가 돌기 시작하면 sync→광고 순서로 진행된다. */
esp_err_t ble_prov_start(ble_on_connected_cb_t    on_connected,
                         ble_on_scan_request_cb_t on_scan_request,
                         ble_on_credentials_cb_t  on_credentials)
{
    /* main.c 가 넘긴 함수들을 전역 포인터에 저장 → 나중에 사건 발생 시 호출 */
    s_on_connected    = on_connected;
    s_on_scan_request = on_scan_request;
    s_on_credentials  = on_credentials;
    nimble_port_freertos_init(ble_host_task);  /* ble_host_task 를 새 태스크로 실행 */
    return ESP_OK;
}

/* ble_prov_stop: BLE 를 완전히 끈다(연결 종료 → 광고 중지 → 스택 해제).
 * ※ 현재 프로젝트는 재설정을 위해 BLE 를 계속 켜두므로 이 함수는 호출하지 않음. */
void ble_prov_stop(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM); /* 연결 끊기 */
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();    /* 광고 중지 */
    nimble_port_stop();    /* 이벤트 루프 중지 */
    nimble_port_deinit();  /* 스택 해제 */
    ESP_LOGI(TAG, "BLE 종료");
}

/* ble_prov_notify_wifi_list: WiFi 목록(JSON 한 줄)을 폰으로 'notify' 전송.
 * main.c 의 스캔 태스크가 한 줄씩 이 함수를 호출해 목록을 흘려보낸다. */
void ble_prov_notify_wifi_list(const char *json)
{
    /* read 로도 가져갈 수 있게 마지막 값을 버퍼에도 저장 */
    strncpy(s_wifi_list_buf, json, sizeof(s_wifi_list_buf) - 1);

    /* 연결이 없거나 핸들이 아직 없으면 notify 보낼 수 없으니 그냥 종료 */
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_wifi_list_val_hdl == 0) return;

    /* 보낼 문자열을 mbuf 로 만들고, 해당 특성 핸들로 notify 발사 */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om) {
        int rc = ble_gatts_notify_custom(s_conn_handle, s_wifi_list_val_hdl, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "WiFi 목록 알림 실패: %d", rc);
        }
    }
}

/* ble_prov_notify_status: 연결 결과 문자열("CONNECTING"/"CONNECTED"/"FAILED")을
 * 폰으로 notify 전송. 위 함수와 구조가 동일하고, 쓰는 특성만 STATUS 다. */
void ble_prov_notify_status(const char *json)
{
    strncpy(s_status_buf, json, sizeof(s_status_buf) - 1);

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_val_hdl == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om) {
        int rc = ble_gatts_notify_custom(s_conn_handle, s_status_val_hdl, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "상태 알림 실패: %d", rc);
        }
    }
}
