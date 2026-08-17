# Sleep Power B

> Auto-saved from overnight research agent. Static analysis only — nothing here was tested on hardware.

**Summary:** Settled the open question in docs/BACKLIGHT.md, and it falsifies the doc's central premise. FUN_0800e438 is a GPIO parking routine (save MODER/OTYPER/OSPEEDR/PUPDR for ports A-E into a 20-word buffer at 0x200059F8, then GPIO_Init 15 groups to IN or AN); FUN_0800e610 restores exactly those four registers for A-E — it is NOT FSMC, so §7's "stock saves and restores the FSMC registers" is wrong and can be deleted. During stock's sleep PA1 is high-Z with a pull-down and TIM2 is DISABLED, and PC8 is an output driven push-pull LOW with TIM8 DISABLED — because the backlight has already been ramped to a hard 0 before FUN_0800e304 is even entered. Level "B" (the idle target) is hard-coded 0: FUN_0800e8aa and FUN_0800e8ca both `return 0`. The request comes from the low-power task at 0x0800E1D2 (`FUN_0801a876(1)`), which then spins on FUN_0801a92a() until the fade completes before calling FUN_0800e304. FUN_0801a8c2 is never in the sleep path — it has exactly one caller in the whole 512 KB image, FUN_0800e292, the STANDBY/shutdown path. So stock holds both dim inputs static, with both timers stopped, for the entire sleep, and the light comes back: the "driver IC latches when edges stop" theory does not hold, and §3's table rows are misattributed. Separately, the reason STOP mode "flashes the screen" is almost certainly not SysTick: safety.c arms the IWDG at ~8 s, the IWDG keeps running in STOP on F2, and lowpower_stop() has no timed wake source — so an idle remote resets every 8 s and re-runs lcd_hw_init(). The RTC design below therefore exists first to feed the watchdog and only second to correct uptime.

**Open questions:**
- Is the 30 kHz vs stock's 2 kHz PWM frequency what actually breaks our relight? The image proves the frequency difference but nothing more. Test: pin PSC=9/ARR=3000/CCR2=pct*30 for all 1..99, set BRIGHT_ASLEEP 0 so the endpoint path runs, and replicate stock exactly. This is the single cheapest remaining experiment and it needs one flash, not a meter.
- Is an LSE crystal fitted? Stock never touches the RTC, so the firmware cannot say. PC14/PC15 being parked as pulled-up inputs argues against one. Resolve by PCB inspection (look for a 32.768 kHz can near PC14/PC15), not by asserting LSEON — if those pins are a strap rather than a crystal, enabling the oscillator drives PC15.
- Confirm STM32F2 truly has no RTC_SSR / SHIFTR / CALR (RM0033 §22.6). The 1-second-resolution claim, and therefore the whole shape of the uptime correction, rests on it.
- Is the RTC wakeup period (WUTR+1) or WUTR ck_spre periods? RM wording is easy to misread. Verify empirically once with a stopwatch and a marker write; a 4 s chunk has 4 s of margin against the 8 s IWDG either way.
- Must RTC_CR.WUTIE be set for EXTI line 22 to pulse when only EXTI_EMR (not IMR) is enabled? The code above sets WUTIE and leaves the NVIC channel disabled, which should be safe in both readings, but it is untested.
- Actual STOP-mode current. Still the one number that cannot be obtained any other way, and three attempts have now failed in ways a single meter reading would have settled. Buy the meter before the next attempt.
- Does the parked keypad matrix actually produce a clean falling edge? Stock leaves rows IN+NOPULL and columns IN+pull-down, so a keypress pulls a row down through a ~40 kOhm internal resistor against whatever external pull-up the rows have. I could not establish the external network from the image; the wake mechanism is inferred, not proven.
- Why did §3's experiments latch, given stock does the same thing? Once the 2 kHz test above is done, if the relight works then the §3 rows were all frequency artifacts. If it still fails, the next thing to instrument is TIM2 CCER/CCMR1 and TIM8 BDTR.MOE in the failed state - the §2 snapshot recorded MODER, CEN and CCR2 but never those, so an output stage that is configured but not enabled would have looked identical.

---

## 11. FUN_0800e438 / FUN_0800e610 decompiled — §8's open question, settled

`FUN_0800ea24` is ST's `GPIO_Init(GPIOx, GPIO_InitTypeDef*)` — identified by its
own assert string, `.../CPU/ST/STM32/src/stm32f2xx_gpio.c`. Struct layout as
used: `+0` u32 Pin mask, `+4` Mode, `+5` Speed, `+6` OType, `+7` PuPd.

Literal pool at `0x0800e738`–`0x0800e750`:

```
0x0800e738: 0x40020800  GPIOC
0x0800e740: 0x200059F8  save buffer (20 words)
0x0800e744: 0x40020000  GPIOA
0x0800e748: 0x40020400  GPIOB
0x0800e74c: 0x40020C00  GPIOD
0x0800e750: 0x40021000  GPIOE
0x0800e734: 0x200055D8  low-power task state
```

### 11.1 `FUN_0800e438` — save + park GPIOA..E

Phase 1 saves 20 words to `0x200059F8`. The compiler reaches each next port's
`MODER` as `[prev_base + 0x400]`, which is why the decompile shows
`puVar3[0x100]`:

```
+0x00 A MODER   +0x04 A OTYPER  +0x08 A OSPEEDR  +0x0C A PUPDR
+0x10 B MODER   +0x14 B OTYPER  +0x18 B OSPEEDR  +0x1C B PUPDR
+0x20 C MODER   +0x24 C OTYPER  +0x28 C OSPEEDR  +0x2C C PUPDR
+0x30 D MODER   +0x34 D OTYPER  +0x38 D OSPEEDR  +0x3C D PUPDR
+0x40 E MODER   +0x44 E OTYPER  +0x48 E OSPEEDR  +0x4C E PUPDR
```

Phase 2 is 15 `GPIO_Init` calls. The raw asm writes only `[sp,#0]` (Pin),
`[sp,#4]` (Mode) and `[sp,#7]` (PuPd) — **Speed and OType are never stored**,
which is safe because every group is Mode 0 (IN) or 3 (AN), and `GPIO_Init`
skips `OSPEEDR`/`OTYPER` unless Mode is OUT or AF. PuPd is *not* rewritten on
the analog calls, so those inherit the previous call's value (shown in
parentheses); harmless, since F2 disables the pull resistors in analog mode.

| # | Port | Pin mask | Mode | PuPd | Pins |
|---|---|---|---|---|---|
| 1 | A | `0x1AFD` | 3 AN | *(uninit)* | PA0,2,3,4,5,6,7,9,11,12 |
| 2 | A | `0x0502` | **0 IN** | **2 DOWN** | **PA1**, PA8, PA10 |
| 3 | A | `0x8000` | 0 IN | 1 UP | PA15 |
| 4 | B | `0x3F12` | 3 AN | *(1)* | PB1,4,8,9,10,11,12,13 |
| 5 | B | `0xC0ED` | 0 IN | 2 DOWN | PB0,2,3,5,6,7,14,15 |
| 6 | C | `0x0C00` | 3 AN | *(2)* | PC10,11 |
| 7 | C | `0x00FF` | 0 IN | 2 DOWN | PC0–7 (keypad cols) |
| 8 | C | `0xD200` | 0 IN | 1 UP | PC9, **PC12**, PC14, PC15 |
| 9 | D | `0x3F0C` | 3 AN | *(1)* | PD2,3,8,9,10,11,12,13 |
| 10 | D | `0xC003` | 0 IN | 2 DOWN | PD0,1,14,15 |
| 11 | D | `0x00F0` | 0 IN | 1 UP | PD4,5,6,7 (PD6 = panel reset) |
| 12 | E | `0x0868` | 3 AN | *(1)* | PE3,5,6,11 |
| 13 | E | `0x0780` | 0 IN | 2 DOWN | PE7,8,9,10 (FSMC data) |
| 14 | E | `0x0010` | 0 IN | 1 UP | PE4 |
| 15 | E | `0xF007` | 0 IN | **0 NONE** | PE0,1,2,12–15 (keypad rows) |

Deliberately **not** parked: PA13/PA14 (SWD), **PC8 (keypad backlight)**, PC13.
The exclusion list is hand-maintained, so treat this table as authoritative.

### 11.2 `FUN_0800e610` — restore. It is **not** FSMC

```
GPIOA->MODER/OSPEEDR/OTYPER/PUPDR = buf[0x00/0x08/0x04/0x0C]
GPIOB (base+0x400) = buf[0x10/0x18/0x14/0x1C]
GPIOC (base+0x800) = buf[0x20/0x28/0x24/0x2C]
GPIOD (base+0xC00) = buf[0x30/0x38/0x34/0x3C]
GPIOE              = buf[0x40/0x48/0x44/0x4C]
```

Only those four registers, only ports A–E. **AFRL/AFRH, ODR, IDR and LCKR are
neither saved nor restored.** No FSMC address appears in either function.

> **§7 correction:** "stock saves and restores the FSMC registers across STOP
> (`FUN_0800e610`)" is **wrong**. Delete that requirement. §4's "stock touches
> no FSMC register in its sleep cycle" was right all along.

### 11.3 PA1 and PC8 during stock's sleep

Both backlights are already at a **hard, driven zero with their timers
disabled** before `FUN_0800e304` is entered. The chain:

1. Low-power task, `0x0800E1D2`: `movs r0,#1; bl FUN_0801a876` → target level B.
2. **Level B is the constant 0.** `FUN_0801a770` inits it to 0; the only writer
   is `FUN_0801a838(A,B)`, whose caller `FUN_0801c10e` sources B from
   `FUN_0800e8aa()` / `FUN_0800e8ca()` — both decompile to `return 0`.
3. Backlight task `0x0801a93c` ramps in steps of 7 (40 ms/step downward,
   10 ms upward, 100 ms idle; skipped while `FUN_0801734a()` audio is busy).
4. `0x0800E1E6` spins on `FUN_0801a92a()` (state==5, "still fading") until the
   fade **completes** — only then `0x0800E1F4 bl FUN_0800e304`.
5. Final step is `FUN_0801a9fa(0)` / `FUN_0801ab1e(0)`:

```c
/* FUN_0801a9fa(0)  — LCD */            /* FUN_0801ab1e(0) — keypad */
TIM_Cmd(TIM2, DISABLE);                 TIM_Cmd(TIM8, DISABLE);
GPIO_Init(GPIOA, {0x0002, OUT,2,PP,PD});GPIO_Init(GPIOC, {0x0100, OUT,2,PP,PD});
GPIO_ResetBits(GPIOA, 0x0002);          GPIO_ResetBits(GPIOC, 0x0100);
```

6. `FUN_0800e438` then flips PA1 `MODER` 1→0. PUPDR was *already* pull-down
   (every `FUN_0801a9fa` path sets PuPd=2), so only the mode changes.

**Answer:**

| | State during stock's STOP |
|---|---|
| **PA1** | input, **pull-down**, high-Z. `TIM2 CEN = 0`. Not PWM, not driven. |
| **PC8** | **output push-pull, driven LOW**. `TIM8 CEN = 0`. Never re-parked. |
| PC12 | `GPIO_SetBits` first, then group 8 makes it **input + pull-up** (~40 kΩ) |
| PD6 | input + pull-up — reset stays de-asserted, matching §4 |

**What restores them, in order inside `FUN_0800e304`:**

```
SysTick->CTRL &= ~1
FUN_0801c246(1,1)      PWR_EnterSTOPMode(LPDS=1, WFI)   [PDDS=0 -> STOP]
SysTick->CTRL |= 1
FUN_0800e680()         disable EXTI9
FUN_0800e610()         MODER/OTYPER/OSPEEDR/PUPDR for A-E
                       -> PA1 back to OUTPUT, ODR bit1 still 0 = driven low
FUN_0800e400()         HSE on -> wait -> PLL on -> wait -> SYSCLK=PLL
FUN_0800fb2c(0,5)      panel-on walk
...
FUN_0801a876(0)   @0x0800E3E4   -> state 2, ramp up to level A
```

The actual relight is the ramp task's next 10 ms tick calling
`FUN_0801a9fa(7)`, which **fully rebuilds the PWM**: `TIM_TimeBaseInit` (PSC 9,
ARR 3000) → `TIM_OC2Init` (PWM2 `0x70`, `CC2E`, `CC2P`=Low, CCR2=pct·30) →
`TIM_OC2PreloadConfig` → `GPIO_Init` PA1→AF → `TIM_Cmd(ENABLE)`.
`FUN_0801ab1e(7)` does the same on TIM8 **plus `TIM_CtrlPWMOutputs(TIM8,
ENABLE)`** for MOE. 100% is reached in 15 steps ≈ 150 ms.

### 11.4 Does the sleep path call `FUN_0801a8c2` or the ramp?

- **`FUN_0801a8c2`: no.** Exactly one caller in the whole 512 KB image —
  `FUN_0800e292`, the **STANDBY/shutdown** path (`PWR_EnterSTANDBYMode` then an
  AIRCR `SYSRESETREQ`), reached from case 10 of the low-power task's `tbb`
  dispatch at `0x0800E17C`. Verified by Ghidra xrefs *and* an exhaustive
  BL/B.W decode of the image.
- **The ramp: yes, but never from `FUN_0800e304`.** It is driven by the
  backlight task and requested from `0x0800E1D2` (down, before sleep) and
  `0x0800E3E4` (up, on wake).

`FUN_0801a876` has **four** callers (`0x0800E148`, `0x0800E1D2`, `0x0800E3E4`,
`0x0801DD3C`). `xrefs.py` reports one, because `0x0800E0D4`–`0x0800E213` is a
region Ghidra never made into a function. **Do not trust `xrefs.py` alone in
this module.**

### 11.5 §8 resolved — and §2's premise falsified

Both earlier agents were right about *different paths*: `FUN_0801a8c2` is the
shutdown hard-zero, `FUN_0800e438` is the sleep park. What neither found is
that **the sleep path also reaches a hard zero**, via the ramp, before
`FUN_0800e304` runs.

So on this unit stock holds PA1 weakly low and PC8 strongly low, **with TIM2 and
TIM8 both disabled**, for an unbounded STOP — and the backlight comes back.

> **§2 and §6 corrections.** "The backlight driver IC latches off whenever its
> PWM dim input stops receiving edges. Only removing power clears it" cannot be
> the mechanism. The §3 table rows are misattributed, and **`BRIGHT_ASLEEP 3` is
> not required by the hardware.**

I audited every other callee of `FUN_0800e304` for backlight side effects and
there are none: `FUN_0800ec2c` = `GPIO_SetBits(GPIOC,PC12)`;
`FUN_0801bd74`/`bd80` = ADC1 (`0x40012000`) off/on; `FUN_0801c1cc`/`c1d4` = ADC
channel off/on; `FUN_0801bf02`/`bee6` = `GPIO_ResetBits(GPIOA,PA10)` / FunLight
restore; `FUN_08010f6c`/`08010dfa` = keypad row EXTI disable/enable;
`FUN_08006732` = PA3 EXTI3 enable; `FUN_0800e6ae`/`e680` = PA9 EXTI9
enable/disable. The TIM2 base `0x40000000` and TIM8 base `0x40010400` appear as
literals nowhere in the `0x0800Exxx` sleep module.

### 11.6 The one divergence left: PWM frequency

`FUN_08011394` = `RCC_GetClocksFreq(&c)` returning field `+4` =
**HCLK = 120 MHz**. So `FUN_0801a9fa` computes
`PSC = (120e6 >> 1)/6e6 - 1 = 9` and `ARR = 3000`:
TIM2 input 60 MHz → 6 MHz → **2.000 kHz, unconditionally, at every brightness**.
(That also independently confirms our `BL_TIMER_HZ 60000000`.)

Ours runs `PSC=0`, `arr = 60e6/hz`, `hz` capped at `BL_MAX_HZ 30000` for
pct ≥ 15 — and §2's failed-state snapshot `CCR2=1000/2000` **is 30 kHz**.

**Leading hypothesis, not proven by the image.** Cheapest next experiment:
pin `PSC=9 / ARR=3000 / CCR2=pct*30` for all 1..99, set `BRIGHT_ASLEEP 0` so the
endpoint path runs, and replicate stock exactly. One free difference to remove
while you're there: our `pin_out()` never writes PUPDR, whereas stock sets
PA1/PC8 PuPd = pull-down in *every* configuration (OUT, AF and IN).

---

## 12. Making STOP viable — the IWDG first, the RTC second

### 12.1 The blocker is not SysTick

`src/safety.c`: `IWDG_PR = 6` (÷256), `IWDG_RLR = 1000`. LSI ≈ 32 kHz →
32000/256 = 125 Hz → **8.0 s timeout**. RM0033: the IWDG is functional in Stop
and Standby. `lowpower_stop()` arms **no timed wake source** — only wake.c's
EXTI edges.

So an idle remote sits in STOP until the IWDG fires at 8 s, resets, and re-runs
`lcd_hw_init()` → `panel_init()` → backlight → LVGL splash. **That is a visible
flash every 8 s**, with `k_uptime_get()` and `last_active` restarting each time —
which produces the "stale idle delta" symptom as a *consequence of the reset*.

Two things support this over the SysTick story. `power.c`'s
`display_blanking_on()` sits behind an `else if` that is skipped while `asleep`
is true, so re-sleep cannot toggle the panel. And §10 already records that
`st-flash` sets DBGMCU `DBG_IWDG_STOP`, freezing the IWDG whenever a probe
attaches — so the failure vanishes under a debugger, matching `power.c`'s note
that a stopped CPU "has not proven attachable over SWD".

**Falsify it in 30 seconds, no meter:** set `USE_STOP_MODE 1`, walk away 30 s,
read `safety_reset_cause()` (already in the UI) and boot attempts at
`0x2001FF84`. `WATCHDOG` confirms it.

This makes the RTC wakeup timer **mandatory**, not a nicety.

### 12.2 STM32F2 RTC constraints

- F2 has the **first-generation** calendar RTC: `TR DR CR ISR PRER WUTR CALIBR
  ALRMAR ALRMBR WPR TSTR TSDR TAFCR BKP0..19R`. **No `SSR`, no `SHIFTR`, no
  `CALR`** — those are F4 additions. Resolution is therefore **1 second**, which
  is fine: the only consumer is a 30 s idle timer. *(Load-bearing — confirm
  against RM0033 §22.6.)*
- RTC wakeup is **EXTI line 22**.
- Stock **never touches the RTC**: zero references to `0x40002800`, `0x40023870`
  (`RCC_BDCR`) or `0x40002850` in the image.
- **Use LSI.** `FUN_0800e438` parks PC14/PC15 (OSC32_IN/OUT) as pulled-up
  inputs — you would not do that to a crystal. Keep LSE behind a `#define` and
  do **not** autodetect by asserting `LSEON`: if those pins are a strap, you
  would drive PC15.
- LSI is already running — `safety.c`'s `IWDG_KR = 0xCCCC` starts it.
- LSI is nominal 32 kHz but spec'd ~17–47 kHz, so `LSI_HZ` is a **calibration
  knob**, not a constant.

### 12.3 Code — all of it in `src/lowpower.c`

```c
/* ---- src/lowpower.c: add below the existing defines ---- */

#define RTC_BASE   0x40002800u
#define RTC_TR     REG32(RTC_BASE + 0x00)
#define RTC_CR     REG32(RTC_BASE + 0x08)
#define RTC_ISR    REG32(RTC_BASE + 0x0C)
#define RTC_PRER   REG32(RTC_BASE + 0x10)
#define RTC_WUTR   REG32(RTC_BASE + 0x14)
#define RTC_WPR    REG32(RTC_BASE + 0x24)
#define RTC_ISR_A  (RTC_BASE + 0x0C)          /* for spin() */

#define RCC_BDCR   REG32(RCC_BASE + 0x70)
#define RCC_CSR    REG32(RCC_BASE + 0x74)
#define EXTI_EMR   REG32(0x40013C04)
#define EXTI_RTSR  REG32(0x40013C08)

#define PWR_CR_DBP        (1u << 8)
#define BDCR_RTCSEL_LSI   (2u << 8)
#define BDCR_RTCSEL_MSK   (3u << 8)
#define BDCR_RTCEN        (1u << 15)
#define CSR_LSION         (1u << 0)
#define CSR_LSIRDY        (1u << 1)
#define ISR_WUTWF         (1u << 2)
#define ISR_RSF           (1u << 5)
#define ISR_INITF         (1u << 6)
#define ISR_INIT          (1u << 7)
#define ISR_WUTF          (1u << 10)
#define CR_WUTE           (1u << 10)
#define CR_WUTIE          (1u << 14)
#define EXTI_RTC_WKUP     (1u << 22)

/* Nominal LSI. Spec'd 17-47 kHz on F2, so this is a tuning knob: if the 30 s
 * idle timeout measures wrong on hardware, adjust it here. */
#define LSI_HZ        32000u

/* Sleep in chunks shorter than the IWDG period (safety.c: PR=6, RLR=1000,
 * ~8 s) so main.c's safety_watchdog_feed() still runs. This is the whole
 * reason the RTC is here; correcting uptime is the bonus. */
#define STOP_CHUNK_S  4u

static int64_t stop_ms;        /* ms elapsed while SysTick was stopped */
static uint32_t sod_entry;

int64_t lowpower_stopped_ms(void) { return stop_ms; }

static void rtc_unlock(void) { RTC_WPR = 0xCA; RTC_WPR = 0x53; }
static void rtc_lock(void)   { RTC_WPR = 0xFF; }

static bool spin(uint32_t addr, uint32_t mask, bool want)
{
	for (int i = 0; i < CLK_TIMEOUT; i++) {
		if (!!(REG32(addr) & mask) == want) { return true; }
	}
	return false;   /* never hang the remote on a dead clock */
}

/* Seconds-of-day from the BCD time register. The F2 RTC has no sub-second
 * register, so 1 s is the best available - plenty for a 30 s idle timer. */
static uint32_t rtc_sod(void)
{
	uint32_t tr = RTC_TR;
	return ((((tr >> 20) & 0x3) * 10 + ((tr >> 16) & 0xF)) * 3600u)
	     + ((((tr >> 12) & 0x7) * 10 + ((tr >>  8) & 0xF)) * 60u)
	     +   (((tr >>  4) & 0x7) * 10 + ( tr        & 0xF));
}

/* TR/DR are shadow registers clocked by RTCCLK; after STOP they hold the
 * pre-STOP time until RSF re-asserts. Reading without this is silently wrong. */
static void rtc_sync(void)
{
	rtc_unlock();
	RTC_ISR &= ~ISR_RSF;
	rtc_lock();
	(void)spin(RTC_ISR_A, ISR_RSF, true);
}

bool lowpower_rtc_init(void)
{
	RCC_APB1ENR |= RCC_APB1ENR_PWREN;
	PWR_CR |= PWR_CR_DBP;                 /* backup domain writable */

	RCC_CSR |= CSR_LSION;                 /* already on: the IWDG started it */
	if (!spin(RCC_BASE + 0x74, CSR_LSIRDY, true)) { return false; }

	/* RTCSEL is write-once until a backup-domain reset, and the backup domain
	 * survives warm resets - so if it is already ours, leave the calendar
	 * alone and keep counting straight through a watchdog reset. */
	if ((RCC_BDCR & (BDCR_RTCSEL_MSK | BDCR_RTCEN))
	    != (BDCR_RTCSEL_LSI | BDCR_RTCEN)) {
		RCC_BDCR = (RCC_BDCR & ~BDCR_RTCSEL_MSK) | BDCR_RTCSEL_LSI;
		RCC_BDCR |= BDCR_RTCEN;

		rtc_unlock();
		RTC_ISR |= ISR_INIT;
		if (!spin(RTC_ISR_A, ISR_INITF, true)) { rtc_lock(); return false; }
		/* ck_apre = LSI/128 = 250 Hz, ck_spre = /250 = 1 Hz.
		 * PREDIV_S must be written before PREDIV_A, in two writes. */
		RTC_PRER = (LSI_HZ / 128u) - 1u;
		RTC_PRER = (127u << 16) | ((LSI_HZ / 128u) - 1u);
		RTC_CR &= ~(1u << 6);              /* FMT = 24 h */
		RTC_ISR &= ~ISR_INIT;
		rtc_lock();
	}

	/* Wake from STOP on the wakeup timer: EXTI line 22, event only. EMR rather
	 * than IMR means no ISR and no NVIC entry - WFE just returns and the main
	 * loop carries on and feeds the watchdog. */
	EXTI_RTSR |= EXTI_RTC_WKUP;
	EXTI_EMR  |= EXTI_RTC_WKUP;
	return true;
}

static void rtc_wut_arm(uint32_t secs)
{
	rtc_unlock();
	RTC_CR &= ~CR_WUTE;
	(void)spin(RTC_ISR_A, ISR_WUTWF, true);
	RTC_WUTR = secs - 1u;                  /* period = (WUTR + 1) x 1 s */
	RTC_CR = (RTC_CR & ~7u) | 4u;          /* WUCKSEL = 10x -> ck_spre */
	RTC_ISR &= ~ISR_WUTF;
	RTC_CR |= CR_WUTE | CR_WUTIE;
	rtc_lock();
}
```

Then three insertions into the existing `lowpower_stop()`:

```c
void lowpower_stop(void)
{
	stops++;
	RCC_APB1ENR |= RCC_APB1ENR_PWREN;

	rtc_wut_arm(STOP_CHUNK_S);          /* <-- ADD: bounded sleep */
	sod_entry = rtc_sod();              /* <-- ADD */

	PWR_CR |= PWR_CR_CWUF;
	EXTI_PR = 0xFFFFFFFFu;              /* already covers PR22 */
	...  /* unchanged through SEV; WFE; WFE */
	SCB_SCR &= ~SCR_SLEEPDEEP;

	/* <-- ADD: WUTF latched high means EXTI22 never edges again. */
	rtc_unlock(); RTC_ISR &= ~ISR_WUTF; rtc_lock();
	rtc_sync();
	{
		uint32_t now = rtc_sod();
		uint32_t d = (now >= sod_entry) ? now - sod_entry
		                                : now + 86400u - sod_entry;
		stop_ms += (int64_t)d * 1000;
	}

	/* --- existing HSE -> PLL -> 120 MHz restore, unchanged --- */
```

`src/lowpower.h`:

```c
bool    lowpower_rtc_init(void);
int64_t lowpower_stopped_ms(void);   /* ms spent with SysTick stopped */
```

`src/power.c` — one helper, then swap all four `k_uptime_get()` call sites
(lines 57, 74, 85, 98) for it:

```c
/* SysTick stops in STOP mode, so k_uptime_get() undercounts by exactly the
 * time we were stopped. The RTC keeps that account. */
static inline int64_t pm_now(void) { return k_uptime_get() + lowpower_stopped_ms(); }
```

`main.c`: call `lowpower_rtc_init()` before `power_init()` and refuse to STOP if
it returns false (fall back to `wake_wait`).

**Skipped:** a Zephyr `counter`/RTC driver, `CONFIG_PM`, tick injection into the
kernel, LSI-vs-HSE hardware calibration via TIM5. Nothing outside `power.c`
reads this timebase, so a local `pm_now()` is the whole fix. Add calibration
only if a measured 30 s idle is visibly wrong.

### 12.4 The one runnable check

The BCD decode and the midnight wrap are the only non-trivial logic, and both
are pure — so test them on the host, no board and no framework:

```c
/* tests/rtc_sod.c   cc -o t tests/rtc_sod.c && ./t */
#include <assert.h>
#include <stdint.h>
static uint32_t sod(uint32_t tr) {
	return ((((tr >> 20) & 0x3) * 10 + ((tr >> 16) & 0xF)) * 3600u)
	     + ((((tr >> 12) & 0x7) * 10 + ((tr >>  8) & 0xF)) * 60u)
	     +   (((tr >>  4) & 0x7) * 10 + ( tr        & 0xF));
}
static uint32_t delta(uint32_t a, uint32_t b) {
	return (b >= a) ? b - a : b + 86400u - a;
}
int main(void) {
	assert(sod(0x00000000) == 0);
	assert(sod(0x00120000) == 12u * 3600u);
	assert(sod(0x00235959) == 23u * 3600u + 59u * 60u + 59u);
	assert(delta(sod(0x00100000), sod(0x00100004)) == 4);
	assert(delta(sod(0x00235958), sod(0x00000002)) == 4);   /* midnight */
	return 0;
}
```

### 12.5 Bonus from the same trace: interrupt-driven touch wake

`FUN_08006732` → `FUN_080068cc` sets PA4 analog, PA5 IN, **PA3 IN + pull-up**,
**PA6 OUT push-pull driven LOW**, then enables **EXTI3 falling** (NVIC ch 9).
That is stock's touch wake, and it would replace our 250 ms `touch_read()` poll.
It must use PA3 — PA4/PA6 are in use and EXTI5 is taken by the accelerometer.

Also worth knowing: stock's STOP wake set is keypad rows (EXTI 0,1,2,12–15) +
**PA9 rising** (OTG_FS VBUS — "woke on charger", EXTI9) + PA3 falling. **EXTI5
(accel INT1) is never enabled and `FUN_0800e438` puts PE5 in analog mode** — so
stock deliberately drops accelerometer wake during sleep. Ours works only
because we do not park pins.
