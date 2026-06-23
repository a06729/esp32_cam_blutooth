# BLE 콜백 (`ble_on_scan_request_cb_t`) 동작 설명

`ble_on_scan_request_cb_t`는 **함수 포인터 타입(typedef)** 입니다.
이걸 이용해 "콜백"을 만드는 흐름을 처음부터 끝까지 따라가 봅니다.

## 1. 타입 정의 — 함수 포인터에 이름 붙이기

`main/ble_prov.h:26`:
```c
typedef void (*ble_on_scan_request_cb_t)(void);
```

이건 **"인자 없고, 반환값 없는(`void`) 함수를 가리키는 포인터"** 라는 타입에
`ble_on_scan_request_cb_t`라는 별명을 붙인 것입니다.
즉 변수에 "함수 자체"를 담을 수 있게 됩니다.

## 2. 그 타입의 변수를 전역으로 하나 만들기

`main/ble_prov.c:88`:
```c
static ble_on_scan_request_cb_t  s_on_scan_request = NULL; /* "SCAN" write 받았을 때 부를 함수 */
```

"SCAN 요청이 오면 호출할 함수"를 보관할 빈 상자입니다. 처음엔 `NULL`(아직 등록 안 됨).

## 3. 등록(register) — 누가 이 상자를 채우나

`main.c`가 BLE를 시작할 때 자기 함수를 넘겨줍니다.

`main/main.c:70`, `main/main.c:158`:
```c
static void on_ble_scan_request(void)   // ← 시그니처가 typedef와 동일 (void, void)
{
    xTaskCreate(scan_and_notify_task, "wifi_scan", 4096, NULL, 5, NULL);
}
...
ble_prov_start(on_ble_connected, on_ble_scan_request, on_ble_credentials);
```

`main/ble_prov.c:378-385`:
```c
esp_err_t ble_prov_start(... ble_on_scan_request_cb_t on_scan_request, ...)
{
    s_on_scan_request = on_scan_request;  // ← 넘겨받은 함수를 상자에 저장
    ...
}
```

이 시점에 `s_on_scan_request`는 `main.c`의 `on_ble_scan_request`를 가리키게 됩니다.

## 4. 호출(call) — 사건이 발생하면 상자 속 함수를 실행

폰이 BLE로 "SCAN"을 write하면 NimBLE가 `wifi_list_access_cb`를 불러주고, 그 안에서:

`main/ble_prov.c:139-141`:
```c
if (s_on_scan_request) {     // NULL 체크 (등록 안 됐으면 호출 안 함 → 크래시 방지)
    s_on_scan_request();     // ← 상자에 담긴 main.c의 함수를 실제로 호출
}
```

## 핵심 — 왜 이렇게 하나 (역할 분리)

```
폰이 "SCAN" write
        │
        ▼
[ble_prov.c]  BLE 통신만 담당 → "무엇을 할지"는 모름
   s_on_scan_request()  ───┐  (함수 포인터로 간접 호출)
                           ▼
[main.c]      on_ble_scan_request() → WiFi 스캔 태스크 생성
```

- `ble_prov.c`는 **BLE 프로토콜만** 알고, "스캔이 실제로 무슨 일을 해야 하는지"는 모릅니다.
- `main.c`가 **할 일**을 함수로 정의해 등록해 둡니다.
- 덕분에 `ble_prov.c`는 `main.c`를 직접 `#include`하거나 의존하지 않아도 됩니다.
  (의존성 역전 / 모듈 분리)

이 패턴이 콜백의 핵심입니다:
**"네가 호출할 함수를 내가 미리 너에게 등록해 둘게, 그 일이 생기면 그때 불러줘."**

> 참고: 같은 파일의 `s_on_connected`(연결됨), `s_on_credentials`(SSID+PASS 수신 완료)도
> 완전히 동일한 방식으로 동작합니다.
