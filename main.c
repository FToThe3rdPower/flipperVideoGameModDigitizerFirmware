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

// ─── Frame format (sent to Flipper) ──────────────────────────────────────────
// [ 0xAA 0x55 ] [ n_hi n_lo ] [ sample0_hi sample0_lo ] ... [ crc8 ]
// Header: 2 bytes magic, 2 bytes count (big-endian), N*2 bytes samples, 1 byte CRC8
#define FRAME_MAGIC_0   0xAA
#define FRAME_MAGIC_1   0x55

// ─── Globals ─────────────────────────────────────────────────────────────────
static uint16_t adc_buf[BUF_SAMPLES];
static int      dma_chan;

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

    // ── Sample loop ───────────────────────────────────────────────────────────
    while(true) {
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

        // Scale 12-bit ADC values to mV (3300 mV / 4096 counts)
        for(int i = 0; i < BUF_SAMPLES; i++)
            adc_buf[i] = (uint16_t)((uint32_t)adc_buf[i] * 3300 / 4095);

        send_frame(adc_buf, BUF_SAMPLES);
    }
}
