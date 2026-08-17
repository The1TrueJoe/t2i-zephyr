# Sleep, backlight & low power — complete record of what was tried

Written after a long session that went in circles. **Read this before touching
the sleep or backlight path.** Most of the obvious approaches are already ruled
out *on hardware*, several notes in HARDWARE.md turned out to be wrong, and at
least three hours were lost to misdiagnoses recorded here so they are not
repeated.

---

## 1. Current working configuration

| | Awake | Asleep |
|---|---|---|
| Panel (HX8347) | on | powered down (`0x1F` walk + oscillator off) |
| LCD backlight (PA1/TIM2) | 50% PWM | **1% PWM — never stopped** |
| Keypad backlight (PC8/TIM8) | 90% PWM | **1% PWM — never stopped** ⚠️ unverified |
| CPU | WFI between frames | WFI, woken by EXTI |
| Wake sources | — | keypad rows, accel INT1 (EXTI); touch is polled |
| PC12 (shared rail) | HIGH | **HIGH — never drop it** |

Sleep after 30 s idle (`SLEEP_AFTER_MS` in `power.c`). The screen is genuinely
black because the *panel* is off; a faint backlight glow remains and cannot
currently be removed (see §3).

---

## 2. The core problem

**The backlight driver IC latches off whenever its PWM dim input stops receiving
edges. Only removing power clears it.**

Measured over SWD in the failed state, after a keypress that should have woken it:

```
PA1  MODER=2 (AF)      TIM2 CEN=1   CCR2=1000/2000 = 50%
PC12 ODR=1             (boost rail up)
```

Every register says "backlight on"; no light is produced. So the wake logic, the
panel sequence and the rail are all **fine** — the driver IC is latched.

**Both backlights share the boost converter enabled by PC12.** This is why
turning the *keypad* backlight off during sleep twice killed the *LCD* backlight
with no apparent connection between them — taking either dim input static
appears to latch the shared converter.

---

## 3. Ruled out ON HARDWARE — do not retry

Every way of stopping the PWM. All latch; all need a power cycle.

| Attempt | Commit | Result |
|---|---|---|
| `backlight_set(0)` — TIM2 disabled, PA1 driven push-pull low | — | latched |
| Ramp 50%→0 in steps of 7 (stock's ramp shape) | `261e6c6` | latched |
| Park PA1 high-Z + pull-down, TIM2 left running (stock's `FUN_0800e438`) | — | latched |
| Cycle PC12 to restart the converter | — | browns out panel + lights low-battery LED |
| Keypad backlight to 0 during sleep | `1754924` | killed the **LCD** wake (shared converter) |
| Full `panel_init()` on wake instead of stock's walk | — | no improvement; also illegal, see §5 |

**The only thing that works is continuous PWM at a low duty.**

---

## 4. Ruled out by decomp — do not investigate

- **FSMC re-init on wake** — stock touches no FSMC register in its sleep cycle
- **PD6 (panel reset) on wake** — stock never pulses it; the HX8347 retains
  registers and GRAM through standby
- **RCC clock gating on wake** — stock gates nothing
- **PC12 mishandling** — stock also holds it HIGH through sleep, same as us
  (`FUN_0800e304` re-asserts it right after the panel-off call)
- **A second writer to the panel/backlight pins** — audited; there isn't one
- **GRAM writes during sleep** — the main loop skips rendering; verified
- **FSMC bus hanging on a dead panel** — the controller has no stall mechanism
  in this configuration

---

## 4b. The boot path latched it too — found 2026-08-17

Reflashing left the screen dark until a manual power cycle, and the cause was
ours: `lcd_hw_init()` opened with `backlight_set(0)`, which stops TIM2 and drives
PA1 push-pull low — **the first entry in the §3 table**. It stayed that way for
the whole of boot (panel reset, `panel_init()`, GRAM clear) before anything
raised it again.

A cold boot survives it because the driver is not powered yet and never sees the
static low. A warm reset does not: the driver is live and running, takes a
multi-hundred-millisecond static low, and latches. Fixed by opening with
`backlight_set(1)` instead — the same "dark but still switching" value sleep
uses, which hides the power-up noise just as well.

**Also ruled out on hardware:** cycling **PC12** at boot (low 120 ms, then low
400 ms, then high) does *not* unlatch the converter. PC12 is a shared rail, not
the driver's own supply — do not retry this.

### The diagnostic that made it findable

`hx8347_panel_id()` reads HX8347 **R00**, which returns the device code. Emitted
over USB CDC at boot as `PANEL id=0x____`:

- **`0x4747`** (the 8-bit bus returns `0x47` for both reads) — panel and FSMC are
  alive, so a dark screen is **purely** the backlight
- anything else — the panel itself did not come up

This is the split to reach for first; it turns "the screen is dark" into one of
two much smaller problems. It also proved the panel survives a warm reset
untouched, which is what pointed at the backlight and nothing else.

## 5. `panel_init()` must NOT be called on wake

While the panel is in standby (`STB=1`) the HX8347 accepts only two operations,
so most of `panel_init()`'s ~45 register writes are silently discarded. It also
violates the mandatory **>5 ms wait between oscillator start (`0x19=0x01`) and
standby release (`0x1F` walk)**.

Stock's abbreviated walk (`FUN_0800fb2c`, cases 6 and 5, gated on panel ID `0x47`):

```c
/* OFF — case 6 */                       /* ON — case 5 */
lcd_reg(0x28, 0x38); k_msleep(40);       lcd_reg(0x19, 0x01); k_msleep(6);
lcd_reg(0x28, 0x20); k_msleep(5);        lcd_reg(0x1F, 0xAC); k_msleep(5);
lcd_reg(0x1F, 0xA9); k_msleep(5);        lcd_reg(0x1F, 0xA4); k_msleep(5);
lcd_reg(0x19, 0x00);                     lcd_reg(0x1F, 0xB4); k_msleep(5);
                                         lcd_reg(0x1F, 0xF4); k_msleep(5);
                                         lcd_reg(0x1F, 0xD4); k_msleep(5);
                                         lcd_reg(0x28, 0x38); k_msleep(5);
                                         lcd_reg(0x28, 0x3C);
```

Writing only `0x28=0x38` is **not enough** — the screen stays faintly viewable.
GRAM does not survive in practice, so the app forces a repaint (`ui_invalidate()`).

---

## 6. Backlight electrical facts (verified)

- **PA1 = TIM2_CH2**, high = brighter. Endpoints: `0` = timer disabled + pin low,
  `100` = timer disabled + pin high, `1..99` = PWM mode 2 + CC2P.
- **Stock runs the LCD backlight at 2 kHz.** `FUN_0801a9fa` computes prescaler
  `(SysClk/2)/6000000 - 1 = 9` (6 MHz timer), period 3000, pulse `pct*30`.
  ⚠️ An earlier HARDWARE.md note called 2 kHz "RTI's idle state" and said to use
  30 kHz. **That was wrong and cost hours.**
- **Minimum pulse ≈5 µs.** At 30 kHz a 1% duty is a 333 ns pulse — too short for
  this driver to act on. We therefore scale frequency with duty: highest
  frequency keeping the ON pulse ≥5 µs, capped at 30 kHz (inaudible ≥15%
  brightness, dropping toward 2 kHz only at the dim end).
- **Keypad backlight (PC8/TIM8)**: stock curve `FUN_0801ab1e` is period 300,
  pulse `pct*2 + 75` (floors ~25%, tops ~92%); endpoints static GPIO, low = off.
  ⚠️ We had it on PWM mode **1** with active-low polarity, which *inverts* duty —
  `CCR3=271/300` meant ~10% brightness, which reads as "off". Now mode 2.
  At low duty its prescaler is raised (PSC 9→99, 20 kHz→2 kHz) so 1% is still 5 µs.
- **PC12 is a shared rail** — panel logic + battery monitor + both backlights.
  Dropping it lights the low-battery LED and browns out the panel, which then
  needs a full re-init. Never drop it.

---

## 7. STOP mode — built, disabled, and why

`lowpower.c` implements STM32 STOP with a **correct** entry sequence:

- clear `PWR_CR` **CWUF** (a set wakeup flag makes the next entry fall through)
- clear pending **`EXTI->PR`** (a stale edge wakes you instantly)
- **`SEV; WFE; WFE`** rather than `WFI` (first WFE eats a pending event)
- IRQs off across entry, restored only after the PLL is back
- clock restore: HSE on → wait → PLL on → wait → SYSCLK=PLL (matches stock's
  `FUN_0800e400` exactly)

**It is off (`USE_STOP_MODE 0`) because STOP stops SysTick**, so `k_uptime_get()`
freezes. Our sleep timer is uptime-based, so every wake sees a stale idle delta
and immediately re-sleeps — the panel toggles and **the screen visibly flashes**.

To make STOP viable you need one of:
1. a free-running timebase that survives STOP (the **RTC**) to correct uptime on wake
2. proper `CONFIG_PM` — impossible on F2: no `power.c`, no LPTIM ([zephyr#19755](https://github.com/zephyrproject-rtos/zephyr/issues/19755))
3. stop using uptime for the sleep timer

Also required: stock **saves and restores the FSMC registers** across STOP
(`FUN_0800e610`). We do not, and would need to.

Expected saving: ~15–25 mA (WFI) → ~0.5 mA. **Needs a current meter** — its value
is a number that cannot be observed any other way, and three attempts failed in
ways one meter reading would have settled immediately.

---

## 8. The open question

**Stock sleeps on this same unit and its backlight comes back. Ours does not.**

Two decomp agents reading the same image disagreed:
- one found stock driving the backlight to a hard zero (TIM2 off, PA1 low) —
  which would contradict the latch theory entirely
- others found `FUN_0800e438` parking PA1 high-Z + pull-down with TIM2 running,
  and `FUN_0800e610` restoring MODER to AF on wake

We implemented the second and **it still latched**. So either there is a further
step in stock's sleep we have not found, or this board's driver differs from the
one stock targets.

**Start here next time:** decompile `FUN_0800e438` and `FUN_0800e610` directly
and confirm exactly which pins and registers they touch. Do not trust the
summary above — it is second-hand.

---

## 9. Misdiagnoses made (so they are not repeated)

- "The RGB grid at boot comes from main" — no, `lcd_hw_init()` lit the backlight
  during driver init, before `main()` ran.
- "PC12 is the backlight/boost enable" — it is a **shared rail**; dropping it
  browns out the panel and lights the low-battery LED.
- "24 KB LVGL pool is proven sufficient" — proven for the *old* label+button UI,
  not the one with a splash image. Hung `ui_init()`.
- "The watchdog isn't running" — it was armed correctly; `st-flash` sets
  `DBGMCU_APB1_FZ` **DBG_IWDG_STOP**, freezing it whenever a probe attaches.
- "Matching stock's register sequence will fix the relight" — it did not.
- "The ramp is the mechanism" — stock does ramp, but ramping did not fix it.
- "This unit's hardware must be different" — premature; stock works on it.

---

## 10. Debug recipes

**Marker map** (⚠️ `reset_hook.c` BOOTMARK(0..1) collides with `main.c`
MARK(0x00/0x04) — boot markers are clobbered once main runs; move BOOTMARK to
`0x2001FF60`):

```
0x2001FF00 main phase    0x2001FF04 heartbeat     0x2001FF88 ui_init step
0x2001FF80 safety magic  0x2001FF84 boot attempts
0x2001FF8C asleep        0x2001FF90 brightness    0x2001FF94 wakes
```

**Read the panel's chip ID over FSMC** — proves whether the panel logic is alive.
Freeze the IWDG first or the watchdog resets the target mid-session:

```
target extended-remote :4242
set *(unsigned int *)0xE0042008 = *(unsigned int *)0xE0042008 | 0x1800
set mem inaccessible-by-default off
break lv_timer_handler
continue
set *(volatile unsigned char *)0x60000000 = 0
x/1xb 0x60040000            # 0x47 = panel logic alive
```

**Zero-tool triage after a failed wake:** are the keypad keys still lit? They
share the PC12 rail.
- keys lit + LCD dark → rail up; fault is the LCD backlight string or the panel
- keys dark too → shared rail/converter is down

**Register snapshot** (`st-flash read` into a file, then decode):
```
0x40020000 GPIOA  (PA1 MODER@0x00 bits3:2, PUPDR@0x0C, ODR@0x14 bit1)
0x40020800 GPIOC  (PC8 MODER bits17:16, PC12 ODR bit12)
0x40000000 TIM2   (CR1@0x00, ARR@0x2C, CCR2@0x38)
0x40010400 TIM8   (CR1@0x00, ARR@0x2C, CCR3@0x3C, BDTR@0x44)
```

**Flashing:** always `./tools/flash.sh` — it forces `--flash=512k` (st-flash's
size detection intermittently reads 0 → "Unknown memory region"), warms the link
with a probe, retries, and resets afterwards. If the target is unreachable, hold
any key while powering on: recovery mode never sleeps.

⚠️ A failed build does not always stop the flash step — check the build output.
