#include <string.h>
#include "gba.h"

u8 bios[0x4000];
u8 ewram[0x40000];
u8 iwram[0x8000];
u8 io[0x400];
u8 pram[0x400];
u8 vram[0x18000];
u8 oam[0x400];
u8 *rom;
size_t rom_size;

int bus_cycles;

static u32 open_bus;

static const u8 ws_n[4] = { 4, 3, 2, 8 };
static const u8 ws_s[3][2] = { { 2, 1 }, { 4, 1 }, { 8, 1 } };

static int rom_wait(u32 addr, bool seq, bool word)
{
	u16 wc = io_read16(REG_WAITCNT);
	int region = (addr >> 25) & 3;
	if (region > 2) region = 2;

	int n, s;
	switch (region) {
	case 0: n = ws_n[(wc >> 2) & 3]; s = ws_s[0][(wc >> 4) & 1]; break;
	case 1: n = ws_n[(wc >> 5) & 3]; s = ws_s[1][(wc >> 7) & 1]; break;
	default: n = ws_n[(wc >> 8) & 3]; s = ws_s[2][(wc >> 10) & 1]; break;
	}
	int c = (seq ? s : n) + 1;
	if (word) c += s + 1;
	return c;
}

static int access_cycles(u32 addr, bool word)
{
	switch (addr >> 24) {
	case 0x02: return word ? 6 : 3;
	case 0x05: case 0x06: return word ? 2 : 1;
	case 0x08: case 0x09: case 0x0A:
	case 0x0B: case 0x0C: case 0x0D: return rom_wait(addr, false, word);
	case 0x0E: case 0x0F: return (io_read16(REG_WAITCNT) & 3) == 3 ? 9 : ws_n[io_read16(REG_WAITCNT) & 3] + 1;
	default: return 1;
	}
}

static inline u32 vram_offset(u32 addr)
{
	addr &= 0x1FFFF;
	if (addr >= 0x18000) addr -= 0x8000;
	return addr;
}

/* ---- I/O ---- */

static u16 io_read_reg(u32 addr)
{
	addr &= 0x3FF;
	switch (addr) {
	case REG_VCOUNT: return ppu_vcount();
	case REG_KEYINPUT: return keys_state;
	case 0x100: case 0x104: case 0x108: case 0x10C:
		return timer_read_counter((addr - 0x100) >> 2);
	}
	/* DMA word counts always read back 0 */
	if (addr >= 0x0B0 && addr < 0x0E0) {
		int off = (addr - 0x0B0) % 12;
		if (off < 8) return 0;
	}
	return io_read16(addr);
}

static void io_write_reg(u32 addr, u16 v)
{
	addr &= 0x3FF;

	if (addr >= 0x060 && addr < 0x0A0) {
		apu_write8(addr, (u8)v);
		apu_write8(addr + 1, (u8)(v >> 8));
		return;
	}

	switch (addr) {
	case REG_DISPSTAT:
		ppu_write_dispstat(v);
		return;
	case REG_VCOUNT:
		return;
	case REG_IF:
		io_write16(REG_IF, io_read16(REG_IF) & ~v);
		return;
	case REG_KEYINPUT:
		return;
	case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
		io_write16(addr, v);
		dma_write_control((addr - 0x0BA) / 12, v);
		return;
	case 0x102: case 0x106: case 0x10A: case 0x10E:
		io_write16(addr, v);
		timer_write_control((addr - 0x102) >> 2, v);
		return;
	case 0x100: case 0x104: case 0x108: case 0x10C:
		timer_write_reload((addr - 0x100) >> 2, v);
		return;
	}

	io_write16(addr, v);

	if (addr >= REG_BG2X && addr <= REG_BG3Y + 2)
		ppu_write_affine_ref(addr);
}

/* ---- reads ---- */

u8 bus_read8(u32 addr)
{
	bus_cycles += access_cycles(addr, false);

	switch (addr >> 24) {
	case 0x00: return addr < 0x4000 ? bios[addr] : 0;
	case 0x02: return ewram[addr & 0x3FFFF];
	case 0x03: return iwram[addr & 0x7FFF];
	case 0x04: {
		u16 v = io_read_reg(addr & ~1);
		return (addr & 1) ? (u8)(v >> 8) : (u8)v;
	}
	case 0x05: return pram[addr & 0x3FF];
	case 0x06: return vram[vram_offset(addr)];
	case 0x07: return oam[addr & 0x3FF];
	case 0x08: case 0x09: case 0x0A:
	case 0x0B: case 0x0C: case 0x0D: {
		u32 off = addr & 0x01FFFFFF;
		return off < rom_size ? rom[off] : (u8)(off >> 1);
	}
	case 0x0E: case 0x0F:
		return cart_read_sram(addr);
	}
	return (u8)open_bus;
}

u16 bus_read16(u32 addr)
{
	addr &= ~1u;
	bus_cycles += access_cycles(addr, false);

	switch (addr >> 24) {
	case 0x00: return addr < 0x4000 ? *(u16 *)&bios[addr] : 0;
	case 0x02: return *(u16 *)&ewram[addr & 0x3FFFE];
	case 0x03: return *(u16 *)&iwram[addr & 0x7FFE];
	case 0x04: return io_read_reg(addr);
	case 0x05: return *(u16 *)&pram[addr & 0x3FE];
	case 0x06: return *(u16 *)&vram[vram_offset(addr)];
	case 0x07: return *(u16 *)&oam[addr & 0x3FE];
	case 0x08: case 0x09: case 0x0A:
	case 0x0B: case 0x0C:
	case 0x0D: {
		if (cart_is_eeprom_addr(addr)) return cart_read_eeprom();
		u32 off = addr & 0x01FFFFFF;
		return off + 1 < rom_size ? *(u16 *)&rom[off] : (u16)(off >> 1);
	}
	case 0x0E: case 0x0F: {
		u8 b = cart_read_sram(addr);
		return (u16)(b | (b << 8));
	}
	}
	return (u16)open_bus;
}

u32 bus_read32(u32 addr)
{
	addr &= ~3u;
	bus_cycles += access_cycles(addr, true);

	switch (addr >> 24) {
	case 0x00: return addr < 0x4000 ? *(u32 *)&bios[addr] : 0;
	case 0x02: return *(u32 *)&ewram[addr & 0x3FFFC];
	case 0x03: return *(u32 *)&iwram[addr & 0x7FFC];
	case 0x04: return io_read_reg(addr) | ((u32)io_read_reg(addr + 2) << 16);
	case 0x05: return *(u32 *)&pram[addr & 0x3FC];
	case 0x06: return *(u32 *)&vram[vram_offset(addr)];
	case 0x07: return *(u32 *)&oam[addr & 0x3FC];
	case 0x08: case 0x09: case 0x0A:
	case 0x0B: case 0x0C: case 0x0D: {
		u32 off = addr & 0x01FFFFFF;
		if (off + 3 < rom_size) return *(u32 *)&rom[off];
		return ((off >> 1) & 0xFFFF) | ((((off + 2) >> 1) & 0xFFFF) << 16);
	}
	case 0x0E: case 0x0F: {
		u8 b = cart_read_sram(addr);
		u32 w = b | (b << 8);
		return w | (w << 16);
	}
	}
	return open_bus;
}

u32 bus_fetch32(u32 addr)
{
	u32 v = bus_read32(addr);
	open_bus = v;
	return v;
}

u16 bus_fetch16(u32 addr)
{
	u16 v = bus_read16(addr);
	open_bus = v | ((u32)v << 16);
	return v;
}

/* ---- writes ---- */

void bus_write8(u32 addr, u8 v)
{
	bus_cycles += access_cycles(addr, false);

	switch (addr >> 24) {
	case 0x02: ewram[addr & 0x3FFFF] = v; return;
	case 0x03: iwram[addr & 0x7FFF] = v; return;
	case 0x04: {
		u32 a = addr & ~1u;
		if (addr == 0x04000301) { cpu.halt = true; return; }
		if (addr >= 0x040000A0 && addr < 0x040000A8) { apu_write8(addr & 0x3FF, v); return; }
		u16 cur = io_read16(a & 0x3FF);
		u16 nv = (addr & 1) ? (u16)((cur & 0x00FF) | (v << 8)) : (u16)((cur & 0xFF00) | v);
		io_write_reg(a, nv);
		return;
	}
	/* byte writes to palette and BG vram mirror into both halves; OAM ignores them */
	case 0x05: *(u16 *)&pram[addr & 0x3FE] = (u16)(v | (v << 8)); return;
	case 0x06: {
		u32 off = vram_offset(addr);
		u32 limit = (io_read16(REG_DISPCNT) & 7) >= 3 ? 0x14000 : 0x10000;
		if (off < limit) *(u16 *)&vram[off & ~1u] = (u16)(v | (v << 8));
		return;
	}
	case 0x07: return;
	case 0x0E: case 0x0F: cart_write_sram(addr, v); return;
	}
}

void bus_write16(u32 addr, u16 v)
{
	addr &= ~1u;
	bus_cycles += access_cycles(addr, false);

	switch (addr >> 24) {
	case 0x02: *(u16 *)&ewram[addr & 0x3FFFE] = v; return;
	case 0x03: *(u16 *)&iwram[addr & 0x7FFE] = v; return;
	case 0x04: io_write_reg(addr, v); return;
	case 0x05: *(u16 *)&pram[addr & 0x3FE] = v; return;
	case 0x06: *(u16 *)&vram[vram_offset(addr)] = v; return;
	case 0x07: *(u16 *)&oam[addr & 0x3FE] = v; return;
	case 0x0D:
		if (cart_is_eeprom_addr(addr)) { cart_write_eeprom(v, 0); return; }
		return;
	case 0x0E: case 0x0F: cart_write_sram(addr, (u8)(v >> ((addr & 1) * 8))); return;
	}
}

void bus_write32(u32 addr, u32 v)
{
	addr &= ~3u;
	bus_cycles += access_cycles(addr, true);

	switch (addr >> 24) {
	case 0x02: *(u32 *)&ewram[addr & 0x3FFFC] = v; return;
	case 0x03: *(u32 *)&iwram[addr & 0x7FFC] = v; return;
	case 0x04:
		if (addr == 0x040000A0) { apu_fifo_write(0, v, 32); return; }
		if (addr == 0x040000A4) { apu_fifo_write(1, v, 32); return; }
		io_write_reg(addr, (u16)v);
		io_write_reg(addr + 2, (u16)(v >> 16));
		return;
	case 0x05: *(u32 *)&pram[addr & 0x3FC] = v; return;
	case 0x06: *(u32 *)&vram[vram_offset(addr)] = v; return;
	case 0x07: *(u32 *)&oam[addr & 0x3FC] = v; return;
	case 0x0E: case 0x0F: cart_write_sram(addr, (u8)v); return;
	}
}

void irq_raise(u16 flag)
{
	io_write16(REG_IF, io_read16(REG_IF) | flag);
	if (io_read16(REG_IE) & flag) cpu.halt = false;
}
