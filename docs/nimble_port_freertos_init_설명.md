# `nimble_port_freertos_init()` 동작 설명

**NimBLE 호스트(BLE 스택)를 돌릴 전용 FreeRTOS 태스크를 하나 생성해서 실행시키는 함수**입니다.
ESP-IDF의 NimBLE 포팅 레이어가 제공합니다 (`nimble/nimble_port_freertos.h`).

## 무슨 일을 하나

`main/ble_prov.c:386`:
```c
nimble_port_freertos_init(ble_host_task);
```

이 한 줄이 내부적으로 `xTaskCreate(...)`를 호출해서:

1. 새 FreeRTOS 태스크를 만들고
2. 그 태스크의 본체로 인자로 넘긴 `ble_host_task` 함수를 실행합니다.

즉, **"이 함수를 BLE 전용 백그라운드 스레드로 돌려줘"** 라는 뜻입니다.
스택 크기·우선순위 같은 설정은 `sdkconfig`(menuconfig의 NimBLE 옵션)에서 자동으로 가져옵니다.

## 왜 별도 태스크가 필요한가

BLE 스택의 심장은 **무한 이벤트 루프**(`nimble_port_run()`)입니다.
이 루프는 한번 들어가면 BLE가 꺼지기 전까지 **빠져나오지 않는 블로킹 함수**입니다.

`main/ble_prov.c:342-346`:
```c
static void ble_host_task(void *param)
{
    nimble_port_run();              /* BLE 이벤트 루프(블로킹, 여기서 안 빠져나옴) */
    nimble_port_freertos_deinit();  /* 루프가 끝나면(BLE 종료 시) 태스크 정리 */
}
```

만약 이걸 `app_main()`에서 직접 호출하면 거기서 영원히 멈춰
카메라·WiFi 등 다른 코드가 못 돌아갑니다.
그래서 별도 태스크로 분리하는 겁니다.

## 전체 그림

```
app_main / ble_prov_start
   │
   ├─ nimble_port_init()                          ← 스택 초기화 (ble_prov.c:355)
   │
   └─ nimble_port_freertos_init(ble_host_task)    ← 새 태스크 생성 (ble_prov.c:386)
            │
            ▼  (별도 태스크에서)
        ble_host_task()
            └─ nimble_port_run()                  ← 무한 이벤트 루프 (BLE 동작 시작)
```

## 짝이 되는 함수들

| 함수 | 역할 | 위치 |
|------|------|------|
| `nimble_port_init()` | NimBLE 스택 메모리/구조 초기화 | `ble_prov.c:355` |
| **`nimble_port_freertos_init()`** | **호스트 태스크 생성 → 이벤트 루프 시작** | `ble_prov.c:386` |
| `nimble_port_freertos_deinit()` | 루프 종료 후 그 태스크 삭제 | `ble_prov.c:345` |
| `nimble_port_run()` | 실제 BLE 이벤트 처리 무한 루프 | `ble_prov.c:344` |
| `nimble_port_stop()` | 이벤트 루프 중지 신호 | `ble_prov.c:399` |
| `nimble_port_deinit()` | 스택 완전 해제 | `ble_prov.c:400` |

## 핵심 요약

> `nimble_port_freertos_init`은 **"BLE 스택의 엔진을 전용 스레드 위에서 시동 거는"** 함수입니다.
