# Sleep Power

> Auto-saved from overnight research agent. Static analysis only — nothing here was tested on hardware.

**Summary:** Both subsystems are now fully reversed from static analysis of the stock image. (A) The battery is ADC1 channel 7 = PA7 (confirmed by `GPIO_Init(GPIOA, {Pin=0x80, Mode=AN})` plus `ADC_RegularChannelConfig(ADC1, 7, 1, ADC_SampleTime_480Cycles)` in FUN_0801bd58); PB1 is a *separate* subsystem — ADC2 channel 9, the "ALS Task" ambient-light sensor at FUN_0801c030. So HARDWARE.md's two guesses were both right but belong to two different modules on two different ADCs. There is **no volts or percent conversion anywhere** in the firmware: stock compares raw 12-bit counts against fixed thresholds 2460 (trip) / 2731 (clear) after subtracting a self-calibrating offset that re-anchors to 2820 counts whenever the charger reports "charge complete", and exposes only a 5-step debounce counter whose value 4 means "low". Low battery **does** drive a front-panel indicator: FUN_0800e214 drives PC12 LOW when level==4, and sets the RGB fun light blue/red/green from the charge state read off three new GPIOs (PC9 charger-present, PC14/PC15 two-bit charger status, all input pull-up). (B) FUN_0801bf50 is **not** I2C, SPI, or a GPIO expander — it is a bit-banged single-wire pulse-count protocol on **PA10** (literal 0x40020000 = GPIOA, mask 0x400, and FUN_0801bf0c enables only AHB1 bit 0 = GPIOA): it pulses the line `reg` times, waits 300 us, pulses it `val` times, with interrupts locked for the whole frame. There is no clock, no chip select and no slave address. FUN_0801bf02 is `GPIO_ResetBits(GPIOA, PIN10)` — it holds PA10 low during sleep.

**Open questions:**
- Front-panel LED part number is unidentified. The protocol is fully characterised (single-wire pulse count on PA10, registers 0x10-0x15, inverted 1..32 levels, bare command 0x21) but there are no chip-name strings anywhere near the module, so the part cannot be named from firmware. A photo of the front-panel PCB silkscreen would settle it — and would also reveal whether the two register banks (0x10-0x12 / 0x13-0x15) are two physical LEDs or two halves of one.
- Meaning of LED command 0x21 (33 pulses, sent alone with no value field at init and on wake, before the colour is re-applied). Most likely reset or re-sync, but this is an inference from call position only.
- Battery divider ratio and Vref are not in the firmware, so raw counts cannot be converted to pack volts by analysis. Measuring PA7 against a known pack voltage on the bench unit would give the scale — and would confirm whether 2460 counts is a sensible Li-ion cutoff or whether the pack is multi-cell.
- Physical polarity of the ambient light sensor was inferred from which brightness preset each state selects (higher count -> dim preset not applied -> brighter room), not measured. Worth a 30-second bench check with a torch on PB1.
- Whether *(u32*)0x20011E20 can ever be non-zero. Only two references to that literal exist in the image and the single write stores 0, which makes charge states 1 and 3 unreachable — but an aliased write through a struct base pointer would not show up in a literal scan. Harmless either way, since the UI message collapses 1/2 and 3/4.
- Exact argument order of FUN_0801a838(a, b) — which of the two ramp targets is the LCD backlight and which is the keypad. The ALS calls it as (brightness, 0), which would mean the keypad target is forced to zero on every light-state change; that reads oddly and deserves a cross-check against the existing backlight/ramp work in docs/BACKLIGHT.md before anything is ported.
- Whether our firmware wants stock's ADC2 mutex discipline (touchscreen channels 4/5 vs ALS channel 9) or should instead put the light sensor on ADC1 alongside the battery, which would sidestep the contention entirely. ADC1 IN9 is the same PB1 pin, so this is a free choice.

---

## Battery / charger sensing and the front-panel LED (reversed from stock)

All addresses are in the stock image (`stock_flash_backup.bin`, loads at `0x08000000`).
The stock firmware is µC/OS-II on the ST standard peripheral library, so most helpers
resolve to named library calls:

| Stock fn | Identity | Proof |
|---|---|---|
| `FUN_0800ea24` | `GPIO_Init(GPIOx, *init)` | writes MODER +0x00, OTYPER +0x04, OSPEEDR +0x08, PUPDR +0x0C |
| `FUN_0800ec2c` | `GPIO_SetBits` | writes BSRR low half (+0x18) |
| `FUN_0800ec88` | `GPIO_ResetBits` | writes BSRR high half (+0x1A) |
| `FUN_0800eb6a` | `GPIO_ReadInputDataBit` | reads IDR (+0x10) |
| `FUN_0800d9ce` | `RCC_AHB1PeriphClockCmd` | AHB1ENR |
| `FUN_0800dafa` | `RCC_APB2PeriphClockCmd` | APB2ENR |
| `FUN_0801f1e6` | `ADC_CommonInit` | writes ADC_CCR |
| `FUN_0801f078` | `ADC_Init` | CR1 +0x04, CR2 +0x08, SQR1 +0x2C |
| `FUN_0801f352` | `ADC_RegularChannelConfig` | SMPR1/2, SQR3 |
| `FUN_0801f2fe` | `ADC_Cmd` | CR2 bit 0 (ADON) |
| `FUN_0801f4a6` | `ADC_SoftwareStartConv` | CR2 \|= 0x40000000 |
| `FUN_0801f4d6` | `ADC_GetConversionValue` | DR +0x4C |
| `FUN_0801f51c` | `ADC_GetFlagStatus` | SR +0x00 |
| `FUN_080113a4` | `delay_us(n)` | TIM5 @1 MHz output compare, `n-7` compensation |
| `FUN_0801b990` | `NVIC_SystemReset` | `SCB->AIRCR = (AIRCR & 0x700) \| 0x05FA0004; for(;;);` |

`GPIO_InitTypeDef` layout used throughout: `+0 Pin (u32), +4 Mode (u8), +5 Speed (u8),
+6 OType (u8), +7 PuPd (u8)`. `Mode`: 0 = IN, 1 = OUT, 2 = AF, 3 = **AN**.

---

## A. Battery and light sensing

### Pin map (new / corrected)

| Pin | Role | Config |
|---|---|---|
| **PA7** | **Battery sense** — ADC1 **IN7** | analog (`Mode=3`, no pull) |
| **PB1** | **Ambient light** — ADC2 **IN9** | analog (`Mode=3`, no pull) |
| **PC9** | Charger / dock present, **active low** | input, pull-up |
| **PC14** | Charger status bit 1 (charging), **active low** | input, pull-up |
| **PC15** | Charger status bit 2 (complete), **active low** | input, pull-up |
| **PC12** | **Low-battery indicator — drive LOW to light it** | output (already in HARDWARE.md as the shared rail enable) |

`HARDWARE.md` guessed PA7 and PB1. Both are right, but they are **two different
subsystems on two different ADCs**: PA7/ADC1 is the battery (`"BatMon Task"`,
`FUN_0801b9ac`), PB1/ADC2 is the ambient light sensor (`"ALS Task"`, `FUN_0801c030`).
PC9/PC14/PC15 are new — they were not in HARDWARE.md.

> **PC12 warning still applies.** Stock only ever drops PC12 inside the sleep/wake
> path (`FUN_0800e214`, reached from `FUN_0800e304`). It carries the panel logic
> supply as well as the indicator, so dropping it while running blanks the LCD and
> loses the HX8347 init. Do not use it as a plain status LED.

### ADC1 configuration (battery), from `FUN_0801b9ac` / `FUN_0801bd58`

```c
RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC, ENABLE); /* 0x5 */
RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);                        /* 0x100 */

/* PA7 analog */
GPIO_Init(GPIOA, &(GPIO_InitTypeDef){ .GPIO_Pin  = GPIO_Pin_7,
                                      .GPIO_Mode = GPIO_Mode_AN,
                                      .GPIO_PuPd = GPIO_PuPd_NOPULL });
/* PC9 / PC14 / PC15 input pull-up */
GPIO_Init(GPIOC, &(GPIO_InitTypeDef){ .GPIO_Pin  = GPIO_Pin_9 | GPIO_Pin_14 | GPIO_Pin_15,
                                      .GPIO_Mode = GPIO_Mode_IN,
                                      .GPIO_PuPd = GPIO_PuPd_UP });

ADC_CommonInit(&(ADC_CommonInitTypeDef){
    .ADC_Mode             = ADC_Mode_Independent,
    .ADC_Prescaler        = ADC_Prescaler_Div8,      /* 0x00030000 */
    .ADC_DMAAccessMode    = ADC_DMAAccessMode_Disabled,
    .ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles });

ADC_Init(ADC1, &(ADC_InitTypeDef){
    .ADC_Resolution           = ADC_Resolution_12b,
    .ADC_ScanConvMode         = DISABLE,
    .ADC_ContinuousConvMode   = ENABLE,              /* note: continuous */
    .ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None,
    .ADC_DataAlign            = ADC_DataAlign_Right,
    .ADC_NbrOfConversion      = 1 });

ADC_RegularChannelConfig(ADC1, ADC_Channel_7, 1, ADC_SampleTime_480Cycles);
ADC_Cmd(ADC1, ENABLE);
ADC_SoftwareStartConv(ADC1);   /* continuous mode: fires once, keeps converting */
```

Power management: `FUN_0801bd80` (veneer `0x0801BB4E`) = channel config + `ADC_Cmd(ENABLE)`
+ `SoftwareStartConv`; `FUN_0801bd74` (veneer `0x0801BB4C`) = `ADC_Cmd(DISABLE)`. Both are
called from the sleep/shutdown paths (`0x0800E2B0`, `0x0800E326`, `0x0800E3A4`).

### There is no volts-or-percent conversion

Stock never converts to volts and has **no battery percentage**. The whole gauge is:

```
corrected = raw - offset                     /* raw = 12-bit ADC1 IN7, offset is s16 in RAM */
low  when corrected <  2460   (0x99C)
clear when corrected >= 2731  (0xAAB)
offset = raw - 2820 (0x0B04)  captured when the charger reports "charge complete"
```

So `2820` is a *reference point*, not a reading: after a full charge the corrected value
reads exactly 2820, "low" means the pack has drooped **360 counts** below the last
full-charge reading, and it clears at **89 counts** below. The offset lives in RAM
(`0x200028C0`) and is zero after a cold boot, so on a fresh boot the thresholds apply to
the raw reading directly — which is also what the boot-time check uses.

At Vref 3.3 V, 2460 counts ≈ 1.98 V and 2731 ≈ 2.20 V at PA7, and 360 counts ≈ 0.29 V of
pin droop. **The divider ratio is not present in the firmware**, so an absolute pack
voltage cannot be derived by static analysis. Measure PA7 against a known pack voltage on
the bench unit if you want a real scale.

### Low-battery threshold → front-panel indicator (`FUN_0800e214`)

```c
if (bat_chg_state() == 0) {                     /* on battery */
    funlight_set_color(7);                      /* blue */
    if (bat_level() == 4) gpio_reset(GPIOC, GPIO_Pin_12);   /* LOW  -> indicator ON  */
    else                  gpio_set  (GPIOC, GPIO_Pin_12);
} else {                                        /* charger attached */
    gpio_set(GPIOC, GPIO_Pin_12);
    if (chg == 1 || chg == 3) funlight_set_color(3);  /* magenta (unreachable, see below) */
    else if (chg == 2)        funlight_set_color(5);  /* red   = charging */
    else                      funlight_set_color(6);  /* green = charge complete */
}
```

### Charge-state decode (`FUN_0801bb8e`)

| PC9 | PC14 | PC15 | `chg_state` | Meaning | Fun light |
|---|---|---|---|---|---|
| 1 | x | x | 0 | no charger | blue |
| 0 | 1 | 1 | 0 | charger idle / standby | blue |
| 0 | 0 | 1 | 2 | charging | red |
| 0 | 1 | 0 | 4 | charge complete | green |
| 0 | 0 | 0 | 2 if previous was 0, else 4 | fault / sticky | red or green |

The 1-vs-2 and 3-vs-4 split is gated on `*(u32*)0x20011E20`. A whole-image scan finds only
two references to that literal, and the single write (`0x080410CE`) stores 0 — so
`chg_state` is 0, 2 or 4 in practice and states 1/3 look unreachable. The UI message
collapses 1/2 → 3 and 3/4 → 4 anyway, so nothing depends on it. **Unknown:** whether an
aliased write can set it non-zero.

### Reimplementation sketch

```c
/* Battery monitor. Mirrors stock FUN_0801bb8e: poll at 1 Hz, four consecutive
 * low samples to assert, hysteresis to clear.
 *
 * Charger: PC9 present (active low), PC14/PC15 status (active low).
 * Indicator: PC12 LOW == lit -- but PC12 is the shared rail enable, so only touch
 * it on the sleep path (see HARDWARE.md).
 */
#define BAT_FULL_REF   2820   /* corrected value defined as "just charged" */
#define BAT_TRIP       2460   /* corrected <  TRIP  -> counts toward "low"  */
#define BAT_CLEAR      2731   /* corrected >= CLEAR -> clears "low"         */
#define BAT_STEPS      4      /* consecutive 1 Hz samples to assert         */

enum bat_chg { BAT_CHG_NONE = 0, BAT_CHG_CHARGING = 2, BAT_CHG_FULL = 4 };

static int16_t bat_offset;    /* ponytail: raw-count self-cal, no volts. Swap for a
                               * real divider ratio + Vref only if you actually need
                               * a percentage -- stock never had one. */
static uint8_t bat_steps;     /* 0..4; 4 == low battery */

static enum bat_chg bat_read_chg(void)
{
    if (gpio_pin_get_dt(&pc9))            return BAT_CHG_NONE;   /* high = no charger */
    bool st1 = !gpio_pin_get_dt(&pc14);   /* active low */
    bool st2 = !gpio_pin_get_dt(&pc15);
    if (st1 && !st2) return BAT_CHG_CHARGING;
    if (st2 && !st1) return BAT_CHG_FULL;
    if (st1 && st2)  return bat_steps_prev_chg ? BAT_CHG_FULL : BAT_CHG_CHARGING;
    return BAT_CHG_NONE;                  /* both released */
}

/* call once per second */
bool bat_poll(void)
{
    static enum bat_chg prev = BAT_CHG_NONE;
    enum bat_chg chg = bat_read_chg();

    /* stock re-anchors only when entering "full" from another state */
    bool recal = (chg == BAT_CHG_FULL && prev != BAT_CHG_FULL);
    prev = chg;

    uint16_t raw = adc_read_bat();                 /* ADC1 IN7 */
    if (recal) bat_offset = (int16_t)(raw - BAT_FULL_REF);
    uint16_t corrected = (uint16_t)(raw - bat_offset);

    if (chg != BAT_CHG_NONE) {                     /* charging: no debounce */
        bat_steps = (corrected < BAT_TRIP) ? BAT_STEPS : 0;
    } else if (bat_steps >= BAT_STEPS) {           /* latched low: hysteresis */
        if (corrected >= BAT_CLEAR) bat_steps = 0;
    } else {
        bat_steps = (corrected < BAT_TRIP) ? bat_steps + 1 : 0;
    }
    return bat_steps >= BAT_STEPS;                 /* true == low battery */
}
```

**Do not copy the boot lockout.** `FUN_0801b9ac` samples ADC1 three times at boot and, if
raw < 2460 *and* PC9 is high (no charger), waits one second and calls `NVIC_SystemReset` —
an indefinite reset loop until power is plugged in. A tired pack plus that check makes a
remote look bricked. Log and continue instead.

### Ambient light sensor (`"ALS Task"`)

```c
RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);   /* 0x2   */
RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC2, ENABLE);    /* 0x200 */
GPIO_Init(GPIOB, &(GPIO_InitTypeDef){ .GPIO_Pin = GPIO_Pin_1, .GPIO_Mode = GPIO_Mode_AN });
ADC_RegularChannelConfig(ADC2, ADC_Channel_9, 1, ADC_SampleTime_15Cycles);
```

* Threshold **2048** counts: `< 2048` = dark → apply the dim preset (config byte `+0x61`,
  default 30); `>= 2048` = bright → apply the bright preset (config byte `+0x5F`, clamp 100).
  So **a higher count means more ambient light**.
* Debounce: three consecutive agreeing samples ~100 ms apart, then a 5 s settle after acting.
* On a change it sets the LCD backlight ramp target (`FUN_0801a838`) *and*
  `FunLightSetBrightness` to the same value.
* **ADC2 is shared with the touchscreen** (channels 4 and 5). Stock takes an RTOS mutex
  (handle `0x20006318`) around the whole ALS read. Any reimplementation must serialise
  ADC2 access the same way.
* Stock bug not to copy: `FUN_0801c10e` reads `r4` (the previous light state) before ever
  writing it, so its first decision starts from the caller's leftover register. Initialise
  the previous-state variable explicitly.

### Event delivery

`FUN_0801bd22` posts message type **0x0C** on the main command queue with one byte:
`0` = normal, `2` = low battery, `3` = charging, `4` = charge complete. It fires on any
change of charge state or on the low-battery flag toggling.

### Stock RAM layout (useful for SWD comparison)

```
0x200028BC + 0  u8   adc_running
           + 1  u8   chg_state   (0 / 2 / 4)
           + 2  u8   level       (0..4; 4 == low battery)
           + 4  s16  offset
           + 8       BatMon task stack, 196 words, up to +0x314
0x20002450 + 0  u8   als_state   (0 = bright, 1 = dark)
           + 4       ALS task stack, 196 words, up to +0x310
0x20017F14 + 0  u8   funlight colour index
           + 2  u16  funlight brightness (0..100)
0x20011E20      u32  charge-substate selector (only observed write stores 0)
```

---

## B. Front-panel LEDs

### Transport: single-wire pulse count on PA10 — not I2C, not SPI, no expander

`FUN_0801bf50(reg, val)` bit-bangs **PA10** (`GPIOA` base `0x40020000`, mask `0x400`). The
port is confirmed twice: the literal at `0x0801BFD0` is `0x40020000`, and `FUN_0801bf0c`
enables only AHB1 bit 0 (`RCC_AHB1Periph_GPIOA`) before configuring pin `0x400`.

There is **no clock line, no chip select, no slave address and no acknowledgement**. The
register number and the value are each transmitted as a *count of pulses*, separated by a
gap. Exact behaviour from the disassembly at `0x0801BF50`–`0x0801BFC8`:

```
cpsid i                          ; interrupts locked for the whole frame
repeat reg times:                ; a count of 0 sends nothing
    PA10 = 0 ; delay_us(25)
    PA10 = 1 ; delay_us(25)
delay_us(300)                    ; field separator
if (reg != 0x21 && val != 0):
    repeat val times:
        PA10 = 0 ; delay_us(25)
        PA10 = 1 ; delay_us(25)
cpsie i
delay_us(300)                    ; inter-frame gap (tail call)
```

`delay_us` is `FUN_080113a4`: TIM5 at a 1 MHz timebase, output compare, polled — the
argument is microseconds with ~7 µs of call overhead subtracted, so the electrical pulse is
roughly 25 µs low / 25 µs high (~20 kHz) and the gaps are ~300 µs. The receiver counts
edges, so the timing tolerance is presumably wide; the only structural requirement is that
the pulse period stays well below the 300 µs field separator.

Line polarity: `FUN_0801bf0c` drives PA10 **low** and then configures it as output
push-pull, 2 MHz, pull-up. Each pulse ends with the line high, so between frames the line
**idles high**. `FUN_0801bf02` (called from the shutdown path `FUN_0800e292` and the
sleep/wake cycle `FUN_0800e304`) is exactly `GPIO_ResetBits(GPIOA, GPIO_Pin_10)` — it
**holds PA10 low during sleep**.

**Unknown:** the part number. There are no chip-name strings anywhere near the module
(only `"@FunLightSetColor"` at `0x0801BFD3`, `"FunLightSetBrightness"` at `0x0801BFE8`,
`"Funlight Init"` at `0x0801DF64`), and the protocol is generic pulse-counting. The
behaviour is fully specified by the code above, so a driver does not need the part number —
but a datasheet would settle what `0x21` means.

### Register map

| Reg | Channel | Fed from |
|---|---|---|
| `0x10` | bank A blue | table byte 2 |
| `0x11` | bank A red | table byte 0 |
| `0x12` | bank A green | table byte 1 |
| `0x13` | bank B blue | table byte 2 |
| `0x14` | bank B red | table byte 0 |
| `0x15` | bank B green | table byte 1 |
| `0x21` | bare command, no value field — sent alone at init and on wake (**meaning unknown**, most likely reset / re-sync) |

Values are **inverted**: `0x01` = brightest, `0x20` = off, computed as
`level = 32 - (brightness * 31) / 100` (`rsb r0,r0,r0 lsl #5` / `sdiv #100` / `rsb #32`).
Value `0` is unsendable by construction — `FUN_0801bf50` suppresses the value field when
`val == 0` — which is why the scale starts at 1.

Stock write order in `FunLightSetColor`: `0x15, 0x12, 0x14, 0x11, 0x13, 0x10`.

### Colour table at `0x0801C000`

16 entries × 3 bytes RGB (table ends at `0x0801C02F`; the bytes at `0x0801C030` are
`push {lr}; sub sp,#0x24`, the entry of `FUN_0801c030`):

| idx | bytes | authored colour | effective on this hardware |
|---|---|---|---|
| 0 | `00 00 00` | off | off |
| 1 | `64 64 64` | white | white |
| 2 | `64 64 00` | yellow | yellow |
| 3 | `64 00 64` | magenta | magenta |
| 4 | `00 b7 eb` | cyan #00B7EB | cyan |
| 5 | `64 00 00` | red | red |
| 6 | `00 64 00` | green | green |
| 7 | `00 00 64` | blue | blue |
| 8 | `ff fd d0` | cream | white |
| 9 | `ad d8 e6` | LightBlue | white |
| 10 | `ff c0 cb` | Pink | white |
| 11 | `ff 7f 00` | orange | yellow |
| 12 | `64 00 64` | purple | magenta |
| 13 | `90 ee 90` | LightGreen | white |
| 14 | `00 64 64` | teal | cyan |
| 15 | `70 80 90` | SlateGray | white |

`FunLightSetColor` only ever tests each byte with `cbz`/`cbnz`, so **per-channel intensity
is discarded** — the hardware gives you the 8 corners of the RGB cube times one global
brightness. Several entries are exact X11 colours (#ADD8E6, #FFC0CB, #90EE90, #708090),
which is what marks the table as authored RGB rather than misparsed code.

### Driver sketch

```c
/* Front-panel "fun light": one-wire pulse-count LED driver on PA10.
 * Not I2C, not SPI, no address. Line idles high; each pulse is 25us low + 25us high;
 * a 300us gap separates the register field from the value field and follows each frame.
 */
#define FL_PULSE_US   25
#define FL_GAP_US     300
#define FL_CMD_RESET  0x21
#define FL_OFF        0x20      /* 0x01 = brightest, 0x20 = off */

enum { FL_A_BLUE = 0x10, FL_A_RED = 0x11, FL_A_GREEN = 0x12,
       FL_B_BLUE = 0x13, FL_B_RED = 0x14, FL_B_GREEN = 0x15 };

static const struct gpio_dt_spec fl_dat =
    GPIO_DT_SPEC_GET(DT_NODELABEL(funlight), gpios);   /* PA10, active high */

static void fl_pulses(unsigned n)
{
    while (n--) {
        gpio_pin_set_dt(&fl_dat, 0);
        k_busy_wait(FL_PULSE_US);
        gpio_pin_set_dt(&fl_dat, 1);
        k_busy_wait(FL_PULSE_US);
    }
}

void fl_write(uint8_t reg, uint8_t val)
{
    unsigned int key = irq_lock();        /* stock locks IRQs for the whole frame */
    fl_pulses(reg);
    k_busy_wait(FL_GAP_US);
    if (reg != FL_CMD_RESET && val != 0)
        fl_pulses(val);
    irq_unlock(key);
    k_busy_wait(FL_GAP_US);
}

void fl_init(void)
{
    gpio_pin_configure_dt(&fl_dat, GPIO_OUTPUT_INACTIVE);   /* low, as stock does */
    fl_write(FL_CMD_RESET, 0);                              /* 33 bare pulses */
}

void fl_sleep(void) { gpio_pin_set_dt(&fl_dat, 0); }        /* stock holds PA10 low */

/* brightness 0..100 -> 0x20 (off) .. 0x01 (max).
 * ponytail: linear like stock. If the LEDs look wrong at the low end, this single
 * line is the calibration knob -- swap in a gamma table, don't restructure. */
static inline uint8_t fl_level(uint8_t pct) { return 32 - (pct * 31) / 100; }

void fl_set_rgb(bool r, bool g, bool b, uint8_t pct)
{
    uint8_t lv = fl_level(pct);
    uint8_t rv = r ? lv : FL_OFF, gv = g ? lv : FL_OFF, bv = b ? lv : FL_OFF;
    fl_write(FL_B_GREEN, gv); fl_write(FL_A_GREEN, gv);     /* stock order */
    fl_write(FL_B_RED,   rv); fl_write(FL_A_RED,   rv);
    fl_write(FL_B_BLUE,  bv); fl_write(FL_A_BLUE,  bv);
}
```

One runnable check for the level maths (the only non-trivial arithmetic here):

```c
static void fl_selftest(void)
{
    __ASSERT(fl_level(0)   == 0x20, "0%% must be off");
    __ASSERT(fl_level(100) == 0x01, "100%% must be max");
    for (int p = 0; p <= 100; p++) {
        uint8_t v = fl_level(p);
        __ASSERT(v >= 0x01 && v <= 0x20, "level out of range at %d%%", p);
    }
}
```

A full frame costs `(reg + val) * 50 us + 600 us`; a six-channel colour change is
therefore roughly 6 × (about 2 ms) ≈ 12 ms with interrupts locked in ~2 ms slices. That is
long enough to matter — do not call `fl_set_rgb` from a hot path or an ISR.

### Documentation corrections

* `HARDWARE.md` "PA10 (out) — Control/enable line (exact function not individually
  pinned; safe)" → **PA10 is the front-panel LED one-wire data line.**
* Add PC9 / PC14 / PC15 as charger-status inputs (pull-up, active low).
* PC12's low-battery note is confirmed: it lights when `bat_level == 4`, driven only from
  the sleep/wake path.
* `HARDWARE.md` "PA0 / TIM5_CH1 — PWM, purpose TBD (driving it lit nothing)": TIM5 is the
  microsecond busy-delay timebase (`FUN_080113a4`, 1 MHz, polled output compare, output
  never enabled), which likely explains why driving it lit nothing. Secondary observation —
  PA0's alternate-function config was not checked.

