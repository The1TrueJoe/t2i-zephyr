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
| PB15 | 5 | SPI2_MOSI — **IR envelope** (DMA1 Stream4 clocks mark/space bytes) |
| PC10 / PC11 | 7 | USART3 TX / RX (radio-module comms / debug) |
| PD4 / PD5 / PD7 / PD13 | 12 | FSMC: NOE(RD) / NWE(WR) / NE1(CS) / A18(RS/DC) — **LCD** |
| PD14 / PD15 / PD0 / PD1 | 12 | FSMC D0 / D1 / D2 / D3 — LCD data |
| PE7 / PE8 / PE9 / PE10 | 12 | FSMC D4 / D5 / D6 / D7 — LCD data |

## Analog (DO NOT drive)

| Pin | Function |
|---|---|
| PA7 | ADC — sensor (light/battery?) |
| PB1 | ADC — sensor |

(PA4 was previously guessed as DAC/speaker — it is actually a **touchscreen**
electrode, see below. The beeper is **PB7 = TIM4_CH2**, a plain 400/500 Hz
square wave gated on for 20 ms per key click. There is no DAC and no sample
playback anywhere in stock.)

## Touchscreen — 4-wire resistive (reversed)

Read directly by the STM32 (no touch controller IC). Driver = RTI `FUN_08015928`:
it reconfigures the four electrode pins between push-pull drive and analog, then
reads the perpendicular plane via **ADC2**.

| Pin | Role |
|---|---|
| PA3 | X− (ADC2 IN3) |
| PA4 | X+ (ADC2 IN4) |
| PA5 | Y+ (ADC2 IN5) |
| PA6 | Y− (ADC2 IN6) |

**The plates are PA3+PA4 and PA5+PA6** — pairing them across plates (PA3+PA5)
drives a diagonal and yields a squashed, useless range. Confirmed from the stock
`GPIO_Init` calls:

| | `FUN_08015928` (X) | `FUN_08015a0c` (Y) |
|---|---|---|
| driven high | PA4 | PA5 |
| driven low | PA3 (out, pull-down) | PA6 (out, pull-down) |
| left analog | PA6 | PA3 |
| ADC2 channel sampled | 5 (PA5) | 4 (PA4) |

Read sequence: energize one axis (drive its two electrodes as GPIO high/low),
set the other two to analog, ADC2-sample the perpendicular wire to get one
coordinate; repeat swapped for the other coordinate. Touch/no-touch: drive X−
low with a pull-up on Y+ and sense Y+ (cross-plate, so a press shorts it low).

## Timer PWMs (match RTI config exactly)

| Pin | Timer | Function |
|---|---|---|
| PC8 | TIM8_CH3 | **Keypad backlight** ✅ confirmed. Stock curve (`FUN_0801ab1e`): period 300, pulse = `pct*2 + 75`, active-low — so its usable range floors around 25% and tops out ~92%. 0 and 100 are special-cased to a static GPIO level with the timer stopped, exactly as the LCD backlight is. |
| PA1 | TIM2_CH2 | **LCD backlight** ✅ confirmed — PWM-dimming input to a backlight driver IC, PWM mode 2, active-low. CCR2/ARR = brightness (driver regulates LED current → duty only changes brightness, cannot over-volt). **Stock runs it at 2 kHz** — confirmed from `FUN_0801a9fa`, which computes prescaler `(SysClk/2)/6000000-1 = 9` (6 MHz timer), period 3000, pulse `pct*30`. An earlier note here claimed 2 kHz was merely RTI's idle state and that we should use 30 kHz; that was wrong. 30 kHz makes low duty cycles unusable (1% = 333 ns, below the driver's response) and appears to drop the converter into a shutdown that only a power cycle clears. |
| PA0 | TIM5_CH1 | PWM — purpose TBD (driving it lit nothing) |

## LCD sleep / wake (verified in decomp)

> **See [docs/BACKLIGHT.md](docs/BACKLIGHT.md)** for the full sleep/backlight
> investigation, including four approaches already ruled out on hardware and the
> one open question (why stock's backlight recovers and ours does not).


Stock gets a genuinely black screen in sleep by powering the **panel** down, not
by killing the backlight. From the LCD control switch `FUN_0800fb2c`, cases 6
(off) and 5 (on), both gated on the panel ID reading `0x47`:

| off (case 6) | on (case 5) |
|---|---|
| `0x28=0x38`, wait 40 ms | `0x19=0x01`, wait 6 ms (oscillator on) |
| `0x28=0x20`, wait 5 ms | `0x1F=0xAC/0xA4/0xB4/0xF4/0xD4`, 5 ms each |
| `0x1F=0xA9`, wait 5 ms (power control down) | `0x28=0x38`, wait 5 ms |
| `0x19=0x00` (oscillator off) | `0x28=0x3C` (display on) |

Writing only `0x28=0x38` is **not** enough — the screen stays faintly viewable.
The `0x1F` power-control walk and the oscillator stop are what make it black.

Powering the panel down loses GRAM, so the app must force a full repaint on wake.

Doing it this way also avoids the backlight entirely, which matters: driving the
LCD backlight to a static 0 blanks the screen but this driver IC does not recover
from it without a power cycle. Stock never hits that because stock never blanks
via the backlight.

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

## Front-panel LEDs (reversed, not yet implemented)

`FunLightSetColor` (`FUN_0801be26`) drives six channels at registers `0x10`–`0x15`
through `FUN_0801bf50(reg, val)`. Values are **inverted**: `0x20` = off, `0x01` =
brightest, computed as `0x20 - pct*0x1F/100`. Registers `0x10`/`0x13`, `0x11`/`0x14`
and `0x12`/`0x15` are written in pairs, suggesting three colours × two banks.
`FunLightSetBrightness` (`FUN_0801becc`) stores the level then re-applies the colour.

## ZigBee / EM250 (reversed, not yet implemented)

Host link is **USART3 on PC10/PC11**. The protocol is **RTI's own**, not EZSP:
strings are `ZbxRx`, `ZBX_STATS_GOING_NORMAL_FAIL`, `ZbxRx stack stat ind`,
`ZIGBEE!ROAM` — with no `EZSP`/`ASH`/`Ember` strings anywhere in the image.
State machine around `FUN_080194f4`; `ZBXport CS` is an RTOS **critical-section**
name, not a chip select.

The stock firmware shows **no ability to reflash the EM250** — the only update
strings in the image concern the T2i itself. Putting custom firmware on the radio
would mean the Ember standalone bootloader (if present) or the SIF port.

## Control / chip-select GPIO outputs (reversed)

All MCU-driven, no bus contention, none are power/boost pins → safe to drive. Roles
reversed from RTI (some inferred):

| Pin | Role |
|---|---|
| PB6 | SPI-flash driver control line (used with the flash CS PA15) |
| PB14 | **RF path enable**, active high — the CC1150 433 MHz RF module (**desoldered** on the bench unit). Must be held **LOW** to select the IR path. |
| PB0 | **IR carrier** — TIM3_CH3 (AF2), 50% duty at the code's carrier frequency, idles LOW = off. Earlier notes calling this "RF modulation / tone" were wrong; see [docs/IR-BUZZER.md](docs/IR-BUZZER.md). |
| PC13 | Peripheral **chip-select** (pulsed around a bus transfer) |
| PC12 | **Shared power-rail enable — drive HIGH and leave it there.** Must be high or *both* backlights stay dark even though TIM2/TIM8 run correctly and the panel answers on the FSMC bus. Stock drives it in `FUN_08021354` (output PP + pull-up). It latches across warm resets, so a board that lost this pin only goes dark after a true power removal — which makes it look like a firmware regression. **Do not drop it to save power:** it feeds more than the backlight. Taking it low lights the front-panel **low-battery indicator** and removes the panel's logic supply, so the HX8347 loses its initialisation and the image cannot be restored by re-enabling the backlight alone (it would need a full panel re-init). Set once in `lcd_hw_init()`. |
| PA10 | Control/enable line (exact function not individually pinned; safe) |

(PA6 was previously listed here — it is actually a **touchscreen electrode**, see above.)

The ZigBee (EM250) / RF (CC1150) radio was **desoldered** on the bench unit, so its
control pins (PB14 and USART3 if used for the radio) are inert here — but PB0 is
**not** inert: it is the IR carrier and works regardless of the radio.

TIM1 and TIM5 are never touched by stock, so the old "PA0 = TIM5_CH1" note leads
nowhere.
