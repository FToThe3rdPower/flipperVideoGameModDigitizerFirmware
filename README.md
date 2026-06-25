# vgm-scope

RP2040 firmware for the **Flipper Zero Video Game Module** that captures an analog signal at 500 kSPS and streams sample frames over UART to the Flipper, where a companion oscilloscope app renders the waveform in real time.

```
[Analog signal] → GP26 (ADC0) → DMA → Frame → GP0 (UART TX) → Flipper RX
```

---

## Hardware

| Signal | VGM Pin | Flipper Pin |
|--------|---------|-------------|
| UART TX | GP0 | USART1 RX |
| UART RX | GP1 | USART1 TX |
| Analog input | GP26 (ADC0) | — (probe wire) |
| GND | GND | GND |

The input on GP26 is a standard 0–3.3 V ADC input. **Do not exceed 3.3 V** — there is no input protection on the RP2040 ADC.

---

## Wire Protocol

Each frame sent over UART is:

```
[ 0xAA ] [ 0x55 ] [ n_hi ] [ n_lo ] [ s0_hi ] [ s0_lo ] ... [ crc8 ]
```

| Field | Size | Description |
|-------|------|-------------|
| Magic | 2 bytes | `0xAA 0x55` — frame start marker |
| Count | 2 bytes | Number of samples, big-endian (`n = 512`) |
| Samples | `n × 2` bytes | 12-bit ADC reading scaled to mV (0–3300), big-endian uint16 |
| CRC8 | 1 byte | CRC-8/MAXIM over header + sample bytes, initialized to `0xFF`, poly `0x31` |

Total frame size: **1029 bytes** at 512 samples/frame.

**Throughput:** 921600 baud → ~90 KB/s raw → ~3.4 kframes/s theoretical max. At 500 kSPS the firmware produces ~976 frames/s, well within budget.

---

## Parameters

Defined at the top of [main.c](main.c):

| Constant | Default | Description |
|----------|---------|-------------|
| `SAMPLE_RATE_HZ` | 500 000 | ADC sample rate (500 kSPS is the RP2040 practical max) |
| `BUF_SAMPLES` | 512 | Samples per frame |
| `UART_BAUD` | 921 600 | UART baud rate |
| `ADC_PIN` | 26 | GPIO pin for analog input |
| `UART_TX_PIN` | 0 | GPIO pin for UART TX |

---

## Building

**Prerequisites**

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) installed and `PICO_SDK_PATH` set
- CMake ≥ 3.13
- `arm-none-eabi-gcc` toolchain

```sh
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Outputs land in `build/`:

| File | Use |
|------|-----|
| `vgm_scope.uf2` | Drag-and-drop flash via BOOTSEL |
| `vgm_scope.hex` | OpenOCD / picoprobe flashing |
| `vgm_scope.bin` | Raw binary |

---

## Flashing

1. Hold **BOOTSEL** on the VGM while connecting USB — it mounts as a mass-storage drive.
2. Copy `build/vgm_scope.uf2` onto the drive.
3. The board reboots and begins streaming immediately.

---

## Companion App

The Flipper-side oscilloscope app lives in a separate repository. It reads frames from USART1, validates the CRC, and renders the waveform on the Flipper's 128×64 display.
