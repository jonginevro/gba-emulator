#include <stdio.h>
#include <string.h>
#include "gba.h"

bool use_hle_bios = true;
u16 keys_state = 0x03FF;

bool gba_load_bios(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	size_t n = fread(bios, 1, sizeof bios, f);
	fclose(f);

	if (n != sizeof bios) return false;

	use_hle_bios = false;
	return true;
}

void gba_reset(void)
{
	memset(ewram, 0, sizeof ewram);
	memset(iwram, 0, sizeof iwram);
	memset(io, 0, sizeof io);
	memset(pram, 0, sizeof pram);
	memset(vram, 0, sizeof vram);
	memset(oam, 0, sizeof oam);

	if (use_hle_bios) bios_init_stub();

	io_write16(REG_DISPCNT, 0x0080);
	io_write16(REG_BG2PA, 0x0100);
	io_write16(REG_BG2PA + 6, 0x0100);
	io_write16(REG_BG3PA, 0x0100);
	io_write16(REG_BG3PA + 6, 0x0100);
	io_write16(REG_SOUNDBIAS, 0x0200);
	keys_state = 0x03FF;

	ppu_reset();
	dma_reset();
	timer_reset();
	apu_reset();
	cpu_reset();
}

void gba_run_frame(void)
{
	ppu_frame_done = false;

	while (!ppu_frame_done) {
		cpu_check_irq();

		int c = cpu.halt ? 8 : cpu_step();

		timer_run(c);
		ppu_run(c);
		apu_run(c);
	}
}
