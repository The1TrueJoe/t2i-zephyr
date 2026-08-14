/*
 * T2i LCD bring-up test — ILI9341 320x240 on an 8-bit FSMC parallel bus.
 * Reverse-engineered from stock RTI:
 *   command byte -> *(0x60000000), data byte -> *(0x60040000)  (RS/DC = A18 = PD13)
 *   FSMC bank1/NE1, 8-bit; data PD14/15/0/1 + PE7-10; RD=PD4 WR=PD5 CS=PD7 RS=PD13
 *   PD6 = LCD control (reset). Backlight pin unknown -> drive candidates high.
 * Draws color bars so we can see it working.
 */
#include <zephyr/kernel.h>
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define RCC_AHB1ENR REG(0x40023830)
#define RCC_AHB3ENR REG(0x40023838)

#define GPIO(p)   (0x40020000u + (p) * 0x400u)   /* p: 0=A..4=E */
#define MODER(p)  REG(GPIO(p) + 0x00)
#define OTYPER(p) REG(GPIO(p) + 0x04)
#define OSPEEDR(p)REG(GPIO(p) + 0x08)
#define PUPDR(p)  REG(GPIO(p) + 0x0C)
#define ODR(p)    REG(GPIO(p) + 0x14)
#define BSRR(p)   REG(GPIO(p) + 0x18)
#define AFRL(p)   REG(GPIO(p) + 0x20)
#define AFRH(p)   REG(GPIO(p) + 0x24)

#define FSMC_BCR1 REG(0xA0000000)
#define FSMC_BTR1 REG(0xA0000004)

#define LCD_CMD  (*(volatile uint8_t *)0x60000000)
#define LCD_DAT  (*(volatile uint8_t *)0x60040000)

static void pin_af(int port, int pin, int af)
{
	MODER(port) = (MODER(port) & ~(3u << (pin * 2))) | (2u << (pin * 2));   /* AF mode */
	OSPEEDR(port) |= (3u << (pin * 2));                                     /* high speed */
	if (pin < 8) AFRL(port) = (AFRL(port) & ~(0xFu << (pin * 4))) | ((uint32_t)af << (pin * 4));
	else         AFRH(port) = (AFRH(port) & ~(0xFu << ((pin - 8) * 4))) | ((uint32_t)af << ((pin - 8) * 4));
}
static void pin_out(int port, int pin, int val)
{
	MODER(port) = (MODER(port) & ~(3u << (pin * 2))) | (1u << (pin * 2));   /* output */
	OSPEEDR(port) |= (3u << (pin * 2));
	BSRR(port) = val ? (1u << pin) : (1u << (pin + 16));
}

static inline void lcd_cmd(uint8_t c)  { LCD_CMD = c; }
static inline void lcd_dat(uint8_t d)  { LCD_DAT = d; }
static inline void lcd_reg(uint8_t c, uint8_t d) { LCD_CMD = c; LCD_DAT = d; }

/* Register-based init for the T2i's 0x47 controller (RTI FUN_0800f610 branch1).
 * Sets the window to full-screen (regs 0x02-0x09 = 0..239 x 0..319). */
static void panel_init(void)
{
	lcd_reg(0xEA,0x00); lcd_reg(0xEB,0x20); lcd_reg(0xEC,0x0C); lcd_reg(0xED,0xC4);
	lcd_reg(0xE8,0x40); lcd_reg(0xE9,0x38); lcd_reg(0xF1,0x01); lcd_reg(0xF2,0x10);
	lcd_reg(0x27,0xA3);
	lcd_reg(0x40,0x01); lcd_reg(0x41,0x07); lcd_reg(0x42,0x06); lcd_reg(0x43,0x0A);
	lcd_reg(0x44,0x0C); lcd_reg(0x45,0x3D); lcd_reg(0x46,0x02); lcd_reg(0x47,0x43);
	lcd_reg(0x48,0x07); lcd_reg(0x49,0x14); lcd_reg(0x4A,0x19); lcd_reg(0x4B,0x1A);
	lcd_reg(0x4C,0x1E);
	lcd_reg(0x50,0x02); lcd_reg(0x51,0x33); lcd_reg(0x52,0x35); lcd_reg(0x53,0x39);
	lcd_reg(0x54,0x38); lcd_reg(0x55,0x3E); lcd_reg(0x56,0x3C); lcd_reg(0x57,0x7D);
	lcd_reg(0x58,0x01); lcd_reg(0x59,0x05); lcd_reg(0x5A,0x06); lcd_reg(0x5B,0x0B);
	lcd_reg(0x5C,0x18); lcd_reg(0x5D,0xFF);
	lcd_reg(0x1B,0x1B); lcd_reg(0x1A,0x01); lcd_reg(0x24,0x45); lcd_reg(0x25,0x1F);
	lcd_reg(0x23,0x8A); lcd_reg(0x18,0x36); lcd_reg(0x19,0x01); lcd_reg(0x01,0x00);
	lcd_reg(0x1F,0x88); k_msleep(5); lcd_reg(0x1F,0x80); k_msleep(5);
	lcd_reg(0x1F,0x90); k_msleep(5); lcd_reg(0x1F,0xD0); k_msleep(5);
	lcd_reg(0x16,0x40); lcd_reg(0x17,0x05); lcd_reg(0x36,0x02);
	lcd_reg(0x28,0x38); k_msleep(40); lcd_reg(0x28,0x3C);
	lcd_reg(0x02,0x00); lcd_reg(0x03,0x00); lcd_reg(0x04,0x00); lcd_reg(0x05,0xEF);
	lcd_reg(0x06,0x00); lcd_reg(0x07,0x00); lcd_reg(0x08,0x01); lcd_reg(0x09,0x3F);
}

static void lcd_init(void)
{
	/* clocks: GPIOA,C,D,E + FSMC */
	RCC_AHB1ENR |= (1u<<0)|(1u<<2)|(1u<<3)|(1u<<4);
	RCC_AHB3ENR |= (1u<<0);

	/* Backlight: TIM8_CH3 PWM on PC8 (AF3), config copied from RTI:
	 * ARR=300, CCR3=271 (~90%), active-low (CC3P), MOE required (advanced timer). */
	REG(0x40023844) |= (1u<<1);              /* RCC_APB2ENR TIM8EN */
	pin_af(2, 8, 3);                         /* PC8 -> AF3 = TIM8_CH3 */
	REG(0x40010428) = 99;                    /* PSC -> ~4 kHz */
	REG(0x4001042C) = 300;                   /* ARR */
	REG(0x4001043C) = 271;                   /* CCR3 duty */
	REG(0x4001041C) = (6u<<4)|(1u<<3);       /* CCMR2: OC3M=PWM1, OC3PE */
	REG(0x40010420) = (1u<<8)|(1u<<9);       /* CCER: CC3E + CC3P */
	REG(0x40010444) = (1u<<15);              /* BDTR: MOE */
	REG(0x40010400) = (1u<<0);               /* CR1: CEN */
	/* keypad backlight confirmed = TIM8_CH3/PC8 (above). */

	/* LCD backlight: TIM5_CH1 PWM on PA0 (AF2), the other configured PWM in RTI.
	 * Drive it bright (90%) to make the screen clearly visible. */
	REG(0x40023840) |= (1u<<3);              /* RCC_APB1ENR TIM5EN */
	pin_af(0, 0, 2);                         /* PA0 -> AF2 = TIM5_CH1 */
	REG(0x40000C28) = 119;                   /* PSC -> ~1 kHz */
	REG(0x40000C2C) = 1000;                  /* ARR */
	REG(0x40000C34) = 900;                   /* CCR1 = 90% */
	REG(0x40000C18) = (6u<<4)|(1u<<3);       /* CCMR1: OC1M=PWM1, OC1PE */
	REG(0x40000C20) = (1u<<0);               /* CCER: CC1E */
	REG(0x40000C00) = (1u<<0);               /* CR1: CEN */

	/* FSMC data + control pins -> AF12 */
	int dpins[] = {0,1,4,5,7,13,14,15};
	for (unsigned i=0;i<sizeof(dpins)/sizeof(dpins[0]);i++) pin_af(3, dpins[i], 12);  /* GPIOD */
	int epins[] = {7,8,9,10};
	for (unsigned i=0;i<sizeof(epins)/sizeof(epins[0]);i++) pin_af(4, epins[i], 12);  /* GPIOE */

	/* control: PD6 = reset; backlight candidates high */
	pin_out(3, 6, 1);   /* PD6 */
	pin_out(0, 10, 1);  /* PA10 backlight? */
	pin_out(2, 12, 1);  /* PC12 backlight? */
	pin_out(2, 13, 1);  /* PC13 backlight? */

	/* FSMC bank1: 8-bit NOR, WREN, FACCEN (no EXTMOD -> BTR used for r/w) */
	FSMC_BTR1 = 0x00102D11;
	FSMC_BCR1 = 0x00001049;
	FSMC_BCR1 |= 0x00000001;   /* MBKEN */

	/* hardware reset via PD6 */
	pin_out(3, 6, 0); k_msleep(20);
	pin_out(3, 6, 1); k_msleep(150);

	panel_init();   /* 0x47 controller register-based init (window set full-screen) */
}

static void fill_window(void)
{
	lcd_cmd(0x22);   /* GRAM write (0x47 controller); window already full-screen */
	static const uint16_t bars[8] = {0xF800,0x07E0,0x001F,0xFFE0,0x07FF,0xF81F,0xFFFF,0x0000};
	for (int y = 0; y < 320; y++) {
		uint16_t c = bars[(y / 40) & 7];
		for (int x = 0; x < 240; x++) { lcd_dat(c >> 8); lcd_dat(c & 0xFF); }
	}
}

int main(void)
{
	*(volatile uint32_t *)0x2001FF00 = 0x1CD00001;   /* marker: reached main */
	lcd_init();
	*(volatile uint32_t *)0x2001FF00 = 0x1CD00002;   /* marker: init done */
	fill_window();
	*(volatile uint32_t *)0x2001FF00 = 0x1CD00003;   /* marker: filled */
	while (1) { k_msleep(1000); }
	return 0;
}
