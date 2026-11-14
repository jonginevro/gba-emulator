#include <string.h>
#include "gba.h"

u16 framebuffer[SCREEN_W * SCREEN_H];
bool ppu_frame_done;

static int vcount;
static int dot;
static int cycles;

static s32 ref_x[2], ref_y[2];

static u16 bg_buf[4][SCREEN_W];
static u16 obj_buf[SCREEN_W];
static u8  obj_prio[SCREEN_W];
static u8  obj_semi[SCREEN_W];
static u8  obj_win[SCREEN_W];
static u8  win_mask[SCREEN_W];

#define PAL ((u16 *)pram)

static inline u32 io32(u32 a) { return io_read16(a) | ((u32)io_read16(a + 2) << 16); }

static s32 sign28(u32 v)
{
	return (s32)(v << 4) >> 4;
}

u16 ppu_vcount(void)
{
	return (u16)vcount;
}

void ppu_write_dispstat(u16 v)
{
	u16 cur = io_read16(REG_DISPSTAT);
	io_write16(REG_DISPSTAT, (u16)((cur & 0x0007) | (v & ~0x0007)));
}

void ppu_write_affine_ref(u32 addr)
{
	addr &= 0x3FC;
	if (addr == REG_BG2X || addr == REG_BG2X + 2) ref_x[0] = sign28(io32(REG_BG2X));
	else if (addr == REG_BG2Y || addr == REG_BG2Y + 2) ref_y[0] = sign28(io32(REG_BG2Y));
	else if (addr == REG_BG3X || addr == REG_BG3X + 2) ref_x[1] = sign28(io32(REG_BG3X));
	else if (addr == REG_BG3Y || addr == REG_BG3Y + 2) ref_y[1] = sign28(io32(REG_BG3Y));
}

/* ---- backgrounds ---- */

static void render_text(int bg, int line)
{
	u16 cnt = io_read16(REG_BG0CNT + bg * 2);
	u32 char_base = ((cnt >> 2) & 3) * 0x4000;
	u32 screen_base = ((cnt >> 8) & 0x1F) * 0x800;
	bool pal256 = cnt & 0x80;
	u32 size = (cnt >> 14) & 3;

	u32 hofs = io_read16(REG_BG0HOFS + bg * 4) & 0x1FF;
	u32 vofs = io_read16(REG_BG0HOFS + bg * 4 + 2) & 0x1FF;

	if (cnt & 0x40) {
		int mv = ((io_read16(REG_MOSAIC) >> 4) & 0xF) + 1;
		line -= line % mv;
	}

	int width = (size & 1) ? 512 : 256;
	int height = (size & 2) ? 512 : 256;
	int y = (line + vofs) & (height - 1);

	for (int x = 0; x < SCREEN_W; x++) {
		int tx = (x + hofs) & (width - 1);

		u32 map = screen_base;
		if (tx >= 256) map += 0x800;
		if (y >= 256) map += (width == 512) ? 0x1000 : 0x800;

		u16 entry = *(u16 *)&vram[map + ((y & 255) >> 3) * 64 + ((tx & 255) >> 3) * 2];
		u32 tile = entry & 0x3FF;
		int px = tx & 7, py = y & 7;
		if (entry & 0x400) px ^= 7;
		if (entry & 0x800) py ^= 7;

		if (pal256) {
			u32 off = char_base + tile * 64 + py * 8 + px;
			if (off >= 0x10000) continue;
			u8 idx = vram[off];
			if (idx) bg_buf[bg][x] = PAL[idx] | 0x8000;
		} else {
			u32 off = char_base + tile * 32 + py * 4 + (px >> 1);
			if (off >= 0x10000) continue;
			u8 b = vram[off];
			u8 idx = (px & 1) ? (b >> 4) : (b & 0xF);
			if (idx) bg_buf[bg][x] = PAL[((entry >> 12) & 0xF) * 16 + idx] | 0x8000;
		}
	}
}

static void render_affine(int bg)
{
	u16 cnt = io_read16(REG_BG0CNT + bg * 2);
	u32 char_base = ((cnt >> 2) & 3) * 0x4000;
	u32 screen_base = ((cnt >> 8) & 0x1F) * 0x800;
	bool wrap = cnt & 0x2000;
	int size = 128 << ((cnt >> 14) & 3);
	int tiles = size >> 3;

	int n = bg - 2;
	u32 base = (bg == 2) ? REG_BG2PA : REG_BG3PA;
	s16 pa = (s16)io_read16(base), pc = (s16)io_read16(base + 4);
	s32 cx = ref_x[n], cy = ref_y[n];

	for (int x = 0; x < SCREEN_W; x++, cx += pa, cy += pc) {
		s32 px = cx >> 8, py = cy >> 8;

		if (wrap) {
			px &= size - 1;
			py &= size - 1;
		} else if (px < 0 || px >= size || py < 0 || py >= size) {
			continue;
		}

		u8 tile = vram[(screen_base + (py >> 3) * tiles + (px >> 3)) & 0xFFFF];
		u32 off = char_base + tile * 64 + (py & 7) * 8 + (px & 7);
		if (off >= 0x10000) continue;

		u8 idx = vram[off];
		if (idx) bg_buf[bg][x] = PAL[idx] | 0x8000;
	}
}

static void render_bitmap(int mode)
{
	u16 dispcnt = io_read16(REG_DISPCNT);
	int w = (mode == 5) ? 160 : 240;
	int h = (mode == 5) ? 128 : 160;
	u32 base = (mode != 3 && (dispcnt & 0x10)) ? 0xA000 : 0;

	s16 pa = (s16)io_read16(REG_BG2PA), pc = (s16)io_read16(REG_BG2PA + 4);
	s32 cx = ref_x[0], cy = ref_y[0];

	for (int x = 0; x < SCREEN_W; x++, cx += pa, cy += pc) {
		s32 px = cx >> 8, py = cy >> 8;
		if (px < 0 || px >= w || py < 0 || py >= h) continue;

		u32 off = py * w + px;
		if (mode == 4) {
			u8 idx = vram[base + off];
			if (idx) bg_buf[2][x] = PAL[idx] | 0x8000;
		} else {
			bg_buf[2][x] = *(u16 *)&vram[base + off * 2] | 0x8000;
		}
	}
}

/* ---- sprites ---- */

static const u8 obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const u8 obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void render_sprites(int line)
{
	u16 dispcnt = io_read16(REG_DISPCNT);
	bool map1d = dispcnt & 0x40;
	u32 tile_floor = (dispcnt & 7) >= 3 ? 512 : 0;

	for (int n = 0; n < 128; n++) {
		u16 a0 = *(u16 *)&oam[n * 8];
		u16 a1 = *(u16 *)&oam[n * 8 + 2];
		u16 a2 = *(u16 *)&oam[n * 8 + 4];

		bool affine = a0 & 0x100;
		bool dbl = a0 & 0x200;
		if (!affine && dbl) continue;

		int mode = (a0 >> 10) & 3;
		if (mode == 3) continue;

		int shape = (a0 >> 14) & 3;
		if (shape == 3) continue;
		int size = (a1 >> 14) & 3;

		int w = obj_w[shape][size], h = obj_h[shape][size];
		int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;

		int ly = (line - (a0 & 0xFF)) & 0xFF;
		if (ly >= bh) continue;

		int sx = a1 & 0x1FF;
		if (sx & 0x100) sx -= 512;

		s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
		if (affine) {
			int g = ((a1 >> 9) & 0x1F) * 32;
			pa = *(s16 *)&oam[g + 6];
			pb = *(s16 *)&oam[g + 14];
			pc = *(s16 *)&oam[g + 22];
			pd = *(s16 *)&oam[g + 30];
		}

		bool pal256 = a0 & 0x2000;
		u32 tile = a2 & 0x3FF;
		if (tile < tile_floor) continue;
		int prio = (a2 >> 10) & 3;
		u32 pal = (a2 >> 12) & 0xF;

		for (int i = 0; i < bw; i++) {
			int x = sx + i;
			if (x < 0 || x >= SCREEN_W) continue;

			int tx, ty;
			if (affine) {
				int ox = i - bw / 2, oy = ly - bh / 2;
				tx = ((pa * ox + pb * oy) >> 8) + w / 2;
				ty = ((pc * ox + pd * oy) >> 8) + h / 2;
				if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
			} else {
				tx = (a1 & 0x1000) ? w - 1 - i : i;
				ty = (a1 & 0x2000) ? h - 1 - ly : ly;
			}

			u32 off;
			if (pal256) {
				u32 tn = tile + (map1d ? (ty >> 3) * (w >> 3) * 2 : (ty >> 3) * 32);
				off = 0x10000 + tn * 32 + (tx >> 3) * 64 + (ty & 7) * 8 + (tx & 7);
			} else {
				u32 tn = tile + (map1d ? (ty >> 3) * (w >> 3) : (ty >> 3) * 32);
				off = 0x10000 + tn * 32 + (tx >> 3) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
			}
			if (off >= 0x18000) continue;

			u8 idx;
			if (pal256) idx = vram[off];
			else idx = (tx & 1) ? (vram[off] >> 4) : (vram[off] & 0xF);
			if (!idx) continue;

			if (mode == 2) {
				obj_win[x] = 1;
				continue;
			}

			if (obj_buf[x] && obj_prio[x] <= prio) continue;

			obj_buf[x] = (pal256 ? PAL[256 + idx] : PAL[256 + pal * 16 + idx]) | 0x8000;
			obj_prio[x] = (u8)prio;
			obj_semi[x] = (mode == 1);
		}
	}
}

/* ---- windows ---- */

static bool in_window(u32 hreg, u32 vreg, int line, int x)
{
	u16 hv = io_read16(hreg), vv = io_read16(vreg);
	int x1 = hv >> 8, x2 = hv & 0xFF;
	int y1 = vv >> 8, y2 = vv & 0xFF;

	if (x2 > SCREEN_W || x1 > x2) x2 = SCREEN_W;
	if (y2 > SCREEN_H || y1 > y2) y2 = SCREEN_H;

	return x >= x1 && x < x2 && line >= y1 && line < y2;
}

static void build_windows(int line)
{
	u16 dispcnt = io_read16(REG_DISPCNT);

	if (!(dispcnt & 0xE000)) {
		memset(win_mask, 0x3F, sizeof win_mask);
		return;
	}

	u16 winin = io_read16(REG_WININ), winout = io_read16(REG_WINOUT);

	for (int x = 0; x < SCREEN_W; x++) {
		u8 m = winout & 0x3F;
		if ((dispcnt & 0x8000) && obj_win[x]) m = (winout >> 8) & 0x3F;
		if ((dispcnt & 0x4000) && in_window(REG_WIN1H, REG_WIN1V, line, x)) m = (winin >> 8) & 0x3F;
		if ((dispcnt & 0x2000) && in_window(REG_WIN0H, REG_WIN0V, line, x)) m = winin & 0x3F;
		win_mask[x] = m;
	}
}

/* ---- blending ---- */

static inline u16 blend(u16 a, u16 b)
{
	u16 ba = io_read16(REG_BLDALPHA);
	int eva = ba & 0x1F, evb = (ba >> 8) & 0x1F;
	if (eva > 16) eva = 16;
	if (evb > 16) evb = 16;

	int r = (((a & 31) * eva) + ((b & 31) * evb)) >> 4;
	int g = ((((a >> 5) & 31) * eva) + (((b >> 5) & 31) * evb)) >> 4;
	int bl = ((((a >> 10) & 31) * eva) + (((b >> 10) & 31) * evb)) >> 4;

	if (r > 31) r = 31;
	if (g > 31) g = 31;
	if (bl > 31) bl = 31;
	return (u16)(r | (g << 5) | (bl << 10));
}

static inline u16 fade(u16 c, bool up)
{
	int evy = io_read16(REG_BLDY) & 0x1F;
	if (evy > 16) evy = 16;

	int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
	if (up) {
		r += ((31 - r) * evy) >> 4;
		g += ((31 - g) * evy) >> 4;
		b += ((31 - b) * evy) >> 4;
	} else {
		r -= (r * evy) >> 4;
		g -= (g * evy) >> 4;
		b -= (b * evy) >> 4;
	}
	return (u16)(r | (g << 5) | (b << 10));
}

/* ---- scanline ---- */

static void compose(int line)
{
	u16 dispcnt = io_read16(REG_DISPCNT);
	u16 bldcnt = io_read16(REG_BLDCNT);
	int effect = (bldcnt >> 6) & 3;
	u16 *out = &framebuffer[line * SCREEN_W];
	u16 backdrop = PAL[0] & 0x7FFF;

	bool bg_on[4];
	int bg_prio[4];
	for (int b = 0; b < 4; b++) {
		bg_on[b] = dispcnt & (0x100 << b);
		bg_prio[b] = io_read16(REG_BG0CNT + b * 2) & 3;
	}
	bool obj_on = dispcnt & 0x1000;

	for (int x = 0; x < SCREEN_W; x++) {
		u8 m = win_mask[x];
		int l1 = 5, l2 = 5;
		u16 c1 = backdrop, c2 = backdrop;
		int found = 0;

		for (int prio = 0; prio < 4 && found < 2; prio++) {
			if (obj_on && (m & 0x10) && obj_buf[x] && obj_prio[x] == prio) {
				if (found == 0) { l1 = 4; c1 = obj_buf[x] & 0x7FFF; }
				else { l2 = 4; c2 = obj_buf[x] & 0x7FFF; }
				found++;
			}
			for (int b = 0; b < 4 && found < 2; b++) {
				if (!bg_on[b] || bg_prio[b] != prio || !(m & (1 << b))) continue;
				if (!bg_buf[b][x]) continue;
				if (found == 0) { l1 = b; c1 = bg_buf[b][x] & 0x7FFF; }
				else { l2 = b; c2 = bg_buf[b][x] & 0x7FFF; }
				found++;
			}
		}

		u16 col = c1;
		bool fx = m & 0x20;
		bool semi = l1 == 4 && obj_semi[x];

		if (semi && (bldcnt & (0x100 << l2)) && found > 1) {
			col = blend(c1, c2);
		} else if (fx && effect) {
			if (bldcnt & (1 << l1)) {
				if (effect == 1) {
					if (found > 1 && (bldcnt & (0x100 << l2))) col = blend(c1, c2);
				} else {
					col = fade(c1, effect == 2);
				}
			}
		}

		out[x] = col;
	}
}

static void render_line(int line)
{
	u16 dispcnt = io_read16(REG_DISPCNT);
	u16 *out = &framebuffer[line * SCREEN_W];

	if (dispcnt & 0x80) {
		for (int x = 0; x < SCREEN_W; x++) out[x] = 0x7FFF;
		return;
	}

	memset(bg_buf, 0, sizeof bg_buf);
	memset(obj_buf, 0, sizeof obj_buf);
	memset(obj_win, 0, sizeof obj_win);
	memset(obj_semi, 0, sizeof obj_semi);
	memset(obj_prio, 4, sizeof obj_prio);

	if (dispcnt & 0x1000) render_sprites(line);

	switch (dispcnt & 7) {
	case 0:
		for (int b = 0; b < 4; b++)
			if (dispcnt & (0x100 << b)) render_text(b, line);
		break;
	case 1:
		if (dispcnt & 0x100) render_text(0, line);
		if (dispcnt & 0x200) render_text(1, line);
		if (dispcnt & 0x400) render_affine(2);
		break;
	case 2:
		if (dispcnt & 0x400) render_affine(2);
		if (dispcnt & 0x800) render_affine(3);
		break;
	case 3: case 4: case 5:
		if (dispcnt & 0x400) render_bitmap(dispcnt & 7);
		break;
	}

	/* horizontal mosaic is applied after the layer is drawn */
	int mh = (io_read16(REG_MOSAIC) & 0xF) + 1;
	if (mh > 1) {
		for (int b = 0; b < 4; b++) {
			if (!(io_read16(REG_BG0CNT + b * 2) & 0x40)) continue;
			for (int x = 0; x < SCREEN_W; x++) bg_buf[b][x] = bg_buf[b][x - x % mh];
		}
	}

	build_windows(line);
	compose(line);
}

static void advance_affine(void)
{
	for (int n = 0; n < 2; n++) {
		u32 base = n ? REG_BG3PA : REG_BG2PA;
		ref_x[n] += (s16)io_read16(base + 2);
		ref_y[n] += (s16)io_read16(base + 6);
	}
}

static void check_vcount_irq(void)
{
	u16 stat = io_read16(REG_DISPSTAT);

	if (((stat >> 8) & 0xFF) == (u32)vcount) {
		io_write16(REG_DISPSTAT, stat | 4);
		if (stat & 0x20) irq_raise(IRQ_VCOUNT);
	} else {
		io_write16(REG_DISPSTAT, stat & ~4);
	}
}

void ppu_run(int cyc)
{
	cycles += cyc;

	while (cycles >= 4) {
		cycles -= 4;
		dot++;

		if (dot == 240) {
			u16 stat = io_read16(REG_DISPSTAT);
			io_write16(REG_DISPSTAT, stat | 2);

			if (vcount < SCREEN_H) {
				render_line(vcount);
				advance_affine();
				dma_trigger(DMA_HBLANK);
			}
			if (stat & 0x10) irq_raise(IRQ_HBLANK);
			continue;
		}

		if (dot < DOTS_PER_LINE) continue;

		dot = 0;
		vcount++;
		if (vcount == LINES_PER_FRAME) vcount = 0;

		u16 stat = io_read16(REG_DISPSTAT) & ~2u;

		if (vcount == SCREEN_H) {
			stat |= 1;
			io_write16(REG_DISPSTAT, stat);
			if (stat & 0x08) irq_raise(IRQ_VBLANK);
			dma_trigger(DMA_VBLANK);
			ppu_frame_done = true;
		} else if (vcount == LINES_PER_FRAME - 1) {
			io_write16(REG_DISPSTAT, stat & ~1u);
		} else {
			io_write16(REG_DISPSTAT, stat);
		}

		if (vcount == 0) {
			ref_x[0] = sign28(io32(REG_BG2X));
			ref_y[0] = sign28(io32(REG_BG2Y));
			ref_x[1] = sign28(io32(REG_BG3X));
			ref_y[1] = sign28(io32(REG_BG3Y));
		}

		check_vcount_irq();
	}
}

void ppu_reset(void)
{
	vcount = dot = cycles = 0;
	ppu_frame_done = false;
	memset(framebuffer, 0, sizeof framebuffer);
	memset(ref_x, 0, sizeof ref_x);
	memset(ref_y, 0, sizeof ref_y);
}
