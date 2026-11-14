#include <string.h>
#include "gba.h"

Timer timers[4];

static const int prescale[4] = { 0, 6, 8, 10 };

void timer_write_control(int n, u16 v)
{
	bool was_on = timers[n].control & 0x80;

	timers[n].control = v;
	timers[n].shift = prescale[v & 3];

	if (!was_on && (v & 0x80)) {
		timers[n].counter = timers[n].reload;
		timers[n].residue = 0;
	}
}

void timer_write_reload(int n, u16 v)
{
	timers[n].reload = v;
}

u16 timer_read_counter(int n)
{
	return timers[n].counter;
}

void timer_run(int cycles)
{
	int carries = 0;

	for (int n = 0; n < 4; n++) {
		u16 ctl = timers[n].control;

		if (!(ctl & 0x80)) {
			carries = 0;
			continue;
		}

		int ticks;
		if (n > 0 && (ctl & 4)) {
			ticks = carries;
		} else {
			timers[n].residue += cycles;
			ticks = timers[n].residue >> timers[n].shift;
			timers[n].residue -= ticks << timers[n].shift;
		}

		carries = 0;
		if (!ticks) continue;

		u32 next = timers[n].counter + (u32)ticks;

		if (next > 0xFFFF) {
			u32 period = 0x10000u - timers[n].reload;
			u32 over = next - 0x10000u;

			carries = 1 + (int)(over / period);
			timers[n].counter = (u16)(timers[n].reload + over % period);

			if (ctl & 0x40) irq_raise((u16)(IRQ_TIMER0 << n));
			for (int i = 0; i < carries; i++) apu_timer_overflow(n);
		} else {
			timers[n].counter = (u16)next;
		}
	}
}

void timer_reset(void)
{
	memset(timers, 0, sizeof timers);
}
