# Button Names

> Auto-saved from overnight research agent. Static analysis only — nothing here was tested on hardware.

**Summary:** I established the complete, definitive stock-key-code → button-name mapping for all 53 codes (128..180), and cross-validated it against a second independent source. The firmware path: `DiagHandleHardkeyPress` at 0x080046a8 does `code -= 128; if (code > 52) bail; TBH [pc, code*2]` (dispatch at 0x08004706, 53-halfword table at 0x0800470a); each case loads the (x0,y0,x1,y1) of one cell of the "Keypad Test" diag page and falls into the common tail at 0x08004c22 which paints "OK!" (0x08004ef4) over that cell's "--" placeholder (0x08005664). `_DiagPaintPageKeypad` at 0x080053f0 paints those same 53 placeholder rects together with their text labels (3 columns, 23+23+7 rows, y0 = 25 + 13·row). Joining case-rect → label is a perfect bijection: 53 codes, 53 distinct labels, none missing, none duplicated. Independently, Integration Designer's idesign.exe carries the T2i programmable-hardkey list as length-prefixed UTF-16 at file offset 0xfc1588 — 52 entries where entry i is exactly code 128+i; it matches the firmware-derived names 52/52 with zero mismatches, the only absent code being 180 (Backlight, a local function ID does not expose). I also confirmed the keymap at 0x08011344 is indexed row*8+col, and that the existing `src/keypad.c` table, row pins (PE0/1/2/12-15) and column pins (PC0-7) are byte-for-byte correct.

**Open questions:**
- The four softkeys' left/right identities rest on assuming Integration Designer numbers Softkey 1..4 left-to-right (giving Softkey1=Soft Lft=136, Softkey2=Soft Lft Cntr=130, Softkey3=Soft Rht Cntr=177, Softkey4=Soft Rht=137). The firmware's own paint order corroborates this (it paints Soft Lft, Soft Lft Cntr, Soft Rht Cntr, Soft Rht consecutively, which reads left-to-right), but I have no visual confirmation of which physical key is leftmost. If the four ever matter individually, this is the one pair-of-pairs worth eyeballing on hardware. Everything else is unambiguous.
- Who populates the stock runtime name array at 0x20000800 (.bss, zero-filled at boot)? Most plausibly the Integration Designer configuration blob downloaded to the remote — which would also explain both why no pointer array exists in flash and why the ID list order matches the key code order exactly. Not chased further because the delivered mapping does not depend on it.
- idesign.exe contains a second, near-identical copy of the T2i list immediately after the first (starting at index 52, file offset ~0xfc1846) whose tail then diverges (On, Off, Volume Up, ... Select Music). Probably a sibling model (T2-C?) or a duplicated resource. I did not check whether that variant's codes differ.
- No hardware verification is possible right now (read-only w.r.t. the device, and the radio remote can't be opened for ~1 month). Cheapest confirmation once you next flash the bench remote: keypad_scan() already returns row/col, so log `keypad_name(code)` alongside row/col and press keys around the perimeter — Scroll Left/Right and Soft Lft/Soft Rht are the highest-value spot checks.
- The literal label "-/." (code 164, r6 c5) is what the firmware prints; I have not checked it against the physical silkscreen. Integration Designer calls it "-/." too, and a sibling model's list uses "-/--", so the glyph may vary by market.

---

## Stock key code → button name (T2i)

All 53 stock key codes (128..180) are mapped. The mapping is **confirmed twice, independently**, with zero disagreement on the 52 codes both sources cover.

### How it was established

**Source 1 — stock firmware diagnostics.** `DiagHandleHardkeyPress` (entry `0x080046a8`, debug string `"DiagHandleHardkeyPress\r\n"` at `0x08004c3c`) dispatches on the key code:

```
80046f8: f44f 407f  mov.w r0, #0xff00      ; set colour
80046fc: f002 f9a3  bl 0x8006a46
8004700: 3c80       subs r4, #128          ; r4 = code - 128
8004702: 2c34       cmp  r4, #52
8004704: d8f4       bhi.n 0x80046f0        ; out of range -> bail
8004706: e8df f014  tbh  [pc, r4, lsl #1]
800470a: <53 halfwords>                    ; the jump table
```

Each of the 53 cases loads one screen rectangle and joins a common tail that paints `"OK!"` (`0x08004ef4`) over that cell:

```
8004776: movs r0,#162  strh [sp,#0]   ; x0
800477c: movs r0,#182  strh [sp,#4]   ; x1
8004782: movs r0,#25   strh [sp,#2]   ; y0
8004788: movs r0,#36
800478a: b 0x8004c22
...
8004c22: strh.w r0,[sp,#6]            ; y1
8004c26: add r0, sp, #0
8004c28: bl 0x800bd80                 ; fill cell
8004c30: adr r0, 0x8004ef4            ; "OK!"
8004c32: bl 0x800bf28                 ; draw_text(str, rect, len)
```

`_DiagPaintPageKeypad` (entry `0x080053f0`, strings `"_DiagPaintPageKeypad()\r\n"` @ `0x08041784`, `"Keypad Test"` @ `0x080417a0`) paints those same 53 rectangles as `"--"` placeholders (`0x08005664`) each followed immediately by its text label on the same y band:

```
8005438: movs r0,#255 ; bl 0x8006a46     (white)
800543e: r0=0  -> strh [sp,#0]   x0=0
8005444: r0=20 -> strh [sp,#4]   x1=20
800544a: r0=25 -> strh [sp,#2]   y0=25
8005450: r0=36 -> strh [sp,#6]   y1=36
8005456: adr r4, 0x8005664 ("--")
800545e: bl 0x800bf28
8005462: movs r0,#0 ; bl 0x8006a46       (black)
8005468: r0=26 -> strh [sp,#0]   x0=26
800546e: r0=80 -> strh [sp,#4]   x1=80
8005478: ldr r0,[pc] -> [0x08006310] -> 0x080417ac "Soft Lft"
800547c: bl 0x800bf28
```

Layout is 3 columns of 23 / 23 / 7 rows: placeholder `x0` = 0 / 81 / 162, label `x0` = 26 / 107 / 185, `y0` = 25 + 13·row (tail rows at 284 / 296 / 308 — these use `mov.w`, so a `movs`-only disassembly parse silently mis-attributes them).

Joining case-rect → label is a **bijection**: 53 codes, 53 distinct labels, nothing missing, nothing duplicated.

**Source 2 — Integration Designer.** `idesign.exe` stores per-model hardkey name lists in `.rsrc` as length-prefixed UTF-16 (uint16 char count, then chars, no NUL), blocks separated by a zero length. The T2i list begins at **file offset `0xfc1588`** and runs **52 entries, where entry `i` is code `128 + i`**. (A 2-byte `'PA'` chunk marker at `0xfc1712` must be resynced past.) It matches the firmware-derived names 52/52 under the obvious vocabulary aliases (`Soft Lft` = `Softkey 1`, `Vol +` = `Volume Up`, `Scroll Up` = `Joystick Up`, …). Only code **180 (Backlight)** is absent — a local hardware function, not a programmable key.

**Matrix position** comes from the keymap at `0x08011344`, indexed `row*8 + col`:

```
8011240: f20f 1000  addw r0, pc, #256           -> 0x08011344
8011244: eb00 00c5  add.w r0, r0, r5, lsl #3    ; r5 = row (cmp r5,#7)
8011248: f814 8000  ldrb.w r8, [r4, r0]         ; r4 = col (cmp r4,#8)
```

Column pin masks at `0x08011324` = `1,2,4,8,0x10,0x20,0x40,0x80` (PC0..PC7); row masks at `0x08011334` = `1,2,4,0x1000,0x2000,0x4000,0x8000` (PE0, PE1, PE2, PE12..PE15). Port bases `0x40020800` (GPIOC) / `0x40021000` (GPIOE) at `0x080112bc` / `0x080112c0`.

### The table

Paste into `src/keypad.c`:

```c
/* Stock RTI key code (128..180) -> human button name.
 *
 * Confirmed two independent ways, agreeing 52/52 on the codes both cover:
 *
 *  1. Stock firmware. DiagHandleHardkeyPress (0x080046a8) does
 *     `code -= 128; if (code > 52) bail; TBH [pc, code*2]` at 0x08004706 with
 *     the 53-halfword table at 0x0800470a. Each case names one cell of the
 *     "Keypad Test" page; _DiagPaintPageKeypad (0x080053f0) paints the text
 *     label beside that same cell. The join is a bijection over all 53 codes.
 *
 *  2. Integration Designer. idesign.exe holds the T2i programmable-hardkey
 *     list as length-prefixed UTF-16 at file offset 0xfc1588, 52 entries,
 *     entry i == code 128+i. Only 180 (Backlight) is absent - it is a local
 *     function, not programmable. Its ID aliases are noted per row below.
 *
 * Matrix position is from the stock keymap at 0x08011344 (row*8 + col).
 */
static const char *const key_names[53] = {
	/* 128 */ "Exit",            /* r2 c4  PE2 /PC4 */
	/* 129 */ "Mute",            /* r0 c7  PE0 /PC7 */
	/* 130 */ "Soft Lft Cntr",   /* r0 c3  PE0 /PC3  ID: Softkey 2 */
	/* 131 */ "Up",              /* r1 c4  PE1 /PC4 */
	/* 132 */ "Left",            /* r1 c5  PE1 /PC5 */
	/* 133 */ "Right",           /* r1 c7  PE1 /PC7 */
	/* 134 */ "Down",            /* r2 c2  PE2 /PC2 */
	/* 135 */ "OK",              /* r5 c1  PE14/PC1 */
	/* 136 */ "Soft Lft",        /* r0 c4  PE0 /PC4  ID: Softkey 1 */
	/* 137 */ "Soft Rht",        /* r0 c1  PE0 /PC1  ID: Softkey 4 */
	/* 138 */ "Vol +",           /* r1 c6  PE1 /PC6 */
	/* 139 */ "Vol -",           /* r2 c1  PE2 /PC1 */
	/* 140 */ "Ch +",            /* r6 c7  PE15/PC7 */
	/* 141 */ "Ch -",            /* r2 c3  PE2 /PC3 */
	/* 142 */ "Guide",           /* r2 c7  PE2 /PC7 */
	/* 143 */ "Menu",            /* r2 c6  PE2 /PC6 */
	/* 144 */ "Info",            /* r2 c5  PE2 /PC5 */
	/* 145 */ "Off",             /* r0 c5  PE0 /PC5  ID: Power Off */
	/* 146 */ "Play",            /* r4 c2  PE13/PC2 */
	/* 147 */ "Pause",           /* r3 c6  PE12/PC6 */
	/* 148 */ "Stop",            /* r3 c5  PE12/PC5 */
	/* 149 */ "Record",          /* r3 c4  PE12/PC4 */
	/* 150 */ "<<",              /* r4 c4  PE13/PC4  ID: Scan << */
	/* 151 */ ">>",              /* r4 c3  PE13/PC3  ID: Scan >> */
	/* 152 */ "|<<",             /* r4 c6  PE13/PC6  ID: Skip << */
	/* 153 */ ">>|",             /* r4 c5  PE13/PC5  ID: Skip >> */
	/* 154 */ "1",               /* r5 c3  PE14/PC3 */
	/* 155 */ "2",               /* r5 c2  PE14/PC2 */
	/* 156 */ "3",               /* r4 c7  PE13/PC7 */
	/* 157 */ "4",               /* r5 c6  PE14/PC6 */
	/* 158 */ "5",               /* r5 c5  PE14/PC5 */
	/* 159 */ "6",               /* r5 c4  PE14/PC4 */
	/* 160 */ "7",               /* r6 c2  PE15/PC2 */
	/* 161 */ "8",               /* r6 c6  PE15/PC6 */
	/* 162 */ "9",               /* r5 c7  PE14/PC7 */
	/* 163 */ "0",               /* r6 c4  PE15/PC4 */
	/* 164 */ "-/.",             /* r6 c5  PE15/PC5 */
	/* 165 */ "Enter",           /* r6 c3  PE15/PC3 */
	/* 166 */ "Scroll Up",       /* r3 c0  PE12/PC0  ID: Joystick Up */
	/* 167 */ "Scroll Click",    /* r2 c0  PE2 /PC0  ID: Joystick Click */
	/* 168 */ "Scroll Down",     /* r1 c0  PE1 /PC0  ID: Joystick Down */
	/* 169 */ "Scroll Left",     /* r0 c0  PE0 /PC0  ID: Joystick Left */
	/* 170 */ "Scroll Right",    /* r4 c0  PE13/PC0  ID: Joystick Right */
	/* 171 */ "On",              /* r0 c6  PE0 /PC6  ID: Power On */
	/* 172 */ "List",            /* r1 c2  PE1 /PC2 */
	/* 173 */ "Red",             /* r3 c2  PE12/PC2 */
	/* 174 */ "Green",           /* r3 c7  PE12/PC7 */
	/* 175 */ "Yellow",          /* r3 c3  PE12/PC3 */
	/* 176 */ "Blue",            /* r3 c1  PE12/PC1 */
	/* 177 */ "Soft Rht Cntr",   /* r0 c2  PE0 /PC2  ID: Softkey 3 */
	/* 178 */ "Prev",            /* r1 c3  PE1 /PC3 */
	/* 179 */ "Back",            /* r1 c1  PE1 /PC1 */
	/* 180 */ "Backlight",       /* r6 c1  PE15/PC1  (not exposed by ID) */
};
BUILD_ASSERT(ARRAY_SIZE(key_names) == 53);

const char *keypad_name(uint8_t code)
{
	/* Stock FUN_0801107e returns "NONE" for 0 and "UNKNOWN" otherwise;
	 * KEY_NONE is 0xFF here, so one unsigned range test covers both. */
	return (uint8_t)(code - 128u) < 53u ? key_names[code - 128] : "UNKNOWN";
}
```

And in `src/keypad.h`:

```c
/* Human name for a stock key code; "UNKNOWN" outside 128..180. */
const char *keypad_name(uint8_t code);
```

### Matrix layout (all confirmed)

Columns PC0..PC7 driven low one at a time; rows PE0/1/2/12-15 sensed active-low. `—` = unpopulated (0xFF in the stock keymap).

| row | pin | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 |
|---|---|---|---|---|---|---|---|---|---|
| 0 | PE0  | Scroll Left  | Soft Rht  | Soft Rht Cntr | Soft Lft Cntr | Soft Lft | Off  | On    | Mute  |
| 1 | PE1  | Scroll Down  | Back      | List          | Prev          | Up       | Left | Vol + | Right |
| 2 | PE2  | Scroll Click | Vol -     | Down          | Ch -          | Exit     | Info | Menu  | Guide |
| 3 | PE12 | Scroll Up    | Blue      | Red           | Yellow        | Record   | Stop | Pause | Green |
| 4 | PE13 | Scroll Right | —         | Play          | >>            | <<       | >>\| | \|<<  | 3     |
| 5 | PE14 | —            | OK        | 2             | 1             | 6        | 5    | 4     | 9     |
| 6 | PE15 | —            | Backlight | 7             | Enter         | 0        | -/.  | 8     | Ch +  |

Two structural facts fall out and act as sanity checks: **PC0 carries nothing but the 5-way joystick**, and **PE0 carries nothing but the top strip** (4 softkeys + Off/On/Mute, plus the joystick's left contact). The four softkeys are wired in *descending* column order left-to-right: c4 = Soft Lft, c3 = Soft Lft Cntr, c2 = Soft Rht Cntr, c1 = Soft Rht.

### Notes

- `KEY_DEBUG 180` (already in `keypad.h`) is the physical **Backlight** button — the one key Integration Designer does not expose as programmable, so the least likely to collide with a normal remote function. Keep it.
- Stock's own runtime lookup, `FUN_0801107e`, reads `((const char **)0x20000600)[code]` (base literal `0x20000800` at `0x080112b8`, minus `0x200`), i.e. entry[128] at `0x20000800` — the first object in zero-filled `.bss` (`{0x20000800, 0x17d0}` in the IAR init table at `0x080418a0`). That array is populated at runtime by code not yet located; nothing above depends on it. Other `0x20000800` literals in the image (`0x0800d6f8`, `0x0801649c`) are **ST StdPeriph DMA flag bitmasks**, not addresses — their pool neighbours are `0x20000400`, `0x20000200`, `0x40026010`, `0x40026088`.

