#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "gba.h"

/* Minimal stand-in for the real BIOS: just the interrupt dispatcher that games
   expect at 0x18, which calls the handler pointer kept at 0x03FFFFFC. */
void bios_init_stub(void)
{
	static const u32 handler[] = {
		0xE92D500F,   /* stmfd sp!, {r0-r3,r12,lr} */
		0xE3A00301,   /* mov   r0, #0x4000000      */
		0xE28FE000,   /* add   lr, pc, #0          */
		0xE510F004,   /* ldr   pc, [r0, #-4]       */
		0xE8BD500F,   /* ldmfd sp!, {r0-r3,r12,lr} */
		0xE25EF004,   /* subs  pc, lr, #4          */
	};

	memset(bios, 0, sizeof bios);
	*(u32 *)&bios[0x18] = 0xEA000042;
	memcpy(&bios[0x128], handler, sizeof handler);
}

static void swi_cpu_set(void)
{
	u32 src = cpu.r[0], dst = cpu.r[1], ctl = cpu.r[2];
	u32 count = ctl & 0x1FFFFF;
	bool fixed = ctl & (1 << 24);

	if (ctl & (1 << 26)) {
		src &= ~3u;
		dst &= ~3u;
		u32 v = 0;
		for (u32 i = 0; i < count; i++) {
			if (!fixed || i == 0) v = bus_read32(src);
			bus_write32(dst, v);
			if (!fixed) src += 4;
			dst += 4;
		}
	} else {
		src &= ~1u;
		dst &= ~1u;
		u16 v = 0;
		for (u32 i = 0; i < count; i++) {
			if (!fixed || i == 0) v = bus_read16(src);
			bus_write16(dst, v);
			if (!fixed) src += 2;
			dst += 2;
		}
	}
}

static void swi_cpu_fast_set(void)
{
	u32 src = cpu.r[0] & ~3u, dst = cpu.r[1] & ~3u, ctl = cpu.r[2];
	u32 count = ((ctl & 0x1FFFFF) + 7) & ~7u;
	bool fixed = ctl & (1 << 24);

	u32 v = 0;
	for (u32 i = 0; i < count; i++) {
		if (!fixed || i == 0) v = bus_read32(src);
		bus_write32(dst, v);
		if (!fixed) src += 4;
		dst += 4;
	}
}

static void swi_bg_affine(void)
{
	u32 src = cpu.r[0], dst = cpu.r[1], count = cpu.r[2];

	for (u32 i = 0; i < count; i++) {
		s32 cx = (s32)bus_read32(src);
		s32 cy = (s32)bus_read32(src + 4);
		s16 dx = (s16)bus_read16(src + 8);
		s16 dy = (s16)bus_read16(src + 10);
		s16 sx = (s16)bus_read16(src + 12);
		s16 sy = (s16)bus_read16(src + 14);
		u16 angle = bus_read16(src + 16);
		src += 20;

		double th = (angle >> 8) * 2.0 * M_PI / 256.0;
		double c = cos(th), s = sin(th);

		s16 pa = (s16)(sx * c), pb = (s16)(-sx * s);
		s16 pc = (s16)(sy * s), pd = (s16)(sy * c);

		bus_write16(dst, (u16)pa);
		bus_write16(dst + 2, (u16)pb);
		bus_write16(dst + 4, (u16)pc);
		bus_write16(dst + 6, (u16)pd);
		bus_write32(dst + 8, (u32)(cx - (dx * pa + dy * pb)));
		bus_write32(dst + 12, (u32)(cy - (dx * pc + dy * pd)));
		dst += 16;
	}
}

static void swi_obj_affine(void)
{
	u32 src = cpu.r[0], dst = cpu.r[1], count = cpu.r[2], stride = cpu.r[3];

	for (u32 i = 0; i < count; i++) {
		s16 sx = (s16)bus_read16(src);
		s16 sy = (s16)bus_read16(src + 2);
		u16 angle = bus_read16(src + 4);
		src += 8;

		double th = (angle >> 8) * 2.0 * M_PI / 256.0;
		double c = cos(th), s = sin(th);

		bus_write16(dst, (u16)(s16)(sx * c));
		bus_write16(dst + stride, (u16)(s16)(-sx * s));
		bus_write16(dst + stride * 2, (u16)(s16)(sy * s));
		bus_write16(dst + stride * 3, (u16)(s16)(sy * c));
		dst += stride * 4;
	}
}

static void swi_lz77(bool vram_safe)
{
	u32 src = cpu.r[0], dst = cpu.r[1];
	u32 size = bus_read32(src) >> 8;
	src += 4;

	u32 written = 0;
	u16 half = 0;

	while (written < size) {
		u8 flags = bus_read8(src++);

		for (int i = 0; i < 8 && written < size; i++) {
			if (flags & 0x80) {
				u8 b0 = bus_read8(src), b1 = bus_read8(src + 1);
				src += 2;
				u32 len = (b0 >> 4) + 3;
				u32 disp = (((u32)(b0 & 0xF) << 8) | b1) + 1;

				for (u32 k = 0; k < len && written < size; k++) {
					u8 v = bus_read8(dst + written - disp);
					if (vram_safe) {
						if (written & 1) {
							bus_write16(dst + written - 1, (u16)(half | (v << 8)));
						} else {
							half = v;
						}
					} else {
						bus_write8(dst + written, v);
					}
					written++;
				}
			} else {
				u8 v = bus_read8(src++);
				if (vram_safe) {
					if (written & 1) bus_write16(dst + written - 1, (u16)(half | (v << 8)));
					else half = v;
				} else {
					bus_write8(dst + written, v);
				}
				written++;
			}
			flags <<= 1;
		}
	}

	if (vram_safe && (written & 1)) bus_write16(dst + written - 1, half);
}

static void swi_rl(bool vram_safe)
{
	u32 src = cpu.r[0], dst = cpu.r[1];
	u32 size = bus_read32(src) >> 8;
	src += 4;

	u32 written = 0;
	u16 half = 0;

	while (written < size) {
		u8 flag = bus_read8(src++);
		u32 len = (flag & 0x7F) + ((flag & 0x80) ? 3 : 1);
		u8 v = 0;
		if (flag & 0x80) v = bus_read8(src++);

		for (u32 k = 0; k < len && written < size; k++) {
			u8 b = (flag & 0x80) ? v : bus_read8(src++);
			if (vram_safe) {
				if (written & 1) bus_write16(dst + written - 1, (u16)(half | (b << 8)));
				else half = b;
			} else {
				bus_write8(dst + written, b);
			}
			written++;
		}
	}

	if (vram_safe && (written & 1)) bus_write16(dst + written - 1, half);
}

static void swi_huffman(void)
{
	u32 src = cpu.r[0], dst = cpu.r[1];
	u32 header = bus_read32(src);
	u32 size = header >> 8;
	int bits = header & 0xF;
	if (bits != 4 && bits != 8) return;

	u32 tree = src + 4;
	u32 stream = tree + (bus_read8(tree) + 1) * 2;

	u32 node = tree + 1;
	u32 word = 0, out = 0, written = 0;
	int avail = 0, out_bits = 0;

	while (written < size) {
		if (avail == 0) {
			word = bus_read32(stream);
			stream += 4;
			avail = 32;
		}

		int b = (word >> 31) & 1;
		word <<= 1;
		avail--;

		u8 n = bus_read8(node);
		bool leaf = n & (b ? 0x40 : 0x80);
		node = (node & ~1u) + ((n & 0x3F) + 1) * 2 + b;

		if (leaf) {
			u32 v = bus_read8(node) & ((1u << bits) - 1);
			out |= v << out_bits;
			out_bits += bits;

			if (out_bits == 32) {
				bus_write32(dst, out);
				dst += 4;
				written += 4;
				out = 0;
				out_bits = 0;
			}
			node = tree + 1;
		}
	}
}

static void swi_diff_unfilter(bool half)
{
	u32 src = cpu.r[0], dst = cpu.r[1];
	u32 size = bus_read32(src) >> 8;
	src += 4;

	if (half) {
		u16 acc = 0;
		for (u32 i = 0; i < size; i += 2) {
			acc = (u16)(acc + bus_read16(src));
			src += 2;
			bus_write16(dst, acc);
			dst += 2;
		}
	} else {
		u8 acc = 0;
		for (u32 i = 0; i < size; i++) {
			acc = (u8)(acc + bus_read8(src++));
			bus_write8(dst++, acc);
		}
	}
}

static void swi_bit_unpack(void)
{
	u32 src = cpu.r[0], dst = cpu.r[1], info = cpu.r[2];

	u16 len = bus_read16(info);
	u8 src_w = bus_read8(info + 2);
	u8 dst_w = bus_read8(info + 3);
	u32 offset = bus_read32(info + 4);
	bool zero_off = offset & 0x80000000u;
	offset &= 0x7FFFFFFF;

	u32 out = 0;
	int out_bits = 0;

	for (u32 i = 0; i < len; i++) {
		u8 b = bus_read8(src + i);

		for (int s = 0; s < 8; s += src_w) {
			u32 v = (b >> s) & ((1u << src_w) - 1);
			if (v || zero_off) v += offset;

			out |= v << out_bits;
			out_bits += dst_w;

			if (out_bits == 32) {
				bus_write32(dst, out);
				dst += 4;
				out = 0;
				out_bits = 0;
			}
		}
	}
}

static void swi_ram_reset(void)
{
	u32 f = cpu.r[0];

	if (f & 0x01) memset(ewram, 0, sizeof ewram);
	if (f & 0x02) memset(iwram, 0, 0x7E00);
	if (f & 0x04) memset(pram, 0, sizeof pram);
	if (f & 0x08) memset(vram, 0, sizeof vram);
	if (f & 0x10) memset(oam, 0, sizeof oam);
}

void bios_swi(u32 n)
{
	switch (n) {
	case 0x00:
		cpu_set_mode(MODE_SYS);
		cpu.r[15] = 0x08000000;
		cpu.cpsr &= ~FLAG_T;
		cpu_flush();
		break;

	case 0x01:
		swi_ram_reset();
		break;

	case 0x02:
	case 0x03:
	case 0x04:
	case 0x05:
		cpu.halt = true;
		break;

	case 0x06: {
		s32 a = (s32)cpu.r[0], b = (s32)cpu.r[1];
		if (!b) break;
		s32 q = a / b;
		cpu.r[0] = (u32)q;
		cpu.r[1] = (u32)(a % b);
		cpu.r[3] = (u32)(q < 0 ? -q : q);
		break;
	}

	case 0x07: {
		s32 b = (s32)cpu.r[0], a = (s32)cpu.r[1];
		if (!b) break;
		s32 q = a / b;
		cpu.r[0] = (u32)q;
		cpu.r[1] = (u32)(a % b);
		cpu.r[3] = (u32)(q < 0 ? -q : q);
		break;
	}

	case 0x08:
		cpu.r[0] = (u32)sqrt((double)cpu.r[0]);
		break;

	case 0x09: {
		double v = (s16)cpu.r[0] / 16384.0;
		cpu.r[0] = (u32)(s16)(atan(v) / (2.0 * M_PI) * 65536.0);
		break;
	}

	case 0x0A: {
		double x = (s16)cpu.r[0], y = (s16)cpu.r[1];
		double a = atan2(y, x);
		if (a < 0) a += 2.0 * M_PI;
		cpu.r[0] = (u32)(a / (2.0 * M_PI) * 65536.0) & 0xFFFF;
		break;
	}

	case 0x0B: swi_cpu_set(); break;
	case 0x0C: swi_cpu_fast_set(); break;
	case 0x0D: cpu.r[0] = 0xBAAE187F; break;
	case 0x0E: swi_bg_affine(); break;
	case 0x0F: swi_obj_affine(); break;
	case 0x10: swi_bit_unpack(); break;
	case 0x11: swi_lz77(false); break;
	case 0x12: swi_lz77(true); break;
	case 0x13: swi_huffman(); break;
	case 0x14: swi_rl(false); break;
	case 0x15: swi_rl(true); break;
	case 0x16: swi_diff_unfilter(false); break;
	case 0x17: swi_diff_unfilter(false); break;
	case 0x18: swi_diff_unfilter(true); break;

	case 0x1F: {
		double freq = cpu.r[0];
		double adj = (cpu.r[1] - cpu.r[2] / 256.0);
		cpu.r[0] = (u32)(freq / pow(2.0, adj / 12.0));
		break;
	}

	default:
		break;
	}
}
