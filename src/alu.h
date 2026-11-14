#ifndef ALU_H
#define ALU_H

#include "gba.h"

static inline void setf(u32 flag, bool on)
{
	if (on) cpu.cpsr |= flag; else cpu.cpsr &= ~flag;
}

static inline void set_nz(u32 r)
{
	cpu.cpsr = (cpu.cpsr & ~(FLAG_N | FLAG_Z)) | (r & FLAG_N) | (r ? 0 : FLAG_Z);
}

static inline u32 ror32(u32 v, u32 n)
{
	n &= 31;
	return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline bool cond_pass(u32 cond)
{
	u32 c = cpu.cpsr;
	bool n = c & FLAG_N, z = c & FLAG_Z, cy = c & FLAG_C, v = c & FLAG_V;

	switch (cond) {
	case 0x0: return z;
	case 0x1: return !z;
	case 0x2: return cy;
	case 0x3: return !cy;
	case 0x4: return n;
	case 0x5: return !n;
	case 0x6: return v;
	case 0x7: return !v;
	case 0x8: return cy && !z;
	case 0x9: return !cy || z;
	case 0xA: return n == v;
	case 0xB: return n != v;
	case 0xC: return !z && n == v;
	case 0xD: return z || n != v;
	case 0xE: return true;
	default:  return false;
	}
}

/* by_imm distinguishes the shift-amount-is-zero special cases from "no shift" */
static inline u32 do_shift(u32 type, u32 v, u32 amt, bool *c, bool by_imm)
{
	if (!by_imm) {
		if (amt == 0) return v;
		switch (type) {
		case 0:
			if (amt < 32) { *c = (v >> (32 - amt)) & 1; return v << amt; }
			if (amt == 32) { *c = v & 1; return 0; }
			*c = false; return 0;
		case 1:
			if (amt < 32) { *c = (v >> (amt - 1)) & 1; return v >> amt; }
			if (amt == 32) { *c = v >> 31; return 0; }
			*c = false; return 0;
		case 2:
			if (amt < 32) { *c = (v >> (amt - 1)) & 1; return (u32)((s32)v >> amt); }
			*c = v >> 31; return (u32)((s32)v >> 31);
		default:
			amt &= 31;
			if (amt == 0) { *c = v >> 31; return v; }
			*c = (v >> (amt - 1)) & 1;
			return ror32(v, amt);
		}
	}

	switch (type) {
	case 0:
		if (amt == 0) return v;
		*c = (v >> (32 - amt)) & 1;
		return v << amt;
	case 1:
		if (amt == 0) { *c = v >> 31; return 0; }
		*c = (v >> (amt - 1)) & 1;
		return v >> amt;
	case 2:
		if (amt == 0) { *c = v >> 31; return (u32)((s32)v >> 31); }
		*c = (v >> (amt - 1)) & 1;
		return (u32)((s32)v >> amt);
	default:
		if (amt == 0) {           /* ROR #0 means RRX */
			u32 r = (v >> 1) | ((cpu.cpsr & FLAG_C) ? 0x80000000u : 0);
			*c = v & 1;
			return r;
		}
		*c = (v >> (amt - 1)) & 1;
		return ror32(v, amt);
	}
}

static inline u32 alu_add(u32 a, u32 b, u32 cin, bool s)
{
	u64 t = (u64)a + b + cin;
	u32 r = (u32)t;
	if (s) {
		set_nz(r);
		setf(FLAG_C, t >> 32);
		setf(FLAG_V, (~(a ^ b) & (a ^ r)) >> 31);
	}
	return r;
}

static inline u32 alu_sub(u32 a, u32 b, u32 cin, bool s)
{
	u64 t = (u64)a + (u32)~b + cin;
	u32 r = (u32)t;
	if (s) {
		set_nz(r);
		setf(FLAG_C, t >> 32);
		setf(FLAG_V, ((a ^ b) & (a ^ r)) >> 31);
	}
	return r;
}

static inline void write_cpsr(u32 v)
{
	cpu_set_mode(v & 0x1F);
	cpu.cpsr = v;
}

#endif
