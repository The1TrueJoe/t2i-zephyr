# T2i hardware map (STM32F205VET6)

Reverse-engineered from the stock RTI firmware: live GPIO config (MODER/AFR/ODR)
read over SWD from running RTI + the STM32F205 alternate-function table + firmware
decompilation. **Safety: never drive the AF or analog pins as GPIO — they are live
peripherals and could contend with another chip. For the timer/boost PWMs, copy
RTI's exact config; a backlight boost at the wrong duty can over-volt the LEDs.**

## Peripherals — alternate function (DO NOT drive as GPIO)

| Pin(s) | AF | Function |
|---|---|---|
| PA11 / PA12 | 10 | USB OTG-FS D− / D+ |
| PA13 / PA14 | 0 | SWDIO / SWCLK |
| PB3 / PB4 / PB5 | 6 | SPI3 SCK / MISO / MOSI → **SPI flash** (S25FL256S) |
| PB10 / PB11 | 4 | I2C2 SCL / SDA → **ST 3-axis accelerometer** (LIS3DH/LIS302DL family), configured for wake-on-motion (CTRL_REG1-3 @0x20-0x22, INT1 motion @0x30/0x32/0x33) — NOT touch |
| PB15 | 5 | SPI2_MOSI (purpose TBD) |
| PC10 / PC11 | 7 | USART3 TX / RX (radio-module comms / debug) |
| PD4 / PD5 / PD7 / PD13 | 12 | FSMC: NOE(RD) / NWE(WR) / NE1(CS) / A18(RS/DC) — **LCD** |
| PD14 / PD15 / PD0 / PD1 | 12 | FSMC D0 / D1 / D2 / D3 — LCD data |
| PE7 / PE8 / PE9 / PE10 | 12 | FSMC D4 / D5 / D6 / D7 — LCD data |

## Analog (DO NOT drive)

| Pin | Function |
|---|---|
| PA4 | DAC_OUT1 — likely **speaker** audio (or ADC) |
| PA7 | ADC — sensor (light/battery?) |
| PB1 | ADC — sensor |

## Timer PWMs (match RTI config exactly)

| Pin | Timer | Function |
|---|---|---|
| PC8 | TIM8_CH3 | **Keypad backlight** ✅ confirmed (ARR=300, CCR3~79, active-low) |
| PA1 | TIM2_CH2 | **LCD backlight** ✅ confirmed — PWM-dimming input to a backlight driver IC. PSC=9, ARR=3000 (~4 kHz), PWM mode 2, active-low. CCR2 = brightness: 60≈off (RTI idle), 1500=50%, 3000=full. Driver regulates LED current → raising duty only brightens, cannot over-volt. |
| PA0 | TIM5_CH1 | PWM — purpose TBD (driving it lit nothing) |

## LCD (screen)

**Himax HX8347 controller** on the FSMC (8-bit parallel). `lcd_read(0)` returns
0x47 = the HX8347 chip-ID register (gamma regs 0x40–0x5D, window regs 0x02–0x09,
0x22 GRAM-write all match HX8347). Command @ `0x60000000`, data @ `0x60040000`.
**PD6 = LCD reset** (GPIO out). Init + draw reversed from RTI (`src/main.c`) and
**verified on hardware** (color bars). Backlight = TIM2_CH2/PA1 (see above).
Ref libraries for the HX8347 init/gamma: MCUFRIEND_kbv, Adafruit/LCDWIKI HX8347
(no mainline Zephyr driver — our init is a hand-port).

## Keypad (8×7 matrix)

| Role | Pins |
|---|---|
| Columns (drive, output) | **PC0 – PC7** |
| Rows (sense, input) | **PE0, PE1, PE2, PE12, PE13, PE14, PE15** |

## Confirmed single GPIO

| Pin | Function |
|---|---|
| PA15 (out) | SPI3 flash chip-select |
| PD6 (out) | LCD reset |

## Control / chip-select GPIO outputs (reversed)

All MCU-driven, no bus contention, none are power/boost pins → safe to drive. Roles
reversed from RTI (some inferred):

| Pin | Role |
|---|---|
| PB6 | SPI-flash driver control line (used with the flash CS PA15) |
| PB14 | Write-only **SPI2 chip-select** — the CC1150 433 MHz RF module (**desoldered** on the bench unit → inert) |
| PB0 | Tied to a PWM/frequency routine alongside PB14 — RF modulation / tone (radio desoldered → inert) |
| PC13 | Peripheral **chip-select** (pulsed around a bus transfer) |
| PC12 | Control/enable line (peripheral) |
| PA6, PA10 | Control/enable lines (exact function not individually pinned; safe) |

The ZigBee (EM250) / RF (CC1150) radio was **desoldered** on the bench unit, so its
control pins (PB14/PB0, and USART3 if used for the radio) are inert here.
