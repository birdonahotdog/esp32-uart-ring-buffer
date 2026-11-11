#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "ring_buffer.h"

#define UART_PORT_NUM        UART_NUM_0
#define UART_BAUD_RATE       (115200)

#define UART_RX_BUF_SIZE     (1024)
#define UART_TX_BUF_SIZE     (0)

/* Number of events the driver can queue before new events are dropped. */
#define UART_EVENT_QUEUE_LEN (20)

#define UART_TXD_PIN         (UART_PIN_NO_CHANGE)
#define UART_RXD_PIN         (UART_PIN_NO_CHANGE)
#define UART_RTS_PIN         (UART_PIN_NO_CHANGE) /* Hardware flow control unused */
#define UART_CTS_PIN         (UART_PIN_NO_CHANGE)

#define ENTER_CHAR           ('\r')
#define NEWLINE_SEQ          "\r\n"
#define RX_CHUNK_SIZE        (32)
#define ECHO_OUT_BUF_SIZE    (64) /* Output staging buffer for the echo task. */

#define UART_RX_TASK_STACK   (4096)
#define UART_RX_TASK_PRIO    (12)
#define ECHO_TASK_STACK      (4096)
#define ECHO_TASK_PRIO       (11)

static const char *TAG = "UART_RING";

static QueueHandle_t uart_event_queue;
static ring_buffer_t rx_ring; /* Ring buffer shared between uart_rx_task and echo_task */
static TaskHandle_t echo_task_handle;
static void wake_echo_task(void)
{
    if (echo_task_handle != NULL) {
        xTaskNotifyGive(echo_task_handle);
    }
}

static void move_driver_data_to_ring(size_t total)
{
    uint8_t chunk[RX_CHUNK_SIZE];

    while (total > 0) {
        size_t free_slots = (RING_BUFFER_CAPACITY - 1U) - ring_buffer_available(&rx_ring);

        if (free_slots == 0) {
            wake_echo_task();
            vTaskDelay(1);
            continue;
        }

        size_t to_read = total;
        if (to_read > free_slots) {
            to_read = free_slots;
        }
        if (to_read > sizeof(chunk)) {
            to_read = sizeof(chunk);
        }

        /* Zero timeout: the driver already reported these bytes as buffered. */
        int n = uart_read_bytes(UART_PORT_NUM, chunk, to_read, 0);
        if (n <= 0) {
            break; /* Error, or the data was flushed in the meantime */
        }

        for (int i = 0; i < n; i++) {
            if (ring_buffer_push(&rx_ring, chunk[i]) != RING_BUFFER_OK) {
                /* Cannot happen given the free-space check above. */
                ESP_LOGE(TAG, "ring buffer push failed");
            }
        }

        wake_echo_task();
        total -= (size_t)n;
    }
}

static void uart_rx_task(void *pvParameters)
{
    uart_event_t event;

    for (;;) {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {

        case UART_DATA: {
            size_t buffered = 0;
            uart_get_buffered_data_len(UART_PORT_NUM, &buffered);
            move_driver_data_to_ring(buffered);
            break;
        }

        case UART_FIFO_OVF:
            ESP_LOGW(TAG, "hw fifo overflow");
            uart_flush_input(UART_PORT_NUM);
            xQueueReset(uart_event_queue);
            break;

        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "driver rx buffer full");
            uart_flush_input(UART_PORT_NUM);
            xQueueReset(uart_event_queue);
            break;

        case UART_BREAK:
            ESP_LOGW(TAG, "uart rx break");
            break;

        case UART_PARITY_ERR:
            ESP_LOGE(TAG, "uart parity error");
            break;

        case UART_FRAME_ERR:
            ESP_LOGE(TAG, "uart frame error");
            break;

        default:
            break;
        }
    }
}

static void echo_task(void *pvParameters)
{
    uint8_t out[ECHO_OUT_BUF_SIZE];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (!ring_buffer_is_empty(&rx_ring)) {
            size_t len = 0;
            uint8_t byte;

            while (len + 2 <= sizeof(out) &&
                   ring_buffer_pop(&rx_ring, &byte) == RING_BUFFER_OK) {
                if (byte == ENTER_CHAR) {
                    memcpy(&out[len], NEWLINE_SEQ, strlen(NEWLINE_SEQ));
                    len += strlen(NEWLINE_SEQ);
                } else {
                    out[len++] = byte;
                }
            }

            if (len > 0) {
                uart_write_bytes(UART_PORT_NUM, (const char *)out, len);
            }
        }
    }
}

void app_main(void)
{
    const uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ring_buffer_init(&rx_ring);
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_RX_BUF_SIZE * 2, UART_TX_BUF_SIZE, UART_EVENT_QUEUE_LEN, &uart_event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD_PIN, UART_RXD_PIN, UART_RTS_PIN, UART_CTS_PIN));

    if (xTaskCreate(echo_task, "echo_task", ECHO_TASK_STACK, NULL, ECHO_TASK_PRIO, &echo_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create echo task");
        return;
    }

    if (xTaskCreate(uart_rx_task, "uart_rx_task", UART_RX_TASK_STACK, NULL, UART_RX_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
    }
}
