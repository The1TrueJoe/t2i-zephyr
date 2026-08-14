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
| PB10 / PB11 | 4 | I2C2 SCL / SDA (touch/sensor?) |
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
| PC8 | TIM8_CH3 | **Keypad backlight** ✅ confirmed (ARR=300, CCR3=271, active-low) |
| PA1 | TIM2_CH2 | **LCD backlight** (likely; ~20 kHz boost, active-low) — *unconfirmed* |
| PA0 | TIM5_CH1 | PWM — purpose TBD (driving it lit nothing) |

## LCD (screen)

ILI-style **0x47 controller** on the FSMC (8-bit parallel). `lcd_read(0)` returns
0x47 (NOT ILI9341). Command @ `0x60000000`, data @ `0x60040000`. **PD6 = LCD reset**
(GPIO out). Init + draw reversed from RTI (`src/main.c`). Backlight = TIM2_CH2/PA1 (TBD).

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

## Still-unverified GPIO outputs (reverse before relying on them)

`PA6, PA10, PB0, PB6, PB14, PC12, PC13` — control/enable lines (power rails, LCD
backlight enable, boost enables, radio control). NOTE: the ZigBee/RF radio module
was **desoldered** on the bench unit, so radio-control pins are inert here.
Electrically safe to drive as GPIO (MCU-driven, no contention), but function TBD.
