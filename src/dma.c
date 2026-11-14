#include <string.h>
#include "gba.h"

DmaChannel dma[4];

static inline u32 ch_base(int n) { return 0x0B0 + n * 12; }
static inline u32 io32(u32 a) { return io_read16(a) | ((u32)io_read16(a + 2) << 16); }

static u32 src_mask(int n) { return n == 0 ? 0x07FFFFFF : 0x0FFFFFFF; }
static u32 dst_mask(int n) { return n == 3 ? 0x0FFFFFFF : 0x07FFFFFF; }
static u32 max_count(int n) { return n == 3 ? 0x10000 : 0x4000; }

static u32 latched_count(int n)
{
	u32 c = io_read16(ch_base(n) + 8);
	if (n != 3) c &= 0x3FFF;
	return c ? c : max_count(n);
}

static void dma_run(int n)
{
	u32 base = ch_base(n);
	u16 ctl = io_read16(base + 10);
	int timing = (ctl >> 12) & 3;
	int dst_ctl = (ctl >> 5) & 3;
	int src_ctl = (ctl >> 7) & 3;
	bool word = ctl & 0x400;

	bool fifo = (n == 1 || n == 2) && timing == DMA_SPECIAL;
	u32 count = dma[n].count;

	if (fifo) {
		word = true;
		count = 4;
		dst_ctl = 2;
	}

	int step = word ? 4 : 2;

	for (u32 i = 0; i < count; i++) {
		if (!word && cart_is_eeprom_addr(dma[n].dst))
			cart_write_eeprom(bus_read16(dma[n].src), count);
		else if (word) bus_write32(dma[n].dst, bus_read32(dma[n].src));
		else bus_write16(dma[n].dst, bus_read16(dma[n].src));

		if (src_ctl == 0) dma[n].src += step;
		else if (src_ctl == 1) dma[n].src -= step;

		if (dst_ctl == 0) dma[n].dst += step;
		else if (dst_ctl == 1) dma[n].dst -= step;
	}

	if (ctl & 0x4000) irq_raise((u16)(IRQ_DMA0 << n));

	if ((ctl & 0x200) && timing != DMA_NOW) {
		dma[n].count = fifo ? 4 : latched_count(n);
		if (dst_ctl == 3) dma[n].dst = io32(base + 4) & dst_mask(n);
	} else {
		io_write16(base + 10, (u16)(ctl & ~0x8000));
		dma[n].running = false;
	}
}

void dma_write_control(int ch, u16 v)
{
	u32 base = ch_base(ch);

	if (!(v & 0x8000)) {
		dma[ch].running = false;
		return;
	}

	if (dma[ch].running) return;

	dma[ch].running = true;
	dma[ch].src = io32(base) & src_mask(ch);
	dma[ch].dst = io32(base + 4) & dst_mask(ch);
	dma[ch].count = latched_count(ch);

	if (((v >> 12) & 3) == DMA_NOW) dma_run(ch);
}

void dma_trigger(int timing)
{
	for (int n = 0; n < 4; n++) {
		u16 ctl = io_read16(ch_base(n) + 10);
		if (!(ctl & 0x8000)) continue;
		if (((ctl >> 12) & 3) != timing) continue;
		if (timing == DMA_SPECIAL) continue;
		dma_run(n);
	}
}

void dma_fifo(int fifo)
{
	u32 target = fifo ? 0x040000A4 : 0x040000A0;

	for (int n = 1; n <= 2; n++) {
		u16 ctl = io_read16(ch_base(n) + 10);
		if (!(ctl & 0x8000)) continue;
		if (((ctl >> 12) & 3) != DMA_SPECIAL) continue;
		if ((io32(ch_base(n) + 4) & 0x0FFFFFFF) != target) continue;
		dma_run(n);
	}
}

void dma_reset(void)
{
	memset(dma, 0, sizeof dma);
}
