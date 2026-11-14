#ifndef GBA_H
#define GBA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define SCREEN_W 240
#define SCREEN_H 160

#define DOTS_PER_LINE    308
#define LINES_PER_FRAME  228
#define CYCLES_PER_FRAME (DOTS_PER_LINE * LINES_PER_FRAME * 4)

/* ---- CPU ---- */

enum {
	MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
	MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F
};

#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u
#define FLAG_I 0x00000080u
#define FLAG_F 0x00000040u
#define FLAG_T 0x00000020u

enum { BANK_USR, BANK_FIQ, BANK_IRQ, BANK_SVC, BANK_ABT, BANK_UND, BANK_COUNT };

typedef struct {
	u32 r[16];
	u32 cpsr;
	u32 spsr;

	u32 bank_r13[BANK_COUNT];
	u32 bank_r14[BANK_COUNT];
	u32 bank_spsr[BANK_COUNT];
	u32 bank_fiq[5];   /* r8-r12 while in FIQ */
	u32 bank_usr[5];   /* r8-r12 otherwise */

	u32 pipe[2];
	int bank;
	bool flush;
	bool halt;
} CPU;

extern CPU cpu;

void cpu_reset(void);
int  cpu_step(void);
void cpu_flush(void);
void cpu_set_mode(u32 mode);
void cpu_check_irq(void);
void cpu_swi(u32 comment);
void cpu_undefined(void);

void arm_exec(u32 op);
void thumb_exec(u16 op);

static inline bool cpu_thumb(void) { return (cpu.cpsr & FLAG_T) != 0; }

/* ---- Memory ---- */

extern u8 bios[0x4000];
extern u8 ewram[0x40000];
extern u8 iwram[0x8000];
extern u8 io[0x400];
extern u8 pram[0x400];
extern u8 vram[0x18000];
extern u8 oam[0x400];
extern u8 *rom;
extern size_t rom_size;

extern int bus_cycles;

u8   bus_read8(u32 addr);
u16  bus_read16(u32 addr);
u32  bus_read32(u32 addr);
void bus_write8(u32 addr, u8 v);
void bus_write16(u32 addr, u16 v);
void bus_write32(u32 addr, u32 v);

u32 bus_fetch32(u32 addr);
u16 bus_fetch16(u32 addr);

#define IO8(a)  (io[(a) & 0x3FF])
static inline u16 io_read16(u32 a) { return (u16)(io[a & 0x3FF] | (io[(a + 1) & 0x3FF] << 8)); }
static inline void io_write16(u32 a, u16 v) { io[a & 0x3FF] = (u8)v; io[(a + 1) & 0x3FF] = (u8)(v >> 8); }

/* ---- I/O registers ---- */

#define REG_DISPCNT   0x000
#define REG_DISPSTAT  0x004
#define REG_VCOUNT    0x006
#define REG_BG0CNT    0x008
#define REG_BG0HOFS   0x010
#define REG_BG2PA     0x020
#define REG_BG2X      0x028
#define REG_BG2Y      0x02C
#define REG_BG3PA     0x030
#define REG_BG3X      0x038
#define REG_BG3Y      0x03C
#define REG_WIN0H     0x040
#define REG_WIN1H     0x042
#define REG_WIN0V     0x044
#define REG_WIN1V     0x046
#define REG_WININ     0x048
#define REG_WINOUT    0x04A
#define REG_MOSAIC    0x04C
#define REG_BLDCNT    0x050
#define REG_BLDALPHA  0x052
#define REG_BLDY      0x054
#define REG_SOUNDCNT_L 0x080
#define REG_SOUNDCNT_H 0x082
#define REG_SOUNDCNT_X 0x084
#define REG_SOUNDBIAS  0x088
#define REG_FIFO_A    0x0A0
#define REG_FIFO_B    0x0A4
#define REG_DMA0SAD   0x0B0
#define REG_TM0CNT_L  0x100
#define REG_KEYINPUT  0x130
#define REG_KEYCNT    0x132
#define REG_IE        0x200
#define REG_IF        0x202
#define REG_WAITCNT   0x204
#define REG_IME       0x208
#define REG_HALTCNT   0x301

enum {
	IRQ_VBLANK = 1 << 0,  IRQ_HBLANK = 1 << 1,  IRQ_VCOUNT = 1 << 2,
	IRQ_TIMER0 = 1 << 3,  IRQ_TIMER1 = 1 << 4,  IRQ_TIMER2 = 1 << 5,
	IRQ_TIMER3 = 1 << 6,  IRQ_SERIAL = 1 << 7,  IRQ_DMA0   = 1 << 8,
	IRQ_DMA1   = 1 << 9,  IRQ_DMA2   = 1 << 10, IRQ_DMA3   = 1 << 11,
	IRQ_KEYPAD = 1 << 12, IRQ_GAMEPAK = 1 << 13
};

void irq_raise(u16 flag);

/* ---- PPU ---- */

extern u16 framebuffer[SCREEN_W * SCREEN_H];

void ppu_reset(void);
void ppu_run(int cycles);
void ppu_write_dispstat(u16 v);
void ppu_write_affine_ref(u32 addr);
u16  ppu_vcount(void);

/* ---- DMA ---- */

enum { DMA_NOW, DMA_VBLANK, DMA_HBLANK, DMA_SPECIAL };

typedef struct {
	u32 src, dst;
	u32 count;
	bool running;
} DmaChannel;

extern DmaChannel dma[4];

void dma_reset(void);
void dma_write_control(int ch, u16 v);
void dma_trigger(int timing);
void dma_fifo(int fifo);

/* ---- Timers ---- */

typedef struct {
	u16 counter, reload;
	u16 control;
	int shift;
	int residue;
} Timer;

extern Timer timers[4];

void timer_reset(void);
void timer_run(int cycles);
void timer_write_control(int n, u16 v);
void timer_write_reload(int n, u16 v);
u16  timer_read_counter(int n);

/* ---- APU ---- */

#define AUDIO_RATE 32768

void apu_reset(void);
void apu_run(int cycles);
void apu_write8(u32 addr, u8 v);
void apu_fifo_write(int fifo, u32 value, int width);
int  apu_read_samples(s16 *out, int frames);
void apu_timer_overflow(int timer_id);

/* ---- Cartridge ---- */

enum { SAVE_NONE, SAVE_SRAM, SAVE_EEPROM512, SAVE_EEPROM8K, SAVE_FLASH64, SAVE_FLASH128 };

extern int save_type;

bool cart_load(const char *path);
void cart_unload(void);
u8   cart_read_sram(u32 addr);
void cart_write_sram(u32 addr, u8 v);
bool cart_is_eeprom_addr(u32 addr);
u16  cart_read_eeprom(void);
void cart_write_eeprom(u16 v, u32 dma_count);
void cart_save_battery(void);
void cart_load_battery(void);
const char *cart_title(void);

/* ---- BIOS HLE ---- */

void bios_init_stub(void);
void bios_swi(u32 comment);

/* ---- System ---- */

extern u16 keys_state;   /* active low, matches KEYINPUT */
extern bool use_hle_bios;

void gba_reset(void);
void gba_run_frame(void);
bool gba_load_bios(const char *path);

#endif
