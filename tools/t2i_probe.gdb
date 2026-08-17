# t2i_probe.gdb — read-only LCD/backlight state snapshot over SWD.
#
#   terminal A:  st-util -p 4242 --no-reset
#   terminal B:  $HOME/zephyr-sdks/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb \
#                    -q -x tools/t2i_probe.gdb build/zephyr/zephyr.elf
#
# Run it TWICE and diff the two outputs:
#   (1) BASELINE — right after power-on while the splash is up and the screen works.
#   (2) FAILED WAKE — let it idle 30 s, press a key, confirm the screen stayed dark,
#       then attach. Do NOT power-cycle in between.
#
# Nothing here writes flash. The only writes are the HX8347 index register
# (idempotent) and, at the very end, the operator-driven backlight override.

set confirm off
set pagination off
set stack-cache off
set code-cache off
# st-util publishes a memory map that excludes FSMC (0x6000_0000) and the
# peripheral bus; without this gdb refuses to touch them.
set mem inaccessible-by-default off

target extended-remote :4242

# ---------------------------------------------------------------------------
# FIRST THING, ALWAYS: freeze the IWDG.
# reset_hook.c:59 and safety.c:113 both CLEAR DBGMCU_APB1_FZ bits 11/12, so the
# independent watchdog keeps counting while the core is halted and resets the
# target ~8 s in — taking the evidence with it.
# ---------------------------------------------------------------------------
set *(unsigned int *)0xE0042008 = *(unsigned int *)0xE0042008 | 0x1800
printf "DBGMCU_APB1_FZ = 0x%08x  (bits 11/12 must be set)\n", *(unsigned int *)0xE0042008

printf "\n=== firmware markers ===\n"
printf "phase   0x2001FF00 = 0x%08x\n", *(unsigned int *)0x2001FF00
printf "beat    0x2001FF04 = %u        (re-run: must be climbing = loop alive)\n", *(unsigned int *)0x2001FF04
printf "boots   0x2001FF84 = %u\n", *(unsigned int *)0x2001FF84
printf "uimark  0x2001FF88 = %u\n", *(unsigned int *)0x2001FF88
printf "asleep  0x2001FF8C = %u        (0 = firmware believes it is awake)\n", *(unsigned int *)0x2001FF8C
printf "bright  0x2001FF90 = %u        (128 after a wake)\n", *(unsigned int *)0x2001FF90
printf "wakes   0x2001FF94 = %u\n", *(unsigned int *)0x2001FF94

printf "\n=== TIM2 — LCD backlight PWM (PA1 / CH2) ===\n"
printf "CR1   0x40000000 = 0x%08x   bit0 CEN\n", *(unsigned int *)0x40000000
printf "CCMR1 0x40000018 = 0x%08x   expect 0x7800 (OC2M=PWM2 + OC2PE)\n", *(unsigned int *)0x40000018
printf "CCER  0x40000020 = 0x%08x   expect 0x30 (CC2E|CC2P)\n", *(unsigned int *)0x40000020
printf "CNT   0x40000024 = 0x%08x   re-run: changing = timer really running\n", *(unsigned int *)0x40000024
printf "PSC   0x40000028 = 0x%08x\n", *(unsigned int *)0x40000028
printf "ARR   0x4000002C = 0x%08x\n", *(unsigned int *)0x4000002C
printf "CCR2  0x40000038 = 0x%08x   duty = CCR2/ARR\n", *(unsigned int *)0x40000038

printf "\n=== TIM8 — keypad backlight (PC8 / CH3), same boost rail ===\n"
printf "CR1   0x40010400 = 0x%08x\n", *(unsigned int *)0x40010400
printf "ARR   0x4001042C = 0x%08x\n", *(unsigned int *)0x4001042C
printf "CCR3  0x4001043C = 0x%08x\n", *(unsigned int *)0x4001043C
printf "BDTR  0x40010444 = 0x%08x   bit15 MOE\n", *(unsigned int *)0x40010444

printf "\n=== GPIOA — PA1 = backlight ===\n"
printf "MODER 0x40020000 = 0x%08x   bits[3:2]: 0=in 1=out 2=AF\n", *(unsigned int *)0x40020000
printf "PUPDR 0x4002000C = 0x%08x\n", *(unsigned int *)0x4002000C
printf "IDR   0x40020010 = 0x%08x   bit1 = actual pin level\n", *(unsigned int *)0x40020010
printf "ODR   0x40020014 = 0x%08x   bit1\n", *(unsigned int *)0x40020014
printf "AFRL  0x40020020 = 0x%08x   bits[7:4] = PA1 AF (1 = TIM2_CH2)\n", *(unsigned int *)0x40020020

printf "\n=== GPIOC — PC12 boost/rail enable, PC8 keypad BL ===\n"
printf "MODER 0x40020800 = 0x%08x   bits[25:24] PC12 must be 01 (output)\n", *(unsigned int *)0x40020800
printf "IDR   0x40020810 = 0x%08x   bit12\n", *(unsigned int *)0x40020810
printf "ODR   0x40020814 = 0x%08x   bit12 MUST be 1\n", *(unsigned int *)0x40020814

printf "\n=== GPIOD — PD6 = LCD reset (idle high) ===\n"
printf "MODER 0x40020C00 = 0x%08x\n", *(unsigned int *)0x40020C00
printf "IDR   0x40020C10 = 0x%08x   bit6 must be 1 = reset released\n", *(unsigned int *)0x40020C10
printf "ODR   0x40020C14 = 0x%08x   bit6\n", *(unsigned int *)0x40020C14

printf "\n=== clocks / FSMC ===\n"
printf "AHB1ENR 0x40023830 = 0x%08x  bits 0,2,3,4 = GPIOA,C,D,E\n", *(unsigned int *)0x40023830
printf "AHB3ENR 0x40023838 = 0x%08x  bit0 = FSMC\n", *(unsigned int *)0x40023838
printf "APB1ENR 0x40023840 = 0x%08x  bit0 = TIM2\n", *(unsigned int *)0x40023840
printf "APB2ENR 0x40023844 = 0x%08x  bit1 = TIM8\n", *(unsigned int *)0x40023844
printf "BCR1    0xA0000000 = 0x%08x  expect 0x00001049\n", *(unsigned int *)0xA0000000
printf "BTR1    0xA0000004 = 0x%08x  expect 0x00102D11\n", *(unsigned int *)0xA0000004

# ---------------------------------------------------------------------------
# HX8347 register reads. Stock RTI's read primitive is FUN_0800f446:
#     strb index -> 0x60000000 ; ldrb <- 0x60040000        (no dummy read)
# FSMC bank1 is MWID=8, and only A18 (PD13) reaches the panel as RS/DC — A0/A1
# are not routed. A 32-bit access is therefore expanded into four 8-bit bus
# cycles that all land on the SAME port, so writing 0xNNNNNNNN sets the index to
# 0xNN four times (idempotent) and a word read returns the byte replicated.
# Word-sized access is used deliberately: st-util's gdb server always issues
# 32-bit aligned transfers, so byte commands would be widened anyway.
# ---------------------------------------------------------------------------
printf "\n=== HX8347 registers (byte value is replicated 4x) ===\n"
set *(unsigned int *)0x60000000 = 0x00000000
printf "reg 0x00 device code = 0x%08x   expect 0x47474747\n", *(unsigned int *)0x60040000
set *(unsigned int *)0x60000000 = 0x19191919
printf "reg 0x19 OSC control = 0x%08x   expect 0x01010101 after panel_init\n", *(unsigned int *)0x60040000
set *(unsigned int *)0x60000000 = 0x1F1F1F1F
printf "reg 0x1F power ctrl  = 0x%08x   expect 0xD0D0D0D0 after panel_init\n", *(unsigned int *)0x60040000
set *(unsigned int *)0x60000000 = 0x28282828
printf "reg 0x28 display ctl = 0x%08x   expect 0x3C3C3C3C after panel_init\n", *(unsigned int *)0x60040000
set *(unsigned int *)0x60000000 = 0x26262626
printf "reg 0x26 gate ctrl   = 0x%08x\n", *(unsigned int *)0x60040000
# Read-back of non-ID registers is not guaranteed on every HX8347 variant. That
# is exactly why you diff against the BASELINE run rather than against a
# datasheet expectation.

printf "\n"
printf "=====================================================================\n"
printf " DECISIVE STEP — paste these three while WATCHING THE SCREEN.\n"
printf " They take TIM2, the PWM and the AF mux out of the path entirely and\n"
printf " hold PA1 at VDD as a plain push-pull output (stock RTI's own 100%%\n"
printf " endpoint, FUN_0801a9fa param==100).\n"
printf "\n"
printf "   set *(unsigned int *)0x40000000 = 0\n"
printf "   set *(unsigned int *)0x40020000 = (*(unsigned int *)0x40020000 & ~0xc) | 4\n"
printf "   set *(unsigned int *)0x40020018 = 2\n"
printf "\n"
printf " ANY light at all (glow, grey wash, image)  -> backlight + rail alive,\n"
printf "                                               the PANEL is the fault.\n"
printf " Still pitch black                          -> the BACKLIGHT is the fault.\n"
printf "\n"
printf " Then, with the light forced on, toggle the panel's display bit:\n"
printf "   set *(unsigned int *)0x60000000 = 0x28282828\n"
printf "   set *(unsigned int *)0x60040000 = 0x38383838     # display OFF\n"
printf "   set *(unsigned int *)0x60000000 = 0x28282828\n"
printf "   set *(unsigned int *)0x60040000 = 0x3c3c3c3c     # display ON\n"
printf " Visible change -> panel is driving. No change -> panel is not driving.\n"
printf "=====================================================================\n"
