/*
 * t2i_regs.h — STM32F205 register map for the T2i bare-metal peripherals we
 * drive directly (FSMC LCD bus, GPIO AF/out, backlight timers). Reverse-
 * engineered from stock RTI; see HARDWARE.md for the pin functions.
 */
#ifndef T2I_REGS_H
#define T2I_REGS_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG8(a)  (*(volatile uint8_t  *)(a))

/* ---- RCC ---- */
#define RCC_BASE       0x40023800u
#define RCC_AHB1ENR    REG32(RCC_BASE + 0x30)   /* GPIOA..E clock enables (bit p) */
#define RCC_AHB3ENR    REG32(RCC_BASE + 0x38)   /* bit0 = FSMC */
#define RCC_APB1ENR    REG32(RCC_BASE + 0x40)   /* bit0 = TIM2 */
#define RCC_APB2ENR    REG32(RCC_BASE + 0x44)   /* bit1 = TIM8 */
#define RCC_AHB1ENR_GPIO(p) (1u << (p))         /* p: 0=A..4=E */

/* ---- GPIO (p: 0=A .. 4=E) ---- */
#define GPIO_BASE(p)   (0x40020000u + (p) * 0x400u)
#define GPIO_MODER(p)  REG32(GPIO_BASE(p) + 0x00)
#define GPIO_OSPEEDR(p)REG32(GPIO_BASE(p) + 0x08)
#define GPIO_BSRR(p)   REG32(GPIO_BASE(p) + 0x18)
#define GPIO_AFRL(p)   REG32(GPIO_BASE(p) + 0x20)
#define GPIO_AFRH(p)   REG32(GPIO_BASE(p) + 0x24)
#define GPIO_PORT_A 0
#define GPIO_PORT_B 1
#define GPIO_PORT_C 2
#define GPIO_PORT_D 3
#define GPIO_PORT_E 4
/* MODER field values */
#define GPIO_MODE_INPUT  0u
#define GPIO_MODE_OUTPUT 1u
#define GPIO_MODE_AF     2u
#define GPIO_MODE_ANALOG 3u

/* ---- FSMC (bank1 / NE1, 8-bit; LCD command @ base, data @ base+A18) ---- */
#define FSMC_BCR1      REG32(0xA0000000)
#define FSMC_BTR1      REG32(0xA0000004)
#define FSMC_BCR1_CFG  0x00001049u   /* MBKEN|MWID=8|MTYP=NOR|FACCEN|WREN */
#define FSMC_BTR1_CFG  0x00102D11u   /* ADDSET/DATAST timing (from RTI) */
#define LCD_CMD_ADDR   0x60000000u   /* A18=0 -> command/index */
#define LCD_DAT_ADDR   0x60040000u   /* A18=1 (PD13 = RS/DC) -> data */
#define LCD_CMD_REG    REG8(LCD_CMD_ADDR)
#define LCD_DAT_REG    REG8(LCD_DAT_ADDR)

/* ---- TIM2 (LCD backlight, CH2 -> PA1 AF1). PWM-dimming input to the driver IC.
 *       Timer clock = 60 MHz (APB1 /4, x2). ---- */
#define TIM2_BASE      0x40000000u
#define TIM2_CR1       REG32(TIM2_BASE + 0x00)
#define TIM2_EGR       REG32(TIM2_BASE + 0x14)
#define TIM2_CCMR1     REG32(TIM2_BASE + 0x18)
#define TIM2_CCER      REG32(TIM2_BASE + 0x20)
#define TIM2_PSC       REG32(TIM2_BASE + 0x28)
#define TIM2_ARR       REG32(TIM2_BASE + 0x2C)
#define TIM2_CCR2      REG32(TIM2_BASE + 0x38)

/* ---- TIM8 (keypad backlight, CH3 -> PC8 AF3). Advanced timer (needs MOE).
 *       Timer clock = 120 MHz (APB2 /2, x2). ---- */
#define TIM4_BASE      0x40000800u
#define TIM4_CR1       REG32(TIM4_BASE + 0x00)
#define TIM4_EGR       REG32(TIM4_BASE + 0x14)
#define TIM4_CCMR1     REG32(TIM4_BASE + 0x18)
#define TIM4_CCER      REG32(TIM4_BASE + 0x20)
#define TIM4_PSC       REG32(TIM4_BASE + 0x28)
#define TIM4_ARR       REG32(TIM4_BASE + 0x2C)
#define TIM4_CCR2      REG32(TIM4_BASE + 0x38)

#define TIM8_BASE      0x40010400u
#define TIM8_CR1       REG32(TIM8_BASE + 0x00)
#define TIM8_EGR       REG32(TIM8_BASE + 0x14)
#define TIM8_CCMR2     REG32(TIM8_BASE + 0x1C)
#define TIM8_CCER      REG32(TIM8_BASE + 0x20)
#define TIM8_PSC       REG32(TIM8_BASE + 0x28)
#define TIM8_ARR       REG32(TIM8_BASE + 0x2C)
#define TIM8_CCR3      REG32(TIM8_BASE + 0x3C)
#define TIM8_BDTR      REG32(TIM8_BASE + 0x44)

/* Common timer bit fields */
#define TIM_CR1_CEN    (1u << 0)
#define TIM_EGR_UG     (1u << 0)
#define TIM_CCMR_PWM1_PE(shift)  ((6u << (shift)) | (1u << ((shift) - 1)))  /* OCxM=PWM1 + OCxPE */
#define TIM_BDTR_MOE   (1u << 15)

/* ---- extra GPIO regs (touch needs pull config + input read) ---- */
#define GPIO_PUPDR(p)  REG32(GPIO_BASE(p) + 0x0C)
#define GPIO_IDR(p)    REG32(GPIO_BASE(p) + 0x10)

/* ---- ADC2 (touchscreen, channels IN3..IN6 = PA3..PA6). APB2. ---- */
#define ADC2_BASE      0x40012100u
#define ADC2_SR        REG32(ADC2_BASE + 0x00)
#define ADC2_CR1       REG32(ADC2_BASE + 0x04)
#define ADC2_CR2       REG32(ADC2_BASE + 0x08)
#define ADC2_SMPR1     REG32(ADC2_BASE + 0x0C)   /* channels 10..18 */
#define ADC2_SMPR2     REG32(ADC2_BASE + 0x10)   /* channels 0..9 */
#define ADC2_SQR1      REG32(ADC2_BASE + 0x2C)
#define ADC2_SQR3      REG32(ADC2_BASE + 0x34)
#define ADC2_DR        REG32(ADC2_BASE + 0x4C)
#define ADC_CCR        REG32(0x40012300u + 0x04) /* common control (prescaler) */
#define ADC_SR_EOC     (1u << 1)
#define ADC_CR2_ADON   (1u << 0)
#define ADC_CR2_SWSTART (1u << 30)

/* ---- TIM3 (IR CARRIER (not the beeper - see docs/IR-BUZZER.md), CH3 -> PB0 AF2). Timer clock = 60 MHz. ---- */
#define TIM3_BASE      0x40000400u
#define TIM3_CR1       REG32(TIM3_BASE + 0x00)
#define TIM3_CCMR2     REG32(TIM3_BASE + 0x1C)
#define TIM3_CCER      REG32(TIM3_BASE + 0x20)
#define TIM3_PSC       REG32(TIM3_BASE + 0x28)
#define TIM3_ARR       REG32(TIM3_BASE + 0x2C)
#define TIM3_EGR       REG32(TIM3_BASE + 0x14)
#define TIM3_CCR3      REG32(TIM3_BASE + 0x3C)

/* ---- SPI2 (IR envelope on PB15/MOSI) + DMA1 Stream4 ch0 = SPI2_TX ---- */
#define SPI2_BASE      0x40003800u
#define SPI2_CR1       REG32(SPI2_BASE + 0x00)
#define SPI2_CR2       REG32(SPI2_BASE + 0x04)
#define SPI2_SR        REG32(SPI2_BASE + 0x08)
#define SPI2_DR        REG32(SPI2_BASE + 0x0C)

#define DMA1_BASE      0x40026000u
#define DMA1_HISR      REG32(DMA1_BASE + 0x04)
#define DMA1_HIFCR     REG32(DMA1_BASE + 0x0C)
#define DMA1_S4CR      REG32(DMA1_BASE + 0x70)
#define DMA1_S4NDTR    REG32(DMA1_BASE + 0x74)
#define DMA1_S4PAR     REG32(DMA1_BASE + 0x78)
#define DMA1_S4M0AR    REG32(DMA1_BASE + 0x7C)
#define DMA1_S4FCR     REG32(DMA1_BASE + 0x84)
#define DMA_HISR_TCIF4 (1u << 5)

#endif /* T2I_REGS_H */
