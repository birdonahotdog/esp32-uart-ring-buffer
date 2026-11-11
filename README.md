# ESP32 UART Echo with a Custom Ring Buffer

A UART echo application for the ESP32 built on **ESP-IDF** and **FreeRTOS**. Characters typed in a serial terminal are received by the UART driver, passed through a custom single-producer / single-consumer (SPSC) ring buffer, and echoed back. Pressing Enter moves the cursor to the start of the next line.

The echo itself is simple on purpose. The project is about the plumbing: a clean producer/consumer split, backpressure instead of data loss, and proper handling of UART error events.

## Architecture

```
keyboard -> UART0 driver -> uart_rx_task --push--> ring buffer
                                                       |
                                                      pop
                                                       v
terminal <- UART0 driver <------------------------ echo_task
```

| Task | Role | Priority |
|------|------|----------|
| `uart_rx_task` | **Producer.** Sleeps on the UART driver event queue and moves received bytes into the ring buffer. | 12 |
| `echo_task` | **Consumer.** Pops bytes, translates Enter (`\r`) into `\r\n`, and writes the result back to the UART. | 11 |

The ring buffer is SPSC, so exactly one task pushes and exactly one task pops. That is what makes it safe to use without a mutex.

## Design highlights

- **Backpressure, not data loss.** Bytes are only read from the driver when the ring buffer has room. Anything that does not fit stays in the driver's larger RX buffer until the consumer frees space.
- **Event-driven, no polling.** The producer blocks on the driver's event queue, and the consumer blocks on a task notification. Both tasks are idle when there is no input.
- **Task notifications as a counting semaphore.** `xTaskNotifyGive()` / `ulTaskNotifyTake()` is a lightweight way to wake the consumer, and a wake-up sent while it is busy is not lost.
- **Robust buffered-length handling.** The producer calls `uart_get_buffered_data_len()` instead of trusting `event.size`, because several events can be queued for data that has already been read.
- **Error handling for UART events.** FIFO overflow and driver buffer full flush the input and reset the event queue. Break, parity, and framing errors are logged.
- **Batched output.** The echo task collects output into a local buffer and issues one `uart_write_bytes()` call per burst instead of one per byte.
- **No per-keystroke logging**, so echoed characters are never interleaved with log output.
- **Safe startup order.** The ring buffer is initialized first and the consumer task is created before the producer, so the consumer handle is valid before anything tries to wake it.

## Hardware

- Any ESP32 development board with a USB-serial bridge (UART0 is the console UART)
- USB cable

No wiring is needed. The default UART0 pins (GPIO1 TX / GPIO3 RX on the classic ESP32) are already connected to the bridge.

## Software requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) (v5.x recommended)
- A serial terminal (the one built into `idf.py monitor` works)

## Build, flash, and run

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with your serial port (for example `/dev/ttyUSB0` or `COM3`). Type in the monitor and each character is echoed back. Press Enter to start a new line. Exit the monitor with `Ctrl+]`.

## Configuration

Key settings are `#define`s at the top of the source file.

| Macro | Default | Description |
|-------|---------|-------------|
| `UART_BAUD_RATE` | `115200` | UART baud rate |
| `UART_RX_BUF_SIZE` | `1024` | Base size for the driver RX buffer (the driver is installed with 2x this value) |
| `UART_EVENT_QUEUE_LEN` | `20` | Driver events queued before new ones are dropped |
| `ENTER_CHAR` | `'\r'` | Byte treated as Enter. Change to `'\n'` if your terminal sends LF only |
| `RX_CHUNK_SIZE` | `32` | Bytes moved from the driver to the ring buffer per read |
| `ECHO_OUT_BUF_SIZE` | `64` | Echo staging buffer (each input byte expands to at most 2 output bytes) |

The ring buffer capacity (`RING_BUFFER_CAPACITY`) is defined in `ring_buffer.h`. One slot is always kept empty, so the usable size is `RING_BUFFER_CAPACITY - 1`.

## Project structure

```
.
├── CMakeLists.txt
└── main
    ├── CMakeLists.txt
    ├── main.c            # UART setup, producer and consumer tasks
    ├── ring_buffer.c     # SPSC ring buffer implementation
    └── ring_buffer.h     # Ring buffer API
```

Adjust the file names above to match your repository layout.

## Ring buffer API used

| Function | Purpose |
|----------|---------|
| `ring_buffer_init()` | Reset the buffer to empty |
| `ring_buffer_push()` | Add one byte (producer only) |
| `ring_buffer_pop()` | Remove one byte (consumer only) |
| `ring_buffer_available()` | Number of bytes currently stored |
| `ring_buffer_is_empty()` | Check whether the buffer is empty |

## Possible improvements

- Handle backspace so the terminal line can be edited
- Add a line-based command parser on top of the consumer
- Replace the `vTaskDelay(1)` retry when the ring is full with a notification from the consumer
- Add unit tests for the ring buffer on the host (for example with Unity)

## License

MIT. See [LICENSE](LICENSE).
