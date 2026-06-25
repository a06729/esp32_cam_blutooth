# ble_prov.c 코드 분석 — 전자공학과 학생을 위한 설명

## 이 파일이 하는 일 (한 줄 요약)

> ESP32를 BLE 주변장치(Peripheral)로 만들어, 스마트폰 앱이 WiFi 정보를 보낼 수 있는 통신 채널을 제공한다.

---

## 배경 지식 — BLE를 회로로 비유하면

BLE는 마치 **여러 핀이 달린 IC 칩**과 같습니다.

| BLE 개념 | 회로 비유 |
|----------|----------|
| Service (서비스) | IC 칩의 기능 블록 (예: UART 블록, ADC 블록) |
| Characteristic (특성) | 그 블록의 특정 핀 또는 레지스터 |
| Write | 마스터가 슬레이브 레지스터에 값 쓰기 (I2C write) |
| Read | 마스터가 슬레이브 레지스터 값 읽기 (I2C read) |
| Notify | 슬레이브가 인터럽트로 마스터에게 데이터 푸시 |
| GAP | 물리 계층 — 연결/해제, 광고(Advertising) 담당 |
| GATT | 프로토콜 계층 — 데이터 구조와 교환 규칙 담당 |

---

## GATT 구조 — 이 파일이 제공하는 데이터 채널

```
WiFi 프로비저닝 서비스 (UUID: ...def0)
   │
   ├─ WIFI_LIST 특성 (...def4)   [READ | WRITE | NOTIFY]
   │    앱이 "SCAN" 을 write  → ESP32가 WiFi 목록을 notify로 전송
   │
   ├─ SSID 특성 (...def1)       [WRITE]
   │    앱이 WiFi 이름을 write
   │
   ├─ PASS 특성 (...def2)       [WRITE]
   │    앱이 비밀번호를 write
   │
   └─ STATUS 특성 (...def3)     [READ | NOTIFY]
        ESP32가 연결 결과를 notify로 전송
```

UUID는 각 서비스/특성을 구분하는 **고유 주소**입니다 (I2C의 디바이스 주소와 유사).

---

## 코드 흐름 분석

### 1단계 — UUID 정의 (ble_prov.c:64~73)

```c
#define UUID128_BASE(last) BLE_UUID128_INIT(                       \
    (last), 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,             \
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12)

static const ble_uuid128_t PROV_SVC_UUID  = UUID128_BASE(0xf0);
static const ble_uuid128_t WIFI_LIST_UUID = UUID128_BASE(0xf4);
static const ble_uuid128_t SSID_UUID      = UUID128_BASE(0xf1);
static const ble_uuid128_t PASS_UUID      = UUID128_BASE(0xf2);
static const ble_uuid128_t STATUS_UUID    = UUID128_BASE(0xf3);
```

UUID는 128비트(16바이트)짜리 고유 ID입니다.  
`UUID128_BASE` 매크로로 끝자리(f0~f4)만 바꿔서 5개를 한 번에 만듭니다.

> **주의:** NimBLE는 UUID 바이트를 **역순(little-endian)** 으로 저장합니다.  
> 앱에서 `...def0`이면 코드에는 `0xf0, 0xde, ...` 순으로 입력해야 합니다.

---

### 2단계 — 내부 상태 변수 (ble_prov.c:80~97)

```c
static uint16_t s_conn_handle       = BLE_HS_CONN_HANDLE_NONE; // 현재 연결 ID
static uint16_t s_wifi_list_val_hdl = 0;  // WIFI_LIST 특성의 내부 주소 (notify용)
static uint16_t s_status_val_hdl    = 0;  // STATUS 특성의 내부 주소 (notify용)

static ble_on_connected_cb_t    s_on_connected    = NULL;  // 콜백 함수 포인터들
static ble_on_scan_request_cb_t s_on_scan_request = NULL;
static ble_on_credentials_cb_t  s_on_credentials  = NULL;

static char s_wifi_list_buf[512] = "[]"; // 마지막 WiFi 목록 버퍼
static char s_status_buf[128]    = "";   // 마지막 상태 문자열 버퍼
static char s_pending_ssid[33]   = {0};  // SSID 임시 보관 (PASS 오기 전까지)
```

**`s_conn_handle`** — 연결된 폰의 ID입니다. notify를 보낼 때 "누구에게"를 지정하는 데 사용합니다.  
**`val_handle`** — 특성의 내부 번지수입니다. `ble_gatts_add_svcs()` 호출 후 NimBLE가 자동으로 채워줍니다.  
**콜백 포인터** — `main.c`가 등록해 둔 함수를 사건 발생 시 호출합니다 (함수 포인터 패턴).

---

### 3단계 — GATT 접근 콜백 함수들 (ble_prov.c:128~189)

폰이 특성에 read/write하면 NimBLE가 자동으로 해당 콜백을 호출합니다.  
`ctxt->op`로 read인지 write인지 구분합니다.

#### WIFI_LIST 특성 콜백

```c
static int wifi_list_access_cb(...) {
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        // 폰이 read → 저장된 목록(s_wifi_list_buf) 돌려줌
        os_mbuf_append(ctxt->om, s_wifi_list_buf, strlen(s_wifi_list_buf));
        break;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        // 폰이 "SCAN" write → 스캔 트리거 (내용은 무시)
        if (s_on_scan_request) s_on_scan_request();
        break;
    }
    return 0;
}
```

#### SSID 특성 콜백

```c
static int ssid_access_cb(...) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        read_chr_value(ctxt, s_pending_ssid, sizeof(s_pending_ssid));
        // PASS가 올 때까지 s_pending_ssid에 보관
    }
    return 0;
}
```

#### PASS 특성 콜백

```c
static int pass_access_cb(...) {
    char pass[65] = {0};
    read_chr_value(ctxt, pass, sizeof(pass));

    if (s_pending_ssid[0] && s_on_credentials) {
        s_on_credentials(s_pending_ssid, pass);  // SSID + PASS 둘 다 모이면 호출
    }
    return 0;
}
```

> **핵심:** SSID는 `s_pending_ssid`에 임시 저장되고, PASS가 오는 순간 둘을 합쳐 WiFi 연결을 시작합니다.  
> 반드시 **SSID → PASS 순서**로 와야 합니다.

#### mbuf란? — `read_chr_value()` 헬퍼 (ble_prov.c:107~114)

NimBLE는 수신 데이터를 `os_mbuf`(네트워크 버퍼 조각들의 체인)로 전달합니다.  
`read_chr_value()`는 이걸 일반 `char[]` 문자열로 변환해 주는 도우미입니다.

```c
static void read_chr_value(struct ble_gatt_access_ctxt *ctxt, char *out, size_t out_size) {
    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);         // 받은 바이트 수
    if (om_len >= out_size) om_len = out_size - 1;      // 버퍼 넘침 방지
    ble_hs_mbuf_to_flat(ctxt->om, out, om_len, NULL);   // mbuf → char[] 복사
    out[om_len] = '\0';                                  // 문자열 끝 표시
}
```

---

### 4단계 — GATT 서비스 테이블 (ble_prov.c:202~234)

```c
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &PROV_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &WIFI_LIST_UUID.u,
                .access_cb  = wifi_list_access_cb,
                .val_handle = &s_wifi_list_val_hdl,  // ← 등록 후 NimBLE가 채워줌
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                              BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { .uuid = &SSID_UUID.u,   .access_cb = ssid_access_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &PASS_UUID.u,   .access_cb = pass_access_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &STATUS_UUID.u, .access_cb = status_access_cb,
              .val_handle = &s_status_val_hdl,        // ← 등록 후 NimBLE가 채워줌
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 }   // 끝 표시 (센티넬)
        },
    },
    { 0 }           // 끝 표시 (센티넬)
};
```

이 구조체 배열이 **메뉴판** 역할을 합니다.  
NimBLE는 이 메뉴판을 보고 앱의 요청을 올바른 콜백 함수로 라우팅합니다.

---

### 5단계 — 광고(Advertising) (ble_prov.c:245~278)

광고란 ESP32가 주변에 **"나 여기 있어요!"** 라는 패킷을 주기적으로 뿌리는 것입니다.  
폰의 BLE 스캔에 잡히고, 앱이 연결할 수 있게 됩니다.

```c
static void ble_start_advertising(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.name     = (const uint8_t *)"ESP32-CAM";
    fields.name_len = 9;
    // ... 기타 플래그 설정

    ble_gap_adv_set_fields(&fields);   // 광고 패킷 내용 설정

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  // 누구나 연결 가능
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // 일반 발견 모드

    ble_gap_adv_start(..., BLE_HS_FOREVER, ..., gap_event_handler, ...);
    //                      ↑ 시간 제한 없이 계속 광고
}
```

> 광고 패킷은 최대 **31바이트** 제한이 있습니다. 기기 이름이 너무 길면 실패합니다.

---

### 6단계 — GAP 이벤트 처리 (ble_prov.c:281~311)

연결/해제/MTU 같은 **연결 수준 사건**이 `gap_event_handler`로 들어옵니다.

```c
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:          // 폰이 연결을 시도함
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle; // 연결 ID 저장
            s_on_connected();            // main.c에 "연결됨" 알림
        } else {
            ble_start_advertising();     // 실패 시 다시 광고
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:       // 연결이 끊김
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_start_advertising();         // 끊기면 다시 광고 → 재연결 대기
        break;

    case BLE_GAP_EVENT_MTU:             // 최대 전송 단위 협상
        // MTU = 한 번에 보낼 수 있는 최대 바이트
        break;
    }
    return 0;
}
```

---

### 7단계 — sync 콜백과 초기화 흐름 (ble_prov.c:315~388)

```c
static void ble_on_sync(void) {
    ble_hs_util_ensure_addr(0);                    // BLE MAC 주소 확보
    ble_hs_id_infer_auto(0, &s_own_addr_type);     // public/random 타입 결정
    ble_start_advertising();                        // 광고 시작
}
```

```c
esp_err_t ble_prov_init(void) {
    nimble_port_init();          // ① NimBLE 스택 초기화
    ble_svc_gap_init();          // ② GAP 기본 서비스
    ble_svc_gatt_init();         // ③ GATT 기본 서비스
    ble_svc_gap_device_name_set("ESP32-CAM");  // ④ 기기 이름
    ble_gatts_count_cfg(s_gatt_svcs);          // ⑤ 메모리 계산
    ble_gatts_add_svcs(s_gatt_svcs);           // ⑥ 서비스 등록
    ble_hs_cfg.sync_cb = ble_on_sync;          // ⑦ "준비되면 이거 불러줘" 예약
}

esp_err_t ble_prov_start(...) {
    s_on_connected    = on_connected;    // 콜백 등록
    s_on_scan_request = on_scan_request;
    s_on_credentials  = on_credentials;
    nimble_port_freertos_init(ble_host_task);  // ⑧ BLE 호스트 태스크 시작
}
```

**왜 `sync_cb`가 필요한가?**  
`nimble_port_init()`은 소프트웨어만 초기화하고, 실제 BLE 하드웨어(컨트롤러)와  
소프트웨어(호스트)가 동기화 완료되는 순간은 조금 나중입니다.  
그 순간 NimBLE가 `sync_cb`를 자동 호출하기 때문에, 여기서 광고를 시작하는 것이 안전합니다.

```
ble_prov_init()   → 준비 + sync_cb 예약
ble_prov_start()  → 호스트 태스크 실제 가동
                         ↓
                   호스트 ↔ 컨트롤러 동기화 완료
                         ↓ NimBLE 자동 호출
                   ble_on_sync()
                         ↓
                   ble_start_advertising()   ← 여기서 광고 시작
```

---

### 8단계 — Notify 전송 (ble_prov.c:406~439)

ESP32가 앱으로 데이터를 **밀어주는(push)** 방식입니다. 앱이 read 요청을 하지 않아도 됩니다.

```c
void ble_prov_notify_wifi_list(const char *json) {
    strncpy(s_wifi_list_buf, json, sizeof(s_wifi_list_buf) - 1); // 버퍼에도 저장

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_wifi_list_val_hdl == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json)); // 문자열 → mbuf
    ble_gatts_notify_custom(s_conn_handle, s_wifi_list_val_hdl, om); // notify 발송
}
```

`ble_prov_notify_status()`도 동일한 구조이며, 사용하는 핸들만 `s_status_val_hdl`로 다릅니다.

---

## 전체 초기화 ~ 통신 타임라인

```
[ESP32 부팅]
    │
    ├─ ble_prov_init()
    │    ① nimble_port_init()
    │    ② GAP/GATT 기본 서비스 등록
    │    ③ 우리 서비스(s_gatt_svcs) 등록
    │    ④ sync_cb = ble_on_sync 예약
    │
    └─ ble_prov_start()
         ⑤ 콜백 함수들 저장
         ⑥ nimble_port_freertos_init(ble_host_task) → BLE 태스크 시작
                  │
                  ▼ (BLE 태스크에서 nimble_port_run() 실행)
         호스트 ↔ 컨트롤러 동기화
                  │
                  ▼ ble_on_sync() 자동 호출
         ble_start_advertising()  ← "ESP32-CAM" 광고 시작

[앱이 연결]
    gap_event_handler(CONNECT)
      └─ s_conn_handle 저장
      └─ on_ble_connected() 호출

[앱이 WIFI_LIST에 "SCAN" write]
    wifi_list_access_cb(WRITE)
      └─ on_ble_scan_request() 호출
           └─ scan_and_notify_task 생성
                └─ WiFi AP 스캔
                └─ ble_prov_notify_wifi_list() 반복 호출 (JSON 한 줄씩)
                └─ "END\n" 전송

[앱이 SSID write]
    ssid_access_cb(WRITE)
      └─ s_pending_ssid 저장

[앱이 PASS write]
    pass_access_cb(WRITE)
      └─ on_ble_credentials(ssid, pass) 호출
           └─ ble_prov_notify_status("CONNECTING")
           └─ wifi_manager_connect()
           └─ ble_prov_notify_status("CONNECTED" or "FAILED")

[앱 연결 해제]
    gap_event_handler(DISCONNECT)
      └─ ble_start_advertising()  ← 다시 광고 시작
```

---

## 핵심 설계 원칙

### 역할 분리 (콜백 패턴)

`ble_prov.c`는 **BLE 통신만** 담당합니다.  
"WiFi 스캔을 어떻게 할지", "연결 성공 후 무엇을 할지"는 `main.c`가 결정합니다.  
이 둘을 **함수 포인터(콜백)** 로 연결합니다.

```
[ble_prov.c]  사건 감지 → 콜백 호출
                              ↓
[main.c]      구체적인 동작 수행
```

### 별도 태스크 사용

BLE 콜백은 NimBLE의 이벤트 루프 태스크에서 실행됩니다.  
이 안에서 WiFi 스캔처럼 시간이 오래 걸리는 작업을 하면 BLE 전체가 멈춥니다.  
그래서 콜백에서는 새 FreeRTOS 태스크만 만들고 즉시 반환합니다.

```c
static void on_ble_scan_request(void) {
    xTaskCreate(scan_and_notify_task, "wifi_scan", 4096, NULL, 5, NULL);
    // ↑ 태스크만 만들고 바로 리턴 (BLE 이벤트 루프 안 막음)
}
```

---

## 관련 소스 위치 요약

| 내용 | 파일 | 줄 |
|------|------|----|
| UUID 정의 | `ble_prov.c` | 64~73 |
| 내부 상태 변수 | `ble_prov.c` | 80~97 |
| mbuf → 문자열 변환 헬퍼 | `ble_prov.c` | 107~114 |
| WIFI_LIST 콜백 | `ble_prov.c` | 128~147 |
| SSID 콜백 | `ble_prov.c` | 150~158 |
| PASS 콜백 | `ble_prov.c` | 162~178 |
| STATUS 콜백 | `ble_prov.c` | 182~189 |
| GATT 서비스 테이블 | `ble_prov.c` | 202~234 |
| 광고 시작 | `ble_prov.c` | 245~278 |
| GAP 이벤트 처리 | `ble_prov.c` | 281~311 |
| sync 콜백 (광고 트리거) | `ble_prov.c` | 315~338 |
| BLE 호스트 태스크 | `ble_prov.c` | 342~346 |
| init / start 공개 API | `ble_prov.c` | 353~388 |
| WiFi 목록 notify 전송 | `ble_prov.c` | 406~422 |
| STATUS notify 전송 | `ble_prov.c` | 426~439 |
