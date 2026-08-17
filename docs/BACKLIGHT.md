# LCD backlight & sleep — what we know, and the open problem

Written after a long debugging session. Read this before touching the sleep path
again; several plausible-sounding approaches are already ruled out **on
hardware**, and one wrong note in HARDWARE.md cost hours.

## The open problem, stated precisely

**The backlight driver IC latches off whenever its PWM dim input stops receiving
edges, and only removing power clears it.**

Measured in the failed state, over SWD, after a keypress that should have woken
the remote:

```
PA1  MODER=2 (AF)      TIM2 CEN=1   CCR2=1000/2000 = 50%
PC12 ODR=1             (boost rail up)
```

Every register says "backlight on". No light is produced. So:

- the wake logic works
- the panel power-up sequence works
- the boost rail is up
- the **backlight driver IC** is latched, and only a power cycle clears it

## Ruled out ON HARDWARE (do not retry)

Four separate ways of stopping the PWM. All latch the driver; all require a
power cycle to recover:

| Attempt | Result |
|---|---|
| `backlight_set(0)` — TIM2 disabled, PA1 driven push-pull low | latches |
| Ramp 50% → 0 in steps of 7 (stock's ramp shape) | latches |
| Park PA1 high-Z + pull-down, TIM2 left running (stock's `FUN_0800e438`) | latches |
| Cycle PC12 to restart the converter | browns out the panel, see below |

**The only configuration that works is continuous PWM at a low duty** — 1%,
which is `backlight_set(1)` in `hx8347_blanking_on()`. The panel is genuinely
powered down, so the screen is black; what remains is a faint backlight glow.

## Also ruled out (decomp evidence)

- **FSMC re-init on wake** — stock touches no FSMC register in its sleep cycle
- **PD6 (panel reset) on wake** — stock never pulses it; the HX8347 retains
  registers and GRAM through standby
- **RCC clock gating on wake** — stock gates nothing
- **PC12 mishandling** — stock also holds it HIGH through sleep, exactly as we do
- **A second writer to the panel/backlight pins** — audited, there isn't one
- **GRAM writes during sleep** — the main loop skips rendering; verified

## `panel_init()` must NOT be called on wake

While the panel is in standby (`STB=1`) the HX8347 accepts only two operations,
so most of `panel_init()`'s ~45 register writes are discarded. It also violates
the mandatory **>5 ms wait between oscillator start (`0x19=0x01`) and standby
release (`0x1F` walk)**. Stock's abbreviated case-5 walk is correct:

```c
/* off — FUN_0800fb2c case 6 */          /* on — case 5 */
lcd_reg(0x28, 0x38); k_msleep(40);       lcd_reg(0x19, 0x01); k_msleep(6);
lcd_reg(0x28, 0x20); k_msleep(5);        lcd_reg(0x1F, 0xAC); k_msleep(5);
lcd_reg(0x1F, 0xA9); k_msleep(5);        lcd_reg(0x1F, 0xA4); k_msleep(5);
lcd_reg(0x19, 0x00);                     lcd_reg(0x1F, 0xB4); k_msleep(5);
                                         lcd_reg(0x1F, 0xF4); k_msleep(5);
                                         lcd_reg(0x1F, 0xD4); k_msleep(5);
                                         lcd_reg(0x28, 0x38); k_msleep(5);
                                         lcd_reg(0x28, 0x3C);
```

GRAM does not survive the power-down in practice, so the app forces a repaint on
wake (`ui_invalidate()`).

## Why stock differs — UNRESOLVED

Stock RTI sleeps on this same unit and its backlight comes back. Two agents
reading the same image disagreed about how:

- one found stock driving the backlight to a hard zero (TIM2 off, PA1 low) —
  which would contradict the latch theory entirely
- others found `FUN_0800e438` parking PA1 as high-Z + pull-down with TIM2 left
  running, and `FUN_0800e610` restoring MODER to AF on wake

We implemented the second and **it still latched**. So either there is a further
step in stock's sleep we have not found, or this board's driver IC differs from
the one stock was written for. This is the single open question.

**Next investigation should start here**: decompile `FUN_0800e438` and
`FUN_0800e610` directly and confirm exactly which pins/registers they touch,
rather than trusting the summary above.

## Backlight electrical facts (verified)

- **PA1 = TIM2_CH2**, high = brighter. `0` = TIM2 disabled + pin low (off),
  `100` = TIM2 disabled + pin high (full), `1..99` = PWM
- **Stock runs it at 2 kHz** — `FUN_0801a9fa` computes prescaler
  `(SysClk/2)/6000000 - 1 = 9` (6 MHz timer), period 3000, pulse `pct*30`.
  An earlier HARDWARE.md note calling 2 kHz "RTI's idle state" was **wrong**.
- We use a **duty-dependent frequency**: highest frequency keeping the ON pulse
  ≥5 µs, capped at 30 kHz. Silent at ≥15% brightness, dropping toward 2 kHz only
  at the dim end. At 30 kHz a 1% duty is a 333 ns pulse, too short for this
  driver to act on.
- **PC12 is a shared rail** — panel logic + battery monitor + both backlights.
  Dropping it lights the low-battery LED and browns out the panel. Never drop it.

## Keypad backlight (PC8 = TIM8_CH3)

- Stock curve (`FUN_0801ab1e`): period 300, pulse `pct*2 + 75` — usable range
  floors ~25%, tops ~92%. Endpoints are static GPIO with the timer stopped;
  **low = off, high = full** (`GPIO_ResetBits` for 0, `SetBits` for 100).
- **Bug found and fixed**: we ran PWM mode **1** with active-low polarity, which
  inverts duty — `CCR3=271/300` meant the pin was low 90% of the time, i.e. ~10%
  brightness, which looks like "off". Now PWM mode 2, matching the LCD channel.
- Turning the keypad backlight off during sleep has **twice coincided** with the
  LCD failing to wake, with no mechanism found (different port, different timer,
  PC12 untouched). If you re-add it, do it as the *only* change so it can be
  attributed.

## Debug aids

- Marker map (note the **collision** — `reset_hook.c` BOOTMARK(0..1) and
  `main.c` MARK(0x00/0x04) share the same two words, so boot markers are
  clobbered once main runs; worth moving BOOTMARK to `0x2001FF60`):

```
0x2001FF00 main phase   0x2001FF04 heartbeat   0x2001FF88 ui_init step
0x2001FF80 safety magic 0x2001FF84 boot attempts
0x2001FF8C asleep       0x2001FF90 brightness  0x2001FF94 wakes
```

- Reading the panel's chip ID over FSMC proves whether the panel logic is alive.
  **Freeze the IWDG first** or the watchdog resets the target mid-session:

```
target extended-remote :4242
set *(unsigned int *)0xE0042008 = *(unsigned int *)0xE0042008 | 0x1800
set mem inaccessible-by-default off
break lv_timer_handler
continue
set *(volatile unsigned char *)0x60000000 = 0
x/1xb 0x60040000            # 0x47 = panel logic alive
```

- Fast zero-tool triage after a failed wake: **are the keypad keys still lit?**
  They share the PC12 rail. Keys lit + LCD dark → rail is up, fault is the LCD
  backlight string or the panel. Keys dark too → the shared rail is down.
