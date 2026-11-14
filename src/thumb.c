#include "gba.h"
#include "alu.h"

static void t_shift(u16 op)
{
	u32 type = (op >> 11) & 3, amt = (op >> 6) & 0x1F;
	u32 rs = (op >> 3) & 7, rd = op & 7;
	bool c = (cpu.cpsr & FLAG_C) != 0;

	u32 r = do_shift(type, cpu.r[rs], amt, &c, true);
	cpu.r[rd] = r;
	set_nz(r);
	setf(FLAG_C, c);
}

static void t_addsub(u16 op)
{
	u32 rn = (op >> 6) & 7, rs = (op >> 3) & 7, rd = op & 7;
	u32 b = (op & (1 << 10)) ? rn : cpu.r[rn];

	cpu.r[rd] = (op & (1 << 9)) ? alu_sub(cpu.r[rs], b, 1, true)
	                            : alu_add(cpu.r[rs], b, 0, true);
}

static void t_imm(u16 op)
{
	u32 rd = (op >> 8) & 7, imm = op & 0xFF;

	switch ((op >> 11) & 3) {
	case 0: cpu.r[rd] = imm; set_nz(imm); break;
	case 1: alu_sub(cpu.r[rd], imm, 1, true); break;
	case 2: cpu.r[rd] = alu_add(cpu.r[rd], imm, 0, true); break;
	default: cpu.r[rd] = alu_sub(cpu.r[rd], imm, 1, true); break;
	}
}

static void t_alu(u16 op)
{
	u32 rs = (op >> 3) & 7, rd = op & 7;
	u32 a = cpu.r[rd], b = cpu.r[rs];
	bool c = (cpu.cpsr & FLAG_C) != 0;
	u32 r;

	switch ((op >> 6) & 0xF) {
	case 0x0: r = a & b; cpu.r[rd] = r; set_nz(r); break;
	case 0x1: r = a ^ b; cpu.r[rd] = r; set_nz(r); break;
	case 0x2: r = do_shift(0, a, b & 0xFF, &c, false); cpu.r[rd] = r; set_nz(r); setf(FLAG_C, c); break;
	case 0x3: r = do_shift(1, a, b & 0xFF, &c, false); cpu.r[rd] = r; set_nz(r); setf(FLAG_C, c); break;
	case 0x4: r = do_shift(2, a, b & 0xFF, &c, false); cpu.r[rd] = r; set_nz(r); setf(FLAG_C, c); break;
	case 0x5: cpu.r[rd] = alu_add(a, b, c ? 1 : 0, true); break;
	case 0x6: cpu.r[rd] = alu_sub(a, b, c ? 1 : 0, true); break;
	case 0x7: r = do_shift(3, a, b & 0xFF, &c, false); cpu.r[rd] = r; set_nz(r); setf(FLAG_C, c); break;
	case 0x8: set_nz(a & b); break;
	case 0x9: cpu.r[rd] = alu_sub(0, b, 1, true); break;
	case 0xA: alu_sub(a, b, 1, true); break;
	case 0xB: alu_add(a, b, 0, true); break;
	case 0xC: r = a | b; cpu.r[rd] = r; set_nz(r); break;
	case 0xD: r = a * b; cpu.r[rd] = r; set_nz(r); break;
	case 0xE: r = a & ~b; cpu.r[rd] = r; set_nz(r); break;
	default:  r = ~b; cpu.r[rd] = r; set_nz(r); break;
	}
}

static void t_hireg(u16 op)
{
	u32 rd = (op & 7) | ((op >> 4) & 8);
	u32 rs = (op >> 3) & 0xF;
	u32 v = cpu.r[rs];

	switch ((op >> 8) & 3) {
	case 0:
		cpu.r[rd] += v;
		if (rd == 15) cpu_flush();
		break;
	case 1:
		alu_sub(cpu.r[rd], v, 1, true);
		break;
	case 2:
		cpu.r[rd] = v;
		if (rd == 15) cpu_flush();
		break;
	default:
		setf(FLAG_T, v & 1);
		cpu.r[15] = v;
		cpu_flush();
		break;
	}
}

static void t_ldr_pc(u16 op)
{
	u32 rd = (op >> 8) & 7;
	u32 addr = (cpu.r[15] & ~2u) + (op & 0xFF) * 4;

	cpu.r[rd] = bus_read32(addr);
}

static void t_ldst_reg(u16 op)
{
	u32 ro = (op >> 6) & 7, rb = (op >> 3) & 7, rd = op & 7;
	u32 addr = cpu.r[rb] + cpu.r[ro];

	switch ((op >> 10) & 3) {
	case 0: bus_write32(addr, cpu.r[rd]); break;
	case 1: bus_write8(addr, (u8)cpu.r[rd]); break;
	case 2: cpu.r[rd] = ror32(bus_read32(addr), (addr & 3) * 8); break;
	default: cpu.r[rd] = bus_read8(addr); break;
	}
}

static void t_ldst_ext(u16 op)
{
	u32 ro = (op >> 6) & 7, rb = (op >> 3) & 7, rd = op & 7;
	u32 addr = cpu.r[rb] + cpu.r[ro];

	switch ((op >> 10) & 3) {
	case 0: bus_write16(addr, (u16)cpu.r[rd]); break;
	case 1: cpu.r[rd] = (u32)(s32)(s8)bus_read8(addr); break;
	case 2: cpu.r[rd] = ror32(bus_read16(addr), (addr & 1) * 8); break;
	default:
		if (addr & 1) cpu.r[rd] = (u32)(s32)(s8)bus_read8(addr);
		else cpu.r[rd] = (u32)(s32)(s16)bus_read16(addr);
		break;
	}
}

static void t_ldst_imm(u16 op)
{
	u32 off = (op >> 6) & 0x1F, rb = (op >> 3) & 7, rd = op & 7;
	bool byte = op & (1 << 12), load = op & (1 << 11);
	u32 addr = cpu.r[rb] + (byte ? off : off * 4);

	if (byte) {
		if (load) cpu.r[rd] = bus_read8(addr);
		else bus_write8(addr, (u8)cpu.r[rd]);
	} else {
		if (load) cpu.r[rd] = ror32(bus_read32(addr), (addr & 3) * 8);
		else bus_write32(addr, cpu.r[rd]);
	}
}

static void t_ldst_half(u16 op)
{
	u32 off = ((op >> 6) & 0x1F) * 2, rb = (op >> 3) & 7, rd = op & 7;
	u32 addr = cpu.r[rb] + off;

	if (op & (1 << 11)) cpu.r[rd] = ror32(bus_read16(addr), (addr & 1) * 8);
	else bus_write16(addr, (u16)cpu.r[rd]);
}

static void t_ldst_sp(u16 op)
{
	u32 rd = (op >> 8) & 7;
	u32 addr = cpu.r[13] + (op & 0xFF) * 4;

	if (op & (1 << 11)) cpu.r[rd] = ror32(bus_read32(addr), (addr & 3) * 8);
	else bus_write32(addr, cpu.r[rd]);
}

static void t_load_addr(u16 op)
{
	u32 rd = (op >> 8) & 7, imm = (op & 0xFF) * 4;

	cpu.r[rd] = (op & (1 << 11)) ? cpu.r[13] + imm : (cpu.r[15] & ~2u) + imm;
}

static void t_add_sp(u16 op)
{
	u32 off = (op & 0x7F) * 4;

	cpu.r[13] += (op & 0x80) ? -off : off;
}

static void t_push_pop(u16 op)
{
	u32 list = op & 0xFF;
	bool extra = op & (1 << 8);

	if (op & (1 << 11)) {
		u32 sp = cpu.r[13];
		for (int i = 0; i < 8; i++)
			if (list & (1 << i)) { cpu.r[i] = bus_read32(sp); sp += 4; }
		if (extra) {
			cpu.r[15] = bus_read32(sp) & ~1u;
			sp += 4;
			cpu.r[13] = sp;
			cpu_flush();
			return;
		}
		cpu.r[13] = sp;
	} else {
		u32 sp = cpu.r[13] - (__builtin_popcount(list) + (extra ? 1 : 0)) * 4;
		cpu.r[13] = sp;
		for (int i = 0; i < 8; i++)
			if (list & (1 << i)) { bus_write32(sp, cpu.r[i]); sp += 4; }
		if (extra) bus_write32(sp, cpu.r[14]);
	}
}

static void t_ldstm(u16 op)
{
	u32 rb = (op >> 8) & 7, list = op & 0xFF;
	u32 addr = cpu.r[rb];
	bool load = op & (1 << 11);

	if (list == 0) {
		if (load) { cpu.r[15] = bus_read32(addr); cpu_flush(); }
		else bus_write32(addr, cpu.r[15] + 2);
		cpu.r[rb] = addr + 0x40;
		return;
	}

	u32 wb = addr + __builtin_popcount(list) * 4;

	if (load) {
		for (int i = 0; i < 8; i++)
			if (list & (1 << i)) { cpu.r[i] = bus_read32(addr); addr += 4; }
		if (!(list & (1 << rb))) cpu.r[rb] = wb;
	} else {
		bool first = true;
		for (int i = 0; i < 8; i++) {
			if (!(list & (1 << i))) continue;
			bus_write32(addr, (i == (int)rb && !first) ? wb : cpu.r[i]);
			addr += 4;
			first = false;
		}
		cpu.r[rb] = wb;
	}
}

static void t_bcond(u16 op)
{
	if (!cond_pass((op >> 8) & 0xF)) return;

	cpu.r[15] += (s32)(s8)(op & 0xFF) * 2;
	cpu_flush();
}

static void t_branch(u16 op)
{
	cpu.r[15] += (s32)((u32)(op & 0x7FF) << 21) >> 20;
	cpu_flush();
}

static void t_bl(u16 op)
{
	if (!(op & (1 << 11))) {
		cpu.r[14] = cpu.r[15] + ((s32)((u32)(op & 0x7FF) << 21) >> 21 << 12);
		return;
	}

	u32 next = (cpu.r[15] - 2) | 1;
	cpu.r[15] = cpu.r[14] + ((op & 0x7FF) << 1);
	cpu.r[14] = next;
	cpu_flush();
}

void thumb_exec(u16 op)
{
	switch (op >> 12) {
	case 0x0: case 0x1:
		if ((op & 0x1800) == 0x1800) t_addsub(op); else t_shift(op);
		break;
	case 0x2: case 0x3: t_imm(op); break;
	case 0x4:
		if (op & 0x0800) t_ldr_pc(op);
		else if (op & 0x0400) t_hireg(op);
		else t_alu(op);
		break;
	case 0x5:
		if (op & 0x0200) t_ldst_ext(op); else t_ldst_reg(op);
		break;
	case 0x6: case 0x7: t_ldst_imm(op); break;
	case 0x8: t_ldst_half(op); break;
	case 0x9: t_ldst_sp(op); break;
	case 0xA: t_load_addr(op); break;
	case 0xB:
		if ((op & 0x0F00) == 0) t_add_sp(op); else t_push_pop(op);
		break;
	case 0xC: t_ldstm(op); break;
	case 0xD:
		if (((op >> 8) & 0xF) == 0xF) cpu_swi(op & 0xFF);
		else if (((op >> 8) & 0xF) == 0xE) cpu_undefined();
		else t_bcond(op);
		break;
	case 0xE: t_branch(op); break;
	default:  t_bl(op); break;
	}
}
