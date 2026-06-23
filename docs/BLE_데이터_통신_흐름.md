# BLE 데이터 통신 흐름 — 앱 ↔ ESP32

## GATT 구조 (데이터 채널)

앱과 ESP32는 BLE의 **GATT** 프로토콜로 통신합니다.
데이터는 "서비스 > 특성(Characteristic)" 구조로 주고받습니다.

```
WiFi 프로비저닝 서비스 (UUID: ...def0)
   ├─ WIFI_LIST 특성 (...def4)   앱 → write("SCAN") / ESP32 → notify(목록 JSON)
   ├─ SSID     특성 (...def1)   앱 → write(WiFi 이름)
   ├─ PASS     특성 (...def2)   앱 → write(비밀번호)
   └─ STATUS   특성 (...def3)   ESP32 → notify("CONNECTING"/"CONNECTED"/"FAILED")
```

| 특성 | 앱이 할 수 있는 것 | ESP32가 할 수 있는 것 |
|------|------------------|---------------------|
| WIFI_LIST | write ("SCAN" 전송) | notify (목록 JSON 전송) |
| SSID | write (SSID 전송) | — |
| PASS | write (비밀번호 전송) | — |
| STATUS | read (마지막 상태 조회) | notify (결과 문자열 푸시) |

---

## 전체 통신 순서

### Phase 0 — ESP32 부팅 및 BLE 준비

```
[ESP32]
  app_main()
    │
    ├─ NVS 초기화
    ├─ WiFi 드라이버 초기화
    ├─ ble_prov_init()           ← NimBLE 스택 초기화 + GATT 서비스 등록
    └─ ble_prov_start()          ← BLE 호스트 태스크 시작
             │
             ▼ (BLE 태스크에서)
         ble_on_sync()           ← 스택 준비 완료 신호
             │
             ▼
         ble_start_advertising() ← "ESP32-CAM" 이름으로 주변에 신호 발송 시작
```

이 시점부터 앱이 "ESP32-CAM"을 검색할 수 있습니다.

---

### Phase 1 — 연결 (Connection)

```
[앱]                              [ESP32]
  "ESP32-CAM" 검색 후 연결 시도
        │
        │──── BLE CONNECT ────────►│
                                   │ gap_event_handler (BLE_GAP_EVENT_CONNECT)
                                   │   s_conn_handle 저장
                                   │   on_ble_connected() 콜백 호출
                                   │     → "BLE 연결됨, 스캔 요청 대기" 로그
```

---

### Phase 2 — WiFi 목록 스캔 요청 및 수신

```
[앱]                              [ESP32]
  WIFI_LIST 특성에 "SCAN" write
        │
        │──── WRITE "SCAN" ───────►│
                                   │ wifi_list_access_cb() (WRITE_CHR)
                                   │   on_ble_scan_request() 콜백 호출
                                   │     → scan_and_notify_task 생성 (별도 태스크)
                                   │
                                   │  [wifi_scan 태스크]
                                   │    wifi_manager_scan() — WiFi AP 목록 수집
                                   │
        │◄─── NOTIFY (AP 1) ───────│  ble_prov_notify_wifi_list()
        │  {"ssid":"A","rssi":-60,"secure":true}\n
        │
        │◄─── NOTIFY (AP 2) ───────│  (20ms 간격으로 한 줄씩 전송)
        │  {"ssid":"B","rssi":-75,"secure":false}\n
        │
        │  ...
        │
        │◄─── NOTIFY "END\n" ──────│  목록 끝 표시
```

**포맷:**
- AP 1개당 JSON 1줄 + `\n`
- 마지막에 반드시 `END\n`을 보내 "목록 끝"을 알림
- 20ms 간격으로 전송 (BLE 버퍼 넘침 방지)

---

### Phase 3 — SSID + 비밀번호 전송

```
[앱]                              [ESP32]
  SSID 특성에 WiFi 이름 write
        │
        │──── WRITE "MyWifi" ─────►│
                                   │ ssid_access_cb() (WRITE_CHR)
                                   │   s_pending_ssid 에 임시 보관
                                   │   ("비밀번호가 올 때까지 대기")
  PASS 특성에 비밀번호 write
        │
        │──── WRITE "password" ───►│
                                   │ pass_access_cb() (WRITE_CHR)
                                   │   s_pending_ssid + pass 둘 다 모임
                                   │   on_ble_credentials() 콜백 호출
                                   │     → connect_wifi_task 생성 (별도 태스크)
```

> **핵심:** SSID와 비밀번호는 **반드시 SSID 먼저, PASS 나중** 순서로 와야 합니다.
> SSID 없이 PASS가 먼저 오면 연결을 건너뜁니다 (`ble_prov.c:175`).

---

### Phase 4 — WiFi 연결 시도 및 결과 통보

```
[앱]                              [ESP32]
                                   │  [wifi_conn 태스크]
        │◄─── NOTIFY "CONNECTING"──│  ble_prov_notify_status()
        │
                                   │    wifi_manager_connect() 실행 중...
                                   │
        │◄─── NOTIFY "CONNECTED" ──│  (성공 시) WiFi 연결됨 + NVS에 저장
        │                          │    → 800ms 후 s_prov_done 세마포어 해제
        │                          │       → app_main 깨어나 카메라·HTTP 서버 시작
      또는
        │◄─── NOTIFY "FAILED" ─────│  (실패 시) 앱에서 다시 시도 가능
```

---

### Phase 5 — WiFi 연결 후에도 BLE는 살아 있음

```
[ESP32]
  WiFi 연결 성공 후에도 ble_prov_stop() 호출하지 않음
  → BLE 광고 계속 유지
  → 앱에서 언제든 다시 WiFi 재설정 가능 (Phase 2~4 반복)
```

---

## 전체 시퀀스 한눈에 보기

```
앱                                          ESP32
│                                              │
│                                    부팅 → BLE 광고 시작("ESP32-CAM")
│                                              │
│──── BLE 연결 ───────────────────────────────►│
│                                   on_ble_connected() 호출
│                                              │
│──── WIFI_LIST write "SCAN" ────────────────►│
│                                   scan_and_notify_task 생성
│                                   WiFi AP 스캔
│◄─── NOTIFY {"ssid":"A",...}\n ──────────────│
│◄─── NOTIFY {"ssid":"B",...}\n ──────────────│
│◄─── NOTIFY "END\n" ─────────────────────────│
│                                              │
│──── SSID write "MyWifi" ───────────────────►│
│                                   s_pending_ssid 저장
│──── PASS write "1234" ─────────────────────►│
│                                   connect_wifi_task 생성
│◄─── NOTIFY "CONNECTING" ────────────────────│
│◄─── NOTIFY "CONNECTED" / "FAILED" ──────────│
│                                              │
│         (이후에도 재설정 가능 — BLE 유지)        │
```

---

## 관련 소스 위치

| 동작 | 파일 | 줄 |
|------|------|----|
| GATT 서비스/특성 정의 | `ble_prov.c` | 202~234 |
| 광고 시작 | `ble_prov.c` | 245~278 |
| 연결/해제 이벤트 처리 | `ble_prov.c` | 281~311 |
| WIFI_LIST write 처리 (스캔 트리거) | `ble_prov.c` | 128~147 |
| SSID write 처리 | `ble_prov.c` | 150~158 |
| PASS write 처리 + 콜백 호출 | `ble_prov.c` | 162~178 |
| WiFi 목록 notify 전송 | `ble_prov.c` | 406~422 |
| STATUS notify 전송 | `ble_prov.c` | 426~439 |
| WiFi 스캔 후 notify (JSON 한 줄씩) | `main.c` | 37~63 |
| WiFi 연결 + 결과 notify | `main.c` | 84~113 |
