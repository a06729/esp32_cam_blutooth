#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// ===== write 명령 예시 (데이터 1바이트 전송) =====
// $   SlaveId  W    주소  DATA  checkSum  \n
//0x24 0x01    0x57 0x01  0x..  0x..      0x0A
//
// ===== read 명령 =====
// $   SlaveId  R    주소  checkSum  \n
//0x24 0x01    0x52 0x05  0x..      0x0A

// --- 프로토콜 정의 ---
#define MY_SLAVE_ID             0x01   // 이 장치의 Slave ID
#define PROTOCOL_BUFFER_SIZE    16     // 수신 패킷 버퍼 크기 ('$'...'\n' 포함)

// 프레임 공통 인덱스
#define FRAME_IDX_START         0      // 시작 문자 '$'
#define FRAME_IDX_SlaveID       1      // Slave ID
#define FRAME_IDX_CMD           2      // 명령(W/R)
#define FRAME_IDX_ADDR          3      // 주소 (모터1/2 등 분기)

// W 명령: 데이터 1바이트(8비트)
#define FRAME_IDX_W_DATA        4      // 8비트 데이터
#define FRAME_IDX_W_CHECKSUM    5      // 체크섬
#define FRAME_IDX_W_END         6      // 종료 문자 '\n'

// R 명령
#define FRAME_IDX_R_CHECKSUM    4
#define FRAME_IDX_R_END         5

// 프레임 길이
#define FRAME_LEN_W             7      // $ ID W Addr DATA Chk \n
#define FRAME_LEN_R             6      // $ ID R Addr Chk \n

// --- UART 핀/설정 (보드에 맞게 수정) ---
#define PROTOCOL_UART_NUM       UART_NUM_2
#define PROTOCOL_UART_TXD       1     // ESP32-CAM은 사용 가능 핀 확인 필요
#define PROTOCOL_UART_RXD       2
#define PROTOCOL_UART_BAUD      115200

uint8_t calculate_checksum(const uint8_t *buffer, uint8_t length);
void    send_response(uint8_t slave_id, uint8_t cmd, uint8_t addr, uint8_t data);
void    process_packet(const uint8_t *buffer, uint8_t length);

// UART 초기화 + 수신 태스크 시작
void    protocol_uart_init(void);

#endif // PROTOCOL_H