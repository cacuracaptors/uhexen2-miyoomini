/*
 * vid_miyoo.c -- video driver for the Miyoo Mini (Plus), software renderer.
 *
 * Derived from vid_sdl.c (Hammer of Thyrion), GPL v2 or later.
 *
 * The renderer keeps drawing into an ordinary 8-bit buffer. The size is
 * 320x240 by default; "-width 400", "-width 480" or "-width 640" give
 * 400x300, 480x360 and 640x480 (always 4:3).
 *
 * On every VID_Update() the main thread only copies that 8-bit picture
 * (and the palette) into a second buffer and goes on with the next frame.
 * A thread of its own, pinned to the second core, then turns the picture
 * into 32-bit pixels through the palette, scales it to the 640x480 panel
 * (nearest neighbour), rotates it by 180 degrees (the panel is mounted
 * upside down), copies it into the hidden page of /dev/fb0 and shows that
 * page at the next vsync. This is the same page-flip approach the OpenLara
 * port uses on this device.
 *
 * "-ui320" renders the 3D view at 640x480 but keeps everything else (HUD,
 * menus, console, text) in a 320x240 picture of its own, so it stays big and
 * readable. The two pictures are combined when the frame is shown: where the
 * 320x240 picture is "transparent" (palette index 255 inside the 3D view
 * area) the 640x480 3D picture shows through. The engine never notices: it
 * draws the 2D parts into the 320x240 picture and renders the 3D view while
 * VID_Use3D() has switched vid.width/height/buffer to the big picture.
 *
 * "-perf" prints timing lines to stderr every 2 s (they end up in log.txt).
 * "-nopin" leaves the thread-to-core pinning off.
 *
 * SDL 1.2 is still initialised (SDL_SetVideoMode), because its fbcon driver
 * is what delivers the keyboard events, but it never draws anything.
 */

#include "quakedef.h"
#include "d_local.h"
#include "cfgfile.h"
#include "bgmusic.h"
#include "cdaudio.h"
#include "sdl_inc.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <linux/fb.h>

#define PANEL_W		640
#define PANEL_H		480

unsigned short	d_8to16table[256];
unsigned int	d_8to24table[256];

byte globalcolormap[VID_GRADES*256], lastglobalcolor = 0;
byte *lastsourcecolormap = NULL;

static unsigned char	vid_curpal[256*3];
static uint32_t		pal32[256];	/* palette as XRGB8888 */

static qboolean	vid_initialized = false;
static int	lockcount;
qboolean	in_mode_set;
static int	enable_mouse;
static qboolean	palette_changed;

static SDL_Surface	*sdlscreen;	/* only to keep SDL's fbcon (keyboard) alive */

viddef_t	vid;			/* global video state */
modestate_t	modestate = MS_UNINIT;

/* the cvars below are only kept so that a config.cfg from the PC version
 * does not complain: the values are not used for anything. */
static cvar_t	vid_config_glx = {"vid_config_glx", "640", CVAR_ARCHIVE};
static cvar_t	vid_config_gly = {"vid_config_gly", "480", CVAR_ARCHIVE};
static cvar_t	vid_config_swx = {"vid_config_swx", "320", CVAR_ARCHIVE};
static cvar_t	vid_config_swy = {"vid_config_swy", "240", CVAR_ARCHIVE};
static cvar_t	vid_config_fscr= {"vid_config_fscr", "1", CVAR_ARCHIVE};
static cvar_t	vid_showload = {"vid_showload", "1", CVAR_NONE};

cvar_t		_enable_mouse = {"_enable_mouse", "0", CVAR_ARCHIVE};

static byte	*vid_fbuf;		/* the 8-bit picture the engine draws into (the 2D picture when split) */
static byte	*vid_fbuf3d;		/* the 3D picture, only when vid_3dscale > 1 */
static int	ui_w, ui_h;		/* size of vid_fbuf */
static int	v3_w, v3_h;		/* size of the 3D picture */
static int	use3d_depth;
static struct
{
	int	width, height, rowbytes, conwidth, conheight, conrowbytes;
	pixel_t	*buffer, *conbuffer, *direct;
} saved2d;
static byte	*vid_surfcache;
static int	vid_surfcachesize;
static int	VID_highhunkmark;

/* ------------------------------------------------------------------------ */
/* the 8-bit picture -> panel pixels (pure function, tested off the device)   */
/* ------------------------------------------------------------------------ */

/* BEGIN_CONVERT */
/*
 * Both functions below build each panel row in rowbuf (cached memory) and
 * copy it to dst in one go. dst can then be the framebuffer itself: it is only
 * written, front to back, never read (reading framebuffer memory is very slow,
 * writing it in sequence is fast). A row that repeats the one before is
 * copied from rowbuf again.
 */
static uint32_t	rowbuf[PANEL_W];

/*
 * src: w x h 8-bit picture, pal: its palette (XRGB8888), dst: PANEL_W x PANEL_H.
 * Scales to the panel with nearest neighbour and rotates by 180 degrees.
 * xmap[x] = x * w / PANEL_W and ymap[y] = y * h / PANEL_H (unrotated panel
 * coordinates). The 2x and 1x cases have their own faster loops.
 * Miyoo: done in pieces ("units" u0..u1, see convert_units), so the second
 * core can do some of them between its drawing jobs; rb is the row buffer of
 * the thread doing it. Any split gives the same picture.
 */
static int convert_units (int w, int h)
{
	if ((w * 2 == PANEL_W && h * 2 == PANEL_H) || (w == PANEL_W && h == PANEL_H))
		return h;	/* source rows */
	return PANEL_H;		/* panel rows */
}

static void convert_rows (const byte *src, const uint32_t *pal, int w, int h,
			  uint32_t *dst, const uint16_t *xmap, const uint16_t *ymap,
			  int u0, int u1, uint32_t *rb)
{
	int	x, y;

	if (w * 2 == PANEL_W && h * 2 == PANEL_H)
	{
		for (y = u0; y < u1; y++)
		{
			const byte	*s = src + y * w;
			uint32_t	*d0 = dst + (PANEL_H - 1 - 2 * y) * PANEL_W;
			uint32_t	*d = rb;
			for (x = w - 1; x >= 0; x--)
			{
				uint32_t c = pal[s[x]];
				d[0] = c;
				d[1] = c;
				d += 2;
			}
			memcpy (d0 - PANEL_W, rb, PANEL_W * 4);
			memcpy (d0, rb, PANEL_W * 4);
		}
	}
	else if (w == PANEL_W && h == PANEL_H)
	{
		for (y = u0; y < u1; y++)
		{
			const byte	*s = src + y * w;
			uint32_t	*d = rb;
			for (x = w - 1; x >= 0; x--)
				*d++ = pal[s[x]];
			memcpy (dst + (PANEL_H - 1 - y) * PANEL_W, rb, PANEL_W * 4);
		}
	}
	else
	{
		for (y = u0; y < u1; y++)
		{
			uint32_t	*d = dst + (PANEL_H - 1 - y) * PANEL_W;
			const byte	*s;

			if (!(y > u0 && ymap[y] == ymap[y - 1]))
			{	/* a new source row (else rb still holds the right one) */
				s = src + ymap[y] * w;
				for (x = 0; x < PANEL_W; x++)
					rb[PANEL_W - 1 - x] = pal[s[xmap[x]]];
			}
			memcpy (d, rb, PANEL_W * 4);
		}
	}
}

static void convert_frame (const byte *src, const uint32_t *pal, int w, int h,
			   uint32_t *dst, const uint16_t *xmap, const uint16_t *ymap)
{
	convert_rows (src, pal, w, h, dst, xmap, ymap, 0, convert_units (w, h), rowbuf);
}
/* END_CONVERT */

/* BEGIN_COMPOSITE */
/*
 * Split mode: lo is the 320x240 picture of everything but the 3D view, hi the
 * 640x480 3D picture, (vx,vy,vw,vh) the 3D view area in 640x480 coordinates.
 * Inside that area a lo pixel of index 255 means "show the 3D picture here".
 * pal colours the 2D picture, palhi the 3D picture (darkened while a menu fades it).
 * The result is rotated by 180 degrees like convert_frame().
 * Panel rows y0..y1 only (Miyoo: in pieces, see convert_rows).
 */
static void composite_rows (const byte *lo, const byte *hi, const uint32_t *pal,
			    const uint32_t *palhi, int vx, int vy, int vw, int vh, uint32_t *dst,
			    int y0, int y1, uint32_t *rb)
{
	const int	lw = PANEL_W / 2;
	int		x, y, x0, x1;

	x0 = (vx < 0) ? 0 : vx;
	x1 = (vx + vw > PANEL_W) ? PANEL_W : vx + vw;

	for (y = y0; y < y1; y++)
	{
		const byte	*lrow = lo + (y >> 1) * lw;
		uint32_t	*d = rb;
		int		in_rect = (y >= vy && y < vy + vh && x1 > x0);

		if (!in_rect)
		{
			int prev_in_rect = (y > 0 && (y - 1) >= vy && (y - 1) < vy + vh && x1 > x0);
			if (!((y & 1) && y > y0 && !prev_in_rect))
			{	/* else: same 2D row as the row just before, still in rb */
				for (x = 0; x < lw; x++)
				{
					uint32_t c = pal[lrow[x]];
					d[PANEL_W - 1 - 2 * x] = c;
					d[PANEL_W - 2 - 2 * x] = c;
				}
			}
		}
		else
		{
			const byte	*hrow = hi + y * PANEL_W;
			uint32_t	*o;
			unsigned int	c;

			for (x = 0; x < x0; x++)
				d[PANEL_W - 1 - x] = pal[lrow[x >> 1]];
			/* Miyoo: the 3D part, 8 pixels at a time where the 2D picture
			 * is transparent over all of them (most of the view): one
			 * check and the 3D pixels read 4 by 4, instead of a check
			 * per pixel. Same result as the plain loop. */
			if (x < x1 && (x & 1))
			{
				c = lrow[x >> 1];
				d[PANEL_W - 1 - x] = (c != 255) ? pal[c] : palhi[hrow[x]];
				x++;
			}
			o = d + PANEL_W - 1 - x;
			for ( ; x + 8 <= x1; x += 8, o -= 8)
			{
				uint32_t	lw, h0, h1;
				memcpy (&lw, lrow + (x >> 1), 4);
				if (lw == 0xFFFFFFFFu)
				{
					memcpy (&h0, hrow + x, 4);
					memcpy (&h1, hrow + x + 4, 4);
					o[0] = palhi[h0 & 255];
					o[-1] = palhi[(h0 >> 8) & 255];
					o[-2] = palhi[(h0 >> 16) & 255];
					o[-3] = palhi[h0 >> 24];
					o[-4] = palhi[h1 & 255];
					o[-5] = palhi[(h1 >> 8) & 255];
					o[-6] = palhi[(h1 >> 16) & 255];
					o[-7] = palhi[h1 >> 24];
				}
				else
				{
					int k;
					for (k = 0; k < 8; k++)
					{
						c = lrow[(x + k) >> 1];
						o[-k] = (c != 255) ? pal[c] : palhi[hrow[x + k]];
					}
				}
			}
			for ( ; x < x1; x++)
			{
				c = lrow[x >> 1];
				d[PANEL_W - 1 - x] = (c != 255) ? pal[c] : palhi[hrow[x]];
			}
			for ( ; x < PANEL_W; x++)
				d[PANEL_W - 1 - x] = pal[lrow[x >> 1]];
		}
		memcpy (dst + (PANEL_H - 1 - y) * PANEL_W, rb, PANEL_W * 4);
	}
}

static void composite_frame (const byte *lo, const byte *hi, const uint32_t *pal,
			     const uint32_t *palhi, int vx, int vy, int vw, int vh, uint32_t *dst)
{
	composite_rows (lo, hi, pal, palhi, vx, vy, vw, vh, dst, 0, PANEL_H, rowbuf);
}
/* END_COMPOSITE */

/* ------------------------------------------------------------------------ */
/* framebuffer pages + flip thread                                           */
/* ------------------------------------------------------------------------ */

static int			fb_fd = -1;
static struct fb_var_screeninfo	fb_var;
static uint8_t			*fb_mem;
static int			fb_page;	/* page being shown */
static qboolean			fb_vsync;	/* page flipping works */
static uint32_t			*outbuf;	/* PANEL_W x PANEL_H, cached memory; flip thread only */

/* the picture handed from the main thread to the flip thread */
static byte			*snap8;
static byte			*snap_hi;	/* copy of the 3D picture, split mode only */
static uint32_t			snap_pal[256];
static uint32_t			snap_palhi[256];
static int			snap_w, snap_h;
static qboolean			snap_split;
static vrect_t			snap_vr;
static uint16_t			xmap[PANEL_W], ymap[PANEL_H];

static pthread_t		flip_thread;
static pthread_mutex_t		flip_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t		flip_cond = PTHREAD_COND_INITIALIZER;
static qboolean			flip_started;
static qboolean			flip_quit;
static qboolean			job_pending;	/* a picture waits in snap8, not taken yet */
static qboolean			snap_free = true; /* the flip thread is done with snap8/snap_pal */

static qboolean			opt_perf, opt_nopin;

/* Miyoo: the picture is converted in pieces, by the second core between its
 * drawing jobs (d_mt_idlework) and, if that is not enough by the time the
 * next vsync needs it, by the flip thread itself (both run on that core) */
static int			conv_active;	/* a picture is being converted */
static int			conv_n, conv_chunk;	/* its units (see convert_units), per piece */
static int			conv_next, conv_done;	/* next unit to take / units finished */
static int			conv_helped;	/* units done between drawing jobs */
static int			conv_inside;	/* the drawing core is in conv_idle */
static double			conv_t0, conv_t1;	/* the picture's schedule: from its arrival to the flip thread's deadline */
static uint32_t			*conv_dst;
static pthread_cond_t		conv_cond = PTHREAD_COND_INITIALIZER;	/* with flip_mutex */
static uint32_t			conv_rb_idle[PANEL_W];	/* row buffer of the drawing core */
static int			opt_flipmode;	/* 0 shared, 1 -flipalone (flip thread only, first), 2 -flipold */
static double			vid_work_ema = 16.0;	/* main thread only */
static double			snap_handover, snap_work;	/* when the picture was handed over, the game's usual work then */

static double now_ms (void)
{
	struct timeval tv;
	gettimeofday (&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

#ifdef PERF_START
/* ---- per-phase timing used by -perf: the engine calls these (see perf-fases.patch) ---- */
int		vid_perf;
static double	pf_ms[PF_N];
static double	pc_sum[PC_N];

double VID_PerfNow (void)
{
	return now_ms ();
}

void VID_PerfAdd (int id, double ms)
{
	if (id >= 0 && id < PF_N)
		pf_ms[id] += ms;
}

void VID_PerfCount (int id, int n)
{
	if (id >= 0 && id < PC_N)
		pc_sum[id] += n;
}
#endif

/* pin the calling thread to one core (ignored if it fails) */
static void pin_cpu (int cpu)
{
	unsigned long mask = 1UL << cpu;
	if (opt_nopin)
		return;
	if (syscall (SYS_sched_setaffinity, 0, sizeof(mask), &mask) != 0)
		fprintf (stderr, "pin: core %d refused\n", cpu);
}

/* back to page 0, where OnionOS draws (on exit and on a crash) */
static void fb_restore (void)
{
	if (fb_fd >= 0 && fb_vsync && fb_var.yoffset != 0)
	{
		fb_var.yoffset = 0;
		ioctl (fb_fd, FBIOPAN_DISPLAY, &fb_var);
	}
}

static void fb_signal (int sig)
{
	fb_restore ();
	signal (sig, SIG_DFL);
	raise (sig);
}

static void fb_atexit (void)
{
	fb_restore ();
}

static void fb_init (void)
{
	struct fb_fix_screeninfo	fix;
	void	*mem;

	fb_fd = open ("/dev/fb0", O_RDWR);
	if (fb_fd < 0)
	{
		Con_Printf ("vsync: off (no /dev/fb0)\n");
		return;
	}
	if (ioctl (fb_fd, FBIOGET_VSCREENINFO, &fb_var) ||
	    ioctl (fb_fd, FBIOGET_FSCREENINFO, &fix) ||
	    fb_var.xres != PANEL_W || fb_var.yres != PANEL_H ||
	    fb_var.bits_per_pixel != 32 ||
	    fix.line_length != PANEL_W * 4 ||
	    fb_var.yres_virtual < PANEL_H * 2 ||
	    fix.smem_len < PANEL_W * 4 * PANEL_H * 2)
	{
		Con_Printf ("vsync: off (framebuffer %ux%u virtual %ux%u)\n",
				fb_var.xres, fb_var.yres,
				fb_var.xres_virtual, fb_var.yres_virtual);
		return;
	}
	mem = mmap (NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (mem == MAP_FAILED)
	{
		Con_Printf ("vsync: off (mmap failed)\n");
		return;
	}
	fb_mem = (uint8_t *) mem;
	fb_page = (fb_var.yoffset >= (unsigned int) PANEL_H) ? 1 : 0;
	fb_vsync = true;
	atexit (fb_atexit);
	signal (SIGSEGV, fb_signal);
	signal (SIGBUS, fb_signal);
	signal (SIGABRT, fb_signal);
	signal (SIGFPE, fb_signal);
	signal (SIGILL, fb_signal);
	signal (SIGTERM, fb_signal);
	Con_Printf ("vsync: on (2 framebuffer pages)\n");
}

/* BEGIN_CONVSHARE */
static void conv_units_do (int u0, int u1, uint32_t *rb)
{
	if (snap_split)
		composite_rows (snap8, snap_hi, snap_pal, snap_palhi,
				snap_vr.x, snap_vr.y, snap_vr.width, snap_vr.height, conv_dst, u0, u1, rb);
	else
		convert_rows (snap8, snap_pal, snap_w, snap_h, conv_dst, xmap, ymap, u0, u1, rb);
}

/* takes one piece of the picture and converts it; 0 = none left */
static int conv_claim (uint32_t *rb, int idle)
{
	int u0, u1;

	u0 = __atomic_fetch_add (&conv_next, conv_chunk, __ATOMIC_ACQ_REL);
	if (u0 >= conv_n)
		return 0;
	u1 = u0 + conv_chunk;
	if (u1 > conv_n)
		u1 = conv_n;
	conv_units_do (u0, u1, rb);
	if (idle)
		__atomic_add_fetch (&conv_helped, u1 - u0, __ATOMIC_RELAXED);
	if (__atomic_add_fetch (&conv_done, u1 - u0, __ATOMIC_ACQ_REL) == conv_n)
	{
		pthread_mutex_lock (&flip_mutex);
		pthread_cond_broadcast (&conv_cond);
		pthread_mutex_unlock (&flip_mutex);
	}
	return 1;
}

/* d_mt_idlework: called by the second core when it has no drawing job */
static int conv_idle (void)
{
	int	r = 0;

	/* conv_inside lets the flip thread know when it may set up the next
	 * picture: never while this core may still read the current one */
	__atomic_add_fetch (&conv_inside, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n (&conv_active, __ATOMIC_SEQ_CST))
		r = conv_claim (conv_rb_idle, 1);
	__atomic_sub_fetch (&conv_inside, 1, __ATOMIC_SEQ_CST);
	return r;
}

/* d_mt_duework: the same, but only when the picture is behind a steady pace
 * that ends at the flip thread's deadline. Spread like this over the frame,
 * it no longer lands in one block in the middle of the models (where the
 * first core would wait for it), and the drawing jobs it delays can be taken
 * back by the first core. */
static int conv_due (void)
{
	int	r = 0;

	__atomic_add_fetch (&conv_inside, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n (&conv_active, __ATOMIC_SEQ_CST))
	{
		double	now = now_ms (), span = conv_t1 - conv_t0;
		double	want = (span > 0.5) ? conv_n * (now - conv_t0) / span : conv_n;
		if (__atomic_load_n (&conv_next, __ATOMIC_ACQUIRE) < (int) want)
			r = conv_claim (conv_rb_idle, 1);
	}
	__atomic_sub_fetch (&conv_inside, 1, __ATOMIC_SEQ_CST);
	return r;
}

static void conv_wait_until (double t_ms)
{
	struct timespec	ts;
	double		s = t_ms / 1000.0;

	ts.tv_sec = (time_t) s;
	ts.tv_nsec = (long) ((s - (double) ts.tv_sec) * 1e9);
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait (&conv_cond, &flip_mutex, &ts);
}

#define FLIP_REFRESH	16.67	/* ms between vsyncs */
#define FLIP_ALONE	5.5	/* ms the flip thread needs to convert a picture alone, with margin */

/* the flip thread's part: offer the picture in snap* to the drawing core,
 * then do itself what is left when it must start in order to be on time */
static void conv_picture (uint32_t *dst, double arrive, double handover, double work, double last_vsync)
{
	double	deadline;

	/* the drawing core may still be on its way out of the last picture
	 * (it runs on this same core: sleep, never spin) */
	while (__atomic_load_n (&conv_inside, __ATOMIC_SEQ_CST))
		usleep (20);
	/* the latest start for doing it alone: in time for the next vsync it
	 * can still make, and before the game is ready to hand over the next
	 * picture (its work time, not the time between hand-overs, which would
	 * grow with any wait for this very conversion) */
	deadline = handover + work - (FLIP_ALONE + 0.5);
	if (last_vsync > 0)
	{
		double v = last_vsync + FLIP_REFRESH;
		while (v - FLIP_ALONE < arrive)
			v += FLIP_REFRESH;
		if (v - FLIP_ALONE < deadline)
			deadline = v - FLIP_ALONE;
	}

	conv_dst = dst;
	conv_n = snap_split ? PANEL_H : convert_units (snap_w, snap_h);
	conv_chunk = (conv_n == PANEL_H) ? 8 : 4;
	conv_t0 = arrive;
	conv_t1 = deadline;
	__atomic_store_n (&conv_done, 0, __ATOMIC_RELAXED);
	__atomic_store_n (&conv_next, 0, __ATOMIC_RELEASE);
	__atomic_store_n (&conv_active, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n (&d_mt_idlework, conv_idle, __ATOMIC_RELEASE);
	__atomic_store_n (&d_mt_duework, conv_due, __ATOMIC_RELEASE);
	D_MT_Kick ();
	pthread_mutex_lock (&flip_mutex);
	while (__atomic_load_n (&conv_done, __ATOMIC_ACQUIRE) < conv_n && now_ms () < deadline)
		conv_wait_until (deadline);
	pthread_mutex_unlock (&flip_mutex);

	while (conv_claim (rowbuf, 0))
		;
	/* a piece the drawing core is still on: it runs on this same core,
	 * so sleep (never spin) until it is done */
	pthread_mutex_lock (&flip_mutex);
	while (__atomic_load_n (&conv_done, __ATOMIC_ACQUIRE) < conv_n)
		conv_wait_until (now_ms () + 2.0);
	pthread_mutex_unlock (&flip_mutex);
	__atomic_store_n (&conv_active, 0, __ATOMIC_SEQ_CST);
}
/* END_CONVSHARE */

static void *flip_proc (void *arg)
{
	double	last_vsync = 0;
	double	t_wait = 0, t_conv = 0, t_copy = 0, t_pan = 0;
	double	last_print = 0;
	int	frames = 0;
	(void) arg;

	pin_cpu (1);
	/* Miyoo: this thread shares the second core with the drawing jobs (r_mt).
	 * Its picture must be ready for the next vsync, else the frame is shown
	 * one refresh later and the game waits: so it goes first. It works
	 * ~4 ms and then sleeps in the vsync wait, leaving the core to the jobs.
	 * -flipold gives back the old order (drawing jobs first). */
	opt_flipmode = COM_CheckParm ("-flipold") ? 2 : COM_CheckParm ("-flipalone") ? 1 : 0;
	if (opt_flipmode == 2)
	{
		setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), 5);
		fprintf (stderr, "flip thread: after the drawing jobs (-flipold)\n");
	}
	else
	{
		struct sched_param sp;
		memset (&sp, 0, sizeof(sp));
		sp.sched_priority = 1;
		if (pthread_setschedparam (pthread_self (), SCHED_FIFO, &sp) == 0)
			fprintf (stderr, "flip thread: realtime, %s\n", opt_flipmode ? "converts alone (-flipalone)" :
					"converts what the drawing core has not done between its jobs");
		else
		{
			setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), -10);
			fprintf (stderr, "flip thread: high priority, before the drawing jobs\n");
		}
	}

	for (;;)
	{
		double	t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;

		if (opt_perf)
			t0 = now_ms ();

		pthread_mutex_lock (&flip_mutex);
		while (!job_pending && !flip_quit)
			pthread_cond_wait (&flip_cond, &flip_mutex);
		if (!job_pending)	/* quit, nothing left to show */
		{
			pthread_mutex_unlock (&flip_mutex);
			break;
		}
		job_pending = false;
		pthread_mutex_unlock (&flip_mutex);

		if (opt_perf)
			t1 = now_ms ();

		/* with vsync the picture is built straight into the hidden page of
		 * the framebuffer (no copy of the whole frame afterwards) */
		{
			uint32_t *dst = fb_vsync ?
				(uint32_t *) (fb_mem + (size_t) (fb_page ^ 1) * PANEL_H * PANEL_W * 4) : outbuf;
			double	arrive = now_ms ();

			if (fb_vsync && opt_flipmode == 0 && D_MT_Alive ())
				conv_picture (dst, arrive, snap_handover, snap_work, last_vsync);
			else if (snap_split)
				composite_frame (snap8, snap_hi, snap_pal, snap_palhi,
						snap_vr.x, snap_vr.y, snap_vr.width, snap_vr.height, dst);
			else
				convert_frame (snap8, snap_pal, snap_w, snap_h, dst, xmap, ymap);
		}

		/* snap8 and snap_pal are free again: the main thread may hand over the next picture */
		pthread_mutex_lock (&flip_mutex);
		snap_free = true;
		pthread_cond_broadcast (&flip_cond);
		pthread_mutex_unlock (&flip_mutex);

		if (opt_perf)
			t2 = now_ms ();

		if (fb_vsync)
		{
			/* the hidden page is ready: show it at the next vsync */
			int back = fb_page ^ 1;
			if (opt_perf)
				t3 = now_ms ();
			fb_var.yoffset = back * PANEL_H;
			ioctl (fb_fd, FBIOPAN_DISPLAY, &fb_var);
			fb_page = back;
			last_vsync = now_ms ();	/* the pan returns at the vsync */
		}
		else if (sdlscreen)
		{
			int y;
			if (SDL_MUSTLOCK(sdlscreen))
				SDL_LockSurface (sdlscreen);
			for (y = 0; y < PANEL_H; y++)
				memcpy ((uint8_t *) sdlscreen->pixels + y * sdlscreen->pitch,
					outbuf + y * PANEL_W, PANEL_W * 4);
			if (SDL_MUSTLOCK(sdlscreen))
				SDL_UnlockSurface (sdlscreen);
			if (!(sdlscreen->flags & SDL_HWSURFACE))
				SDL_Flip (sdlscreen);
			if (opt_perf)
				t3 = now_ms ();
		}

		if (opt_perf)
		{
			t4 = now_ms ();
			t_wait += t1 - t0;
			t_conv += t2 - t1;
			t_copy += t3 - t2;
			t_pan  += t4 - t3;
			frames++;
			if (last_print == 0)
				last_print = t4;
			else if (t4 - last_print >= 2000.0 && frames > 0)
			{
				int helped = __atomic_exchange_n (&conv_helped, 0, __ATOMIC_RELAXED);
				fprintf (stderr, "perf flip: wait-for-picture %.2f  convert %.2f (done between drawing jobs %.0f%%)  copy %.2f  pan(vsync) %.2f  ms/frame (%d frames)\n",
					t_wait / frames, t_conv / frames,
					100.0 * helped / ((double) frames * (conv_n > 0 ? conv_n : 1)),
					t_copy / frames, t_pan / frames, frames);
				t_wait = t_conv = t_copy = t_pan = 0;
				frames = 0;
				last_print = t4;
			}
		}
	}
	return NULL;
}

static void flip_begin (void)
{
	flip_quit = false;
	if (pthread_create (&flip_thread, NULL, flip_proc, NULL) != 0)
		Sys_Error ("Couldn't start the video flip thread");
	flip_started = true;
}

static void flip_end (void)
{
	if (!flip_started)
		return;
	pthread_mutex_lock (&flip_mutex);
	while (job_pending || !snap_free)
		pthread_cond_wait (&flip_cond, &flip_mutex);
	flip_quit = true;
	pthread_cond_broadcast (&flip_cond);
	pthread_mutex_unlock (&flip_mutex);
	pthread_join (flip_thread, NULL);
	flip_started = false;
}

/*
 * Hands the finished 8-bit picture to the flip thread. The main thread only
 * waits here if the thread has not yet finished converting the previous
 * picture, which is what paces the game to the panel's refresh.
 */
static void VID_Present (void)
{
	static double	last_exit = 0, last_print = 0;
	static double	s_work = 0, s_wait = 0, s_snap = 0;
	static int	frames = 0;
	static double	last_handover = 0;
	double		t0 = 0, t1 = 0, t2 = 0, tw;

	if (!vid_fbuf || !snap8 || !flip_started)
		return;
	if (vid_3dscale > 1 && !snap_hi)
		return;

	/* Miyoo: how long the game works between two hand-overs (smoothed),
	 * for the flip thread's deadline (conv_picture) */
	tw = now_ms ();
	if (last_handover > 0)
	{
		double w = tw - last_handover;
		if (w > 100.0)
			w = 100.0;
		vid_work_ema += (w - vid_work_ema) * 0.1;
	}
	if (opt_perf)
		t0 = tw;

	pthread_mutex_lock (&flip_mutex);
	while (!snap_free)
		pthread_cond_wait (&flip_cond, &flip_mutex);
	pthread_mutex_unlock (&flip_mutex);

	if (opt_perf)
		t1 = now_ms ();

	memcpy (snap8, vid_fbuf, (size_t) ui_w * ui_h);
	memcpy (snap_pal, pal32, sizeof(snap_pal));
	snap_split = (vid_3dscale > 1);
	if (snap_split)
	{
		int k;
		byte *tmp;
		/* The 3D picture is redrawn completely every frame, so instead of copying
		 * it the two buffers swap roles: the flip thread gets the one just
		 * finished and the engine draws the next frame into the other one. */
		tmp = snap_hi;
		snap_hi = vid_fbuf3d;
		vid_fbuf3d = tmp;
		snap_vr = scr_vrect3d;
		if (vid_fade3d && mainTransTable)
		{	/* the menu fade darkens the 3D picture too: same blend, fixed shade */
			for (k = 0; k < 256; k++)
				snap_palhi[k] = pal32[mainTransTable[(166 << 8) + k]];
		}
		else
			memcpy (snap_palhi, pal32, sizeof(snap_palhi));
		vid_fade3d = 0;
	}

	pthread_mutex_lock (&flip_mutex);
	snap_free = false;
	job_pending = true;
	last_handover = now_ms ();
	snap_handover = last_handover;
	snap_work = vid_work_ema;
	pthread_cond_broadcast (&flip_cond);
	pthread_mutex_unlock (&flip_mutex);

	if (opt_perf)
	{
		t2 = now_ms ();
		if (last_exit != 0)
			s_work += t0 - last_exit;	/* game + rendering since the last hand-over */
		s_wait += t1 - t0;
		s_snap += t2 - t1;
		frames++;
		if (last_print == 0)
			last_print = t2;
		else if (t2 - last_print >= 2000.0)
		{
			fprintf (stderr, "perf main: %.1f fps  work %.2f  wait %.2f  hand-over %.2f  ms/frame\n",
				frames * 1000.0 / (t2 - last_print),
				s_work / frames, s_wait / frames, s_snap / frames);
#ifdef PERF_START
			{
				double f = (double) frames;
				double sum3d = pf_ms[PF_SETUP] + pf_ms[PF_WBUILD] + pf_ms[PF_WSAVE] + pf_ms[PF_WSCAN] +
						pf_ms[PF_MODELS] + pf_ms[PF_TRANS] + pf_ms[PF_VMODEL] +
						pf_ms[PF_PARTS] + pf_ms[PF_WARP] + pf_ms[PF_SNDX];
				fprintf (stderr, "perf phases: input %.2f  server %.2f  client %.2f | screen %.2f = 3D %.2f + 2D %.2f | sound %.2f  ms/frame\n",
					pf_ms[PF_INPUT] / f, pf_ms[PF_SERVER] / f, pf_ms[PF_CLIENT] / f,
					pf_ms[PF_SCREEN] / f, pf_ms[PF_VIEW3D] / f,
					(pf_ms[PF_SCREEN] - pf_ms[PF_VIEW3D]) / f, pf_ms[PF_SOUND] / f);
				fprintf (stderr, "perf 3D: setup %.2f | world: build %.2f  save %.2f  scan %.2f (textures %.2f, lit-rebuild %.2f) | models %.2f | translucent %.2f (drawing %.2f) | vmodel %.2f  parts %.2f  warp %.2f  snd-extra %.2f  other %.2f  ms/frame\n",
					pf_ms[PF_SETUP] / f, pf_ms[PF_WBUILD] / f, pf_ms[PF_WSAVE] / f,
					pf_ms[PF_WSCAN] / f, pf_ms[PF_WSPANS] / f, pf_ms[PF_SBUILD] / f, pf_ms[PF_MODELS] / f,
					pf_ms[PF_TRANS] / f, pf_ms[PF_TSPANS] / f, pf_ms[PF_VMODEL] / f,
					pf_ms[PF_PARTS] / f, pf_ms[PF_WARP] / f, pf_ms[PF_SNDX] / f,
					(pf_ms[PF_VIEW3D] - sum3d) / f);
				fprintf (stderr, "perf counts per frame: world polys %.0f  surfaces drawn %.0f  models %.1f  surfaces rebuilt %.1f  translucent lines %.0f  second-pass frames %.0f of %d\n",
					pc_sum[PC_WPOLY] / f, pc_sum[PC_EPOLY] / f, pc_sum[PC_AMODELS] / f, pc_sum[PC_SURF] / f,
					pc_sum[PC_TLINES] / f, pc_sum[PC_FALLBACK], frames);
				fprintf (stderr, "perf models: triangles %.0f  pixels %.0f per frame | with the weapon: setup %.2f  vertices %.2f  triangles %.2f  waited for the second core %.2f  ms/frame | its rows from %.0f%% (weapon %.0f%%)\n",
					pc_sum[PC_MTRIS] / f, pc_sum[PC_MPIX] / f,
					pf_ms[PF_MSETUP] / f, pf_ms[PF_MVERTS] / f, pf_ms[PF_MTRI] / f, pf_ms[PF_MPWAIT] / f,
					pc_sum[PC_MSPLIT] / f, pc_sum[PC_MSPLITW] / f);
				fprintf (stderr, "perf model kinds: whole in view %.0f tris %.2f ms | partly off the view %.0f tris %.2f ms | small far %.0f tris %.2f ms | drawing calls on the first core ~%.2f ms | hidden behind walls, skipped %.1f models  per frame\n",
					pc_sum[PC_MTU] / f, (pf_ms[PF_MTRI] - pf_ms[PF_MTRIC] - pf_ms[PF_MTRIS]) / f,
					pc_sum[PC_MTC] / f, pf_ms[PF_MTRIC] / f, pc_sum[PC_MTS] / f, pf_ms[PF_MTRIS] / f,
					pf_ms[PF_MRAST] / f, pc_sum[PC_MOCCL] / f);
				fprintf (stderr, "perf second core: share %.0f%% of the rows (water %.0f%%)  drawing %.2f ms/frame  first core waited %.2f ms/frame (water %.2f, cache %.2f, half-texture %.2f)  jobs %.0f/frame  whole textures %.1f/frame | first core took back %.1f jobs/frame (%.2f ms) | texture cache too small in %.0f frames\n",
					d_mt_share * 100.0f, pc_sum[PC_SHARETR] / f, pf_ms[PF_MTBUSY] / f, pf_ms[PF_MTWAIT] / f,
					pf_ms[PF_MTWAITT] / f, pf_ms[PF_MTWAITC] / f, pf_ms[PF_MTWAITU] / f,
					pc_sum[PC_MTJOBS] / f, pc_sum[PC_LITDEF] / f,
					pc_sum[PC_MTSTEAL] / f, pf_ms[PF_MTSTEAL] / f, pc_sum[PC_THRASH]);
				memset (pf_ms, 0, sizeof(pf_ms));
				memset (pc_sum, 0, sizeof(pc_sum));
			}
#endif
			s_work = s_wait = s_snap = 0;
			frames = 0;
			last_print = t2;
		}
		last_exit = t2;
	}
}

/* ------------------------------------------------------------------------ */
/* the usual video driver interface                                          */
/* ------------------------------------------------------------------------ */

qboolean VID_HasMouseOrInputFocus (void)
{
	return true;
}

qboolean VID_IsMinimized (void)
{
	return false;
}

static void ClearAllStates (void)
{
	Key_ClearStates ();
	IN_ClearStates ();
}

static qboolean VID_AllocBuffers (int width, int height)
{
	int	tsize, tbuffersize;

	tbuffersize = width * height * sizeof (*d_pzbuffer);

	tsize = D_SurfaceCacheForRes (width, height);
	/* Miyoo: twice the usual texture cache (unless -surfcachesize says
	 * otherwise): at 640x480 one frame's textures can fill the usual size,
	 * and each texture pushed out is built again the next frame. It is
	 * outside the game's memory (malloc), so the levels keep all of it. */
	if (!COM_CheckParm ("-surfcachesize"))
		tsize *= 2;

	/* see if there's enough memory, allowing for the pixel, z and surface
	 * buffers (same estimate the SDL driver uses) */
	if (host_parms->memsize < tbuffersize + D_SurfaceCacheForRes (width, height) + 0x180000 + 0xC00000)
	{
		Con_SafePrintf ("Not enough memory for video mode\n");
		return false;
	}

	vid_surfcachesize = tsize;

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (VID_highhunkmark);
		d_pzbuffer = NULL;
	}

	VID_highhunkmark = Hunk_HighMark ();

	d_pzbuffer = (short *) Hunk_HighAllocName (tbuffersize, "video");

	free (vid_surfcache);
	vid_surfcache = (byte *) malloc ((size_t) tsize);
	if (!vid_surfcache)
		Sys_Error ("Not enough memory for the texture cache (%d bytes)", tsize);

	return true;
}

static qboolean VID_SetMode (int width, int height, const unsigned char *palette)
{
	in_mode_set = true;

	/* width x height is the size of the 3D picture. In split mode the rest of
	 * the game works on a picture vid_3dscale times smaller. */
	v3_w = width;
	v3_h = height;
	ui_w = width / vid_3dscale;
	ui_h = height / vid_3dscale;

	if (vid_fbuf)
		free (vid_fbuf);
	vid_fbuf = (byte *) calloc (1, (size_t) ui_w * ui_h);
	if (!vid_fbuf)
		return false;

	if (vid_fbuf3d)
	{
		free (vid_fbuf3d);
		vid_fbuf3d = NULL;
	}
	if (vid_3dscale > 1)
	{
		vid_fbuf3d = (byte *) calloc (1, (size_t) v3_w * v3_h);
		if (!vid_fbuf3d)
			return false;
	}

	/* the copies of the pictures the flip thread works on, and the scaler maps */
	if (snap8)
		free (snap8);
	snap8 = (byte *) calloc (1, (size_t) ui_w * ui_h);
	if (!snap8)
		return false;
	if (snap_hi)
	{
		free (snap_hi);
		snap_hi = NULL;
	}
	if (vid_3dscale > 1)
	{
		snap_hi = (byte *) calloc (1, (size_t) v3_w * v3_h);
		if (!snap_hi)
			return false;
	}
	snap_w = ui_w;
	snap_h = ui_h;
	{
		int k;
		for (k = 0; k < PANEL_W; k++)
			xmap[k] = (uint16_t) (k * ui_w / PANEL_W);
		for (k = 0; k < PANEL_H; k++)
			ymap[k] = (uint16_t) (k * ui_h / PANEL_H);
	}

	/* what the engine sees by default: the 2D picture */
	vid.height = vid.conheight = ui_h;
	vid.width = vid.conwidth = ui_w;
	vid.buffer = vid.conbuffer = vid.direct = (pixel_t *) vid_fbuf;
	vid.rowbytes = vid.conrowbytes = ui_w;
	vid.numpages = 1;
	vid.aspect = ((float)ui_h / (float)ui_w) * (320.0 / 240.0);

	/* z buffer and surface cache are for the 3D picture */
	if (!VID_AllocBuffers (v3_w, v3_h))
		return false;

	D_InitCaches (vid_surfcache, vid_surfcachesize);

	modestate = MS_FULLDIB;
	Cvar_SetValueQuick (&vid_config_swx, width);
	Cvar_SetValueQuick (&vid_config_swy, height);

	IN_HideMouse ();

	ClearAllStates ();

	VID_SetPalette (palette);

	if (vid_3dscale > 1)
		Con_SafePrintf ("Video Mode: 3D %dx%dx8 + interface %dx%d (panel %dx%d)\n",
				v3_w, v3_h, ui_w, ui_h, PANEL_W, PANEL_H);
	else
		Con_SafePrintf ("Video Mode: %ux%ux8 (panel %dx%d)\n",
				vid.width, vid.height, PANEL_W, PANEL_H);

	in_mode_set = false;
	vid.recalc_refdef = 1;

	return true;
}

/*
 * Split mode: while the 3D view is calculated (and while R_ViewChanged works
 * out its tables) the engine must see the size of the big 3D picture.
 * VID_Use3D() switches vid.width/height/rowbytes/buffer to it, VID_Use2D()
 * puts the 2D values back. Calls nest. Does nothing when not split.
 */
void VID_Use3D (void)
{
	if (vid_3dscale <= 1 || !vid_fbuf3d)
		return;
	if (use3d_depth++ > 0)
		return;

	saved2d.width = vid.width;		saved2d.height = vid.height;
	saved2d.rowbytes = vid.rowbytes;
	saved2d.conwidth = vid.conwidth;	saved2d.conheight = vid.conheight;
	saved2d.conrowbytes = vid.conrowbytes;
	saved2d.buffer = vid.buffer;		saved2d.conbuffer = vid.conbuffer;
	saved2d.direct = vid.direct;

	vid.width = vid.conwidth = v3_w;
	vid.height = vid.conheight = v3_h;
	vid.rowbytes = vid.conrowbytes = v3_w;
	vid.buffer = vid.conbuffer = vid.direct = (pixel_t *) vid_fbuf3d;
}

void VID_Use2D (void)
{
	if (vid_3dscale <= 1 || use3d_depth == 0)
		return;
	if (--use3d_depth > 0)
		return;

	vid.width = saved2d.width;		vid.height = saved2d.height;
	vid.rowbytes = saved2d.rowbytes;
	vid.conwidth = saved2d.conwidth;	vid.conheight = saved2d.conheight;
	vid.conrowbytes = saved2d.conrowbytes;
	vid.buffer = saved2d.buffer;		vid.conbuffer = saved2d.conbuffer;
	vid.direct = saved2d.direct;
}

/*
 * Split mode: the 3D view area of the 2D picture is empty (transparent) from
 * now on. The engine used to overwrite that area with the 3D view every
 * frame, so whatever it still wants there it will draw again.
 */
void VID_ClearUIView (void)
{
	int	y, x0, x1;

	vid_fade3d = 0;
	if (vid_3dscale <= 1 || !vid_fbuf)
		return;

	x0 = (scr_vrect.x < 0) ? 0 : scr_vrect.x;
	x1 = (scr_vrect.x + scr_vrect.width > ui_w) ? ui_w : scr_vrect.x + scr_vrect.width;
	if (x1 <= x0)
		return;
	for (y = scr_vrect.y; y < scr_vrect.y + scr_vrect.height; y++)
	{
		if (y >= 0 && y < ui_h)
			memset (vid_fbuf + y * ui_w + x0, TRANSPARENT_COLOR, (size_t) (x1 - x0));
	}
}

void VID_LockBuffer (void)
{
	lockcount++;

	if (lockcount > 1)
		return;

	vid.buffer = vid.conbuffer = vid.direct =
		(pixel_t *) ((use3d_depth > 0) ? vid_fbuf3d : vid_fbuf);
	vid.rowbytes = vid.conrowbytes = vid.width;

	if (r_dowarp)
		d_viewbuffer = r_warpbuffer;
	else
		d_viewbuffer = vid.buffer;

	if (r_dowarp)
		screenwidth = WARP_WIDTH;
	else
		screenwidth = vid.rowbytes;
}

void VID_UnlockBuffer (void)
{
	lockcount--;

	if (lockcount > 0)
		return;

	if (lockcount < 0)
		Sys_Error ("Unbalanced unlock");
}

void VID_SetPalette (const unsigned char *palette)
{
	int	i;
	const unsigned char *p = palette;

	palette_changed = true;

	if (palette != vid_curpal)
		memcpy (vid_curpal, palette, sizeof(vid_curpal));

	for (i = 0; i < 256; ++i, p += 3)
		pal32[i] = 0xFF000000u | ((uint32_t) p[0] << 16) |
				((uint32_t) p[1] << 8) | (uint32_t) p[2];
}

void VID_ShiftPalette (const unsigned char *palette)
{
	VID_SetPalette (palette);
}

void VID_Init (const unsigned char *palette)
{
	int	width, height, i, temp;

	opt_perf = (COM_CheckParm ("-perf") != 0);
#ifdef PERF_START
	vid_perf = opt_perf;
#endif
	opt_nopin = (COM_CheckParm ("-nopin") != 0);

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;

	Cvar_RegisterVariable (&vid_config_fscr);
	Cvar_RegisterVariable (&vid_config_swy);
	Cvar_RegisterVariable (&vid_config_swx);
	Cvar_RegisterVariable (&vid_config_gly);
	Cvar_RegisterVariable (&vid_config_glx);
	Cvar_RegisterVariable (&_enable_mouse);
	Cvar_RegisterVariable (&vid_showload);

	if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
	{
		if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
			Sys_Error ("Couldn't init video: %s", SDL_GetError());
	}

	/* SDL's fbcon driver owns the keyboard; take its surface in its native
	 * 32-bit format and leave drawing to us. */
	sdlscreen = SDL_SetVideoMode (PANEL_W, PANEL_H, 32, SDL_HWSURFACE | SDL_NOFRAME);
	if (!sdlscreen)
		Sys_Error ("Couldn't set video mode: %s", SDL_GetError());
	SDL_ShowCursor (0);

	fb_init ();

	outbuf = (uint32_t *) calloc (1, (size_t) PANEL_W * PANEL_H * 4);
	if (!outbuf)
		Sys_Error ("Couldn't allocate the video output buffer");

	/* 320x240 by default; -width 400 / 480 / 640 give 400x300 / 480x360 / 640x480 */
	width = 320;
	height = 240;
	i = COM_CheckParm ("-width");
	if (i && i < com_argc - 1)
	{
		int wv = atoi (com_argv[i + 1]);
		if (wv == 400 || wv == 480 || wv == 640)
		{
			width = wv;
			height = wv * 3 / 4;
		}
		else if (wv != 320)
			Con_Printf ("-width %d ignored (use 320, 400, 480 or 640)\n", wv);
	}

	/* -ui320: 3D at 640x480, everything else at 320x240 */
	vid_3dscale = 1;
	if (COM_CheckParm ("-ui320"))
	{
		width = PANEL_W;
		height = PANEL_H;
		vid_3dscale = 2;
	}

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	if (!VID_SetMode (width, height, palette))
		Sys_Error ("Couldn't set video mode %dx%d", width, height);

	/* the game itself stays on core 0, the flip thread takes core 1 */
	pin_cpu (0);
	flip_begin ();

	scr_disabled_for_loading = temp;
	vid_initialized = true;

	/* no video menu on this driver */
	vid_menudrawfn = NULL;
	vid_menukeyfn = NULL;
}

void VID_Shutdown (void)
{
	if (!vid_initialized)
		return;

	vid_initialized = false;

	flip_end ();
	fb_restore ();

	SDL_QuitSubSystem (SDL_INIT_VIDEO);
}

void VID_Update (vrect_t *rects)
{
	(void) rects;	/* the whole picture is always converted */

	palette_changed = false;

	VID_Present ();

	/* handle the mouse state if that's changed */
	if (_enable_mouse.integer != enable_mouse)
	{
		if (_enable_mouse.integer)
			IN_ActivateMouse ();
		else	IN_DeactivateMouse ();

		enable_mouse = _enable_mouse.integer;
	}
}

void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
	byte	*offset;

	if (!vid_fbuf)
		return;
	if (x < 0)
		x = vid.width + x - 1;
	offset = vid_fbuf + y * vid.width + x;
	while (height--)
	{
		memcpy (offset, pbitmap, width);
		offset += vid.width;
		pbitmap += width;
	}
}

void D_EndDirectRect (int x, int y, int width, int height)
{
	(void) x; (void) y; (void) width; (void) height;
	VID_Present ();
}

#ifndef H2W /* not used in hexenworld */
void D_ShowLoadingSize (void)
{
	#ifdef DRAW_PROGRESSBARS
	static int prev_perc;
	int		cur_perc;
	viddef_t	save_vid;	/* global video state */

	if (!vid_showload.integer)
		return;

	if (!vid_initialized)
		return;

	cur_perc = loading_stage * 100;
	if (total_loading_size)
		cur_perc += current_loading_size * 100 / total_loading_size;
	if (cur_perc == prev_perc)
		return;
	prev_perc = cur_perc;

	save_vid = vid;

	VID_LockBuffer ();

	if (!vid.direct)
		Sys_Error ("NULL vid.direct pointer");

	vid.buffer = vid.direct;

	SCR_DrawLoading ();

	VID_UnlockBuffer ();

	VID_Present ();

	vid = save_vid;
	#endif
}
#endif

void VID_HandlePause (qboolean paused)
{
	if (_enable_mouse.integer)
	{
		if (paused)
			IN_DeactivateMouse ();
		else	IN_ActivateMouse ();
	}
}

/* there is no windowed mode on this device */
void VID_ToggleFullscreen (void)
{
}
