#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba.h"

int save_type = SAVE_NONE;

static u8 save_data[0x20000];
static size_t save_size;
static char save_path[1024];
static bool save_dirty;
static char title[13];

/* ---- flash ---- */

static int flash_phase;
static bool flash_id_mode, flash_erase_armed;
static int flash_bank;

static const u8 flash_id[2][2] = { { 0x32, 0x1B }, { 0x62, 0x13 } };

/* ---- eeprom ---- */

enum { EE_IDLE, EE_CMD, EE_ADDR, EE_DATA, EE_END_R, EE_END_W, EE_READ };

static int ee_phase, ee_cmd, ee_count, ee_addr, ee_width = 6, ee_read_pos;
static u64 ee_data;

static void detect_save(void)
{
	static const struct { const char *tag; int type; size_t size; } tags[] = {
		{ "EEPROM_V",   SAVE_EEPROM8K,  0x2000  },
		{ "SRAM_V",     SAVE_SRAM,      0x8000  },
		{ "SRAM_F_V",   SAVE_SRAM,      0x8000  },
		{ "FLASH_V",    SAVE_FLASH64,   0x10000 },
		{ "FLASH512_V", SAVE_FLASH64,   0x10000 },
		{ "FLASH1M_V",  SAVE_FLASH128,  0x20000 },
	};

	save_type = SAVE_NONE;
	save_size = 0;

	for (size_t i = 0; i + 16 < rom_size; i += 4) {
		for (size_t t = 0; t < sizeof tags / sizeof *tags; t++) {
			size_t n = strlen(tags[t].tag);
			if (memcmp(&rom[i], tags[t].tag, n) != 0) continue;
			/* FLASH1M_V also matches the shorter FLASH_V prefix, so keep the longest */
			if (tags[t].size > save_size) {
				save_type = tags[t].type;
				save_size = tags[t].size;
			}
		}
	}

	if (save_type == SAVE_NONE) {
		save_type = SAVE_SRAM;
		save_size = 0x8000;
	}
}

const char *cart_title(void)
{
	return title;
}

bool cart_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;

	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (n <= 0 || n > 0x2000000) {
		fclose(f);
		return false;
	}

	free(rom);
	rom = malloc((size_t)n);
	if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return false;
	}
	fclose(f);

	rom_size = (size_t)n;

	memcpy(title, &rom[0xA0], 12);
	title[12] = 0;

	detect_save();

	snprintf(save_path, sizeof save_path, "%s", path);
	char *dot = strrchr(save_path, '.');
	if (dot && dot > strrchr(save_path, '/')) *dot = 0;
	strncat(save_path, ".sav", sizeof save_path - strlen(save_path) - 1);

	cart_load_battery();
	return true;
}

void cart_unload(void)
{
	free(rom);
	rom = NULL;
	rom_size = 0;
}

void cart_load_battery(void)
{
	memset(save_data, 0xFF, sizeof save_data);

	FILE *f = fopen(save_path, "rb");
	if (!f) return;

	size_t want = save_size ? save_size : sizeof save_data;
	size_t got = fread(save_data, 1, want, f);
	fclose(f);

	/* a short file is fine, the remainder stays erased */
	(void)got;
	save_dirty = false;
}

void cart_save_battery(void)
{
	if (!save_dirty || !save_size || !save_path[0]) return;

	FILE *f = fopen(save_path, "wb");
	if (!f) return;

	bool ok = fwrite(save_data, 1, save_size, f) == save_size;
	if (fclose(f) != 0) ok = false;

	if (ok) save_dirty = false;
	else fprintf(stderr, "warning: could not write %s\n", save_path);
}

/* ---- sram / flash access ---- */

u8 cart_read_sram(u32 addr)
{
	if (save_type == SAVE_SRAM) return save_data[addr & 0x7FFF];

	if (save_type == SAVE_FLASH64 || save_type == SAVE_FLASH128) {
		if (flash_id_mode && (addr & 0xFFFF) < 2)
			return flash_id[save_type == SAVE_FLASH128][addr & 1];
		return save_data[flash_bank * 0x10000 + (addr & 0xFFFF)];
	}

	return 0xFF;
}

void cart_write_sram(u32 addr, u8 v)
{
	if (save_type == SAVE_SRAM) {
		save_data[addr & 0x7FFF] = v;
		save_dirty = true;
		return;
	}

	if (save_type != SAVE_FLASH64 && save_type != SAVE_FLASH128) return;

	u32 off = addr & 0xFFFF;

	if (flash_phase == 3) {              /* byte program */
		save_data[flash_bank * 0x10000 + off] = v;
		save_dirty = true;
		flash_phase = 0;
		return;
	}

	if (flash_phase == 4) {              /* bank select */
		flash_bank = v & 1;
		flash_phase = 0;
		return;
	}

	if (off == 0x5555 && v == 0xAA && flash_phase == 0) { flash_phase = 1; return; }
	if (off == 0x2AAA && v == 0x55 && flash_phase == 1) { flash_phase = 2; return; }

	if (flash_phase == 2) {
		flash_phase = 0;
		switch (v) {
		case 0x90: flash_id_mode = true; return;
		case 0xF0: flash_id_mode = false; return;
		case 0x80: flash_erase_armed = true; return;
		case 0xA0: flash_phase = 3; return;
		case 0xB0: if (save_type == SAVE_FLASH128) flash_phase = 4; return;
		case 0x10:
			if (flash_erase_armed) {
				memset(save_data, 0xFF, save_size);
				save_dirty = true;
				flash_erase_armed = false;
			}
			return;
		case 0x30:
			if (flash_erase_armed) {
				memset(&save_data[flash_bank * 0x10000 + (off & 0xF000)], 0xFF, 0x1000);
				save_dirty = true;
				flash_erase_armed = false;
			}
			return;
		}
		return;
	}

	if (v == 0x30 && flash_erase_armed) {
		memset(&save_data[flash_bank * 0x10000 + (off & 0xF000)], 0xFF, 0x1000);
		save_dirty = true;
		flash_erase_armed = false;
		flash_phase = 0;
	}
}

/* ---- eeprom ---- */

bool cart_is_eeprom_addr(u32 addr)
{
	if (save_type != SAVE_EEPROM512 && save_type != SAVE_EEPROM8K) return false;
	if ((addr >> 24) != 0x0D) return false;
	/* on large carts eeprom only answers in the top 256 bytes of the region */
	return rom_size <= 0x1000000 || (addr & 0x1FFFF00) == 0x1FFFF00;
}

static void ee_store(void)
{
	u32 base = (u32)(ee_addr * 8) & (sizeof save_data - 1);

	for (int i = 0; i < 8; i++)
		save_data[base + i] = (u8)(ee_data >> (56 - i * 8));

	save_dirty = true;
	if (!save_size) save_size = 0x2000;
}

void cart_write_eeprom(u16 v, u32 dma_count)
{
	if (dma_count == 9 || dma_count == 73) ee_width = 6;
	else if (dma_count == 17 || dma_count == 81) ee_width = 14;

	int bit = v & 1;

	switch (ee_phase) {
	case EE_IDLE:
		ee_cmd = bit;
		ee_phase = EE_CMD;
		break;
	case EE_CMD:
		ee_cmd = (ee_cmd << 1) | bit;
		ee_phase = EE_ADDR;
		ee_count = 0;
		ee_addr = 0;
		break;
	case EE_ADDR:
		ee_addr = (ee_addr << 1) | bit;
		if (++ee_count == ee_width) {
			if (ee_cmd == 3) {
				ee_phase = EE_END_R;
			} else {
				ee_phase = EE_DATA;
				ee_count = 0;
				ee_data = 0;
			}
		}
		break;
	case EE_DATA:
		ee_data = (ee_data << 1) | (u64)bit;
		if (++ee_count == 64) ee_phase = EE_END_W;
		break;
	case EE_END_R:
		ee_read_pos = 0;
		ee_phase = EE_READ;
		break;
	case EE_END_W:
		ee_store();
		ee_phase = EE_IDLE;
		break;
	default:
		ee_phase = EE_IDLE;
		break;
	}
}

u16 cart_read_eeprom(void)
{
	if (ee_phase != EE_READ) return 1;

	int p = ee_read_pos++;
	if (p < 4) return 0;

	int i = p - 4;
	u32 base = (u32)(ee_addr * 8) & (sizeof save_data - 1);
	u64 d = 0;
	for (int k = 0; k < 8; k++) d = (d << 8) | save_data[base + k];

	if (i >= 63) ee_phase = EE_IDLE;
	return (u16)((d >> (63 - i)) & 1);
}
