#include <string.h>
#include "gba.h"

#define RING_FRAMES 16384
#define CYCLES_PER_SAMPLE (16777216 / AUDIO_RATE)

static const u8 duty_table[4] = { 0x01, 0x81, 0x87, 0x7E };

typedef struct {
	int timer;
	int step;
	int volume;
	int env_timer, env_period;
	bool env_up;
	int length;
	bool on, length_on;
	int sweep_timer, sweep_period, sweep_shift;
	bool sweep_down;
	int shadow;
} Square;

typedef struct {
	int timer;
	int pos;
	int length;
	bool on, length_on;
} Wave;

typedef struct {
	int timer;
	u16 lfsr;
	int volume;
	int env_timer, env_period;
	bool env_up;
	int length;
	bool on, length_on;
} Noise;

static Square sq[2];
static Wave wave;
static Noise noise;

static u8 wave_ram[2][16];

static s8 fifo[2][32];
static int fifo_head[2], fifo_len[2];
static s8 fifo_cur[2];

static s16 ring[RING_FRAMES * 2];
static volatile int ring_w, ring_r;

static int sample_timer;

static inline u16 reg(u32 a) { return io_read16(a); }

static int wave_bank(void)
{
	return (reg(0x70) >> 6) & 1;
}

u8 apu_read_wave(u32 addr)
{
	return wave_ram[wave_bank() ^ 1][addr & 0xF];
}

/* ---- channel triggers ---- */

static void trigger_square(int n)
{
	u32 base = n ? 0x68 : 0x62;
	u16 h = reg(base), x = reg(base + (n ? 4 : 2));

	sq[n].on = true;
	sq[n].volume = (h >> 12) & 0xF;
	sq[n].env_period = (h >> 8) & 7;
	sq[n].env_up = h & 0x800;
	sq[n].env_timer = sq[n].env_period * 262144;
	sq[n].length = (64 - (h & 0x3F)) * 65536;
	sq[n].length_on = x & 0x4000;
	sq[n].timer = 0;

	if (n == 0) {
		u16 s = reg(0x60);
		sq[0].sweep_shift = s & 7;
		sq[0].sweep_down = s & 8;
		sq[0].sweep_period = (s >> 4) & 7;
		sq[0].sweep_timer = sq[0].sweep_period * 131072;
		sq[0].shadow = x & 0x7FF;
	}
}

static void trigger_wave(void)
{
	wave.on = true;
	wave.pos = 0;
	wave.timer = 0;
	wave.length = (256 - (reg(0x72) & 0xFF)) * 65536;
	wave.length_on = reg(0x74) & 0x4000;
}

static void trigger_noise(void)
{
	u16 l = reg(0x78), h = reg(0x7C);

	noise.on = true;
	noise.lfsr = 0x7FFF;
	noise.volume = (l >> 12) & 0xF;
	noise.env_period = (l >> 8) & 7;
	noise.env_up = l & 0x800;
	noise.env_timer = noise.env_period * 262144;
	noise.length = (64 - (l & 0x3F)) * 65536;
	noise.length_on = h & 0x4000;
	noise.timer = 0;
}

void apu_write8(u32 addr, u8 v)
{
	addr &= 0x3FF;

	if (addr >= 0x90 && addr < 0xA0) {
		wave_ram[wave_bank() ^ 1][addr & 0xF] = v;
		return;
	}
	if (addr >= 0xA0) return;

	io[addr] = v;

	switch (addr) {
	case 0x65: if (v & 0x80) trigger_square(0); break;
	case 0x6D: if (v & 0x80) trigger_square(1); break;
	case 0x75: if (v & 0x80) trigger_wave(); break;
	case 0x7D: if (v & 0x80) trigger_noise(); break;
	case 0x83:
		if (v & 0x08) { fifo_len[0] = fifo_head[0] = 0; fifo_cur[0] = 0; }
		if (v & 0x80) { fifo_len[1] = fifo_head[1] = 0; fifo_cur[1] = 0; }
		break;
	case 0x84:
		if (!(v & 0x80)) {
			sq[0].on = sq[1].on = wave.on = noise.on = false;
		}
		break;
	}
}

void apu_fifo_write(int f, u32 value, int width)
{
	int bytes = width / 8;

	for (int i = 0; i < bytes; i++) {
		if (fifo_len[f] >= 32) break;
		fifo[f][(fifo_head[f] + fifo_len[f]) & 31] = (s8)(value >> (i * 8));
		fifo_len[f]++;
	}
}

void apu_timer_overflow(int n)
{
	if (n > 1) return;

	u16 ctl = reg(REG_SOUNDCNT_H);

	for (int f = 0; f < 2; f++) {
		if ((int)((ctl >> (f ? 14 : 10)) & 1) != n) continue;

		if (fifo_len[f] > 0) {
			fifo_cur[f] = fifo[f][fifo_head[f]];
			fifo_head[f] = (fifo_head[f] + 1) & 31;
			fifo_len[f]--;
		}
		if (fifo_len[f] <= 16) dma_fifo(f);
	}
}

/* ---- channel stepping ---- */

static void step_square(int n, int cycles)
{
	if (!sq[n].on) return;

	u32 xreg = n ? 0x6C : 0x64;
	int rate = reg(xreg) & 0x7FF;
	int period = (2048 - rate) * 16;

	sq[n].timer += cycles;
	while (sq[n].timer >= period) {
		sq[n].timer -= period;
		sq[n].step = (sq[n].step + 1) & 7;
	}

	if (sq[n].env_period) {
		sq[n].env_timer -= cycles;
		while (sq[n].env_timer <= 0) {
			sq[n].env_timer += sq[n].env_period * 262144;
			if (sq[n].env_up && sq[n].volume < 15) sq[n].volume++;
			else if (!sq[n].env_up && sq[n].volume > 0) sq[n].volume--;
		}
	}

	if (sq[n].length_on) {
		sq[n].length -= cycles;
		if (sq[n].length <= 0) sq[n].on = false;
	}

	if (n == 0 && sq[0].sweep_period && sq[0].sweep_shift) {
		sq[0].sweep_timer -= cycles;
		while (sq[0].sweep_timer <= 0) {
			sq[0].sweep_timer += sq[0].sweep_period * 131072;
			int delta = sq[0].shadow >> sq[0].sweep_shift;
			sq[0].shadow += sq[0].sweep_down ? -delta : delta;
			if (sq[0].shadow > 2047 || sq[0].shadow < 0) {
				sq[0].on = false;
				sq[0].shadow = 0;
			} else {
				io_write16(0x64, (u16)((reg(0x64) & ~0x7FF) | sq[0].shadow));
			}
		}
	}
}

static void step_wave(int cycles)
{
	if (!wave.on || !(reg(0x70) & 0x80)) return;

	int rate = reg(0x74) & 0x7FF;
	int period = (2048 - rate) * 8;
	int len = (reg(0x70) & 0x20) ? 64 : 32;

	wave.timer += cycles;
	while (wave.timer >= period) {
		wave.timer -= period;
		wave.pos = (wave.pos + 1) % len;
	}

	if (wave.length_on) {
		wave.length -= cycles;
		if (wave.length <= 0) wave.on = false;
	}
}

static void step_noise(int cycles)
{
	if (!noise.on) return;

	u16 h = reg(0x7C);
	int r = h & 7, s = (h >> 4) & 0xF;
	int period = (r ? 32 * r : 16) << (s + 1);

	noise.timer += cycles;
	while (noise.timer >= period) {
		noise.timer -= period;
		u16 bit = (noise.lfsr ^ (noise.lfsr >> 1)) & 1;
		noise.lfsr >>= 1;
		noise.lfsr |= bit << 14;
		if (h & 8) {
			noise.lfsr &= ~0x40u;
			noise.lfsr |= bit << 6;
		}
	}

	if (noise.env_period) {
		noise.env_timer -= cycles;
		while (noise.env_timer <= 0) {
			noise.env_timer += noise.env_period * 262144;
			if (noise.env_up && noise.volume < 15) noise.volume++;
			else if (!noise.env_up && noise.volume > 0) noise.volume--;
		}
	}

	if (noise.length_on) {
		noise.length -= cycles;
		if (noise.length <= 0) noise.on = false;
	}
}

/* ---- mixing ---- */

static int square_out(int n)
{
	if (!sq[n].on) return 0;
	u16 h = reg(n ? 0x68 : 0x62);
	int duty = (h >> 6) & 3;
	return (duty_table[duty] >> sq[n].step) & 1 ? sq[n].volume : 0;
}

static int wave_out(void)
{
	if (!wave.on || !(reg(0x70) & 0x80)) return 0;

	int bank = wave_bank();
	int pos = wave.pos;
	if (reg(0x70) & 0x20) {
		bank ^= (pos >= 32);
		pos &= 31;
	}

	u8 byte = wave_ram[bank][pos >> 1];
	int s = (pos & 1) ? (byte & 0xF) : (byte >> 4);

	u16 h = reg(0x72);
	if (h & 0x8000) return s * 3 / 4;

	switch ((h >> 13) & 3) {
	case 0: return 0;
	case 1: return s;
	case 2: return s / 2;
	default: return s / 4;
	}
}

static int noise_out(void)
{
	if (!noise.on) return 0;
	return (~noise.lfsr & 1) ? noise.volume : 0;
}

static void emit_sample(void)
{
	u16 cl = reg(REG_SOUNDCNT_L), ch = reg(REG_SOUNDCNT_H);

	if (!(reg(REG_SOUNDCNT_X) & 0x80)) {
		int w = ring_w;
		ring[w * 2] = ring[w * 2 + 1] = 0;
		ring_w = (w + 1) % RING_FRAMES;
		return;
	}

	int out[4] = { square_out(0), square_out(1), wave_out(), noise_out() };
	int l = 0, r = 0;

	for (int i = 0; i < 4; i++) {
		if (cl & (0x1000 << i)) l += out[i];
		if (cl & (0x100 << i)) r += out[i];
	}

	l = l * (((cl >> 4) & 7) + 1) / 8;
	r = r * ((cl & 7) + 1) / 8;

	static const int psg_shift[4] = { 2, 1, 0, 0 };
	l >>= psg_shift[ch & 3];
	r >>= psg_shift[ch & 3];

	l *= 90;
	r *= 90;

	int a = fifo_cur[0] * ((ch & 4) ? 2 : 1);
	int b = fifo_cur[1] * ((ch & 8) ? 2 : 1);

	if (ch & 0x200) l += a * 48;
	if (ch & 0x100) r += a * 48;
	if (ch & 0x2000) l += b * 48;
	if (ch & 0x1000) r += b * 48;

	if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
	if (r > 32767) r = 32767; else if (r < -32768) r = -32768;

	int w = ring_w;
	int next = (w + 1) % RING_FRAMES;
	if (next == ring_r) return;

	ring[w * 2] = (s16)l;
	ring[w * 2 + 1] = (s16)r;
	ring_w = next;
}

void apu_run(int cycles)
{
	step_square(0, cycles);
	step_square(1, cycles);
	step_wave(cycles);
	step_noise(cycles);

	sample_timer += cycles;
	while (sample_timer >= CYCLES_PER_SAMPLE) {
		sample_timer -= CYCLES_PER_SAMPLE;
		emit_sample();
	}
}

int apu_read_samples(s16 *dst, int frames)
{
	int n = 0;

	while (n < frames && ring_r != ring_w) {
		dst[n * 2] = ring[ring_r * 2];
		dst[n * 2 + 1] = ring[ring_r * 2 + 1];
		ring_r = (ring_r + 1) % RING_FRAMES;
		n++;
	}
	return n;
}

void apu_reset(void)
{
	memset(sq, 0, sizeof sq);
	memset(&wave, 0, sizeof wave);
	memset(&noise, 0, sizeof noise);
	memset(wave_ram, 0, sizeof wave_ram);
	memset(fifo, 0, sizeof fifo);
	memset(fifo_head, 0, sizeof fifo_head);
	memset(fifo_len, 0, sizeof fifo_len);
	fifo_cur[0] = fifo_cur[1] = 0;
	ring_w = ring_r = 0;
	sample_timer = 0;
	noise.lfsr = 0x7FFF;
}
