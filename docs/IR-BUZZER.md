# Em250 Reflash

> Auto-saved from overnight research agent. Static analysis only — nothing here was tested on hardware.

**Summary:** Both subsystems are fully traced, and the headline result is that HARDWARE.md has them swapped. The IR transmitter is the SPI2 + DMA1_Stream4 path: PB0 (TIM3_CH3, AF2) generates a 50%-duty carrier at 60 MHz/(ARR+1), and PB15 (SPI2_MOSI, AF5) carries the mark/space envelope as DMA bursts of N copies of a single fixed RAM byte (0xFF = mark, 0x00 = space) with memory-increment off — so one SPI byte at PCLK1/64 = 468.75 kHz is the IR time quantum, exactly 256/15 = 17.0667 us. The beeper is not a sample path at all: it is a plain 50% square wave on PB7 (TIM4_CH2, AF2) at 400 Hz or 500 Hz, gated on for 20 ms per key click by a uC/OS-II "Buzzer Task"; there is no DAC (base 0x40007400 has zero references image-wide), no I2S, and no sample buffer anywhere. TIM1 and TIM5 are never touched by the stock firmware — a whole-image literal scan plus the fact that only 3 movt instructions exist (none building a peripheral base) makes that conclusive, so HARDWARE.md's "PA0 = TIM5_CH1 PWM" leads nowhere. An IR code is a 13-byte header + u16 timing table + a stream of 1-based indices into that table, read from the external SPI NOR at (24-bit address + 0x1000), alternating mark/space starting with a mark. There is no IR learning or capture path on the remote: no timer input-capture configuration exists, no "learn"/"capture" strings, and Integration Designer ships IRCapEn.dll / IREng32.dll to do it on the PC.

**Open questions:**
- The exact gate between PB0 (carrier) and PB15 (envelope) is inferred, not proven. The evidence for an external AND-style gate is circumstantial but consistent: two independent lines feed one emitter, the carrier idles LOW when off, and carrier_arr == 0 parks PB0 statically HIGH (which only makes sense as 'hold the gate open'). A scope on PB0 and PB15 during ir_selftest(), plus a photodiode on the LED, settles it — and would tell you whether the LED needs both lines high or some other combination.
- Header bytes 0x09, 0x0A and 0x0C are never read by the transmitter. They are presumably metadata for the PC tool or the code database (protocol id, toggle-bit handling, etc.). Unknown, and unknowable from the transmit path alone — an external-flash dump plus a matching Integration Designer project would resolve them.
- What PB14 actually feeds is unproven. The firmware calls it the 'RF' path ('Fmt:Sys RF'), it is a plain active-high GPIO enable, and the envelope goes out on the same PB15 with no carrier. Whether that is the CC1150 or something else, and how PB15 reaches its data input, cannot be checked while the radio unit is USB-only.
- The beeper frequency. My analysis says stock emits 400 Hz or 500 Hz (TIM4 tick = 60 MHz/120 = 500 kHz, ARR 1249/999), because RTI divided HCLK by 1e6 instead of dividing the timer clock. The same idiom in the IR path correctly uses HCLK>>1, which is what makes the 40.000 kHz carrier come out exactly round — so the beeper really is off by 2. One ear or scope test on beep_click() confirms 400 Hz vs 800 Hz.
- PA0's real function is unresolved. TIM5 is definitively never touched by stock firmware, so HARDWARE.md's 'PA0 = TIM5_CH1 PWM' should be struck. PA0 = WKUP1 is the obvious candidate given the STOP/STANDBY code, but I did not verify that the firmware sets PWR_CSR EWUP — worth 10 minutes on FUN_0801c29e.
- The size of stock's inter-burst software gap was not measured. It matters only if a receiver rejects our frames: our version has a smaller gap than stock, so if anything our timing is closer to nominal than RTI's. If a stubborn device only accepts stock's slightly-stretched marks, that is the knob to look at.

---

# IR transmit and beeper (STM32F205, reversed from stock RTI)

Two corrections to `HARDWARE.md` up front, because they invert what is written there:

* **SPI2 + DMA1_Stream4 is the IR/RF envelope generator, not sample playback.** There is
  no DAC in this firmware (base `0x40007400`: zero references image-wide), no I2S
  (`SPI_Init`, never `I2S_Init`; PB12/PB13 never AF-configured), and no sample buffer.
* **The beeper is a plain square wave on PB7 / TIM4_CH2**, 400 Hz or 500 Hz, 20 ms per
  click.

Also: `src/t2i_regs.h:98` labels TIM3_CH3/PB0 as `speaker/beeper`. That pin is the **IR
carrier**.

## Pin map (additions / corrections)

| Pin | AF | Function | Was in HARDWARE.md |
|---|---|---|---|
| **PB0** | 2 | **TIM3_CH3 — IR carrier**, 50 % duty, idles LOW = off | "RF modulation / tone" |
| **PB15** | 5 | **SPI2_MOSI — IR/RF mark-space envelope** | "SPI2_MOSI (purpose TBD)" |
| **PB14** | — | **RF path enable, GPIO out, active HIGH.** Hold LOW for IR | "write-only SPI2 chip-select" |
| **PB7** | 2 | **TIM4_CH2 — beeper**, 50 % square, idles LOW | absent |
| PA0 | — | **TIM5 is never touched by stock firmware.** Not a firmware-driven PWM | "TIM5_CH1 PWM — purpose TBD" |

Clock tree, pinned down from `RCC_PLLCFGR = 0x05403C19` (pool `0x0801150C`) and
`CFGR |= 0x8000` then `|= 0x1400`: HSE 25 MHz, PLLM 25 / PLLN 240 / PLLP 2 / PLLQ 5 →
**SYSCLK = HCLK = 120 MHz, APB1 = 30 MHz, APB2 = 60 MHz, USB = 48 MHz**. APB1 timer
clock = 60 MHz.

The stock app's vector table is at **0x08004000** (`NVIC_SetVectorTable(0x08000000,
0x4000)` at `0x0801da5c`); `0x08000000` is a ~16 KB bootloader. In the app table,
DMA1_Stream4 (IRQ 15), TIM3 (29), TIM4 (30) and SPI2 (36) all point at an
"unexpected IRQ" stub (`movs r0,#<irq>; b 0x0801D982`). **Both subsystems are fully
polled.**

## How the IR emitter works

The emitter needs **two** lines and they are combined outside the MCU:

```
PB0  TIM3_CH3 ── 50 % carrier, f = 60e6/(ARR+1) ──┐
                                                  ├─ gate ─> IR LED driver
PB15 SPI2_MOSI ── mark/space envelope ────────────┘
```

* Carrier idle state is **LOW** (`FUN_080099d6` stops TIM3, makes PB0 a GPIO output and
  drives it low), so the gate is **active-high**.
* `FUN_08009a78(0)` parks PB0 **HIGH** instead — stock's "no carrier" mode, gate held
  open, LED follows the envelope directly. Codes with `carrier_arr == 0` use this.

The envelope is the clever bit. SPI2 is a **1-line-TX master** (`Direction = 0xC000`)
at **PCLK1/64 = 468.75 kHz**, and SPI2_SCK is *never* routed to a pin — SPI2 is used
purely as a byte-paced pattern generator. Each mark or space is **one DMA1_Stream4
transfer of N copies of a single fixed RAM byte with memory-increment turned OFF**
(`0xFF` = mark, `0x00` = space).

**One SPI byte = 8 / 468750 s = 256/15 µs = 17.0667 µs. That is the IR time quantum.**

Two independent constants confirm it: the SysCode framer's half-bit is `0x18` = 24
bytes = **409.60 µs** and its trailer is `0x494` = 1172 bytes = **20.0004 ms**. Both
land on round numbers.

Between bursts the code busy-waits on `DMA_FLAG_TCIF4` (`0x20000020`). SPI stays
enabled, so in 1-line-TX mode MOSI **holds the last bit** during the software gap —
gaps stretch the burst that just ended rather than glitching it.

### IR code format

Stored in the **external SPI NOR** (S25FL256S on SPI3), addressed by a 24-bit value.
`FUN_08013d66(dst, addr, len)` → `FUN_0800d296(dst, addr + 0x1000, len)`, so every IR
address is relative to a `0x1000` partition base. Callers build it as
`key[3]<<16 | key[4]<<8 | key[5]` (`FUN_0801b394`).

| Off | Size | Field |
|---|---|---|
| `0x00` | u16 | carrier ARR for TIM3 (`0` = no carrier, PB0 parked high) |
| `0x02` | u16 | `once_len` — index bytes in the lead-in section |
| `0x04` | u16 | `repeat_len` — index bytes in the repeat section |
| `0x06` | u8  | `n_timings` — u16 entries in the timing table (`0` ⇒ send nothing) |
| `0x07` | u8  | `repeat_count` — extra repeat passes (only if `repeat_len != 0`) |
| `0x08` | u8  | flags1, bit0 = may repeat while the key is held |
| `0x09` | u8  | never read by the transmitter |
| `0x0A` | u8  | never read by the transmitter |
| `0x0B` | u8  | flags2, bit0 = send over **RF** (PB14 high, no carrier) instead of IR |
| `0x0C` | u8  | never read by the transmitter |
| `0x0D` | u16 × `n_timings` | timing table, duration in 17.0667 µs SPI bytes |
| then | u8 × (`once_len`+`repeat_len`) | index stream, **1-based** into the timing table |

The indices are 1-based because the lookup base is `work+0x21E` while the table is
copied to `work+0x220`. Playback alternates mark/space **starting with a mark**; after
the first full pass it loops back to `index[once_len]`.

### No learning path

No `TIM_ICInit`, no `TIM_GetCapture`, no CCMRx write with a non-zero `CCxS` field
anywhere. No `learn` / `capture` strings. Integration Designer ships `IRCapEn.dll` and
`IREng32.dll` — capture is a PC function. Don't budget firmware for a learn mode.

## Register additions for `src/t2i_regs.h`

```c
/* ---- TIM4 (beeper, CH2 -> PB7 AF2). Timer clock = 60 MHz (APB1 /4, x2). ---- */
#define TIM4_BASE      0x40000800u
#define TIM4_CR1       REG32(TIM4_BASE + 0x00)
#define TIM4_EGR       REG32(TIM4_BASE + 0x14)
#define TIM4_CCMR1     REG32(TIM4_BASE + 0x18)
#define TIM4_CCER      REG32(TIM4_BASE + 0x20)
#define TIM4_PSC       REG32(TIM4_BASE + 0x28)
#define TIM4_ARR       REG32(TIM4_BASE + 0x2C)
#define TIM4_CCR2      REG32(TIM4_BASE + 0x38)

/* TIM3 is already declared -- it is the IR CARRIER, not the beeper. Needs EGR: */
#define TIM3_EGR       REG32(TIM3_BASE + 0x14)

/* ---- SPI2 (IR/RF envelope on PB15/MOSI). APB1 = 30 MHz, /64 = 468.75 kHz. ---- */
#define SPI2_BASE      0x40003800u
#define SPI2_CR1       REG32(SPI2_BASE + 0x00)
#define SPI2_CR2       REG32(SPI2_BASE + 0x04)
#define SPI2_SR        REG32(SPI2_BASE + 0x08)
#define SPI2_DR_ADDR   (SPI2_BASE + 0x0C)
#define SPI2_I2SCFGR   REG32(SPI2_BASE + 0x1C)
#define SPI_CR1_SPE     (1u << 6)
#define SPI_CR2_TXDMAEN (1u << 1)
#define SPI_SR_BSY      (1u << 7)
/* 1-line TX master, 8-bit, CPOL=1 CPHA=0, SSM+SSI, BR=/64, MSB first.
 * Exactly stock's SPI_Init struct (FUN_08009a2c / FUN_08016a4e). */
#define SPI2_CR1_IR    0xC32Eu

/* ---- DMA1 Stream4 = SPI2_TX (channel 0) ---- */
#define DMA1_BASE      0x40026000u
#define DMA1_HISR      REG32(DMA1_BASE + 0x04)
#define DMA1_HIFCR     REG32(DMA1_BASE + 0x0C)
#define DMA1_S4CR      REG32(DMA1_BASE + 0x70)
#define DMA1_S4NDTR    REG32(DMA1_BASE + 0x74)
#define DMA1_S4PAR     REG32(DMA1_BASE + 0x78)
#define DMA1_S4M0AR    REG32(DMA1_BASE + 0x7C)
#define DMA_SxCR_EN    (1u << 0)
#define DMA_HISR_TCIF4 (1u << 5)
#define DMA_HIFCR_S4   0x3Du        /* FE|DME|TE|HT|TC for stream 4 */
/* ch0, mem->periph, byte/byte, no increment, normal mode, priority very high */
#define DMA1_S4CR_IR   0x00030040u
```

## `src/beep.c`

```c
/*
 * beep.c -- T2i beeper. PB7 = TIM4_CH2 (AF2), 50 % square wave, gated on for
 * 20 ms per key click. Reversed from stock RTI: FUN_08007638 (on),
 * FUN_080076f8 (off), buzzer task body at 0x080075C4.
 *
 * NOT a DAC/I2S/sample path. There is no DAC in the stock image at all.
 */
#include <zephyr/kernel.h>
#include "t2i_regs.h"

#define BEEP_PORT GPIO_PORT_B
#define BEEP_PIN  7

/* Stock sets PSC = HCLK/1e6 - 1 = 119, so the tick is 60 MHz/120 = 500 kHz --
 * RTI divided HCLK where they should have divided the timer clock (the IR path
 * correctly uses HCLK>>1). ARR 1249 -> 400 Hz, ARR 999 -> 500 Hz. Keeping
 * 500 kHz sounds identical to stock; PSC 59 gives the 800/1000 Hz they seem to
 * have intended. ponytail: hard-coded tick, make it Kconfig only if you A/B it. */
#define BEEP_PSC     119u
#define BEEP_TICK_HZ 500000u

static void beep_pin_mode(uint32_t mode)
{
	GPIO_MODER(BEEP_PORT) = (GPIO_MODER(BEEP_PORT) & ~(3u << (BEEP_PIN * 2)))
	                      | (mode << (BEEP_PIN * 2));
}

void beep_off(void)
{
	TIM4_CR1 = 0;
	beep_pin_mode(GPIO_MODE_OUTPUT);
	GPIO_BSRR(BEEP_PORT) = 1u << (BEEP_PIN + 16);   /* PB7 low = silent */
}

void beep_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(BEEP_PORT);
	RCC_APB1ENR |= (1u << 2);                       /* TIM4 */

	GPIO_AFRL(BEEP_PORT) = (GPIO_AFRL(BEEP_PORT) & ~(0xFu << (BEEP_PIN * 4)))
	                     | (2u << (BEEP_PIN * 4));  /* AF2 = TIM4 */
	GPIO_OSPEEDR(BEEP_PORT) |= (2u << (BEEP_PIN * 2));            /* 50 MHz, as stock */
	GPIO_PUPDR(BEEP_PORT) = (GPIO_PUPDR(BEEP_PORT) & ~(3u << (BEEP_PIN * 2)))
	                      | (2u << (BEEP_PIN * 2));               /* pull-down, as stock */
	beep_off();
}

void beep_on(uint32_t hz)
{
	uint32_t arr = BEEP_TICK_HZ / hz - 1u;

	TIM4_CR1   = 0;
	TIM4_PSC   = BEEP_PSC;
	TIM4_ARR   = arr;
	TIM4_CCR2  = arr / 2u;                                         /* 50 % duty */
	TIM4_CCMR1 = (TIM4_CCMR1 & ~0xFF00u) | TIM_CCMR_PWM1_PE(12);   /* OC2M=PWM1, OC2PE */
	TIM4_CCER  = (TIM4_CCER & ~0xF0u) | (1u << 4) | (1u << 5);      /* CC2E, CC2P=low */
	TIM4_EGR   = TIM_EGR_UG;
	beep_pin_mode(GPIO_MODE_AF);
	TIM4_CR1   = TIM_CR1_CEN;
}

/* Stock key click. Blocking -- call from a thread, not an ISR. */
void beep_click(void)
{
	beep_on(400);
	k_msleep(20);
	beep_off();
}
```

## `src/ir.c`

```c
/*
 * ir.c -- T2i IR transmit. Reversed from stock RTI "IR Xmit" (init FUN_080097c4,
 * send FUN_080098c8, per-burst DMA FUN_08009b4a, diag test FUN_0800981a).
 *
 * The emitter takes TWO lines, combined outside the MCU:
 *   PB0  = TIM3_CH3 (AF2)  -- 50 % carrier, idles LOW = off (gate is active high)
 *   PB15 = SPI2_MOSI (AF5) -- mark/space envelope
 * SPI2 is a 1-line-TX master at PCLK1/64 = 468.75 kHz, so one SPI byte is
 * 8/468750 s = 256/15 us = 17.0667 us -- the IR time quantum. A mark is a
 * DMA1_Stream4 burst of N copies of 0xFF out of ONE fixed RAM byte (memory
 * increment OFF); a space is N copies of 0x00.
 *   PB14 = RF path enable, active HIGH -- must stay LOW for IR.
 * Fully polled: stock's own vector table (0x08004000) routes DMA1_Stream4,
 * TIM3 and SPI2 to its "unexpected IRQ" reporter.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <errno.h>
#include "t2i_regs.h"

#define IRP            GPIO_PORT_B
#define IR_CARRIER_PIN 0     /* PB0  TIM3_CH3 AF2 */
#define IR_ENV_PIN     15    /* PB15 SPI2_MOSI AF5 */
#define IR_RF_PIN      14    /* PB14 RF enable, active high */

#define IR_TIMER_HZ    60000000u   /* TIM3 counter with PSC = 0 */
/* one SPI byte = 256/15 us exactly, so us -> bytes is exact integer math */
#define IR_US_TO_BYTES(us) (((uint32_t)(us) * 15u + 128u) / 256u)

static volatile uint8_t ir_level;  /* the single byte the DMA repeats */

static void pin_mode(uint32_t pin, uint32_t mode)
{
	GPIO_MODER(IRP) = (GPIO_MODER(IRP) & ~(3u << (pin * 2))) | (mode << (pin * 2));
}

void ir_carrier_off(void)
{
	TIM3_CR1 = 0;
	pin_mode(IR_CARRIER_PIN, GPIO_MODE_OUTPUT);
	GPIO_BSRR(IRP) = 1u << (IR_CARRIER_PIN + 16);   /* PB0 low = emitter off */
}

void ir_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(IRP) | (1u << 21);   /* GPIOB, DMA1 */
	RCC_APB1ENR |= (1u << 1) | (1u << 14);               /* TIM3, SPI2 */

	/* PB15 = SPI2_MOSI, AF5, 50 MHz, pull-down (stock FUN_080097c4) */
	GPIO_AFRH(IRP)    = (GPIO_AFRH(IRP) & ~(0xFu << 28)) | (5u << 28);
	GPIO_OSPEEDR(IRP) |= 2u << (IR_ENV_PIN * 2);
	GPIO_PUPDR(IRP)    = (GPIO_PUPDR(IRP) & ~(3u << (IR_ENV_PIN * 2)))
	                   | (2u << (IR_ENV_PIN * 2));
	pin_mode(IR_ENV_PIN, GPIO_MODE_AF);

	/* PB0 = TIM3_CH3, AF2, pull-up (stock FUN_08009a78) */
	GPIO_AFRL(IRP)  = (GPIO_AFRL(IRP) & ~0xFu) | 2u;
	GPIO_PUPDR(IRP) = (GPIO_PUPDR(IRP) & ~3u) | 1u;

	/* PB14 = RF enable: GPIO out, held LOW so the RF path stays off */
	pin_mode(IR_RF_PIN, GPIO_MODE_OUTPUT);
	GPIO_BSRR(IRP) = 1u << (IR_RF_PIN + 16);

	SPI2_I2SCFGR = 0;                 /* SPI mode, not I2S */
	SPI2_CR1     = SPI2_CR1_IR;       /* SPE stays off until a frame starts */
	SPI2_CR2     = SPI_CR2_TXDMAEN;

	DMA1_S4CR = 0;
	while (DMA1_S4CR & DMA_SxCR_EN) {
	}
	DMA1_S4PAR  = SPI2_DR_ADDR;
	DMA1_S4M0AR = (uint32_t)&ir_level;   /* never changes: MemInc is off */
	DMA1_S4CR   = DMA1_S4CR_IR;
	DMA1_HIFCR  = DMA_HIFCR_S4;

	ir_carrier_off();
}

/* arr == 0 reproduces stock's no-carrier mode: PB0 parked HIGH, gate held open,
 * LED follows the envelope directly. NOTE that doubles the average LED current
 * versus a 50 %-duty carrier -- only use it for codes that ask for it. */
static void ir_carrier_on(uint16_t arr)
{
	TIM3_CR1 = 0;
	if (arr == 0) {
		pin_mode(IR_CARRIER_PIN, GPIO_MODE_OUTPUT);
		GPIO_BSRR(IRP) = 1u << IR_CARRIER_PIN;
		return;
	}
	TIM3_PSC   = 0;                                   /* 60 MHz tick */
	TIM3_ARR   = arr;
	TIM3_CCR3  = arr / 2u;                            /* 50 % duty */
	TIM3_CCMR2 = (TIM3_CCMR2 & ~0xFFu) | (7u << 4) | (1u << 3);   /* OC3M=PWM2, OC3PE */
	TIM3_CCER  = (TIM3_CCER & ~0xF00u) | (1u << 8) | (1u << 9);    /* CC3E, CC3P */
	TIM3_EGR   = TIM_EGR_UG;
	pin_mode(IR_CARRIER_PIN, GPIO_MODE_AF);
	TIM3_CR1   = TIM_CR1_CEN;
}

/* One mark (0xFF) or space (0x00) of nbytes SPI bytes; blocks until sent.
 * Stock re-runs DMA_DeInit + a full DMA_Init per burst, but only NDTR ever
 * changes, so rewrite just that. Less inter-burst dead time than stock, which
 * is strictly better: MOSI holds the last bit while the DMA is idle, so any gap
 * stretches the burst that just ended. */
static void ir_burst(uint8_t level, uint16_t nbytes)
{
	if (nbytes == 0) {
		nbytes = 1;                    /* stock clamps 0 -> 1 */
	}
	ir_level = level;
	barrier_dmem_fence_full();
	while (DMA1_S4CR & DMA_SxCR_EN) {  /* must read 0 before reprogramming */
	}
	DMA1_HIFCR  = DMA_HIFCR_S4;
	DMA1_S4NDTR = nbytes;
	DMA1_S4CR  |= DMA_SxCR_EN;

	while (!(DMA1_HISR & DMA_HISR_TCIF4)) {
	}
	DMA1_S4CR &= ~DMA_SxCR_EN;
}

static void ir_frame_begin(uint16_t arr)
{
	ir_carrier_on(arr);
	SPI2_CR1 |= SPI_CR1_SPE;
}

static void ir_frame_end(void)
{
	while (SPI2_SR & SPI_SR_BSY) {     /* the shift register trails the DMA */
	}
	SPI2_CR1 &= ~SPI_CR1_SPE;
	ir_carrier_off();
}

/* ---- raw API: alternating mark/space in microseconds, starting with a mark -- */
void ir_send_raw_us(uint32_t carrier_hz, const uint16_t *us, size_t n)
{
	if (n == 0) {
		return;
	}
	ir_frame_begin(carrier_hz ? (uint16_t)(IR_TIMER_HZ / carrier_hz - 1u) : 0);
	for (size_t i = 0; i < n; i++) {
		ir_burst((i & 1u) ? 0x00 : 0xFF, (uint16_t)IR_US_TO_BYTES(us[i]));
	}
	ir_frame_end();
}

/* ---- stock RTI blob player ------------------------------------------------ */

/* Byte-for-byte as stored in the external SPI NOR. __packed is load-bearing:
 * the on-flash header is 13 bytes, a natural layout would pad it to 14. */
struct ir_code_hdr {
	uint16_t carrier_arr;    /* 0x00 TIM3 ARR; 0 = no carrier */
	uint16_t once_len;       /* 0x02 index bytes in the lead-in */
	uint16_t repeat_len;     /* 0x04 index bytes in the repeat block */
	uint8_t  n_timings;      /* 0x06 u16 entries in the timing table */
	uint8_t  repeat_count;   /* 0x07 extra repeat passes */
	uint8_t  flags1;         /* 0x08 bit0 = may repeat while the key is held */
	uint8_t  unk09;          /* 0x09 never read by the transmitter */
	uint8_t  unk0a;          /* 0x0A never read by the transmitter */
	uint8_t  flags2;         /* 0x0B bit0 = RF (PB14) instead of IR */
	uint8_t  unk0c;          /* 0x0C never read by the transmitter */
} __packed;
/* then: uint16_t timings[n_timings]              -- duration in 17.0667 us bytes
 * then: uint8_t  index[once_len + repeat_len]    -- ONE-BASED into timings[]   */

int ir_send_code(const void *blob, bool allow_hold, bool (*held)(void))
{
	struct ir_code_hdr h;

	memcpy(&h, blob, sizeof h);

	uint32_t end = (uint32_t)h.once_len + h.repeat_len;

	if (h.n_timings == 0 || end == 0) {
		return -EINVAL;
	}
	if (h.flags2 & 1u) {
		return -ENOTSUP;             /* RF path (PB14); no radio on the bench unit */
	}

	const uint8_t  *p   = blob;
	const uint8_t  *tim = p + sizeof h;                       /* may be unaligned */
	const uint8_t  *idx = tim + 2u * h.n_timings;
	uint32_t reps = h.repeat_len ? h.repeat_count : 0u;
	bool     hold = allow_hold && (h.flags1 & 1u) && held;

	ir_frame_begin(h.carrier_arr);

	uint32_t i = 0;
	uint8_t  level = 0xFF;               /* stock always opens with a mark */

	for (;;) {
		uint8_t k = idx[i];              /* 1-based; 0 would be out of table */

		ir_burst(level, k ? sys_get_le16(tim + 2u * (k - 1u)) : 1u);
		level ^= 0xFF;

		if (++i < end) {
			continue;
		}
		if (h.repeat_len == 0) {
			break;
		}
		if (reps == 0) {
			if (!hold || !held()) {
				break;
			}
		} else {
			reps--;
		}
		i = h.once_len;                  /* replay only the repeat block */
		level = 0xFF;
	}

	ir_frame_end();
	return 0;
}

/* ---- convenience: one NEC frame at 38 kHz (LSB-first, byte + complement) --- */
void ir_send_nec(uint8_t addr, uint8_t cmd)
{
	uint32_t bits = (uint32_t)addr
	              | ((uint32_t)(uint8_t)~addr << 8)
	              | ((uint32_t)cmd << 16)
	              | ((uint32_t)(uint8_t)~cmd << 24);
	uint16_t t[2 + 64 + 1];
	size_t k = 0;

	t[k++] = 9000;
	t[k++] = 4500;
	for (int b = 0; b < 32; b++) {
		t[k++] = 560;
		t[k++] = (bits >> b) & 1u ? 1690 : 560;
	}
	t[k++] = 560;
	ir_send_raw_us(38000, t, k);
}

/* Bring-up smoke test, same shape as stock's diag "Test IR" (FUN_0800981a):
 * 40 kHz carrier, ~1.71 ms on / ~1.71 ms off for ~100 ms. A phone camera sees
 * the LED; a photodiode on a scope gives you the carrier to measure. */
void ir_selftest(void)
{
	uint16_t t[58];

	for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
		t[i] = 1707;                 /* 100 SPI bytes, as stock's test buffer */
	}
	ir_send_raw_us(40000, t, ARRAY_SIZE(t));
}

/* The one runnable check -- pure arithmetic, no hardware. These are the numbers
 * stock's own constants pin down; if any moves, the time base is wrong. */
void ir_selfcheck(void)
{
	__ASSERT(IR_US_TO_BYTES(410)   == 24,   "SPI byte time drifted");   /* half-bit */
	__ASSERT(IR_US_TO_BYTES(20000) == 1172, "SPI byte time drifted");   /* trailer  */
	__ASSERT(IR_TIMER_HZ / 40000u - 1u == 0x5DBu, "carrier ARR drifted");
	__ASSERT(sizeof(struct ir_code_hdr) == 13, "ir_code_hdr must be packed");
	__ASSERT(500000u / 400u - 1u == 1249u && 500000u / 500u - 1u == 999u,
	         "beeper ARR drifted");
}
```

Add both to `CMakeLists.txt`: `src/ir.c src/beep.c`.

## Deviations from stock, and why

| Stock | Here | Why |
|---|---|---|
| `DMA_DeInit` + full `DMA_Init` per burst | rewrite `NDTR` only | only NDTR changes; shorter inter-burst gap, which is what stretches marks |
| `SPI_I2S_DMACmd` toggled per burst | `TXDMAEN` left on | the request is level-based; re-enabling the stream is enough |
| waits `BSY` without waiting the last `TC` | waits `TC` then `BSY` | `BSY` can momentarily read 0 on a starved SPI |
| beeper tick 500 kHz (RTI's off-by-2) | kept at 500 kHz | matches stock pitch; `BEEP_PSC 59` gives the intended 800/1000 Hz |

## Bring-up order (bench unit, no radio)

1. `ir_selfcheck()` — arithmetic only.
2. `ir_init()`, then `ir_selftest()`. Confirm PB14 reads LOW throughout.
3. Scope PB0 (expect a 40.000 kHz 50 % square) and PB15 (expect ~1.71 ms high /
   ~1.71 ms low). This is also the measurement that settles the open question about
   the external gate topology.
4. `ir_send_nec(0x00, 0x00)` at a real receiver.
5. `beep_init()` then `beep_click()`; measure the pitch to settle 400 Hz vs 800 Hz.

