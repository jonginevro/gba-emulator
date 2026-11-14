#include "gba.h"
#include "alu.h"

static u32 user_reg_get(int n)
{
	if (n < 8 || n == 15 || cpu.bank == BANK_USR) return cpu.r[n];
	if (n < 13) return cpu.bank == BANK_FIQ ? cpu.bank_usr[n - 8] : cpu.r[n];
	return n == 13 ? cpu.bank_r13[BANK_USR] : cpu.bank_r14[BANK_USR];
}

static void user_reg_set(int n, u32 v)
{
	if (n < 8 || n == 15 || cpu.bank == BANK_USR) { cpu.r[n] = v; return; }
	if (n < 13) {
		if (cpu.bank == BANK_FIQ) cpu.bank_usr[n - 8] = v; else cpu.r[n] = v;
		return;
	}
	if (n == 13) cpu.bank_r13[BANK_USR] = v; else cpu.bank_r14[BANK_USR] = v;
}

/* ---- instruction groups ---- */

static void arm_psr(u32 op)
{
	bool spsr = op & (1 << 22);

	if (!(op & (1 << 21))) {
		cpu.r[(op >> 12) & 0xF] = spsr ? cpu.spsr : cpu.cpsr;
		return;
	}

	u32 v;
	if (op & (1 << 25)) {
		u32 imm = op & 0xFF, rot = ((op >> 8) & 0xF) * 2;
		v = ror32(imm, rot);
	} else {
		v = cpu.r[op & 0xF];
	}

	u32 mask = 0;
	if (op & (1 << 19)) mask |= 0xFF000000;
	if (op & (1 << 18)) mask |= 0x00FF0000;
	if (op & (1 << 17)) mask |= 0x0000FF00;
	if (op & (1 << 16)) mask |= 0x000000FF;

	if (spsr) {
		if (cpu.bank != BANK_USR) cpu.spsr = (cpu.spsr & ~mask) | (v & mask);
		return;
	}

	if ((cpu.cpsr & 0x1F) == MODE_USR) mask &= 0xFF000000;
	write_cpsr((cpu.cpsr & ~mask) | (v & mask));
}

static void arm_data(u32 op)
{
	u32 opcode = (op >> 21) & 0xF;
	bool s = op & (1 << 20);

	if (!s && opcode >= 0x8 && opcode <= 0xB) {
		arm_psr(op);
		return;
	}

	u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
	bool carry = (cpu.cpsr & FLAG_C) != 0;
	bool by_reg = false;
	u32 op2;

	if (op & (1 << 25)) {
		u32 imm = op & 0xFF, rot = ((op >> 8) & 0xF) * 2;
		if (rot) { op2 = ror32(imm, rot); carry = op2 >> 31; }
		else op2 = imm;
	} else {
		u32 rm = op & 0xF, type = (op >> 5) & 3, amt;
		if (op & (1 << 4)) {
			by_reg = true;
			amt = cpu.r[(op >> 8) & 0xF] & 0xFF;
		} else {
			amt = (op >> 7) & 0x1F;
		}
		u32 v = cpu.r[rm];
		if (by_reg && rm == 15) v += 4;
		op2 = do_shift(type, v, amt, &carry, !by_reg);
	}

	u32 a = cpu.r[rn];
	if (by_reg && rn == 15) a += 4;

	u32 res;
	bool logical = false, store = true;

	switch (opcode) {
	case 0x0: res = a & op2; logical = true; break;
	case 0x1: res = a ^ op2; logical = true; break;
	case 0x2: res = alu_sub(a, op2, 1, s); break;
	case 0x3: res = alu_sub(op2, a, 1, s); break;
	case 0x4: res = alu_add(a, op2, 0, s); break;
	case 0x5: res = alu_add(a, op2, (cpu.cpsr & FLAG_C) ? 1 : 0, s); break;
	case 0x6: res = alu_sub(a, op2, (cpu.cpsr & FLAG_C) ? 1 : 0, s); break;
	case 0x7: res = alu_sub(op2, a, (cpu.cpsr & FLAG_C) ? 1 : 0, s); break;
	case 0x8: res = a & op2; logical = true; store = false; break;
	case 0x9: res = a ^ op2; logical = true; store = false; break;
	case 0xA: res = alu_sub(a, op2, 1, s); store = false; break;
	case 0xB: res = alu_add(a, op2, 0, s); store = false; break;
	case 0xC: res = a | op2; logical = true; break;
	case 0xD: res = op2; logical = true; break;
	case 0xE: res = a & ~op2; logical = true; break;
	default:  res = ~op2; logical = true; break;
	}

	if (s && logical) {
		set_nz(res);
		setf(FLAG_C, carry);
	}

	if (!store) {
		/* a comparison with S set and Rd=15 restores CPSR from SPSR instead */
		if (s && rd == 15 && cpu.bank != BANK_USR) write_cpsr(cpu.spsr);
		return;
	}

	cpu.r[rd] = res;

	if (rd == 15) {
		if (s) write_cpsr(cpu.spsr);
		cpu_flush();
	}
}

static void arm_branch(u32 op)
{
	s32 off = ((s32)(op << 8)) >> 6;

	if (op & (1 << 24)) cpu.r[14] = cpu.r[15] - 4;
	cpu.r[15] += off;
	cpu_flush();
}

static void arm_bx(u32 op)
{
	u32 v = cpu.r[op & 0xF];

	setf(FLAG_T, v & 1);
	cpu.r[15] = v;
	cpu_flush();
}

static void arm_mul(u32 op)
{
	u32 rd = (op >> 16) & 0xF, rn = (op >> 12) & 0xF;
	u32 rs = (op >> 8) & 0xF, rm = op & 0xF;

	u32 res = cpu.r[rm] * cpu.r[rs];
	if (op & (1 << 21)) res += cpu.r[rn];

	cpu.r[rd] = res;
	if (op & (1 << 20)) set_nz(res);
}

static void arm_mull(u32 op)
{
	u32 rdhi = (op >> 16) & 0xF, rdlo = (op >> 12) & 0xF;
	u32 rs = (op >> 8) & 0xF, rm = op & 0xF;

	u64 res;
	if (op & (1 << 22)) res = (u64)((s64)(s32)cpu.r[rm] * (s32)cpu.r[rs]);
	else res = (u64)cpu.r[rm] * cpu.r[rs];

	if (op & (1 << 21)) res += ((u64)cpu.r[rdhi] << 32) | cpu.r[rdlo];

	cpu.r[rdlo] = (u32)res;
	cpu.r[rdhi] = (u32)(res >> 32);

	if (op & (1 << 20)) {
		setf(FLAG_N, res >> 63);
		setf(FLAG_Z, res == 0);
	}
}

static void arm_swp(u32 op)
{
	u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF, rm = op & 0xF;
	u32 addr = cpu.r[rn];

	if (op & (1 << 22)) {
		u8 t = bus_read8(addr);
		bus_write8(addr, (u8)cpu.r[rm]);
		cpu.r[rd] = t;
	} else {
		u32 t = ror32(bus_read32(addr), (addr & 3) * 8);
		bus_write32(addr, cpu.r[rm]);
		cpu.r[rd] = t;
	}
}

static void arm_sdt(u32 op)
{
	bool p = op & (1 << 24), u = op & (1 << 23);
	bool b = op & (1 << 22), w = op & (1 << 21), l = op & (1 << 20);
	u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;

	u32 off;
	if (op & (1 << 25)) {
		bool c = (cpu.cpsr & FLAG_C) != 0;
		off = do_shift((op >> 5) & 3, cpu.r[op & 0xF], (op >> 7) & 0x1F, &c, true);
	} else {
		off = op & 0xFFF;
	}

	u32 addr = cpu.r[rn];
	if (p) addr += u ? off : -off;

	if (l) {
		u32 v = b ? bus_read8(addr) : ror32(bus_read32(addr), (addr & 3) * 8);
		if (!p) addr += u ? off : -off;
		if (!p || w) cpu.r[rn] = addr;
		cpu.r[rd] = v;
		if (rd == 15) cpu_flush();
	} else {
		u32 v = cpu.r[rd];
		if (rd == 15) v += 4;
		if (b) bus_write8(addr, (u8)v); else bus_write32(addr, v);
		if (!p) addr += u ? off : -off;
		if (!p || w) cpu.r[rn] = addr;
	}
}

static void arm_hdt(u32 op)
{
	bool p = op & (1 << 24), u = op & (1 << 23);
	bool w = op & (1 << 21), l = op & (1 << 20);
	u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
	u32 sh = (op >> 5) & 3;

	u32 off = (op & (1 << 22)) ? (((op >> 4) & 0xF0) | (op & 0xF)) : cpu.r[op & 0xF];

	u32 addr = cpu.r[rn];
	if (p) addr += u ? off : -off;

	if (l) {
		u32 v;
		switch (sh) {
		case 1: v = ror32(bus_read16(addr), (addr & 1) * 8); break;
		case 2: v = (u32)(s32)(s8)bus_read8(addr); break;
		default:
			/* LDRSH on an odd address degrades to a sign-extended byte load */
			if (addr & 1) v = (u32)(s32)(s8)bus_read8(addr);
			else v = (u32)(s32)(s16)bus_read16(addr);
			break;
		}
		if (!p) addr += u ? off : -off;
		if (!p || w) cpu.r[rn] = addr;
		cpu.r[rd] = v;
		if (rd == 15) cpu_flush();
	} else {
		u32 v = cpu.r[rd];
		if (rd == 15) v += 4;
		bus_write16(addr, (u16)v);
		if (!p) addr += u ? off : -off;
		if (!p || w) cpu.r[rn] = addr;
	}
}

static void arm_block(u32 op)
{
	bool p = op & (1 << 24), u = op & (1 << 23);
	bool s = op & (1 << 22), w = op & (1 << 21), l = op & (1 << 20);
	u32 rn = (op >> 16) & 0xF;
	u32 list = op & 0xFFFF;

	int count = __builtin_popcount(list);
	if (list == 0) {           /* empty list acts on r15 and moves base by 0x40 */
		list = 0x8000;
		count = 16;
	}

	u32 base = cpu.r[rn];
	u32 addr, wb;

	if (u) { addr = base + (p ? 4 : 0); wb = base + count * 4; }
	else   { addr = base - count * 4 + (p ? 0 : 4); wb = base - count * 4; }

	bool user_bank = s && !(l && (list & 0x8000));
	bool first = true;

	for (int i = 0; i < 16; i++) {
		if (!(list & (1 << i))) continue;

		if (l) {
			u32 v = bus_read32(addr);
			if (user_bank) user_reg_set(i, v); else cpu.r[i] = v;
		} else {
			u32 v = user_bank ? user_reg_get(i) : cpu.r[i];
			if (i == 15) v += 4;
			if (i == (int)rn && w && !first) v = wb;
			bus_write32(addr, v);
		}

		addr += 4;
		first = false;
	}

	if (w && !(l && (list & (1 << rn)))) cpu.r[rn] = wb;

	if (l && (list & 0x8000)) {
		if (s) write_cpsr(cpu.spsr);
		cpu_flush();
	}
}

void arm_exec(u32 op)
{
	u32 cond = op >> 28;
	if (cond != 0xE && !cond_pass(cond)) return;

	if ((op & 0x0FFFFFF0) == 0x012FFF10) { arm_bx(op); return; }
	if ((op & 0x0E000000) == 0x0A000000) { arm_branch(op); return; }
	if ((op & 0x0F000000) == 0x0F000000) { cpu_swi(op & 0xFFFFFF); return; }
	if ((op & 0x0C000000) == 0x04000000) { arm_sdt(op); return; }
	if ((op & 0x0E000000) == 0x08000000) { arm_block(op); return; }
	if ((op & 0x0FB00FF0) == 0x01000090) { arm_swp(op); return; }
	if ((op & 0x0FC000F0) == 0x00000090) { arm_mul(op); return; }
	if ((op & 0x0F8000F0) == 0x00800090) { arm_mull(op); return; }
	if ((op & 0x0E000090) == 0x00000090) { arm_hdt(op); return; }
	if ((op & 0x0C000000) == 0x00000000) { arm_data(op); return; }

	cpu_undefined();
}
