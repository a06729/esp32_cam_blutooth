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

static const char *TAG = "ble_prov";

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
#define UUID128_BASE(last) BLE_UUID128_INIT(                       \
    (last), 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,             \
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12)

static const ble_uuid128_t PROV_SVC_UUID   = UUID128_BASE(0xf0);  /* 서비스 */
static const ble_uuid128_t WIFI_LIST_UUID  = UUID128_BASE(0xf4);  /* WiFi 목록 ("SCAN" write / notify) */
static const ble_uuid128_t SSID_UUID       = UUID128_BASE(0xf1);  /* SSID (write) */
static const ble_uuid128_t PASS_UUID       = UUID128_BASE(0xf2);  /* 비밀번호 (write) */
static const ble_uuid128_t STATUS_UUID     = UUID128_BASE(0xf3);  /* 상태 (read/notify) */

#define DEVICE_NAME         "ESP32-CAM"
#define WIFI_LIST_BUF_SIZE  512
#define STATUS_BUF_SIZE     128

static uint16_t s_conn_handle        = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_wifi_list_val_hdl  = 0;
static uint16_t s_status_val_hdl     = 0;
static uint8_t  s_own_addr_type      = 0;  /* sync 시 자동 추론된 주소 타입 */

static ble_on_connected_cb_t     s_on_connected    = NULL;
static ble_on_scan_request_cb_t  s_on_scan_request = NULL;
static ble_on_credentials_cb_t   s_on_credentials  = NULL;

static char s_wifi_list_buf[WIFI_LIST_BUF_SIZE] = "[]";
static char s_status_buf[STATUS_BUF_SIZE]        = "";

/* SSID write 와 PASS write 가 따로 오므로 SSID 를 잠시 보관 */
static char s_pending_ssid[33] = { 0 };

/* ------------------------------------------------------------------ */
/* mbuf → 평문 문자열 추출 헬퍼                                          */
/* ------------------------------------------------------------------ */
static void read_chr_value(struct ble_gatt_access_ctxt *ctxt,
                           char *out, size_t out_size)
{
    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len >= out_size) om_len = out_size - 1;
    ble_hs_mbuf_to_flat(ctxt->om, out, om_len, NULL);
    out[om_len] = '\0';
}

/* ------------------------------------------------------------------ */
/* GATT 접근 콜백                                                      */
/* ------------------------------------------------------------------ */
/* WiFi 목록: 앱이 "SCAN" 을 write 하면 스캔을 트리거하고, read 하면 마지막 결과 반환 */
static int wifi_list_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        os_mbuf_append(ctxt->om, s_wifi_list_buf, strlen(s_wifi_list_buf));
        break;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* 페이로드("SCAN")는 무시하고 스캔만 트리거 */
        if (s_on_scan_request) {
            s_on_scan_request();
        }
        break;
    default:
        break;
    }
    return 0;
}

/* SSID write: 비밀번호가 올 때까지 보관 */
static int ssid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        read_chr_value(ctxt, s_pending_ssid, sizeof(s_pending_ssid));
        ESP_LOGI(TAG, "SSID 수신: %s", s_pending_ssid);
    }
    return 0;
}

/* PASS write: SSID + 비밀번호가 모두 모였으므로 연결 콜백 호출 */
static int pass_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    char pass[65] = { 0 };
    read_chr_value(ctxt, pass, sizeof(pass));
    ESP_LOGI(TAG, "비밀번호 수신 (길이=%d)", (int)strlen(pass));

    if (s_pending_ssid[0] && s_on_credentials) {
        s_on_credentials(s_pending_ssid, pass);
    } else {
        ESP_LOGW(TAG, "SSID 가 먼저 수신되지 않음 — 연결 생략");
    }
    return 0;
}

/* 상태: 앱이 read 하면 마지막 상태 문자열 반환 */
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
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &PROV_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &WIFI_LIST_UUID.u,
                .access_cb  = wifi_list_access_cb,
                .val_handle = &s_wifi_list_val_hdl,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                              BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid      = &SSID_UUID.u,
                .access_cb = ssid_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid      = &PASS_UUID.u,
                .access_cb = pass_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &STATUS_UUID.u,
                .access_cb  = status_access_cb,
                .val_handle = &s_status_val_hdl,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ------------------------------------------------------------------ */
/* GAP 이벤트 및 광고                                                   */
/* ------------------------------------------------------------------ */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name                  = (const uint8_t *)DEVICE_NAME;
    fields.name_len              = strlen(DEVICE_NAME);
    fields.name_is_complete      = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "광고 데이터 설정 실패: %d (광고 패킷이 31바이트 초과인지 확인)", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "광고 시작 실패: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE 광고 시작 — 폰에서 '%s' 검색하세요", DEVICE_NAME);
    }
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE 연결됨 (handle=%d)", s_conn_handle);
            if (s_on_connected) {
                s_on_connected();
            }
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 연결 해제 (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 협상: %d bytes", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void ble_on_sync(void)
{
    /* 컨트롤러에 사용할 BLE 주소가 등록되어 있는지 보장 (없으면 랜덤 생성) */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 실패: %d", rc);
        return;
    }

    /* 사용할 주소 타입 자동 추론 (public 없으면 random) */
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "주소 타입 추론 실패: %d", rc);
        return;
    }

    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE 주소 타입=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             s_own_addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    ble_start_advertising();
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/* 공개 API                                                             */
/* ------------------------------------------------------------------ */
esp_err_t ble_prov_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 실패: %d", err);
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count_cfg 실패: %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs 실패: %d", rc); return ESP_FAIL; }

    ble_hs_cfg.sync_cb = ble_on_sync;
    return ESP_OK;
}

esp_err_t ble_prov_start(ble_on_connected_cb_t    on_connected,
                         ble_on_scan_request_cb_t on_scan_request,
                         ble_on_credentials_cb_t  on_credentials)
{
    s_on_connected    = on_connected;
    s_on_scan_request = on_scan_request;
    s_on_credentials  = on_credentials;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

void ble_prov_stop(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    ESP_LOGI(TAG, "BLE 종료");
}

void ble_prov_notify_wifi_list(const char *json)
{
    strncpy(s_wifi_list_buf, json, sizeof(s_wifi_list_buf) - 1);

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_wifi_list_val_hdl == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om) {
        int rc = ble_gatts_notify_custom(s_conn_handle, s_wifi_list_val_hdl, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "WiFi 목록 알림 실패: %d", rc);
        }
    }
}

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
