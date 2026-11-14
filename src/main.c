#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio;
static u32 pixels[SCREEN_W * SCREEN_H];

static const struct { SDL_Scancode key; u16 bit; } keymap[] = {
	{ SDL_SCANCODE_Z,         1 << 0 },
	{ SDL_SCANCODE_X,         1 << 1 },
	{ SDL_SCANCODE_BACKSPACE, 1 << 2 },
	{ SDL_SCANCODE_RETURN,    1 << 3 },
	{ SDL_SCANCODE_RIGHT,     1 << 4 },
	{ SDL_SCANCODE_LEFT,      1 << 5 },
	{ SDL_SCANCODE_UP,        1 << 6 },
	{ SDL_SCANCODE_DOWN,      1 << 7 },
	{ SDL_SCANCODE_S,         1 << 8 },
	{ SDL_SCANCODE_A,         1 << 9 },
};

static void audio_callback(void *user, Uint8 *stream, int len)
{
	(void)user;
	int frames = len / 4;
	int got = apu_read_samples((s16 *)stream, frames);

	if (got < frames) memset(stream + got * 4, 0, (size_t)(frames - got) * 4);
}

static void update_keys(void)
{
	const Uint8 *state = SDL_GetKeyboardState(NULL);
	u16 k = 0x03FF;

	for (size_t i = 0; i < sizeof keymap / sizeof *keymap; i++)
		if (state[keymap[i].key]) k &= ~keymap[i].bit;

	keys_state = k;

	u16 ctl = io_read16(REG_KEYCNT);
	if (ctl & 0x4000) {
		u16 want = ctl & 0x3FF;
		u16 down = (u16)~k & 0x3FF;
		bool hit = (ctl & 0x8000) ? (down & want) == want : (down & want) != 0;
		if (hit) irq_raise(IRQ_KEYPAD);
	}
}

static void present(void)
{
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
		u16 c = framebuffer[i];
		u32 r = (c & 31) * 255 / 31;
		u32 g = ((c >> 5) & 31) * 255 / 31;
		u32 b = ((c >> 10) & 31) * 255 / 31;
		pixels[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
	}

	SDL_UpdateTexture(texture, NULL, pixels, SCREEN_W * 4);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);
}

static bool write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;

	fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
		u16 c = framebuffer[i];
		fputc((c & 31) * 255 / 31, f);
		fputc(((c >> 5) & 31) * 255 / 31, f);
		fputc(((c >> 10) & 31) * 255 / 31, f);
	}
	fclose(f);
	return true;
}

static int run_headless(int frames, const char *ppm)
{
	gba_reset();
	for (int i = 0; i < frames; i++) gba_run_frame();

	printf("r0-r15:");
	for (int i = 0; i < 16; i++) printf(" %08x", cpu.r[i]);
	printf("\ncpsr: %08x\n", cpu.cpsr);

	if (ppm && !write_ppm(ppm)) {
		fprintf(stderr, "could not write %s\n", ppm);
		return 1;
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [options] rom.gba\n"
		"  -b, --bios <file>   use a real BIOS image instead of the built-in one\n"
		"  -s, --scale <n>     window scale (default 3)\n"
		"      --no-audio      run without sound\n"
		"      --frames <n>    run n frames without a window, then exit\n"
		"      --ppm <file>    with --frames, write the last frame as a ppm\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *rom_path = NULL, *bios_path = NULL;
	const char *ppm_path = NULL;
	int scale = 3, headless = 0;
	bool want_audio = true;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--bios")) {
			if (++i >= argc) { usage(argv[0]); return 1; }
			bios_path = argv[i];
		} else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--scale")) {
			if (++i >= argc) { usage(argv[0]); return 1; }
			scale = atoi(argv[i]);
			if (scale < 1) scale = 1;
		} else if (!strcmp(argv[i], "--frames")) {
			if (++i >= argc) { usage(argv[0]); return 1; }
			headless = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--ppm")) {
			if (++i >= argc) { usage(argv[0]); return 1; }
			ppm_path = argv[i];
		} else if (!strcmp(argv[i], "--no-audio")) {
			want_audio = false;
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 1;
		} else {
			rom_path = argv[i];
		}
	}

	if (!rom_path) {
		usage(argv[0]);
		return 1;
	}

	if (!cart_load(rom_path)) {
		fprintf(stderr, "could not load rom: %s\n", rom_path);
		return 1;
	}

	if (bios_path && !gba_load_bios(bios_path)) {
		fprintf(stderr, "could not load bios: %s\n", bios_path);
		return 1;
	}

	if (headless > 0) return run_headless(headless, ppm_path);

	if (SDL_Init(SDL_INIT_VIDEO | (want_audio ? SDL_INIT_AUDIO : 0)) != 0) {
		fprintf(stderr, "sdl init failed: %s\n", SDL_GetError());
		return 1;
	}

	char caption[64];
	snprintf(caption, sizeof caption, "gba - %s", cart_title());

	window = SDL_CreateWindow(caption, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                          SCREEN_W * scale, SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
	                            SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);

	if (!window || !renderer || !texture) {
		fprintf(stderr, "sdl setup failed: %s\n", SDL_GetError());
		return 1;
	}

	if (want_audio) {
		SDL_AudioSpec want = { 0 };
		want.freq = AUDIO_RATE;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		want.callback = audio_callback;

		audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
		if (audio) SDL_PauseAudioDevice(audio, 0);
	}

	gba_reset();

	bool running = true;
	bool turbo = false;
	int frames = 0;
	Uint64 freq = SDL_GetPerformanceFrequency();
	Uint64 next = SDL_GetPerformanceCounter();

	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) running = false;
			if (ev.type == SDL_KEYDOWN) {
				if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = false;
				if (ev.key.keysym.scancode == SDL_SCANCODE_SPACE) turbo = true;
				if (ev.key.keysym.scancode == SDL_SCANCODE_R && (ev.key.keysym.mod & KMOD_CTRL))
					gba_reset();
			}
			if (ev.type == SDL_KEYUP && ev.key.keysym.scancode == SDL_SCANCODE_SPACE)
				turbo = false;
		}

		update_keys();
		gba_run_frame();
		present();

		if (++frames % 180 == 0) cart_save_battery();

		next += (Uint64)(freq / 59.7275);
		if (!turbo) {
			Uint64 now = SDL_GetPerformanceCounter();
			if (now < next) {
				Uint32 ms = (Uint32)((next - now) * 1000 / freq);
				if (ms > 0) SDL_Delay(ms);
			} else {
				next = now;
			}
		} else {
			next = SDL_GetPerformanceCounter();
		}
	}

	cart_save_battery();

	if (audio) SDL_CloseAudioDevice(audio);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	cart_unload();
	return 0;
}
