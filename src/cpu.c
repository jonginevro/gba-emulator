#include <string.h>
#include "gba.h"

CPU cpu;

static int bank_of(u32 mode)
{
	switch (mode) {
	case MODE_FIQ: return BANK_FIQ;
	case MODE_IRQ: return BANK_IRQ;
	case MODE_SVC: return BANK_SVC;
	case MODE_ABT: return BANK_ABT;
	case MODE_UND: return BANK_UND;
	default: return BANK_USR;
	}
}

void cpu_set_mode(u32 mode)
{
	int nb = bank_of(mode);

	if (nb != cpu.bank) {
		cpu.bank_r13[cpu.bank] = cpu.r[13];
		cpu.bank_r14[cpu.bank] = cpu.r[14];
		cpu.bank_spsr[cpu.bank] = cpu.spsr;
		memcpy(cpu.bank == BANK_FIQ ? cpu.bank_fiq : cpu.bank_usr, &cpu.r[8], 5 * 4);

		cpu.r[13] = cpu.bank_r13[nb];
		cpu.r[14] = cpu.bank_r14[nb];
		cpu.spsr = cpu.bank_spsr[nb];
		memcpy(&cpu.r[8], nb == BANK_FIQ ? cpu.bank_fiq : cpu.bank_usr, 5 * 4);

		cpu.bank = nb;
	}

	cpu.cpsr = (cpu.cpsr & ~0x1Fu) | mode;
}

void cpu_flush(void)
{
	cpu.flush = true;

	if (cpu.cpsr & FLAG_T) {
		cpu.r[15] &= ~1u;
		cpu.pipe[0] = bus_fetch16(cpu.r[15]);
		cpu.r[15] += 2;
		cpu.pipe[1] = bus_fetch16(cpu.r[15]);
		cpu.r[15] += 2;
	} else {
		cpu.r[15] &= ~3u;
		cpu.pipe[0] = bus_fetch32(cpu.r[15]);
		cpu.r[15] += 4;
		cpu.pipe[1] = bus_fetch32(cpu.r[15]);
		cpu.r[15] += 4;
	}
}

static void enter_exception(u32 mode, u32 vector, u32 lr)
{
	u32 saved = cpu.cpsr;

	cpu_set_mode(mode);
	cpu.spsr = saved;
	cpu.r[14] = lr;
	cpu.cpsr &= ~FLAG_T;
	cpu.cpsr |= FLAG_I;
	cpu.r[15] = vector;
	cpu_flush();
}

void cpu_swi(u32 comment)
{
	if (use_hle_bios) {
		bios_swi(comment);
		return;
	}
	enter_exception(MODE_SVC, 0x08, cpu.r[15] - (cpu_thumb() ? 2 : 4));
}

void cpu_undefined(void)
{
	enter_exception(MODE_UND, 0x04, cpu.r[15] - (cpu_thumb() ? 2 : 4));
}

void cpu_check_irq(void)
{
	if (cpu.cpsr & FLAG_I) return;
	if (!(io_read16(REG_IME) & 1)) return;
	if (!(io_read16(REG_IE) & io_read16(REG_IF) & 0x3FFF)) return;

	int width = cpu_thumb() ? 2 : 4;
	enter_exception(MODE_IRQ, 0x18, cpu.r[15] - 2 * width + 4);
	cpu.halt = false;
}

int cpu_step(void)
{
	bus_cycles = 0;
	cpu.flush = false;

	if (cpu.cpsr & FLAG_T) {
		u16 op = (u16)cpu.pipe[0];
		cpu.pipe[0] = cpu.pipe[1];
		cpu.pipe[1] = bus_fetch16(cpu.r[15]);
		thumb_exec(op);
		if (!cpu.flush) cpu.r[15] += 2;
	} else {
		u32 op = cpu.pipe[0];
		cpu.pipe[0] = cpu.pipe[1];
		cpu.pipe[1] = bus_fetch32(cpu.r[15]);
		arm_exec(op);
		if (!cpu.flush) cpu.r[15] += 4;
	}

	return bus_cycles > 0 ? bus_cycles : 1;
}

void cpu_reset(void)
{
	memset(&cpu, 0, sizeof cpu);

	cpu.bank = BANK_USR;
	cpu.cpsr = MODE_SYS;

	if (use_hle_bios) {
		cpu.r[15] = 0x08000000;
		cpu.bank_r13[BANK_IRQ] = 0x03007FA0;
		cpu.bank_r13[BANK_SVC] = 0x03007FE0;
		cpu.r[13] = 0x03007F00;
	} else {
		cpu.r[15] = 0x00000000;
		cpu.cpsr = MODE_SVC | FLAG_I | FLAG_F;
		cpu.bank = BANK_SVC;
	}

	cpu_flush();
	cpu.flush = false;
}
