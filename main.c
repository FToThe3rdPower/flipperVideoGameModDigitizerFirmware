#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/uart.h"

// ─── Config ──────────────────────────────────────────────────────────────────
#define ADC_PIN         26          // GP26 = ADC0 — connect detector here
#define ADC_CHANNEL     0
#define UART_ID         uart0
#define UART_TX_PIN     0           // GP0 → Flipper RX (USART1)
#define UART_RX_PIN     1           // GP1 → Flipper TX (USART1)
#define UART_BAUD       921600      // ~1.8 MB/s effective; fits 512 uint16_t @ ~3.5kHz frame rate
#define BUF_SAMPLES     512
#define ADC_CLOCK_HZ    48000000u
#define SAMPLE_RATE_HZ  500000u     // 500 kSPS — RP2040 ADC max practical rate

// RGB LED GPIO pins — change to match your hardware (active-HIGH assumed)
#define LED_R_PIN       2
#define LED_G_PIN       3
#define LED_B_PIN       4

// ─── Frame format (sent to Flipper) ──────────────────────────────────────────
// [ 0xAA 0x55 ] [ n_hi n_lo ] [ sample0_hi sample0_lo ] ... [ crc8 ]
// Header: 2 bytes magic, 2 bytes count (big-endian), N*2 bytes samples, 1 byte CRC8
#define FRAME_MAGIC_0   0xAA
#define FRAME_MAGIC_1   0x55

// ─── Command bytes received from Flipper ─────────────────────────────────────
// Flipper sends one of these after acquiring the serial port, and again any
// time the display state changes.  The RP2040 does NOT transmit until it
// receives CMD_RUN (or CMD_RECORD/CMD_TRIGGERED) — this prevents the UART TX
// line from being driven during the Flipper's serial initialisation, which
// would otherwise race and crash furi_hal_serial_init.
#define CMD_PAUSE       0x00    // stop streaming, blue LED
#define CMD_RUN         0x01    // stream continuously, green LED
#define CMD_TRIGGERED   0x02    // streaming, yellow LED (pulse detected on Flipper)
#define CMD_RECORD      0x03    // streaming, red LED

// ─── Globals ─────────────────────────────────────────────────────────────────
static uint16_t adc_buf[BUF_SAMPLES];
static int      dma_chan;

// ─── LED ─────────────────────────────────────────────────────────────────────
static void led_set(bool r, bool g, bool b) {
    gpio_put(LED_R_PIN, r);
    gpio_put(LED_G_PIN, g);
    gpio_put(LED_B_PIN, b);
}

static void led_apply_cmd(uint8_t cmd) {
    switch(cmd) {
    case CMD_RUN:       led_set(false, true,  false); break; // green:  running
    case CMD_TRIGGERED: led_set(true,  true,  false); break; // yellow: triggered
    case CMD_RECORD:    led_set(true,  false, false); break; // red:    recording
    case CMD_PAUSE:
    default:            led_set(false, false, true);  break; // blue:   paused / idle
    }
}

// ─── CRC / frame ─────────────────────────────────────────────────────────────
static uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

static void send_frame(const uint16_t* samples, uint16_t n) {
    uint16_t n_be = __builtin_bswap16(n);

    uart_putc_raw(UART_ID, FRAME_MAGIC_0);
    uart_putc_raw(UART_ID, FRAME_MAGIC_1);
    uart_putc_raw(UART_ID, (uint8_t)(n >> 8));
    uart_putc_raw(UART_ID, (uint8_t)(n & 0xFF));

    // Samples are 12-bit, packed as big-endian uint16_t
    uint8_t crc = 0xFF;
    for(uint16_t i = 0; i < n; i++) {
        uint8_t hi = (uint8_t)(samples[i] >> 8);
        uint8_t lo = (uint8_t)(samples[i] & 0xFF);
        uart_putc_raw(UART_ID, hi);
        uart_putc_raw(UART_ID, lo);
        crc ^= hi; for(int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        crc ^= lo; for(int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    uart_putc_raw(UART_ID, crc);
    (void)n_be;
}

int main(void) {
    // ── LED ──────────────────────────────────────────────────────────────────
    gpio_init(LED_R_PIN); gpio_set_dir(LED_R_PIN, GPIO_OUT);
    gpio_init(LED_G_PIN); gpio_set_dir(LED_G_PIN, GPIO_OUT);
    gpio_init(LED_B_PIN); gpio_set_dir(LED_B_PIN, GPIO_OUT);

    // ── UART ─────────────────────────────────────────────────────────────────
    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);

    // ── ADC ──────────────────────────────────────────────────────────────────
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_CHANNEL);
    adc_fifo_setup(
        true,   // write to FIFO
        true,   // enable DMA request
        1,      // DREQ when ≥1 sample available
        false,  // no error bit
        false   // keep full 12-bit (no 8-bit shift)
    );
    // ADC clock divider: 48 MHz / (div+1) = sample rate
    // div = (48MHz / rate) - 1
    uint32_t div = (ADC_CLOCK_HZ / SAMPLE_RATE_HZ) - 1;
    adc_set_clkdiv((float)div);

    // ── DMA ──────────────────────────────────────────────────────────────────
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);     // ADC FIFO: fixed address
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);

    // ── Wait for Flipper handshake before transmitting ────────────────────────
    // Boot into pause state (blue LED, TX silent) so the Flipper can call
    // furi_hal_serial_init without our UART output racing it.
    uint8_t state = CMD_PAUSE;
    led_apply_cmd(state);

    // ── Sample loop ───────────────────────────────────────────────────────────
    while(true) {
        // Drain all pending commands from Flipper — last one wins
        while(uart_is_readable(UART_ID)) {
            uint8_t cmd = uart_getc(UART_ID);
            if(cmd == CMD_RUN || cmd == CMD_TRIGGERED ||
               cmd == CMD_RECORD || cmd == CMD_PAUSE) {
                state = cmd;
                led_apply_cmd(state);
            }
        }

        if(state == CMD_PAUSE) {
            sleep_us(200);
            continue;
        }

        // DMA one buffer-full of samples from ADC FIFO
        dma_channel_configure(dma_chan, &cfg,
            adc_buf,                // destination
            &adc_hw->fifo,          // source: ADC FIFO
            BUF_SAMPLES,
            false                   // don't start yet
        );
        adc_run(true);
        dma_channel_start(dma_chan);
        dma_channel_wait_for_finish_blocking(dma_chan);
        adc_run(false);
        adc_fifo_drain();

        // Scale 12-bit ADC values to mV (3300 mV full-scale for 3.3 V VCC)
        for(int i = 0; i < BUF_SAMPLES; i++)
            adc_buf[i] = (uint16_t)((uint32_t)adc_buf[i] * 3300 / 4095);

        send_frame(adc_buf, BUF_SAMPLES);
    }
}
