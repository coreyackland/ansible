/*

KRIA

note sync mode-- tr + note linked in editing

loop helper display for oct/dur and probs
ext clock mul slightly f'd

pattern copy
pattern play

*/

#include "string.h"

#include "print_funcs.h"
#include "flashc.h"
#include "gpio.h"

#include "monome.h"
#include "i2c.h"
#include "dac.h"
#include "util.h" // rnd
#include "music.h"

#include "main.h"
#include "ansible_grid.h"


#define L2 12
#define L1 8
#define L0 4

bool preset_mode;
uint8_t preset;

u8 key_count = 0;
u8 held_keys[32];
u8 key_times[128];

bool clock_external;
bool view_clock;
bool view_config;
uint32_t clock_period;
uint8_t clock_count;
uint8_t clock_mul;
uint8_t ext_clock_count;
uint8_t ext_clock_phase;

uint8_t time_rough;
uint8_t time_fine;

uint8_t scale_data[16][8];

u8 cur_scale[8];
void calc_scale(uint8_t s);

void (*grid_refresh)(void);


kria_data_t k;

mp_data_t m;
u8 sound;
u8 voice_mode;


void set_mode_grid() {
	switch(ansible_mode) {
	case mGridKria:
		print_dbg("\r\n> mode grid kria");
		app_event_handlers[kEventKey] = &handler_KriaKey;
		app_event_handlers[kEventTr] = &handler_KriaTr;
		app_event_handlers[kEventTrNormal] = &handler_KriaTrNormal;
		app_event_handlers[kEventMonomeGridKey] = &handler_KriaGridKey;
		app_event_handlers[kEventMonomeRefresh] = &handler_KriaRefresh;
		clock = &clock_kria;
		clock_set(f.kria_state.clock_period);
		process_ii = &ii_kria;
		resume_kria();
		update_leds(1);
		break;
	case mGridMP:
		print_dbg("\r\n> mode grid mp");
		app_event_handlers[kEventKey] = &handler_MPKey;
		app_event_handlers[kEventTr] = &handler_MPTr;
		app_event_handlers[kEventTrNormal] = &handler_MPTrNormal;
		app_event_handlers[kEventMonomeGridKey] = &handler_MPGridKey;
		app_event_handlers[kEventMonomeRefresh] = &handler_MPRefresh;
		clock = &clock_mp;
		clock_set(f.mp_state.clock_period);
		process_ii = &ii_mp;
		resume_mp();
		update_leds(2);
		break;
	default:
		break;
	}
	
	// if(connected == conGRID) {
		app_event_handlers[kEventFrontShort] = &handler_GridFrontShort;
		app_event_handlers[kEventFrontLong] = &handler_GridFrontLong;
	// }

	flashc_memset32((void*)&(f.state.none_mode), ansible_mode, 4, true);
	flashc_memset32((void*)&(f.state.grid_mode), ansible_mode, 4, true);
}


void handler_GridFrontShort(s32 data) {
	if(preset_mode) {
		print_dbg("\r\n> PRESET EXIT");
		preset_mode = false;

		if(ansible_mode == mGridMP)
			grid_refresh = &refresh_mp;
		else
			grid_refresh = &refresh_kria;
		view_config = false;
		view_clock = false;
	}
	else {
		print_dbg("\r\n> PRESET ENTER");
		preset_mode = true;
		grid_refresh = &refresh_preset;
		view_config = false;
		view_clock = false;
	}
}

void handler_GridFrontLong(s32 data) {
	if(ansible_mode == mGridKria)
		set_mode(mGridMP);
	else
		set_mode(mGridKria);
}

void refresh_preset(void) {
	u8 i1, i2;//, i3;

	memset(monomeLedBuffer,0,128);

	for(i1=0;i1<128;i1++)
		monomeLedBuffer[i1] = 0;

	monomeLedBuffer[preset * 16] = 11;

	switch(ansible_mode) {
	case mGridMP:
		for(i1=0;i1<8;i1++)
			for(i2=0;i2<8;i2++)
				if(m.glyph[i1] & (1<<i2))
					monomeLedBuffer[i1*16+i2+8] = 9;
		break;
	case mGridKria:
		for(i1=0;i1<8;i1++)
			for(i2=0;i2<8;i2++)
				if(k.glyph[i1] & (1<<i2))
					monomeLedBuffer[i1*16+i2+8] = 9;
		break;
	default: break;
	}

	monome_set_quadrant_flag(0);
	monome_set_quadrant_flag(1);
}

void grid_keytimer(void) {
	for(uint8_t i1=0;i1<key_count;i1++) {
		if(key_times[held_keys[i1]])
		if(--key_times[held_keys[i1]]==0) {
			if(preset_mode == 1) {
				if(held_keys[i1] % 16 == 0) {
					preset = held_keys[i1] / 16;

					// WRITE PRESET

					if(ansible_mode == mGridMP) {
						flashc_memset8((void*)&(f.mp_state.preset), preset, 1, true);
						flashc_memset8((void*)&(f.mp_state.sound), sound, 1, true);
						flashc_memset8((void*)&(f.mp_state.voice_mode), voice_mode, 1, true);
						flashc_memcpy((void *)&f.mp_state.m[preset], &m, sizeof(m), true);
						
						flashc_memcpy((void *)&f.scale, &scale_data, sizeof(scale_data), true);

						preset_mode = false;
						grid_refresh = &refresh_mp;
					} else if(ansible_mode == mGridKria) {
						flashc_memset8((void*)&(f.kria_state.preset), preset, 1, true);
						flashc_memcpy((void *)&f.kria_state.k[preset], &k, sizeof(k), true);
						
						flashc_memcpy((void *)&f.scale, &scale_data, sizeof(scale_data), true);

						preset_mode = false;
						grid_refresh = &refresh_kria;
					}
					
				}
			}

			// print_dbg("\rlong press: "); 
			// print_dbg_ulong(held_keys[i1]);
		}
	}
}


////////////////////////////////////////////////////////////////////////////////
// KRIA

typedef enum {
	mTr, mDur, mNote, mOct, mScale, mPattern
} kria_modes_t;

typedef enum {
	modNone, modLoop, modTime, modProb
} kria_mod_modes_t;

kria_modes_t k_mode;
kria_mod_modes_t k_mod_mode;

u8 track;

u8 loop_count;
u8 loop_first;
s8 loop_last;
u8 loop_edit;

bool note_sync;
uint8_t loop_sync;

u8 pos[4][KRIA_NUM_PARAMS];
u8 pos_mul[4][KRIA_NUM_PARAMS];
bool pos_reset;
u8 tr[4];
u8 note[4];
u8 oct[4];
u16 dur[4];


static void kria_off0(void* o);
static void kria_off1(void* o);
static void kria_off2(void* o);
static void kria_off3(void* o);

bool kria_next_step(uint8_t t, uint8_t p);
static void adjust_loop_start(u8 t, u8 x, u8 m);
static void adjust_loop_end(u8 t, u8 x, u8 m);
static void update_loop_start(u8 t, u8 x, u8 m);
static void update_loop_end(u8 t, u8 x, u8 m);


void default_kria() {
	uint8_t i1;

	flashc_memset32((void*)&(f.kria_state.clock_period), 100, 4, true);
	flashc_memset8((void*)&(f.kria_state.preset), 0, 1, true);
	flashc_memset8((void*)&(f.kria_state.note_sync), true, 1, true);
	flashc_memset8((void*)&(f.kria_state.loop_sync), 2, 1, true);

	for(i1=0;i1<8;i1++)
		k.glyph[i1] = 0;

	memset(k.p[0].t[0].tr, 0, 16);
	memset(k.p[0].t[0].oct, 0, 16);
	memset(k.p[0].t[0].note, 0, 16);
	memset(k.p[0].t[0].dur, 0, 16);
	memset(k.p[0].t[0].p, 3, 16 * KRIA_NUM_PARAMS);
	// memset(k.p[0].t[0].ptr, 3, 16);
	// memset(k.p[0].t[0].poct, 3, 16);
	// memset(k.p[0].t[0].pnote, 3, 16);
	// memset(k.p[0].t[0].pdur, 3, 16);
	k.p[0].t[0].dur_mul = 4;
	memset(k.p[0].t[0].lstart, 0, KRIA_NUM_PARAMS);
	memset(k.p[0].t[0].lend, 5, KRIA_NUM_PARAMS);
	memset(k.p[0].t[0].llen, 6, KRIA_NUM_PARAMS);
	memset(k.p[0].t[0].lswap, 0, KRIA_NUM_PARAMS);
	memset(k.p[0].t[0].tmul, 1, KRIA_NUM_PARAMS);

	k.p[0].t[1] = k.p[0].t[0];
	k.p[0].t[2] = k.p[0].t[0];
	k.p[0].t[3] = k.p[0].t[0];
	k.p[0].scale = 0;

	for(i1=1;i1<KRIA_NUM_PATTERNS;i1++)
		k.p[i1] = k.p[0];

	k.pattern = 0;

	for(i1=0;i1<GRID_PRESETS;i1++)
		flashc_memcpy((void *)&f.kria_state.k[i1], &k, sizeof(k), true);
}

void init_kria() {
	track = 0;
	k_mode = mTr;
	k_mod_mode = modNone;

	note_sync = f.kria_state.note_sync;
	loop_sync = f.kria_state.loop_sync;

	preset = f.kria_state.preset;

	k.pattern = f.kria_state.k[preset].pattern;

	k = f.kria_state.k[preset];

	clock_mul = 1;

	clock_period = f.kria_state.clock_period;
	time_rough = (clock_period - 20) / 16;
	time_fine = (clock_period - 20) % 16;

	clock_mul = 1;
}

void resume_kria() {
	grid_refresh = &refresh_kria;
	view_clock = false;
	view_config = false;

	preset = f.kria_state.preset;

	calc_scale(k.p[k.pattern].scale);

	// re-check clock jack
	clock_external = !gpio_get_pin_value(B10);

	if(clock_external)
		clock = &clock_null;
	else
		clock = &clock_kria;

	dac_set_slew(0,0);
	dac_set_slew(1,0);
	dac_set_slew(2,0);
	dac_set_slew(3,0);

	clr_tr(TR1);
	clr_tr(TR2);
	clr_tr(TR3);
	clr_tr(TR4);

	monomeFrameDirty++;
}

bool kria_next_step(uint8_t t, uint8_t p) {
	pos_mul[t][p]++;

	if(pos_mul[t][p] >= k.p[k.pattern].t[t].tmul[p]) {
		if(pos[t][p] == k.p[k.pattern].t[t].lend[p])
			pos[t][p] = k.p[k.pattern].t[t].lstart[p];
		else {
			pos[t][p]++;
			if(pos[t][p] > 15)
				pos[t][p] = 0;
		}
		pos_mul[t][p] = 0;
		switch(k.p[k.pattern].t[t].p[p][pos[t][p]]) {
		case 0:
			return false;
		case 1:
			// ~50%
			return (rnd() & 0xff) > 128;
		case 2:
			// ~75%
			return (rnd() & 0xff) > 64;
		case 3:
			return true;
		default:
			return true;
		}
	}
	else
		return false;
}

void clock_kria(uint8_t phase) {
	if(phase) {
		clock_count++;

		if(pos_reset) {
			clock_count = 0;
			for(int i1=0;i1<4;i1++)
			for(int i2=0;i2<4;i2++) {
				pos[i1][i2] = k.p[k.pattern].t[i1].lend[i2];
				pos_mul[i1][i2] = k.p[k.pattern].t[i1].tmul[i2];
			}
			pos_reset = false;
		}

		for(uint8_t i1=0;i1<4;i1++) {
			if(kria_next_step(i1, mDur)) {
				dur[i1] = (k.p[k.pattern].t[i1].dur[pos[i1][mDur]]+1) * (k.p[k.pattern].t[i1].dur_mul<<2);
			}

			if(kria_next_step(i1, mOct)) {
				oct[i1] = k.p[k.pattern].t[i1].oct[pos[i1][mOct]];
			}

			if(kria_next_step(i1, mNote)) {
				note[i1] = k.p[k.pattern].t[i1].note[pos[i1][mNote]];
			}

			if(kria_next_step(i1, mTr)) {
				if(k.p[k.pattern].t[i1].tr[pos[i1][mTr]]) {
					dac_set_value(i1, ET[cur_scale[note[i1]] + (oct[0] * 12)] << 2);
					gpio_set_gpio_pin(TR1 + i1);

					switch(i1) {
						case 0:
							timer_remove( &auxTimer[0]);
							timer_add(&auxTimer[0], dur[0], &kria_off0, NULL); break;
						case 1:
							timer_remove( &auxTimer[1]);
							timer_add(&auxTimer[1], dur[1], &kria_off1, NULL); break;
						case 2:
							timer_remove( &auxTimer[2]);
							timer_add(&auxTimer[2], dur[2], &kria_off2, NULL); break;
						case 3:
							timer_remove( &auxTimer[3]);
							timer_add(&auxTimer[3], dur[3], &kria_off3, NULL); break;
						default: break;
					}

					tr[i1] = 1;
				}
			}
		}

		monomeFrameDirty++;

		// may need forced DAC update here
		dac_timer_update();
	}
}

static void kria_off0(void* o) {
	timer_remove( &auxTimer[0]);
	clr_tr(TR1);
	tr[0] = 0;
}

static void kria_off1(void* o) {
	timer_remove( &auxTimer[1]);
	clr_tr(TR2);
	tr[1] = 0;
}

static void kria_off2(void* o) {
	timer_remove( &auxTimer[2]);
	clr_tr(TR3);
	tr[2] = 0;
}

static void kria_off3(void* o) {
	timer_remove( &auxTimer[3]);
	clr_tr(TR4);
	tr[3] = 0;
}



void ii_kria(uint8_t *d, uint8_t l) {
	;;
}


void handler_KriaGridKey(s32 data) { 
	u8 x, y, z, index, i1, found;

	monome_grid_key_parse_event_data(data, &x, &y, &z);
	// print_dbg("\r\n monome event; x: "); 
	// print_dbg_hex(x); 
	// print_dbg("; y: 0x"); 
	// print_dbg_hex(y); 
	// print_dbg("; z: 0x"); 
	// print_dbg_hex(z);

	//// TRACK LONG PRESSES
	index = y*16 + x;
	if(z) {
		held_keys[key_count] = index;
		key_count++;
		key_times[index] = 10;		//// THRESHOLD key hold time
	} else {
		found = 0; // "found"
		for(i1 = 0; i1<key_count; i1++) {
			if(held_keys[i1] == index) 
				found++;
			if(found) 
				held_keys[i1] = held_keys[i1+1];
		}
		key_count--;

		// FAST PRESS
		if(key_times[index] > 0) {
			// PRESET MODE FAST PRESS DETECT
			if(preset_mode == 1) {
				if(x == 0) {
					if(y != preset) {
						preset = y;

						for(i1=0;i1<8;i1++)
							k.glyph[i1] = f.kria_state.k[preset].glyph[i1];

						print_dbg("\r\npreset select:");
						print_dbg_ulong(preset);
					}
 					else if(y == preset) {
 						// flash read
						flashc_memset8((void*)&(f.kria_state.preset), preset, 1, true);
						init_kria();

						preset_mode = false;
						grid_refresh = &refresh_kria;

						print_dbg("\r\npreset RECALL:");
						print_dbg_ulong(preset);
					}
				}
			}
		}
	}

	// PRESET SCREEN
	if(preset_mode) {
		// glyph magic
		if(z && x > 7) {
			k.glyph[y] ^= 1<<(x-8);

			monomeFrameDirty++;	
		}
	}
	else if(view_clock) {
		if(z) {
			if(clock_external) {
				if(y==1) {
					clock_mul = x + 1;
					monomeFrameDirty++;
				}
			}
			else {
				if(y==1)
					time_rough = x;
				else if(y==2)
					time_fine = x;
				else if(y==4) {
					int i = 0;

					switch(x) {
					case 6:
						i = -4;
						break;
					case 7:
						i = -1;
						break;
					case 8:
						i = 1;
						break;
					case 9:
						i = 4;
						break;
					default:
						break;
					}

					i += clock_period;
					if(i < 20)
						i = 20;
					if(clock_period > 265)
						clock_period = 265;
					clock_period = i;

					time_rough = (clock_period - 20) / 16;
					time_fine = (clock_period - 20) % 16;
				}

				clock_period = 20 + (time_rough * 16) + time_fine;

				clock_set(clock_period);

				// print_dbg("\r\nperiod: ");
				// print_dbg_ulong(clock_period);

				monomeFrameDirty++;
			}
		}


		// time_rough = (clock_period - 20) / 16;
		// time_fine = (clock_period - 20) % 16;
	}
	else if(view_config) {
		if(z) {
			if(x<8) {
				note_sync ^= 1;
				flashc_memset8((void*)&(f.kria_state.note_sync), note_sync, 1, true);
			}
			else if(y == 3) {
				if(loop_sync == 1)
					loop_sync = 0;
				else loop_sync = 1;

				flashc_memset8((void*)&(f.kria_state.loop_sync), loop_sync, 1, true);
			}
			else if(y == 5) {
				if(loop_sync == 2)
					loop_sync = 0;
				else loop_sync = 2;

				flashc_memset8((void*)&(f.kria_state.loop_sync), loop_sync, 1, true);
			}
			monomeFrameDirty++;
		}
	}
	// NORMAL
	else {
		// bottom row
		if(y == 7) {
			if(z) {
				switch(x) {
				case 0:
					track = 0; break;
				case 1:
					track = 1; break;
				case 2:
					track = 2; break;
				case 3:
					track = 3; break;
				case 5:
					k_mode = mTr; break;
				case 6:
					k_mode = mNote; break;
				case 7:
					k_mode = mOct; break;
				case 8:
					k_mode = mDur; break;
				case 10:
					k_mod_mode = modLoop;
					loop_count = 0;
					break;
				case 11:
					k_mod_mode = modTime; break;
				case 12:
					k_mod_mode = modProb; break;
				case 14:
					k_mode = mScale; break;
				case 15:
					k_mode = mPattern; break;
				default: break;
				}
			}
			else {
				switch(x) {
				case 10:
				case 11:
				case 12:
					k_mod_mode = modNone;
					break;
				default: break;
				}
			}

			monomeFrameDirty++;
		}
		else {
			switch(k_mode) {
			case mTr:
				switch(k_mod_mode) {
				case modNone:
					if(z) {
						k.p[k.pattern].t[y].tr[x] ^= 1;
						monomeFrameDirty++;
					}
					break;
				case modLoop:
					if(z && y<4) {
						if(loop_count == 0) {
							loop_edit = y;
							loop_first = x;
							loop_last = -1;
						}
						else {
							loop_last = x;
							update_loop_start(loop_edit, loop_first, mTr);
							update_loop_end(loop_edit, loop_last, mTr);
						}

						loop_count++;
					}
					else if(loop_edit == y) {
						loop_count--;
						
						if(loop_count == 0) {
							if(loop_last == -1) {
								if(loop_first == k.p[k.pattern].t[loop_edit].lstart[mTr]) {
									update_loop_start(loop_edit, loop_first, mTr);
									update_loop_end(loop_edit, loop_first, mTr);
								}
								else
									update_loop_start(loop_edit, loop_first, mTr);
							}
							monomeFrameDirty++;
						}
					}
					break;
				case modTime:
					if(z) {
						k.p[k.pattern].t[track].tmul[mTr] = x + 1;
						monomeFrameDirty++;
					}
					break;
				case modProb:
					if(z && y > 1 && y < 6) {
						k.p[k.pattern].t[track].p[mTr][x] = 5 - y;
						monomeFrameDirty++;
					}
					break;
				default: break;
				}

				break;
			case mNote:
				switch(k_mod_mode) {
				case modNone:
					if(z) {
						k.p[k.pattern].t[track].note[x] = 6-y;
						monomeFrameDirty++;
					}
					break;
				case modLoop:
					if(z) {
						if(loop_count == 0) {
							loop_first = x;
							loop_last = -1;
						}
						else {
							loop_last = x;
							update_loop_start(track, loop_first, mNote);
							update_loop_end(track, loop_last, mNote);
						}

						loop_count++;
					}
					else {
						loop_count--;

						if(loop_count == 0) {
							if(loop_last == -1) {
								if(loop_first == k.p[k.pattern].t[track].lstart[mNote]) {
									update_loop_start(track, loop_first, mNote);
									update_loop_end(track, loop_first, mNote);
								}
								else
									update_loop_start(track, loop_first, mNote);
							}
							monomeFrameDirty++;
						}
					}
					break;
				case modTime:
					if(z) {
						k.p[k.pattern].t[track].tmul[mNote] = x + 1;
						monomeFrameDirty++;
					}
					break;
				case modProb:
					if(z && y > 1 && y < 6) {
						k.p[k.pattern].t[track].p[mNote][x] = 5 - y;
						monomeFrameDirty++;
					}
					break;
				default: break;
				}
				break;
			case mOct:
				switch(k_mod_mode) {
				case modNone:
					if(z) {
						if(y>2)
							k.p[k.pattern].t[track].oct[x] = 6-y;
						monomeFrameDirty++;
					}
					break;
				case modLoop:
					if(z) {
						if(loop_count == 0) {
							loop_first = x;
							loop_last = -1;
						}
						else {
							loop_last = x;
							update_loop_start(track, loop_first, mOct);
							update_loop_end(track, loop_last, mOct);
						}

						loop_count++;
					}
					else {
						loop_count--;
						
						if(loop_count == 0) {
							if(loop_last == -1) {
								if(loop_first == k.p[k.pattern].t[track].lstart[mOct]) {
									update_loop_start(track, loop_first, mOct);
									update_loop_end(track, loop_first, mOct);
								}
								else
									update_loop_start(track, loop_first, mOct);
							}
							monomeFrameDirty++;
						}
					}
					break;
				case modTime:
					if(z) {
						k.p[k.pattern].t[track].tmul[mOct] = x + 1;
						monomeFrameDirty++;
					}
					break;
				case modProb:
					if(z && y > 1 && y < 6) {
						k.p[k.pattern].t[track].p[mOct][x] = 5 - y;
						monomeFrameDirty++;
					}
					break;
				default: break;
				}
				break;
			case mDur:
				switch(k_mod_mode) {
				case modNone:
					if(z) {
						if(y==0)
							k.p[k.pattern].t[track].dur_mul = x+1;
						else
							k.p[k.pattern].t[track].dur[x] = y-1;
						monomeFrameDirty++;
					}
					break;
				case modLoop:
					if(y>0) {
						if(z) {
							if(loop_count == 0) {
								loop_first = x;
								loop_last = -1;
							}
							else {
								loop_last = x;
								update_loop_start(track, loop_first, mDur);
								update_loop_end(track, loop_last, mDur);
							}

							loop_count++;
						}
						else {
							loop_count--;
							
							if(loop_count == 0) {
								if(loop_last == -1) {
									if(loop_first == k.p[k.pattern].t[track].lstart[mDur]) {
										update_loop_start(track, loop_first, mDur);
										update_loop_end(track, loop_first, mDur);
									}
									else
										update_loop_start(track, loop_first, mDur);
								}
								monomeFrameDirty++;
							}
						}
					}
					break;
				case modTime:
					if(z) {
						k.p[k.pattern].t[track].tmul[mDur] = x + 1;
						monomeFrameDirty++;
					}
					break;
				case modProb:
					if(z && y > 1 && y < 6) {
						k.p[k.pattern].t[track].p[mDur][x] = 5 - y;
						monomeFrameDirty++;
					}
					break;
				default: break;
				}
				break;
			case mScale:
				if(z) {
					if(x < 8) {
						k.p[k.pattern].scale = (y - 5) * 8 + x;
					}
					else {
						scale_data[k.p[k.pattern].scale][6-y] = x-8;
					}

					calc_scale(k.p[k.pattern].scale);

					monomeFrameDirty++;
				}
				break;
			case mPattern:
				if(z && y ==0) {
					k.pattern = x;
					pos_reset = true;
				}
				break;

			default: break;
			}
		}

	}
}

static void adjust_loop_start(u8 t, u8 x, u8 m) {
	s8 temp;

	temp = pos[t][m] - k.p[k.pattern].t[t].lstart[m] + x;
	if(temp < 0) temp += 16;
	else if(temp > 15) temp -= 16;
	pos[t][m] = temp;

	k.p[k.pattern].t[t].lstart[m] = x;
	temp = x + k.p[k.pattern].t[t].llen[m]-1;
	if(temp > 15) {
		k.p[k.pattern].t[t].lend[m] = temp - 16;
		k.p[k.pattern].t[t].lswap[m] = 1;
	}
	else {
		k.p[k.pattern].t[t].lend[m] = temp;
		k.p[k.pattern].t[t].lswap[m] = 0;
	}
}

static void adjust_loop_end(u8 t, u8 x, u8 m) {
	s8 temp;

	k.p[k.pattern].t[t].lend[m] = x;
	temp = k.p[k.pattern].t[t].lend[m] - k.p[k.pattern].t[t].lstart[m];
	if(temp < 0) {
		k.p[k.pattern].t[t].llen[m] = temp + 17;
		k.p[k.pattern].t[t].lswap[m] = 1;
	}
	else {
		k.p[k.pattern].t[t].llen[m] = temp+1;
		k.p[k.pattern].t[t].lswap[m] = 0;
	}

	temp = pos[t][m];
	if(k.p[k.pattern].t[t].lswap[m]) {
		if(temp < k.p[k.pattern].t[t].lstart[m] && temp > k.p[k.pattern].t[t].lend[m])
			pos[t][m] = k.p[k.pattern].t[t].lstart[m];
	}
	else {
		if(temp < k.p[k.pattern].t[t].lstart[m] || temp > k.p[k.pattern].t[t].lend[m])
			pos[t][m] = k.p[k.pattern].t[t].lstart[m];
	}
}

static void update_loop_start(u8 t, u8 x, u8 m) {
	int i1, i2;
	switch(loop_sync) {
		case 1:
			for(i1=0;i1<KRIA_NUM_PARAMS;i1++)
				adjust_loop_start(t,x,i1);
			break;
		case 2:
			for(i2=0;i2<4;i2++)
			for(i1=0;i1<KRIA_NUM_PARAMS;i1++)
				adjust_loop_start(i2,x,i1);
			break;
		case 0:
			break;
		default:
			break;
	}
}

static void update_loop_end(u8 t, u8 x, u8 m) {
	int i1, i2;
	switch(loop_sync) {
		case 1:
			for(i1=0;i1<KRIA_NUM_PARAMS;i1++)
				adjust_loop_end(t,x,i1);
			break;
		case 2:
			for(i2=0;i2<4;i2++)
			for(i1=0;i1<KRIA_NUM_PARAMS;i1++)
				adjust_loop_end(i2,x,i1);
			break;
		case 0:
			break;
		default:
			break;
	}
}

void handler_KriaRefresh(s32 data) { 
	if(monomeFrameDirty) {
		grid_refresh();

		monome_set_quadrant_flag(0);
		monome_set_quadrant_flag(1);
		(*monome_refresh)();
	}
}

void handler_KriaKey(s32 data) { 
	// print_dbg("\r\n> kria key");
	// print_dbg_ulong(data);

	switch(data) {
	case 0:
		grid_refresh = &refresh_kria;
		view_clock = false;
		break;
	case 1:
		grid_refresh = &refresh_clock;
		print_dbg("\r\ntime: ");
		print_dbg_ulong(time_fine);
		print_dbg(" ");
		print_dbg_ulong(time_rough);
		view_clock = true;
		view_config = false;
		break;
	case 2:
		grid_refresh = &refresh_kria;
		view_config = false;
		break;
	case 3:
		grid_refresh = &refresh_kria_config;
		view_config = true;
		view_clock = false;
		break;
	default:
		break;
	}

	monomeFrameDirty++;
}

void handler_KriaTr(s32 data) { 
	// print_dbg("\r\n> kria tr ");
	// print_dbg_ulong(data);

	switch(data) {
	case 0:
		if(clock_mul == 1)
			clock_kria(0);
		break;
	case 1:
		if(clock_mul == 1)
			clock_kria(1);
		else {
			ext_clock_count++;
			if(ext_clock_count >= clock_mul - 1) {
				ext_clock_count = 0;
				ext_clock_phase ^= 1;
				clock_kria(ext_clock_phase);
			}
		}
		break;
	case 3:
		pos_reset = true;
		break;
	default:
		break;
	}

	monomeFrameDirty++;
}

void handler_KriaTrNormal(s32 data) { 
	// print_dbg("\r\n> kria tr normal ");
	// print_dbg_ulong(data);

	clock_external = data;

	if(clock_external)
		clock = &clock_null;
	else
		clock = &clock_kria;

	monomeFrameDirty++;
}

void refresh_kria(void) {
	u8 i1,i2;

	memset(monomeLedBuffer,0,128);

	// bottom strip

	memset(monomeLedBuffer + R7, L0, 4);
	memset(monomeLedBuffer + R7 + 5, L0, 4);
	monomeLedBuffer[R7 + 10] = L0;
	monomeLedBuffer[R7 + 11] = L0;
	monomeLedBuffer[R7 + 12] = L0;
	monomeLedBuffer[R7 + 14] = L0;
	monomeLedBuffer[R7 + 15] = L0;

	monomeLedBuffer[112+track] = L2;

	switch(k_mode) {
	case mTr:
		i1 = 5; break;
	case mNote:
		i1 = 6; break;
	case mOct:
		i1 = 7; break;
	case mDur:
		i1 = 8; break;
	case mScale:
		i1 = 14; break;
	case mPattern:
		i1 = 15; break;
	default:
		i1 = 0; break;
	}

	monomeLedBuffer[R7 + i1] = L2;

	if(k_mod_mode == modLoop)
		monomeLedBuffer[R7 + 10] = L1;
	else if(k_mod_mode == modTime)
		monomeLedBuffer[R7 + 11] = L1;
	else if(k_mod_mode == modProb)
		monomeLedBuffer[R7 + 12] = L1;



	// modes

	switch(k_mode) {
	case mTr:
		switch(k_mod_mode) {
		case modTime:
			memset(monomeLedBuffer + R1, 3, 16);
			monomeLedBuffer[R1 + k.p[k.pattern].t[track].tmul[mTr] - 1] = L1;
			break;			
		case modProb:
			memset(monomeLedBuffer + R5, 3, 16);
			for(i1=0;i1<16;i1++)
				if(k.p[k.pattern].t[track].p[mTr][i1])
					monomeLedBuffer[(5 - k.p[k.pattern].t[track].p[mTr][i1]) * 16 + i1] = 6;
			break;
		default:
			// steps
			for(i2=0;i2<4;i2++) {
				for(i1=0;i1<16;i1++) {
					if(k.p[k.pattern].t[i2].tr[i1])
						monomeLedBuffer[i2*16 + i1] = 3;
				}
				// playhead
				// if(tr[i2])
				monomeLedBuffer[i2*16 + pos[i2][mTr]] += 4;
			}

			// loop highlight
			for(i1=0;i1<4;i1++) {
				if(k.p[k.pattern].t[i1].lswap[mTr]) {
					for(i2=0;i2<k.p[k.pattern].t[i1].llen[mTr];i2++)
						monomeLedBuffer[i1*16 + (i2+k.p[k.pattern].t[i1].lstart[mTr])%16] += 2 + (k_mod_mode == modLoop);
				}
				else {
					for(i2=k.p[k.pattern].t[i1].lstart[mTr];i2<=k.p[k.pattern].t[i1].lend[mTr];i2++)
						monomeLedBuffer[i1*16 + i2] += 2 + (k_mod_mode == modLoop);
				}
			}
			break;
		}
		break;
	case mNote:
		switch(k_mod_mode) {
		case modTime:
			memset(monomeLedBuffer + R1, 3, 16);
			monomeLedBuffer[R1 + k.p[k.pattern].t[track].tmul[mNote] - 1] = L1;
			break;			
		case modProb:
			memset(monomeLedBuffer + R5, 3, 16);
			for(i1=0;i1<16;i1++)
				if(k.p[k.pattern].t[track].p[mNote][i1])
					monomeLedBuffer[(5 - k.p[k.pattern].t[track].p[mNote][i1]) * 16 + i1] = 6;
			break;
		default:
			for(i1=0;i1<16;i1++)
				monomeLedBuffer[i1 + (6 - k.p[k.pattern].t[track].note[i1] ) * 16] = 3;

			monomeLedBuffer[pos[track][mNote] + (6-k.p[k.pattern].t[track].note[pos[track][mNote]])*16] += 4;

			if(k.p[k.pattern].t[track].lswap[mNote]) {
				for(i1=0;i1<k.p[k.pattern].t[track].llen[mNote];i1++)
					monomeLedBuffer[((i1+k.p[k.pattern].t[track].lstart[mNote])%16)+
						(6-k.p[k.pattern].t[track].note[i1])*16] += 3 + (k_mod_mode == modLoop)*2;
					// monomeLedBuffer[i1*16 + (i2+k.p[k.pattern].t[i1].lstart[mTr])%16] += 2 + (k_mod_mode == modLoop);
			}
			else {
				for(i1=k.p[k.pattern].t[track].lstart[mNote];i1<=k.p[k.pattern].t[track].lend[mNote];i1++)
					monomeLedBuffer[i1+(6-k.p[k.pattern].t[track].note[i1])*16] += 3 + (k_mod_mode == modLoop)*2;
			}
			break;
		}
		break;
	case mOct:
		switch(k_mod_mode) {
		case modTime:
			memset(monomeLedBuffer + R1, 3, 16);
			monomeLedBuffer[R1 + k.p[k.pattern].t[track].tmul[mOct] - 1] = L1;
			break;			
		case modProb:
			memset(monomeLedBuffer + R5, 3, 16);
			for(i1=0;i1<16;i1++)
				if(k.p[k.pattern].t[track].p[mOct][i1])
					monomeLedBuffer[(5 - k.p[k.pattern].t[track].p[mOct][i1]) * 16 + i1] = 6;
			break;
		default:
			for(i1=0;i1<16;i1++) {
					for(i2=0;i2<=k.p[k.pattern].t[track].oct[i1];i2++)
						monomeLedBuffer[R6-16*i2+i1] = L0;

					if(i1 == pos[track][mOct])
						monomeLedBuffer[R6 - k.p[k.pattern].t[track].oct[i1]*16 + i1] += 4;
				}

			if(k.p[k.pattern].t[track].lswap[mOct]) {
				for(i1=0;i1<16;i1++)
					if((i1 < k.p[k.pattern].t[track].lstart[mOct]) && (i1 > k.p[k.pattern].t[track].lend[mOct]))
						for(i2=0;i2<=k.p[k.pattern].t[track].oct[i1];i2++)
							monomeLedBuffer[R6-16*i2+i1] -= 2;
			}
			else {
				for(i1=0;i1<16;i1++)
					if((i1 < k.p[k.pattern].t[track].lstart[mOct]) || (i1 > k.p[k.pattern].t[track].lend[mOct]))
						for(i2=0;i2<=k.p[k.pattern].t[track].oct[i1];i2++)
							monomeLedBuffer[R6-16*i2+i1] -= 2;
			}
			break;
		}
		break;
	case mDur:
		switch(k_mod_mode) {
		case modTime:
			memset(monomeLedBuffer + R1, 3, 16);
			monomeLedBuffer[R1 + k.p[k.pattern].t[track].tmul[mDur] - 1] = L1;
			break;			
		case modProb:
			memset(monomeLedBuffer + R5, 3, 16);
			for(i1=0;i1<16;i1++)
				if(k.p[k.pattern].t[track].p[mDur][i1])
					monomeLedBuffer[(5 - k.p[k.pattern].t[track].p[mDur][i1]) * 16 + i1] = 6;
			break;
		default:
			monomeLedBuffer[k.p[k.pattern].t[track].dur_mul - 1] = L1;

			for(i1=0;i1<16;i1++) {
				for(i2=0;i2<=k.p[k.pattern].t[track].dur[i1];i2++)
					monomeLedBuffer[R1+16*i2+i1] = L0;

				if(i1 == pos[track][mDur])
					monomeLedBuffer[R1+i1+16*k.p[k.pattern].t[track].dur[i1]] += 4;
			}

			if(k.p[k.pattern].t[track].lswap[mDur]) {
				for(i1=0;i1<16;i1++)
					if((i1 < k.p[k.pattern].t[track].lstart[mDur]) && (i1 > k.p[k.pattern].t[track].lend[mDur]))
						for(i2=0;i2<=k.p[k.pattern].t[track].dur[i1];i2++)
							monomeLedBuffer[R1+16*i2+i1] -= 2;
			}
			else {
				for(i1=0;i1<16;i1++)
					if((i1 < k.p[k.pattern].t[track].lstart[mDur]) || (i1 > k.p[k.pattern].t[track].lend[mDur]))
						for(i2=0;i2<=k.p[k.pattern].t[track].dur[i1];i2++)
							monomeLedBuffer[R1+16*i2+i1] -= 2;
			}
			break;
		}
		break;
	case mScale:
		for(i1=0;i1<7;i1++)
			monomeLedBuffer[8+16*i1] = L0;
		for(i1=0;i1<8;i1++) {
			monomeLedBuffer[R5 + i1] = 2;
			monomeLedBuffer[R6 + i1] = 2;
		}
		monomeLedBuffer[R5 + (k.p[k.pattern].scale >> 3) * 16 + (k.p[k.pattern].scale & 0x7)] = L1;

		for(i1=0;i1<7;i1++)
			monomeLedBuffer[scale_data[k.p[k.pattern].scale][i1] + 8 + (6-i1)*16] = L1;

		for(i1=0;i1<4;i1++) {
			if(k.p[k.pattern].t[i1].tr[pos[i1][mTr]])
				monomeLedBuffer[scale_data[k.p[k.pattern].scale][note[i1]] + 8 + (6-note[i1])*16]++;
		}
		break;
	case mPattern:
		memset(monomeLedBuffer, 3, 16);
		for(i1=0;i1<16;i1++)
			monomeLedBuffer[k.pattern] = L1;
		break;
	default: break;
	}
}

void refresh_kria_config(void) {
	// clear grid
	memset(monomeLedBuffer,0,128);

	uint8_t i = note_sync * 4 + 3;

	monomeLedBuffer[R2 + 2] = i;
	monomeLedBuffer[R2 + 3] = i;
	monomeLedBuffer[R2 + 4] = i;
	monomeLedBuffer[R2 + 5] = i;
	monomeLedBuffer[R3 + 2] = i;
	monomeLedBuffer[R3 + 5] = i;
	monomeLedBuffer[R4 + 2] = i;
	monomeLedBuffer[R4 + 5] = i;
	monomeLedBuffer[R5 + 2] = i;
	monomeLedBuffer[R5 + 3] = i;
	monomeLedBuffer[R5 + 4] = i;
	monomeLedBuffer[R5 + 5] = i;

	i = (loop_sync == 1) * 4 + 3;

	monomeLedBuffer[R3 + 10] = i;

	i = (loop_sync == 2) * 4 + 3;

	monomeLedBuffer[R5 + 10] = i;
	monomeLedBuffer[R5 + 11] = i;
	monomeLedBuffer[R5 + 12] = i;
	monomeLedBuffer[R5 + 13] = i;
}

////////////////////////////////////////////////////////////////////////////////
// MP

#define MP_1V 0
#define MP_2V 1
#define MP_4V 2
#define MP_8T 3


u8 edit_row;
u8 mode = 0;
u8 prev_mode = 0;
s8 kcount = 0;
s8 scount[8];
u8 state[8];
u8 pstate[8];
u8 clear[8]; 
s8 position[8];		// current position in cycle
u8 tick[8]; 		// position in speed countdown
u8 pushed[8];		// manual key reset

s8 note_now[4];
u16 note_age[4];


const u8 sign[8][8] = {{0,0,0,0,0,0,0,0},       // o
       {0,24,24,126,126,24,24,0},     			// +
       {0,0,0,126,126,0,0,0},       			// -
       {0,96,96,126,126,96,96,0},     			// >
       {0,6,6,126,126,6,6,0},       			// <
       {0,102,102,24,24,102,102,0},   			// * rnd
       {0,120,120,102,102,30,30,0},   			// <> up/down
       {0,126,126,102,102,126,126,0}};  		// [] sync

uint8_t get_note_slot(uint8_t v);
void mp_note_on(uint8_t n);
void mp_note_off(uint8_t n);


void default_mp() {
	uint8_t i1, i2;

	flashc_memset32((void*)&(f.mp_state.clock_period), 55, 4, true);
	flashc_memset8((void*)&(f.mp_state.preset), 0, 1, true);
	flashc_memset8((void*)&(f.mp_state.sound), 0, 1, true);
	flashc_memset8((void*)&(f.mp_state.voice_mode), 0, 1, true);

	for(i1=0;i1<8;i1++) {
		m.count[i1] = 7+i1;
		m.speed[i1] = 0;
		m.min[i1] = 7+i1;
		m.max[i1] = 7+i1;
		m.trigger[i1] = (1<<i1);
		m.toggle[i1] = 0;
		m.rules[i1] = 1;
		m.rule_dests[i1] = i1;
		m.sync[i1] = (1<<i1);
		m.rule_dest_targets[i1] = 3;
		m.smin[i1] = 0;
		m.smax[i1] = 0;
	}

	for(i1=0;i1<8;i1++)
		m.glyph[i1] = 0;

	for(i1=0;i1<GRID_PRESETS;i1++)
		flashc_memcpy((void *)&f.mp_state.m[i1], &m, sizeof(m), true);

	// default scales
	for(i1=0;i1<7;i1++) {
		flashc_memset8((void*)&(f.scale[i1][0]), 0, 1, true);
		for(i2=0;i2<7;i2++)
			flashc_memset8((void*)&(f.scale[i1][i2+1]), SCALE_INT[i1][i2], 1, true);
	}
	for(i1=7;i1<16;i1++) {
		flashc_memset8((void*)&(f.scale[i1][0]), 0, 1, true);
		for(i2=0;i2<7;i2++)
			flashc_memset8((void*)&(f.scale[i1][i2+1]), 1, 1, true);
	}
}

void init_mp() {
	sound = f.mp_state.sound;
	voice_mode = f.mp_state.voice_mode;

	preset = f.mp_state.preset;

	for(uint8_t i1=0;i1<8;i1++) {
		m = f.mp_state.m[preset];

		// m.count[i1] = f.mp_state.m[preset].count[i1];
		// m.speed[i1] = f.mp_state.m[preset].speed[i1];
		// m.min[i1] = f.mp_state.m[preset].min[i1];
		// m.max[i1] = f.mp_state.m[preset].max[i1];
		// m.trigger[i1] = f.mp_state.m[preset].trigger[i1];
		// m.toggle[i1] = f.mp_state.m[preset].toggle[i1];
		// m.rules[i1] = f.mp_state.m[preset].rules[i1];
		// m.rule_dests[i1] = f.mp_state.m[preset].rule_dests[i1];
		// m.sync[i1] = f.mp_state.m[preset].sync[i1];
		// m.rule_dest_targets[i1] = f.mp_state.m[preset].rule_dest_targets[i1];
		// m.smin[i1] = f.mp_state.m[preset].smin[i1];
		// m.smax[i1] = f.mp_state.m[preset].smax[i1];

		position[i1] = f.mp_state.m[preset].count[i1];
		tick[i1] = 0;
		pushed[i1] = 0;
		scount[i1] = 0;
		clear[i1] = 0;
		state[i1] = 0;
	}

	// for(uint8_t i1=0;i1<8;i1++)
		// m.glyph[i1] = f.mp_state.m[preset].glyph[i1];

	m.scale = f.mp_state.m[preset].scale;

	memcpy(scale_data, f.scale, sizeof(scale_data));

	calc_scale(m.scale);

	note_now[0] = -1;
	note_now[1] = -1;
	note_now[2] = -1;
	note_now[3] = -1;
}

void resume_mp() {
	grid_refresh = &refresh_mp;
	view_clock = false;
	view_config = false;
	preset_mode = false;

	preset = f.mp_state.preset;

	// re-check clock jack
	clock_external = !gpio_get_pin_value(B10);

	if(clock_external)
		clock = &clock_null;
	else
		clock = &clock_mp;

	dac_set_slew(0,0);
	dac_set_slew(1,0);
	dac_set_slew(2,0);
	dac_set_slew(3,0);

	clr_tr(TR1);
	clr_tr(TR2);
	clr_tr(TR3);
	clr_tr(TR4);
}

void clock_mp(uint8_t phase) {
	static u8 i;

	if(phase) {
		clock_count++;

		memcpy(pstate, state, 8);
		// gpio_set_gpio_pin(B10);

		for(i=0;i<8;i++) {
			if(pushed[i]) {
				for(int n=0;n<8;n++) {
					if(m.sync[i] & (1<<n)) {
						position[n] = m.count[n];
						tick[n] = m.speed[n];
					}

					if(m.trigger[i] & (1<<n)) {
						state[n] = 1;
						clear[n] = 1;
					}
					else if(m.toggle[i] & (1<<n)) {
						state[n] ^= 1;
					}
				}

				pushed[i] = 0;
			}

			if(tick[i] == 0) {
				tick[i] = m.speed[i];
				if(position[i] == 0) {
					// RULES
				    if(m.rules[i] == 1) {     // inc
				    	if(m.rule_dest_targets[i] & 1) {
					    	m.count[m.rule_dests[i]]++;
					    	if(m.count[m.rule_dests[i]] > m.max[m.rule_dests[i]]) {
					    		m.count[m.rule_dests[i]] = m.min[m.rule_dests[i]];
					    	}
					    }
					    if(m.rule_dest_targets[i] & 2) {
					    	m.speed[m.rule_dests[i]]++;
					    	if(m.speed[m.rule_dests[i]] > m.smax[m.rule_dests[i]]) {
					    		m.speed[m.rule_dests[i]] = m.smin[m.rule_dests[i]];
					    	}
					    }
				    }
				    else if(m.rules[i] == 2) {  // dec
				    	if(m.rule_dest_targets[i] & 1) {
				    		m.count[m.rule_dests[i]]--;
					    	if(m.count[m.rule_dests[i]] < m.min[m.rule_dests[i]]) {
					    		m.count[m.rule_dests[i]] = m.max[m.rule_dests[i]];
					    	}
					    }
					    if(m.rule_dest_targets[i] & 2) {
					    	m.speed[m.rule_dests[i]]--;
					    	if(m.speed[m.rule_dests[i]] < m.smin[m.rule_dests[i]]) {
					    		m.speed[m.rule_dests[i]] = m.smax[m.rule_dests[i]];
					    	}
					    }
				    }
				    else if(m.rules[i] == 3) {  // max
				    	if(m.rule_dest_targets[i] & 1)
				    		m.count[m.rule_dests[i]] = m.max[m.rule_dests[i]];
				    	if(m.rule_dest_targets[i] & 2)
				    		m.speed[m.rule_dests[i]] = m.smax[m.rule_dests[i]];
				    }
				    else if(m.rules[i] == 4) {  // min
				    	if(m.rule_dest_targets[i] & 1)
					    	m.count[m.rule_dests[i]] = m.min[m.rule_dests[i]];
					    if(m.rule_dest_targets[i] & 2)
					    	m.speed[m.rule_dests[i]] = m.smin[m.rule_dests[i]];
				    }
				    else if(m.rules[i] == 5) {  // rnd
				    	if(m.rule_dest_targets[i] & 1)
				    		m.count[m.rule_dests[i]] = 
				    			(rnd() % (m.max[m.rule_dests[i]] - m.min[m.rule_dests[i]] + 1)) + m.min[m.rule_dests[i]];
				    	if(m.rule_dest_targets[i] & 2)
				    		m.speed[m.rule_dests[i]] = 
				    			(rnd() % (m.smax[m.rule_dests[i]] - m.smin[m.rule_dests[i]] + 1)) + m.smin[m.rule_dests[i]];
				    					
				      // print_dbg("\r\n RANDOM: ");
				      // print_dbg_hex(m.count[m.rule_dests[i]]);
				      // print_dbg_hex(rnd() % 11);
				    }
				    else if(m.rules[i] == 6) {  // pole
				    	if(m.rule_dest_targets[i] & 1) {
					    	if(abs(m.count[m.rule_dests[i]] - m.min[m.rule_dests[i]]) < 
					    		abs(m.count[m.rule_dests[i]] - m.max[m.rule_dests[i]]) ) {
					    		m.count[m.rule_dests[i]] = m.max[m.rule_dests[i]];
					    	}
					    	else {
					    		m.count[m.rule_dests[i]] = m.min[m.rule_dests[i]];
					    	}
					    }
					    if(m.rule_dest_targets[i] & 2) {
					    	if(abs(m.speed[m.rule_dests[i]] - m.smin[m.rule_dests[i]]) < 
					    		abs(m.speed[m.rule_dests[i]] - m.smax[m.rule_dests[i]]) ) {
					    		m.speed[m.rule_dests[i]] = m.smax[m.rule_dests[i]];
					    	}
					    	else {
					    		m.speed[m.rule_dests[i]] = m.smin[m.rule_dests[i]];
					    	}
					    }
				    }
				    else if(m.rules[i] == 7) {  // stop
				    	if(m.rule_dest_targets[i] & 1)
				    		position[m.rule_dests[i]] = -1;
				    }

					position[i]--;

					for(int n=0;n<8;n++) {
						if(m.sync[i] & (1<<n)) {
							position[n] = m.count[n];
							tick[n] = m.speed[n];
						}

						if(m.trigger[i] & (1<<n)) {
							state[n] = 1;
							clear[n] = 1;
						}
						else if(m.toggle[i] & (1<<n)) {
							state[n] ^= 1;
						}
					}
				}
				else if(position[i] > 0) position[i]--;
			}
			else tick[i]--;
		}

		for(i=0;i<8;i++)
			if(state[i] && !pstate[i])
				mp_note_on(i);
				// gpio_set_gpio_pin(outs[i]);
			else if(!state[i] && pstate[i])
				mp_note_off(i);
				// gpio_clr_gpio_pin(outs[i]);

		monomeFrameDirty++;
	}
	else {
		// gpio_clr_gpio_pin(B10);

		for(i=0;i<8;i++) {
			if(clear[i]) {
				mp_note_off(i);
				// gpio_clr_gpio_pin(outs[i]);
				state[i] = 0;
			}
			clear[i] = 0;
		}
 	}
}

uint8_t get_note_slot(uint8_t v) {
	int8_t w = -1;

	for(int i1=0;i1<v;i1++)
		note_age[i1]++;

	// find empty
	for(int i1=0;i1<v;i1++)
		if(note_now[i1] == -1) {
			w = i1;
			break;
		}

	if(w == -1) {
		w = 0;
		for(int i1=1;i1<v;i1++)
			if(note_age[w] < note_age[i1])
				w = i1;
	}

	note_age[w] = 1;

	return w;
}

void mp_note_on(uint8_t n) {
	uint8_t w;
	// print_dbg("\r\nmp note on: ");
	// print_dbg_ulong(n);
	switch(voice_mode) {
	case MP_8T:
			if(n < 4)
				set_tr(TR1 + n);
			else
				dac_set_value(n-4, DAC_10V);
		break;
	case MP_1V:
		note_now[0] = n;
		dac_set_value(0, ET[cur_scale[7-n]] << 2);
		set_tr(TR1);
		break;
	case MP_2V:
		w = get_note_slot(2);
		note_now[w] = n;
		dac_set_value(w, ET[cur_scale[7-n]] << 2);
		set_tr(TR1 + w);
		break;
	case MP_4V:
		w = get_note_slot(4);
		note_now[w] = n;
		dac_set_value(w, ET[cur_scale[7-n]] << 2);
		set_tr(TR1 + w);
		break;
	default:
		break;
	}
}

void mp_note_off(uint8_t n) {
	// print_dbg("\r\nmp note off: ");
	// print_dbg_ulong(n);
	switch(voice_mode) {
	case MP_8T:
			if(n < 4)
				clr_tr(TR1 + n);
			else
				dac_set_value(n-4, 0);
		break;
	case MP_1V:
		if(note_now[0] == n) {
			note_now[0] = -1;
			clr_tr(TR1);
		}
		break;
	case MP_2V:
		for(int i1=0;i1<2;i1++) {
			if(note_now[i1] == n) {
				note_now[i1] = -1;
				clr_tr(TR1 + i1);
			}
		}
		break;
	case MP_4V:
		for(int i1=0;i1<4;i1++) {
			if(note_now[i1] == n) {
				note_now[i1] = -1;
				clr_tr(TR1 + i1);
			}
		}
		break;
	default:
		break;
	}
}

void ii_mp(uint8_t *d, uint8_t l) {
	;;
}

void handler_MPGridKey(s32 data) { 
 	u8 x, y, z, index, i1, found;
	monome_grid_key_parse_event_data(data, &x, &y, &z);
	// print_dbg("\r\n monome event; x: "); 
	// print_dbg_hex(x); 
	// print_dbg("; y: 0x"); 
	// print_dbg_hex(y); 
	// print_dbg("; z: 0x"); 
	// print_dbg_hex(z);

	//// TRACK LONG PRESSES
	index = y*16 + x;
	if(z) {
		held_keys[key_count] = index;
		key_count++;
		key_times[index] = 10;		//// THRESHOLD key hold time
	} else {
		found = 0; // "found"
		for(i1 = 0; i1<key_count; i1++) {
			if(held_keys[i1] == index) 
				found++;
			if(found) 
				held_keys[i1] = held_keys[i1+1];
		}
		key_count--;

		// FAST PRESS
		if(key_times[index] > 0) {
			if(preset_mode) {
				if(x == 0) {
					if(y != preset) {
						preset = y;

						for(i1=0;i1<8;i1++)
							m.glyph[i1] = f.mp_state.m[preset].glyph[i1];

						print_dbg("\r\npreset select:");
						print_dbg_ulong(preset);
					}
 					else if(y == preset) {
 						// flash read
						flashc_memset8((void*)&(f.mp_state.preset), preset, 1, true);
						init_mp();

						preset_mode = false;
						grid_refresh = &refresh_mp;

						print_dbg("\r\npreset RECALL:");
						print_dbg_ulong(preset);
					}
				}

				monomeFrameDirty++;	
			}
			// print_dbg("\r\nfast press: ");
			// print_dbg_ulong(index);
			// print_dbg(": ");
			// print_dbg_ulong(key_times[index]);
		}
	}

	// PRESET SCREEN
	if(preset_mode) {
		// draw glyph
		if(z && x>7)
			m.glyph[y] ^= 1<<(x-8);

		monomeFrameDirty++;	
	}
	else if(view_clock) {
		if(z) {
			if(clock_external) {
				if(y==1) {
					clock_mul = x + 1;
					monomeFrameDirty++;
				}
			}
			else {
				if(y==1)
					time_rough = x;
				else if(y==2)
					time_fine = x;
				else if(y==4) {
					int i = 0;

					switch(x) {
					case 6:
						i = -4;
						break;
					case 7:
						i = -1;
						break;
					case 8:
						i = 1;
						break;
					case 9:
						i = 4;
						break;
					default:
						break;
					}

					i += clock_period;
					if(i < 20)
						i = 20;
					if(clock_period > 265)
						clock_period = 265;
					clock_period = i;

					time_rough = (clock_period - 20) / 16;
					time_fine = (clock_period - 20) % 16;
				}

				clock_period = 20 + (time_rough * 16) + time_fine;

				clock_set(clock_period);

				// print_dbg("\r\nperiod: ");
				// print_dbg_ulong(clock_period);

				monomeFrameDirty++;
			}
		}


		// time_rough = (clock_period - 20) / 16;
		// time_fine = (clock_period - 20) % 16;

	}
	else if(view_config) {
		if(z) {
			if(y < 6 && x < 8) {
				switch(x) {
				case 0:
				case 1:
				case 2:
				case 3:
					if(voice_mode == MP_8T)
						sound ^= 1;
					voice_mode = MP_8T;
					break;
				case 4:
					if(voice_mode == MP_4V)
						sound ^= 1;
					voice_mode = MP_4V;
					break;
				case 5:
					if(voice_mode == MP_2V)
						sound ^= 1;
					voice_mode = MP_2V;
					break;
				case 6:
				case 7:
					if(voice_mode == MP_1V)
						sound ^= 1;
					voice_mode = MP_1V;
					break;
				default:
					break;
				}
			}
			else if(voice_mode != MP_8T) {
				if(x < 8) {
					m.scale = (y - 6) * 8 + x;
				}
				else {
					scale_data[m.scale][7-y] = x-8;
				}

				calc_scale(m.scale);
			}

			monomeFrameDirty++;
		}
	}
	// NORMAL
	else {
		prev_mode = mode;

		// mode check
		if(x == 0) {
			kcount += (z<<1)-1;

			if(kcount < 0)
				kcount = 0;

			// print_dbg("\r\nkey count: ");
			// print_dbg_ulong(kcount);

			if(kcount == 1 && z == 1)
				mode = 1; 
			else if(kcount == 0) {
				mode = 0;
				scount[y] = 0;	
			}

			if(z == 1 && mode == 1) {
				edit_row = y;
			}
		}
		else if(x == 1 && mode != 0) {
			if(mode == 1 && z == 1) {
				mode = 2;
				edit_row = y;
			}
			else if(mode == 2 && z == 0)
				mode = 1;
		}
		// set position / minmax / stop
		else if(mode == 0) {
			scount[y] += (z<<1)-1;
			if(scount[y]<0) scount[y] = 0;		// in case of grid glitch?

			if(z == 1 && scount[y] == 1) {
				position[y] = x;
				m.count[y] = x;
				m.min[y] = x;
				m.max[y] = x;
				tick[y] = m.speed[y];

				if(sound) {
					pushed[y] = 1;
				}
			}
			else if(z == 1 && scount[y] == 2) {
				if(x < m.count[y]) {
					m.min[y] = x;
					m.max[y] = m.count[y];
				}
				else {
					m.max[y] = x;
					m.min[y] = m.count[y];
				}
			}
		}
		// set speeds and trig/tog
		else if(mode == 1) {
			scount[y] += (z<<1)-1;
			if(scount[y]<0) scount[y] = 0;

			if(z==1) {
				if(x > 7) {
					if(scount[y] == 1) {
						m.smin[y] = x-8;
						m.smax[y] = x-8;
						m.speed[y] = x-8;
						tick[y] = m.speed[y];
					}
					else if(scount[y] == 2) {
						if(x-8 < m.smin[y]) {
							m.smax[y] = m.smin[y];
							m.smin[y] = x-8;
						}
						else
							m.smax[y] = x-8;
					}
				}
				else if(x == 5) {
					m.toggle[edit_row] ^= 1<<y;
					m.trigger[edit_row] &= ~(1<<y);
				}
				else if(x == 6) {
					m.trigger[edit_row] ^= 1<<y;
					m.toggle[edit_row] &= ~(1<<y);
				}
				else if(x == 4) {
					sound ^= 1;
				}
				else if(x == 2) {
					if(position[y] == -1) {
						position[y] = m.count[y];
					}
					else {
						position[y] = -1;
					}
				}
				else if(x == 3) {
					m.sync[edit_row] ^= (1<<y);
				}
			}
		}
		else if(mode == 2 && z == 1) {
			if(x > 3 && x < 7) {
				m.rule_dests[edit_row] = y;
				m.rule_dest_targets[edit_row] = x-3;
			  // post("\nrule_dests", edit_row, ":", rule_dests[edit_row]);
			}
			else if(x > 6) {
				m.rules[edit_row] = y;
			  // post("\nrules", edit_row, ":", rules[edit_row]);
			}
		}

		monomeFrameDirty++;
	}
}

void handler_MPRefresh(s32 data) { 
	if(monomeFrameDirty) {
		grid_refresh();

		monome_set_quadrant_flag(0);
		monome_set_quadrant_flag(1);
		(*monome_refresh)();
	}
}

void handler_MPKey(s32 data) { 
	// print_dbg("\r\n> MP key ");
	// print_dbg_ulong(data);

	switch(data) {
	case 0:
		grid_refresh = &refresh_mp;
		view_clock = false;
		break;
	case 1:
		grid_refresh = &refresh_clock;
		print_dbg("\r\ntime: ");
		print_dbg_ulong(time_fine);
		print_dbg(" ");
		print_dbg_ulong(time_rough);
		view_clock = true;
		view_config = false;
		break;
	case 2:
		grid_refresh = &refresh_mp;
		view_config = false;
		break;
	case 3:
		grid_refresh = &refresh_mp_config;
		view_config = true;
		view_clock = false;
		break;
	default:
		break;
	}

	monomeFrameDirty++;
}

void handler_MPTr(s32 data) { 
	// print_dbg("\r\n> MP tr ");
	// print_dbg_ulong(data);

	switch(data) {
	case 0:
		if(clock_mul == 1)
			clock_mp(0);
		break;
	case 1:
		if(clock_mul == 1)
			clock_mp(1);
		else {
			ext_clock_count++;
			if(ext_clock_count >= clock_mul - 1) {
				ext_clock_count = 0;
				ext_clock_phase ^= 1;
				clock_mp(ext_clock_phase);
			}
		}
		break;
	case 3:
		// right jack upwards: RESET
		for(int n=0;n<8;n++) {
			position[n] = m.count[n];
			tick[n] = m.speed[n];
		}
		break;
	default:
		break;
	}

	monomeFrameDirty++;
}

void handler_MPTrNormal(s32 data) { 
	// print_dbg("\r\n> MP tr normal ");
	// print_dbg_ulong(data);

	clock_external = data;

	if(clock_external)
		clock = &clock_null;
	else
		clock = &clock_mp;

	monomeFrameDirty++;
}

void refresh_clock(void) {
	// clear grid
	memset(monomeLedBuffer,0,128);

	monomeLedBuffer[clock_count & 0xf] = L0;

	if(clock_external) {
		memset(monomeLedBuffer + R1,3,16);
		monomeLedBuffer[R1 + clock_mul - 1] = L2;
	}
	else {
		monomeLedBuffer[R1 + time_rough] = L2;
		monomeLedBuffer[R2 + time_fine] = L1;

		monomeLedBuffer[R4+6] = 7;
		monomeLedBuffer[R4+7] = 3;
		monomeLedBuffer[R4+8] = 3;
		monomeLedBuffer[R4+9] = 7;

	}
}

void refresh_mp_config(void) {
	u8 i1;//, i2, i3;
	u8 c;

	// clear grid
	memset(monomeLedBuffer,0,128);

	// voice mode + sound
	c = L0;
	if(voice_mode == MP_8T)
		c = L1 + (4 * sound);

	monomeLedBuffer[R1 + 1] = c;
	monomeLedBuffer[R1 + 2] = c;
	monomeLedBuffer[R2 + 1] = c;
	monomeLedBuffer[R2 + 2] = c;
	monomeLedBuffer[R3 + 1] = c;
	monomeLedBuffer[R3 + 2] = c;
	monomeLedBuffer[R4 + 1] = c;
	monomeLedBuffer[R4 + 2] = c;

	c = L0;
	if(voice_mode == MP_4V)
		c = L1 + (4 * sound);

	monomeLedBuffer[R1 + 4] = c;
	monomeLedBuffer[R2 + 4] = c;
	monomeLedBuffer[R3 + 4] = c;
	monomeLedBuffer[R4 + 4] = c;

	c = L0;
	if(voice_mode == MP_2V)
		c = L1 + (4 * sound);

	monomeLedBuffer[R1 + 5] = c;
	monomeLedBuffer[R2 + 5] = c;

	c = L0;
	if(voice_mode == MP_1V)
		c = L1 + (4 * sound);

	monomeLedBuffer[R1 + 6] = c;

	// scale
	if(voice_mode != MP_8T) {
		for(i1=0;i1<8;i1++) {
			monomeLedBuffer[8+16*i1] = L0;
			monomeLedBuffer[R6 + i1] = 2;
			monomeLedBuffer[R7 + i1] = 2;
		}
		monomeLedBuffer[R6 + (m.scale >> 3) * 16 + (m.scale & 0x7)] = L2;

		for(i1=0;i1<8;i1++)
			monomeLedBuffer[scale_data[m.scale][i1] + 8 + (7-i1)*16] = L1;
	}
}

void refresh_mp(void) {
	u8 i1, i2, i3;

	// clear grid
	memset(monomeLedBuffer,0,128);

	// SHOW POSITIONS
	if(mode == 0) {
		for(i1=0;i1<8;i1++) {
			for(i2=m.min[i1];i2<=m.max[i1];i2++)
				monomeLedBuffer[i1*16 + i2] = L0;
			monomeLedBuffer[i1*16 + m.count[i1]] = L1;
			if(position[i1] >= 0) {
				monomeLedBuffer[i1*16 + position[i1]] = L2;
			}
		}
	}
	// SHOW SPEED
	else if(mode == 1) {
		for(i1=0;i1<8;i1++) {
			if(position[i1] >= 0)
				monomeLedBuffer[i1*16 + position[i1]] = L0;

			if(position[i1] != -1)
				monomeLedBuffer[i1*16 + 2] = 2;

			for(i2=m.smin[i1];i2<=m.smax[i1];i2++)
				monomeLedBuffer[i1*16 + i2+8] = L0;

			monomeLedBuffer[i1*16 + m.speed[i1]+8] = L1;

			if(sound)
				monomeLedBuffer[i1*16 + 4] = 2;

			if(m.toggle[edit_row] & (1 << i1))
				monomeLedBuffer[i1*16 + 5] = L2;
			else
				monomeLedBuffer[i1*16 + 5] = L0;

			if(m.trigger[edit_row] & (1 << i1))
				monomeLedBuffer[i1*16 + 6] = L2;
			else
				monomeLedBuffer[i1*16 + 6] = L0;

			if(m.sync[edit_row] & (1<<i1))
				monomeLedBuffer[i1*16 + 3] = L1;
			else  
				monomeLedBuffer[i1*16 + 3] = L0;
		}

		monomeLedBuffer[edit_row * 16] = L2;
	}
	// SHOW RULES
	else if(mode == 2) {
		for(i1=0;i1<8;i1++) 
			if(position[i1] >= 0)
				monomeLedBuffer[i1*16 + position[i1]] = L0;

		monomeLedBuffer[edit_row * 16] = L1;
		monomeLedBuffer[edit_row * 16 + 1] = L1;

		if(m.rule_dest_targets[edit_row] == 1) {
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 4] = L2;
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 5] = L0;
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 6] = L0;
		}
		else if (m.rule_dest_targets[edit_row] == 2) {
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 4] = L0;
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 5] = L2;
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 6] = L0;
		}
		else {
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 4] = L2;
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 5] = L2;
			monomeLedBuffer[m.rule_dests[edit_row] * 16 + 6] = L0;
		}

		for(i1=8;i1<16;i1++)
			monomeLedBuffer[m.rules[edit_row] * 16 + i1] = L0;


		for(i1=0;i1<8;i1++) {
			i3 = sign[m.rules[edit_row]][i1];
			for(i2=0;i2<8;i2++) {
				if((i3 & (1<<i2)) != 0)
					monomeLedBuffer[i1*16 + 8 + i2] = L2;
			}
		}
	}
}


void calc_scale(uint8_t s) {
	cur_scale[0] = scale_data[s][0];

	for(u8 i1=1;i1<8;i1++) {
		cur_scale[i1] = cur_scale[i1-1] + scale_data[s][i1];
		// print_dbg("\r\n ");
		// print_dbg_ulong(cur_scale[i1]);
		
	}
}
