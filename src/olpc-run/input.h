/* evdev input: built-in XO-1 buttons and USB pads -> libretro joypad bits. */
#ifndef OLPC_RUN_INPUT_H
#define OLPC_RUN_INPUT_H
#include <stdint.h>
#include <linux/input.h>

#define IN_MAXDEV 8
#define IN_BUILTIN 0
#define IN_PAD     1

/* bit n of a mask = RETRO_DEVICE_ID_JOYPAD_n (B=0 Y=1 SELECT=2 START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7 A=8) */
struct inp_dev {
	int fd;
	int cls;                       /* IN_BUILTIN or IN_PAD */
	char path[48];
	int absmin[2], absmax[2];      /* ABS_X, ABS_Y ranges */
	uint16_t btn;                  /* buttons held (incl. BTN_DPAD_*) */
	uint16_t hat, stick;           /* directions from ABS_HAT0 and ABS_X/Y */
	int rotate;                    /* rotate button held (built-in only) */
};

struct input {
	struct inp_dev dev[IN_MAXDEV];
	int ndev;
	int ino;                       /* inotify fd on the device dir, or -1 */
	const char *dir;
	int rescan;                    /* frames until the next rescan, -1 none */
	const char *chord_file;
	int chord, chord_tick;
	int chord_fd, chord_retry;      /* kept open; -1 = not open */
	uint64_t rot_since, ss_since;  /* ns when the exit gesture started, 0 = not held */
	int tap;                       /* a gesture was released since the last input_tap() */
	uint64_t tap_dur;              /* how long it was held, ns */
};

void inp_dev_event(struct inp_dev *d, const struct input_event *e);
int  inp_dev_drain(struct inp_dev *d);          /* reads all pending events; -1 if the node is gone */
uint16_t inp_dev_mask(const struct inp_dev *d);

void input_init(struct input *in, const char *dir, const char *chord_file);
void input_scan(struct input *in);
/* Drain every device, refresh the chord flag; now_ns is a monotonic clock. */
void input_poll(struct input *in, uint64_t now_ns);
uint16_t input_mask(const struct input *in);
/* Sleep until a device has events or ms passed. */
void input_wait(struct input *in, int ms);
/* 1 once per release of rotate (or pad Select+Start) that was held for less than hold_ns. */
int input_tap(struct input *in, uint64_t hold_ns);
/* 1 when rotate (or pad Select+Start) has been held for hold_ns. */
int input_exit(const struct input *in, uint64_t now_ns, uint64_t hold_ns);
#endif
