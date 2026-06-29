#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "protocol.h"

static const char *TAG = "PROTOCOL";

static int32_t g_device_registers[16] = {0};

/**
 * @param buffer '$'를 제외한 데이터(ID부터 Checksum 앞까지)
 * @param length 체크섬 계산에 포함될 바이트 수
 */

 uint8_t calculate_checksum(const uint8_t * buffer,uint8_t lenght){

    uint8_t sum =0;
    for (uint8_t i = 0; i < lenght; i++){
        sum += buffer[i];
    }

    return sum;
    
 }

 /**
 * @brief Master에게 응답 프레임 생성 후 전송 (10바이트)
 */

void send_response(uint8_t slave_id, uint8_t cmd, uint8_t addr, uint8_t data){
    uint8_t response[FRAME_LEN_W];
    uint8_t checksum;

    response[FRAME_IDX_START] = '$';
    response[FRAME_IDX_SlaveID] = slave_id;
    response[FRAME_IDX_CMD] = cmd;
    response[FRAME_IDX_ADDR] = addr;
    response[FRAME_IDX_W_DATA] = data;

    checksum = calculate_checksum(&response[1],4);

    response[FRAME_IDX_W_CHECKSUM] = checksum;
    response[FRAME_IDX_W_END] = '\n';

    uart_write_bytes(PROTOCOL_UART_NUM,(const char *)response,sizeof(response));
 }

/**
 * @brief 수신 패킷 파싱/처리 (Slave 로직)
 * @param buffer '$'...'\n' 포함 전체 패킷
 * @param length 전체 길이
 */
 void process_packet(const uint8_t *buffer, uint8_t length){

    if(length < FRAME_LEN_R){
        return;
    }

    uint8_t slave_id = buffer[FRAME_IDX_SlaveID];

    if(slave_id != MY_SLAVE_ID){
        return;
    }

    uint8_t cmd = buffer[FRAME_IDX_CMD];
    uint8_t addr = buffer[FRAME_IDX_ADDR];
    uint8_t received_checksum;
    uint8_t calculated_checksum;
    uint8_t data_len_for_checksum;

    if(cmd == 'W'){
        if (length != FRAME_LEN_W) {
            return;
        }
        received_checksum = buffer[FRAME_IDX_W_CHECKSUM];
        data_len_for_checksum = 4;
    }else if(cmd == 'R'){
        if(length != FRAME_LEN_R){
            return;
        }
        received_checksum = buffer[FRAME_IDX_R_CHECKSUM];
        data_len_for_checksum = 3;
    }else{
        return;
    }

    calculated_checksum = calculate_checksum(&buffer[1],data_len_for_checksum);

    if(received_checksum != calculated_checksum){
          ESP_LOGW(TAG, "checksum error: rx=0x%02X calc=0x%02X", received_checksum, calculated_checksum);
          return;
    }

    uint8_t data_8bit =0;

    if(cmd == 'W'){
        data_8bit = buffer [FRAME_IDX_W_DATA];

        if(addr == 1){}
        else if(addr == 2){}
        else if(addr == 3){}
        
        if (addr < 16) {
            g_device_registers[addr] = data_8bit; // 필요 시 저장
        }

        // 'W' 응답
        send_response(slave_id, cmd, addr, data_8bit);

    }else{
        if(addr < 16){
            data_8bit = g_device_registers[addr];
        }
        send_response(slave_id,cmd,addr,data_8bit);
    }
 }

 static void uart_rx_task(void *arg){
    uint8_t byte;
    uint8_t frame[PROTOCOL_BUFFER_SIZE];
    uint8_t idx =0;
    bool in_frame = false;

    while (1){
        int len = uart_read_bytes(PROTOCOL_UART_NUM,&byte,1,portMAX_DELAY);
        if(len <=0) continue;

        if(byte == '$'){
            in_frame = true;
            idx=0;
            frame[idx++] = byte;
        }else if(in_frame){
            if(idx < PROTOCOL_BUFFER_SIZE){
                frame[idx++] = byte;
                if(byte == '\n'){
                    process_packet(frame,idx);
                    in_frame=false;
                    idx=0;
                }
            }else{
                in_frame=false;
                idx=0;
            }
        }
    }
 }

 void protocol_uart_init(void){

    const uart_config_t uart_config={
        .baud_rate = PROTOCOL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(
        uart_driver_install(PROTOCOL_UART_NUM,256,0,0,NULL,0)
    );
    
    ESP_ERROR_CHECK(uart_param_config(PROTOCOL_UART_NUM, &uart_config));
    
    ESP_ERROR_CHECK(
        uart_set_pin(
            PROTOCOL_UART_NUM,
            PROTOCOL_UART_TXD,
            PROTOCOL_UART_RXD,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    xTaskCreate(uart_rx_task,"uart_rx_task",4096,NULL,10,NULL);
    
    ESP_LOGI(TAG, "UART%d init done (TX=%d RX=%d, %d bps)",
             PROTOCOL_UART_NUM, PROTOCOL_UART_TXD, PROTOCOL_UART_RXD,
             PROTOCOL_UART_BAUD);


 }