| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# ESP32-CAM : BLE 프로비저닝 + MJPEG 스트리밍 + MQTT + UART 프로토콜

ESP32-S3 + OV2640 카메라 기반 펌웨어입니다.
BLE 로 WiFi 를 설정하고, 연결되면 카메라 영상을 MQTT(또는 HTTP)로 전송하며,
별도의 UART 채널로 외부 장치(Basys3 FPGA 등)와 사용자 정의 프로토콜로 통신합니다.

---

## 1. 주요 기능

| 기능 | 설명 | 소스 |
| ---- | ---- | ---- |
| **BLE WiFi 프로비저닝** | 앱이 BLE 로 연결 → WiFi 목록 전송 → SSID/PW 수신 → 연결. 연결 후에도 광고를 유지해 언제든 재설정 가능 | `ble_prov.c/.h` |
| **WiFi 관리** | NVS 에 자격증명 저장/로드, 스캔, 연결, IP 조회 | `wifi_manager.c/.h` |
| **카메라** | OV2640 초기화 및 JPEG 캡처 | `camera_module.c/.h`, `camera_pins.h` |
| **MQTT 전송 (메인 경로)** | 브로커의 `capture` 명령 수신 시 한 장 찍어 `esp32cam/image` 토픽으로 JPEG 전송 | `mqtt_cam.c/.h` |
| **HTTP 스트리밍 (디버그용)** | `/` 뷰어, `/stream` MJPEG, `/jpg` 정지영상 (현재 main 에서 주석 처리) | `http_server.c/.h` |
| **UART 프로토콜** | UART2 로 외부 장치와 `$...\n` 프레임 송수신 (W/R 명령), USB 시리얼 모니터로 디버깅 | `protocol.c/.h` |

---

## 2. 부팅 동작 순서 (`main/main.c`)

1. NVS / 네트워크 / 이벤트 루프 초기화
2. WiFi 드라이버 초기화 (연결은 아직 안 함)
3. **BLE 프로비저닝 시작** — `"ESP32-CAM"` 으로 광고 (성공 후에도 계속 유지)
4. NVS 에 저장된 자격증명이 있으면 **자동 연결** 시도
5. 저장 정보가 없거나 실패하면 앱이 BLE 로 자격증명을 보낼 때까지 대기
6. WiFi 연결 완료 → **카메라 초기화**
7. **MQTT 클라이언트 시작** (`mqtt_cam_start()`)
8. **UART 프로토콜 초기화** (`protocol_uart_init()`)
9. (선택) HTTP 서버 — 현재 주석 처리되어 있음

---

## 3. UART 프로토콜 (`main/protocol.c` / `protocol.h`)

ESP32 가 **Slave**, 외부 장치(Basys3 등)가 **Master** 인 단순 프레임 프로토콜입니다.

### 핀 / 설정

| 항목 | 값 |
| ---- | ---- |
| UART 포트 | `UART_NUM_2` |
| TX 핀 | GPIO 1 |
| RX 핀 | GPIO 2 |
| Baud | 115200 |
| Slave ID | `0x01` |

> 디버그 로그(`ESP_LOGx`)는 **USB 콘솔(UART0)** 로 나가므로, `idf.py monitor` 시리얼 모니터에서 확인합니다.

### 프레임 포맷

```
W (쓰기, 7바이트):  $  ID  'W'  ADDR  DATA  CHK  \n
R (읽기, 6바이트):  $  ID  'R'  ADDR        CHK  \n
```

- `CHK` = ID 부터 CHK 직전까지의 바이트 합 (1바이트, `calculate_checksum`)
- 주소: `ADDR_MOTOR = 0x01`, `ADDR_LED = 0x02`

### 수신 처리 (`uart_rx_task` → `process_packet`)

`$` ~ `\n` 으로 프레임을 모은 뒤, **아래를 모두 통과한 유효 프레임만** 처리/디버깅합니다.

1. 시작문자 `$` ~ 종료문자 `\n` 구조
2. 슬레이브 ID == `MY_SLAVE_ID`
3. cmd 가 `'W'` / `'R'` 이며 길이가 규칙(W=7, R=6)과 일치
4. 체크섬 일치

규칙을 어긴 프레임/잡음은 조용히 버려지고, 통과한 프레임만 `VALID FRAME` 로그로 출력됩니다.

### 주기 송신 (`uart_tx_task`)

- 2초 간격으로 프레임을 송신하는 디버그용 태스크
- 보낼 값은 뮤텍스로 보호된 공유 구조체(`g_tx_payload`)에 담기며,
  `protocol_set_tx_value(cmd, addr, data)` 로 외부(예: RX 처리 로직)에서 갱신 가능
- 구조: RX 와 TX 가 공유 변수로만 연결되어, 받은 값에 따라 다음 송신값을 바꾸도록 확장 가능

> ⚠️ **주의 (피드백 루프):** TX 가 보내는 프레임은 그 자체로 유효 프레임이므로, TX↔RX 가 루프백되거나 외부 장치가 echo 하면 `process_packet` 의 자동 응답이 무한 재트리거되어 UART 가 폭주할 수 있습니다. 양방향 통신 시 명령/응답 cmd 를 구분(예: 응답은 소문자)하거나 디버그 중에는 자동 응답을 비활성화하세요.

---

## 4. 환경 설정 (menuconfig / MQTT 브로커 주소)

이 프로젝트는 MQTT 브로커(=FastAPI 서버) 주소를 **코드에 하드코딩하지 않고**
Kconfig 설정 항목으로 분리해 둡니다. 그래서 **개인 IP 가 저장소에 노출되지 않으며**,
받는 사람마다 자기 환경의 주소를 넣어 동작시킬 수 있습니다.

### 동작 방식 — IP 가 어디에 저장되나

| 파일 | 역할 | git 추적 |
| ---- | ---- | -------- |
| `main/Kconfig.projbuild` | 설정 **항목의 정의**와 *예시* 기본값(`mqtt://192.168.0.10:1883`) | ✅ 커밋됨 (예시 IP 뿐) |
| `sdkconfig` | `menuconfig` 로 입력한 **실제 IP** 가 저장되는 곳 | ❌ `.gitignore` (커밋 안 됨) |

→ **실제 IP 는 `sdkconfig` 에만 들어가고 이 파일은 커밋되지 않으므로**,
저장소에는 항상 예시 주소만 남습니다.

### `main/Kconfig.projbuild` 작성 예시

```kconfig
menu "ESP32-CAM 애플리케이션 설정"

    config MQTT_BROKER_URI
        string "MQTT 브로커 주소 (URI)"
        default "mqtt://192.168.0.10:1883"   # ← 실제 IP 말고 예시 주소만 둔다
        help
            FastAPI 서버(=MQTT 브로커)가 떠 있는 PC 의 주소입니다.
            예) "mqtt://192.168.0.10:1883"

            실제 주소는 `idf.py menuconfig` →
            "ESP32-CAM 애플리케이션 설정" 메뉴에서 입력하며,
            그 값은 sdkconfig 에 저장됩니다(.gitignore 에 포함되어 커밋되지 않음).

endmenu
```

> ⚠️ `default` 에는 **개인 IP 를 절대 쓰지 마세요.** 예시 사설망 주소(`192.168.x.x`)만 둡니다.
> 실제 주소는 아래 절차대로 menuconfig 에서 입력합니다.

### 처음 받은 사람이 동작시키는 방법

1. 저장소 클론 후 타깃 지정
   ```bash
   idf.py set-target esp32s3
   ```
2. 자기 환경의 MQTT 브로커 주소 입력
   ```bash
   idf.py menuconfig
   ```
   → **ESP32-CAM 애플리케이션 설정 → MQTT 브로커 주소 (URI)** 에서
   `mqtt://<본인 서버 IP>:1883` 으로 수정 후 저장
   (입력값은 `sdkconfig` 에만 저장되어 커밋되지 않음)
3. 빌드 / 플래시
   ```bash
   idf.py build
   idf.py -p <PORT> flash monitor
   ```

> menuconfig 가 번거롭다면, 추적되는 `sdkconfig.defaults` 파일에
> `CONFIG_MQTT_BROKER_URI="mqtt://192.168.0.10:1883"` 한 줄로 예시 기본값을 둘 수도 있습니다.
> (이때도 실제 IP 는 적지 말고 예시만)

---

## 5. 빌드 / 플래시 / 모니터

```bash
idf.py set-target esp32s3
idf.py menuconfig          # MQTT 브로커 주소 등 설정
idf.py build
idf.py -p <PORT> flash monitor
```

UART 프로토콜 디버그는 `monitor` 화면(USB 콘솔)에서 `PROTOCOL` 태그 로그로 확인합니다.

---

## 6. 디렉터리 구조

```
├── CMakeLists.txt
├── main
│   ├── main.c              앱 진입점 / 부팅 시퀀스
│   ├── ble_prov.c/.h       BLE WiFi 프로비저닝
│   ├── wifi_manager.c/.h   WiFi 연결/저장/스캔
│   ├── camera_module.c/.h  OV2640 카메라
│   ├── camera_pins.h       카메라 핀맵
│   ├── http_server.c/.h    MJPEG/정지영상 HTTP 서버 (디버그)
│   ├── mqtt_cam.c/.h       MQTT 카메라 클라이언트 (메인 경로)
│   ├── protocol.c/.h       UART2 사용자 정의 프로토콜
│   └── Kconfig.projbuild   menuconfig 설정 항목
└── README.md               이 문서
```

---

## 7. 참고

- [ESP-IDF Getting Started](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html)
- 업로드 실패 시: 배선 확인 후 `idf.py -p PORT monitor` 로 부팅 로그 확인, menuconfig 에서 다운로드 baud rate 를 낮춰 재시도
