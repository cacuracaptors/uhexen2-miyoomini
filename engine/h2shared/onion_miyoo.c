/*
 * onion_miyoo.c -- OnionOS: the Game Switcher and the save at power-off
 * (Miyoo Mini port). The same as the Tomb Raider (OpenLara) port's onion.h.
 *
 * MENU on its own does what OnionOS's Tweaks set for games ("In-game: Menu
 * button", config/keymap.json, read as keymon reads it): by default a press
 * opens the Game Switcher, holding it (0.7 s) goes back to the OnionOS menu
 * and a double press (within 0.3 s) switches to the previous game. Each one
 * first saves the game as "resume" (a save of its own, which the Load menu
 * does not list, apart from the quick save) and writes the picture the Game
 * Switcher shows. MENU with another button (Menu+Start opens the game's menu,
 * Menu+L2/R2 quick load/save, OnionOS's Menu+Power screenshot, Menu+volume
 * brightness...) does none of them: the buttons are read straight from
 * /dev/input/event0 for that, POWER included.
 *
 * Opened again from the Game Switcher (OnionOS leaves "switcher" in
 * /tmp/prev_state), the game goes straight on from "resume"; opened from the
 * game list, it starts as always.
 *
 * Power-off: OnionOS creates /tmp/.offOrder and sends SIGTERM (the signal does
 * not always arrive, so the file is checked 4 times a second). The game saves
 * the same way, leaves resume_boot in its folder and puts back cmd_to_run.sh
 * (which keymon removes), so OnionOS opens the game again at the next start
 * and it goes on from there. A script in .tmp_update/checkoff makes OnionOS
 * wait for the save (20 s at most). Where the game cannot be saved (the menu
 * before a game starts, a dead player, an intermission...) it just closes, and
 * starts as always.
 *
 * The Game Switcher's picture: /mnt/SDCARD/Saves/CurrentProfile/romScreens/
 * <hash>.png, the hash being OnionOS's FNV1A_Pippip_Yurii of the game's path in
 * the recent list (the .port shortcut), as it writes it for RetroArch.
 *
 * GPL v2 or later, like the rest of the engine.
 */

#include "quakedef.h"
#include "onion_miyoo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/fb.h>

#define ON_DIR		"/mnt/SDCARD/.tmp_update"
#define ON_CMD		ON_DIR "/cmd_to_run.sh"
#define ON_SWITCHER	ON_DIR "/.runGameSwitcher"
#define ON_CHECKOFF	ON_DIR "/checkoff"
#define ON_SCRIPT	ON_CHECKOFF "/hexen2_save.sh"
#define ON_KEYMAP	ON_DIR "/config/keymap.json"
#define ON_ROMSCREENS	"/mnt/SDCARD/Saves/CurrentProfile/romScreens"
#define ON_RECENT	"/mnt/SDCARD/Roms/recentlist.json"
#define ON_RECENT_HID	"/mnt/SDCARD/Roms/recentlist-hidden.json"
#define ON_BOOT		"resume_boot"	/* in the game's folder: turned off in a game */
#define ON_SAVE		"resume"	/* the save of its own, in userdir/data1 */

enum { EXIT_SWITCHER, EXIT_MENU, EXIT_QUICK_SWITCH, EXIT_POWEROFF };
enum { MENU_OFF, MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH, MENU_RA_MENU };	/* keymon's actions */
enum { PRESS_SINGLE, PRESS_LONG, PRESS_DOUBLE };

static qboolean	onion, offAtStart, checkoff, resumeAtStart, started, quitting;
static char	cmd[4096];
static int	cmdLen = -1;
static char	romscreen[200];
static int	menuAction[3] = { MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH };
static volatile sig_atomic_t offSignal;
static double	nextOffPoll;

static double now_s (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static qboolean exists (const char *path)
{
	return access (path, F_OK) == 0;
}

/* the whole file at once: beside the old one, then renamed over it */
static qboolean writeFile (const char *path, const void *data, size_t len)
{
	char		tmp[300];
	const uint8_t	*p = (const uint8_t *) data;
	size_t		left = len;
	int		fd;

	snprintf (tmp, sizeof(tmp), "%s.tmp", path);
	fd = open (tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	while (left)
	{
		ssize_t w = write (fd, p, left);
		if (w <= 0)
		{
			close (fd);
			remove (tmp);
			return false;
		}
		p += w;
		left -= (size_t) w;
	}
	fsync (fd);
	close (fd);
	if (rename (tmp, path) != 0)
	{
		remove (tmp);
		return false;
	}
	return true;
}

static void touch (const char *path)
{
	writeFile (path, "", 0);
}

/* OnionOS's FNV1A_Pippip_Yurii (src/common/utils/hash.h) */
static uint32_t hashPath (const char *str, size_t len)
{
	const uint32_t	prime = 591798841u;
	uint64_t	h = 14695981039346656037ULL;
	uint32_t	h32;

	if (len > 8)
	{
		size_t cycles = ((len - 1) >> 4) + 1;
		const size_t ndhead = len - (cycles << 3);
		for (; cycles--; str += 8)
		{
			uint64_t a, b;
			memcpy (&a, str, 8);
			memcpy (&b, str + ndhead, 8);
			h = (h ^ a) * prime;
			h = (h ^ b) * prime;
		}
	}
	else
	{
		uint64_t a = 0;
		memcpy (&a, str, len);
		h = (h ^ a) * prime;
	}
	h32 = (uint32_t) (h ^ (h >> 32));
	return h32 ^ (h32 >> 16);
}

/* ---- the Game Switcher's picture (PNG, stored without compression) ---- */
static uint32_t crcTable[256];

static uint32_t crc32_ (uint32_t crc, const uint8_t *p, size_t n)
{
	if (!crcTable[1])
	{
		uint32_t i;
		for (i = 0; i < 256; i++)
		{
			uint32_t c = i;
			int k;
			for (k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crcTable[i] = c;
		}
	}
	crc = ~crc;
	while (n--)
		crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static uint8_t *put32 (uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);  p[3] = (uint8_t) v;
	return p + 4;
}

/* the page on screen (32 bit, the panel mounted upside down: it runs backwards) */
static void writeRomscreen (void)
{
	const int	W = 640, H = 480;
	const uint32_t	*page = (const uint32_t *) VID_ScreenPage ();
	size_t		stride, n, blocks, zlen, off;
	uint8_t		*file, *raw, *p, *c, *z;
	uint32_t	a = 1, b = 0;
	qboolean	ok;
	int		x, y;
	double		t0 = now_s ();

	if (!romscreen[0] || !page)
		return;
	stride = (size_t) W * 3 + 1;
	n = stride * H;
	blocks = (n + 65534) / 65535;
	zlen = 2 + n + blocks * 5 + 4;
	file = (uint8_t *) malloc (8 + 25 + 12 + zlen + 12);
	raw = (uint8_t *) malloc (n);
	if (!file || !raw)
	{
		free (file);
		free (raw);
		return;
	}
	for (y = 0; y < H; y++)
	{
		uint8_t *o = raw + (size_t) y * stride;
		const uint32_t *s = page + (size_t) W * H - 1 - (size_t) y * W;
		*o++ = 0;	/* filter: none */
		for (x = 0; x < W; x++)
		{
			const uint32_t px = *s--;
			*o++ = (uint8_t) (px >> 16);
			*o++ = (uint8_t) (px >> 8);
			*o++ = (uint8_t) px;
		}
	}
	p = file;
	{
		static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
		memcpy (p, sig, 8);
		p += 8;
	}
	c = p;
	p = put32 (p, 13); memcpy (p, "IHDR", 4); p += 4;
	p = put32 (p, W); p = put32 (p, H);
	*p++ = 8; *p++ = 2; *p++ = 0; *p++ = 0; *p++ = 0;
	p = put32 (p, crc32_ (0, c + 4, 17));
	c = p;
	p = put32 (p, (uint32_t) zlen); memcpy (p, "IDAT", 4); p += 4;
	z = p;
	*p++ = 0x78; *p++ = 0x01;
	for (off = 0; off < n; off++)
	{
		a = (a + raw[off]) % 65521u;
		b = (b + a) % 65521u;
	}
	for (off = 0; off < n;)
	{
		const size_t len = n - off > 65535 ? 65535 : n - off;
		*p++ = (off + len >= n) ? 1 : 0;
		*p++ = (uint8_t) len; *p++ = (uint8_t) (len >> 8);
		*p++ = (uint8_t) ~len; *p++ = (uint8_t) (~len >> 8);
		memcpy (p, raw + off, len);
		p += len;
		off += len;
	}
	p = put32 (p, (b << 16) | a);
	p = put32 (p, crc32_ (0, c + 4, (size_t) (p - z) + 4));
	c = p;
	p = put32 (p, 0); memcpy (p, "IEND", 4); p += 4;
	p = put32 (p, crc32_ (0, c + 4, 4));
	mkdir (ON_ROMSCREENS, 0777);
	ok = writeFile (romscreen, file, (size_t) (p - file));
	free (raw);
	free (file);
	fprintf (stderr, "onion: imagem do Game Switcher %s (%.0f ms)\n", ok ? "gravada" : "FALHOU", (now_s () - t0) * 1000.0);
}

/* the game's line in OnionOS's recent list: the path the Game Switcher hashes */
static void findRomscreen (void)
{
	char		cwd[256], path[512];
	static char	line[8192];
	const char	*dir;
	FILE		*f;

	if (!getcwd (cwd, sizeof(cwd)))
		return;
	dir = strrchr (cwd, '/');
	dir = dir ? dir + 1 : cwd;	/* the game's folder: "Hexen2" */
	f = fopen (exists (ON_RECENT_HID) ? ON_RECENT_HID : ON_RECENT, "r");
	if (!f)
		return;
	while (fgets (line, sizeof(line), f))
	{
		const char *t = strstr (line, "\"type\":");
		const char *r = strstr (line, "\"rompath\":\"");
		const char *e, *colon, *rom;
		qboolean ours;
		int type;

		if (!t || !r)
			continue;
		type = atoi (t + 7);
		if (type != 5 && type != 17)
			continue;
		r += 11;
		e = strchr (r, '"');
		if (!e || e - r >= (int) sizeof(path))
			continue;
		memcpy (path, r, (size_t) (e - r));
		path[e - r] = 0;
		colon = strchr (path, ':');	/* "launch:rompath" (the search panel) */
		rom = colon ? colon + 1 : path;
		ours = strstr (rom, dir) != NULL;
		if (!ours)
		{	/* the .port shortcut names the folder (GameDir="Hexen2") */
			FILE *s = fopen (rom, "r");
			if (s)
			{
				char buf[4096];
				const size_t k = fread (buf, 1, sizeof(buf) - 1, s);
				buf[k] = 0;
				fclose (s);
				ours = strstr (buf, dir) != NULL;
			}
		}
		if (ours)
		{
			snprintf (romscreen, sizeof(romscreen), ON_ROMSCREENS "/%u.png", (unsigned) hashPath (rom, strlen (rom)));
			fprintf (stderr, "onion: na lista de recentes como %s -> %s\n", rom, romscreen);
			break;
		}
	}
	fclose (f);
}

/* OnionOS's settings for MENU in a game (keymon: _settings_load_keymap and the old flags) */
static int jsonInt (const char *json, const char *key, int fallback)
{
	char		k[64];
	const char	*p;

	snprintf (k, sizeof(k), "\"%s\"", key);
	p = strstr (json, k);
	if (!p || !(p = strchr (p + strlen (k), ':')))
		return fallback;
	return atoi (p + 1);
}

static void readMenuSettings (void)
{
	FILE *f = fopen (ON_KEYMAP, "r");
	if (f)
	{
		char buf[2048];
		const size_t n = fread (buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose (f);
		menuAction[PRESS_SINGLE] = jsonInt (buf, "ingame_single_press", menuAction[PRESS_SINGLE]);
		menuAction[PRESS_LONG]   = jsonInt (buf, "ingame_long_press",   menuAction[PRESS_LONG]);
		menuAction[PRESS_DOUBLE] = jsonInt (buf, "ingame_double_press", menuAction[PRESS_DOUBLE]);
	}
	if (exists (ON_DIR "/config/.menuInverted"))
	{
		menuAction[PRESS_SINGLE] = MENU_EXIT;
		menuAction[PRESS_LONG]   = MENU_SWITCHER;
	}
	if (exists (ON_DIR "/config/.noGameSwitcher"))
	{
		menuAction[PRESS_SINGLE] = MENU_EXIT;
		menuAction[PRESS_LONG]   = MENU_OFF;
	}
	fprintf (stderr, "onion: MENU (Tweaks): toque %d, segurar %d, dois toques %d (1 Game Switcher, 2 menu, 3 jogo anterior)\n",
			menuAction[PRESS_SINGLE], menuAction[PRESS_LONG], menuAction[PRESS_DOUBLE]);
}

/* the previous game, as keymon's quick switch does it: the second game of the
 * recent list moves to the top, its command goes into cmd_to_run.sh, and
 * /tmp/quick_switch has runtime.sh start it when this game ends */
static qboolean quickSwitchPrepare (void)
{
	static char	lines[64][2048];
	static char	next[2200];
	static char	out[64 * 2048];
	const char	*list = exists (ON_RECENT_HID) ? ON_RECENT_HID : ON_RECENT;
	int		n = 0, pick = -1, valid = 0;
	qboolean	complete;
	FILE		*f = fopen (list, "r");

	if (!f)
		return false;
	next[0] = 0;
	while (n < 64 && fgets (lines[n], sizeof(lines[n]), f))
	{
		const char *line = lines[n++];
		const char *t, *r, *e;
		char rom[1024], launch[1024] = "", *colon;
		int type;

		if (pick >= 0)
			continue;
		t = strstr (line, "\"type\":");
		r = strstr (line, "\"rompath\":\"");
		if (!t || !r)
			continue;
		type = atoi (t + 7);
		if (type != 5 && type != 17)
			continue;
		r += 11;
		e = strchr (r, '"');
		if (!e || e - r >= (int) sizeof(rom))
			continue;
		memcpy (rom, r, (size_t) (e - r));
		rom[e - r] = 0;
		colon = strchr (rom, ':');
		if (colon)
		{	/* "launch:rompath" */
			*colon = 0;
			snprintf (launch, sizeof(launch), "%s", rom);
			memmove (rom, colon + 1, strlen (colon + 1) + 1);
		}
		else
		{
			const char *l = strstr (line, "\"launch\":\"");
			const char *le = l ? strchr (l + 10, '"') : NULL;
			if (le && le - (l + 10) < (int) sizeof(launch))
			{
				memcpy (launch, l + 10, (size_t) (le - (l + 10)));
				launch[le - (l + 10)] = 0;
			}
		}
		if (!exists (rom) || !launch[0] || !exists (launch))
			continue;
		if (valid++ == 1)
		{
			pick = n - 1;
			snprintf (next, sizeof(next), "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"", launch, rom);
		}
	}
	complete = feof (f) != 0;
	fclose (f);
	if (pick < 0)
		return false;
	if (pick > 0 && complete)
	{	/* (a list longer than read here is left as it is) */
		size_t len = 0;
		int i;
		for (i = -1; i < n; i++)
		{
			const int k = i < 0 ? pick : i;
			size_t l;
			if (i == pick)
				continue;
			l = strlen (lines[k]);
			memcpy (out + len, lines[k], l);
			len += l;
		}
		writeFile (list, out, len);
	}
	if (!writeFile (ON_CMD, next, strlen (next)))
		return false;
	touch ("/tmp/quick_switch");
	touch ("/tmp/force_auto_load_state");
	fprintf (stderr, "onion: jogo anterior: %s\n", next);
	return true;
}

static void putBackCmd (void)
{
	if (cmdLen > 0 && !exists (ON_CMD))
	{
		const qboolean ok = writeFile (ON_CMD, cmd, (size_t) cmdLen);
		fprintf (stderr, "onion: cmd_to_run.sh devolvido (%s)\n", ok ? "ok" : "FALHOU");
	}
}

static void makeCheckoffScript (void)
{
	char		exe[256] = "hexen2", link[256], s[512];
	const ssize_t	k = readlink ("/proc/self/exe", link, sizeof(link) - 1);
	int		n;

	if (k > 0)
	{
		const char *b;
		link[k] = 0;
		b = strrchr (link, '/');
		snprintf (exe, sizeof(exe), "%s", b ? b + 1 : link);
	}
	mkdir (ON_CHECKOFF, 0777);
	n = snprintf (s, sizeof(s),
		"#!/bin/sh\n"
		"# Hexen II: OnionOS waits here (20 s at most) while the game saves at power-off\n"
		"i=0\n"
		"while grep -q '%s' /proc/%d/cmdline 2>/dev/null && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done\n",
		exe, (int) getpid ());
	checkoff = writeFile (ON_SCRIPT, s, (size_t) n);
	if (checkoff)
		chmod (ON_SCRIPT, 0777);
}

static void onSigterm (int sig)
{
	(void) sig;
	offSignal = 1;
}

/* ---- the buttons, straight from evdev: MENU's presses ---- */
static int	evFd = -1;
static uint8_t	keysDown[KEY_MAX / 8 + 1];
static qboolean	menuDown, menuMixed, menuLongDone, menuSingleWait, menuSkipRelease;
static double	menuPressT;
static int	menuPress = -1;
#define ON_MENU_LONG	0.7
#define ON_MENU_DOUBLE	0.3

static qboolean othersDown (void)
{
	int i;
	for (i = 0; i < (int) sizeof(keysDown); i++)
	{
		uint8_t m = keysDown[i];
		if (i == KEY_ESC / 8)
			m &= (uint8_t) ~(1u << (KEY_ESC % 8));
		if (m)
			return true;
	}
	return false;
}

static void pollKeys (void)
{
	struct input_event	ev[32];
	const double		now = now_s ();

	if (evFd < 0)
		return;
	for (;;)
	{
		const ssize_t r = read (evFd, ev, sizeof(ev));
		int i, n;
		if (r <= 0)
			break;
		n = (int) (r / sizeof(ev[0]));
		for (i = 0; i < n; i++)
		{
			int code;
			if (ev[i].type != EV_KEY)
				continue;
			code = ev[i].code;
			if (code >= 0 && code < KEY_MAX)
			{
				if (ev[i].value)
					keysDown[code / 8] |= (uint8_t) (1u << (code % 8));
				else	keysDown[code / 8] &= (uint8_t) ~(1u << (code % 8));
			}
			if (code == KEY_ESC && ev[i].value == 1)
			{	/* MENU pressed */
				if (menuSingleWait && now - menuPressT <= ON_MENU_DOUBLE)
				{	/* the second press: double */
					menuSingleWait = false;
					menuSkipRelease = true;
					menuPress = PRESS_DOUBLE;
				}
				else	menuSkipRelease = false;
				menuDown = true;
				menuMixed = othersDown ();
				menuLongDone = false;
				menuPressT = now;
			}
			else if (code == KEY_ESC && ev[i].value == 0)
			{	/* MENU released */
				if (menuDown && !menuMixed && !menuLongDone && !menuSkipRelease && !othersDown ())
				{
					if (menuAction[PRESS_DOUBLE] != MENU_OFF)
						menuSingleWait = true;	/* single, unless a second press comes */
					else	menuPress = PRESS_SINGLE;
				}
				menuDown = false;
				menuSkipRelease = false;
			}
			else if (code != KEY_ESC && ev[i].value == 1 && menuDown)
				menuMixed = true;	/* another button with MENU */
		}
	}
	if (menuDown && !menuMixed && !menuLongDone && !menuSkipRelease && now - menuPressT >= ON_MENU_LONG)
	{	/* held: long press */
		menuLongDone = true;
		menuPress = PRESS_LONG;
	}
	if (menuSingleWait && now - menuPressT > ON_MENU_DOUBLE)
	{
		menuSingleWait = false;
		menuPress = PRESS_SINGLE;
	}
}

/* ---- the save of its own ("resume", in userdir/data1) ---- */

static void resumePath (char *buf, size_t size, const char *file)
{
	if (file)
		q_snprintf (buf, size, "%s/" ON_SAVE "/%s", FS_GetUserdir (), file);
	else	q_snprintf (buf, size, "%s/" ON_SAVE, FS_GetUserdir ());
}

static void resumeRemove (void)
{
	char		dir[MAX_OSPATH], f[MAX_OSPATH];
	DIR		*d;
	struct dirent	*e;

	resumePath (dir, sizeof(dir), NULL);
	d = opendir (dir);
	if (!d)
		return;
	while ((e = readdir (d)) != NULL)
	{
		if (e->d_name[0] == '.')
			continue;
		q_snprintf (f, sizeof(f), "%s/%s", dir, e->d_name);
		unlink (f);
	}
	closedir (d);
	rmdir (dir);
}

/* where the game itself lets the player save (Host_Savegame_f's checks) */
static qboolean canSave (void)
{
	int i;

	if (!sv.active || cls.state != ca_connected || cls.signon != SIGNONS)
		return false;
	if (cl.intermission || svs.maxclients != 1)
		return false;
	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active && svs.clients[i].edict->v.health <= 0)
			return false;
	}
	return true;
}

static qboolean resumeSave (void)
{
	char		info[MAX_OSPATH];
	struct stat	st;
	time_t		t0 = time (NULL);

	if (!canSave ())
		return false;
	Cmd_ExecuteString ("save " ON_SAVE, src_command);
	resumePath (info, sizeof(info), "info.dat");
	return stat (info, &st) == 0 && st.st_mtime >= t0 - 1;
}

/* "onion_resume": the game menu closed and the save loaded, as the Load menu does */
static void Onion_Resume_f (void)
{
	if (Key_GetDest () == key_menu)
		Cmd_ExecuteString ("togglemenu", src_command);
	Cmd_ExecuteString ("load " ON_SAVE, src_command);
}

/* ---- start, every frame, end ---- */

/* before the screen opens: what OnionOS left, and how the game was opened */
void Onion_Init (void)
{
	char		prev[32] = "";
	qboolean	boot;
	FILE		*f;

	onion = exists (ON_DIR);
	if (!onion)
		return;
	f = fopen (ON_CMD, "rb");
	if (f)
	{
		const size_t n = fread (cmd, 1, sizeof(cmd) - 1, f);
		if (n > 0 && n < sizeof(cmd) - 1)
			cmdLen = (int) n;
		fclose (f);
	}
	offAtStart = exists ("/tmp/.offOrder");
	makeCheckoffScript ();
	findRomscreen ();
	readMenuSettings ();
	evFd = open ("/dev/input/event0", O_RDONLY | O_NONBLOCK);

	f = fopen ("/tmp/prev_state", "r");
	if (f)
	{
		if (fgets (prev, sizeof(prev), f))
			prev[strcspn (prev, "\r\n")] = 0;
		fclose (f);
	}
	boot = exists (ON_BOOT);
	remove (ON_BOOT);
	resumeAtStart = boot || !strcmp (prev, "switcher") || !strcmp (prev, "game");
	fprintf (stderr, "onion: aberto por '%s'%s: %s\n", prev[0] ? prev : "?", boot ? " (desligou no jogo)" : "",
			resumeAtStart ? "continuando de onde parou" : "comecando do inicio");

	Cmd_AddCommand ("onion_resume", Onion_Resume_f);
}

/* once the screen is open (SDL and the framebuffer have set their own handlers) */
void Onion_Signals (void)
{
	struct sigaction sa;

	if (!onion)
		return;
	memset (&sa, 0, sizeof(sa));
	sa.sa_handler = onSigterm;
	sigaction (SIGTERM, &sa, NULL);
}

/* once the game is up (hexen.rc read): straight back to the game, or from the start */
static void Onion_Start (void)
{
	char info[MAX_OSPATH];

	resumePath (info, sizeof(info), "info.dat");
	if (resumeAtStart && exists (info))
	{
		fprintf (stderr, "onion: continuando do save do Game Switcher\n");
		Cbuf_AddText ("onion_resume\n");	/* after hexen.rc (its menu_main) */
	}
	else
	{
		if (resumeAtStart)
			fprintf (stderr, "onion: nenhum save do Game Switcher: comecando do inicio\n");
		resumeRemove ();	/* opened from the list: an old one would only be in the way */
	}
}

static void exitSaving (int kind)
{
	static const char *names[] = { "Game Switcher", "menu", "jogo anterior", "desligar" };
	qboolean saved;

	quitting = true;
	fprintf (stderr, "onion: saindo (%s)\n", names[kind]);
	writeRomscreen ();
	saved = resumeSave ();
	if (!saved)
		resumeRemove ();
	sync ();
	fprintf (stderr, "onion: %s\n", saved ? "jogo salvo para continuar" : "aqui nao da para salvar: so fechando");
	if (kind == EXIT_POWEROFF && saved)
	{
		touch (ON_BOOT);
		putBackCmd ();
	}
	if (kind == EXIT_SWITCHER)
		touch (ON_SWITCHER);
	if (kind == EXIT_QUICK_SWITCH && !quickSwitchPrepare ())
		fprintf (stderr, "onion: nenhum jogo anterior na lista: de volta ao menu\n");
	sync ();
	fflush (stderr);
	Sys_Quit ();
}

/* every frame, after the input */
void Onion_Frame (void)
{
	qboolean off;

	if (!onion || quitting)
		return;
	if (!started)
	{
		started = true;
		Onion_Start ();
	}
	pollKeys ();
	off = offSignal != 0;
	if (!off && !offAtStart)
	{
		const double now = now_s ();
		if (now >= nextOffPoll)
		{
			nextOffPoll = now + 0.25;
			off = exists ("/tmp/.offOrder");
		}
	}
	if (off)
	{
		exitSaving (EXIT_POWEROFF);
		return;
	}
	if (menuPress >= 0)
	{
		const int press = menuPress;
		menuPress = -1;
		switch (menuAction[press])
		{
		case MENU_SWITCHER:	exitSaving (EXIT_SWITCHER);	break;
		case MENU_EXIT:		exitSaving (EXIT_MENU);		break;
		case MENU_QUICK_SWITCH:	exitSaving (EXIT_QUICK_SWITCH);	break;
		default:	break;	/* (off, or RetroArch's own menu: nothing here) */
		}
	}
}

/* the game is closing (also after the menu's Quit) */
void Onion_End (void)
{
	if (checkoff)
		unlink (ON_SCRIPT);
	checkoff = false;
	if (evFd >= 0)
		close (evFd);
	evFd = -1;
}

/* ---- the console (virtual terminal) ----
 * SDL's fbcon driver, opening the screen, takes a free terminal and waits, with
 * no time limit, for the kernel to make it the active one. The kernel refuses
 * to switch, without saying so, while the active terminal is in graphics mode
 * or switching is locked, which a program that closed without putting them
 * back leaves behind (seen in the Tomb Raider port when opened from the Game
 * Switcher: the game stood still before its first frame until the power-off
 * signal broke the wait). So the active terminal goes back to text mode and
 * the lock is lifted before SDL opens the screen; and if SDL still waits, a
 * watcher interrupts the wait (a signal that does nothing, as the power-off
 * signal did): from 2 s on, once a second, for at most 10 s. */
static volatile int	consoleDone, consoleKicks;
static pthread_t	consoleMain, consoleW;
static qboolean		consoleWatching;
static double		consoleT0;

static void consoleKickHandler (int sig)
{
	(void) sig;
}

static void *consoleWatcher (void *arg)
{
	int i;
	(void) arg;
	for (i = 0; i < 24 && !consoleDone; i++)
	{
		usleep (500000);
		if (!consoleDone && i >= 3 && (i & 1))
		{
			consoleKicks++;
			pthread_kill (consoleMain, SIGRTMIN + 5);
		}
	}
	return NULL;
}

/* the screen mode the previous program left: back to 640x480, 32 bpp, two
 * pages (virtual 640x960), the way OnionOS itself sets it */
static void fbFixMode (void)
{
	struct fb_var_screeninfo v, n;
	int fd = open ("/dev/fb0", O_RDWR);
	qboolean good;
	int r;

	if (fd < 0)
		return;
	if (ioctl (fd, FBIOGET_VSCREENINFO, &v) == 0)
	{
		good = v.xres == 640 && v.yres == 480 && v.bits_per_pixel == 32;
		n = v;
		n.xres = n.xres_virtual = 640;
		n.yres = 480;
		n.yres_virtual = (good && v.yres_virtual >= 480 * 3) ? 480 * 3 : 480 * 2;
		n.xoffset = n.yoffset = 0;
		n.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		if (v.bits_per_pixel != 32)
		{
			n.bits_per_pixel = 32;
			n.red.offset = 16;	n.red.length = 8;
			n.green.offset = 8;	n.green.length = 8;
			n.blue.offset = 0;	n.blue.length = 8;
			n.transp.offset = 24;	n.transp.length = 8;
		}
		r = ioctl (fd, FBIOPUT_VSCREENINFO, &n);
		fprintf (stderr, "screen: %ux%u %ubpp (virtual %u lines), to 640x480 with %u pages: %s\n",
				v.xres, v.yres, v.bits_per_pixel, v.yres_virtual, n.yres_virtual / 480,
				r == 0 ? "ok" : strerror (errno));
		if (r == 0)	/* OnionOS tells batmon and keymon after every mode change (they draw on the screen) */
			if (system ("killall -SIGUSR1 batmon >/dev/null 2>&1; killall -SIGUSR1 keymon >/dev/null 2>&1")) {}
	}
	close (fd);
}

void Onion_BeforeScreen (void)
{
	struct vt_stat		st;
	struct sigaction	sa;
	int			fd0, freeVt = -1;

	if (!onion)
		return;
	fbFixMode ();
	fd0 = open ("/dev/tty0", O_RDWR);
	if (fd0 < 0)
		fprintf (stderr, "console: sem /dev/tty0 (%s)\n", strerror (errno));
	else
	{
		memset (&st, 0, sizeof(st));
		ioctl (fd0, VT_OPENQRY, &freeVt);
		if (ioctl (fd0, VT_GETSTATE, &st) == 0 && st.v_active > 0)
		{
			char		path[32];
			int		fd, kd = -1;
			struct vt_mode	m;

			snprintf (path, sizeof(path), "/dev/tty%d", (int) st.v_active);
			fd = open (path, O_RDWR);
			memset (&m, 0, sizeof(m));
			if (fd >= 0)
			{
				ioctl (fd, KDGETMODE, &kd);
				ioctl (fd, VT_GETMODE, &m);
			}
			fprintf (stderr, "console: ativo tty%d (%s, troca %s), livre tty%d\n", (int) st.v_active,
					kd == KD_GRAPHICS ? "modo grafico" : kd == KD_TEXT ? "modo texto" : "modo ?",
					m.mode == VT_PROCESS ? "feita por um programa" : "automatica", freeVt);
			if (fd >= 0 && kd == KD_GRAPHICS)
			{
				const int r = ioctl (fd, KDSETMODE, KD_TEXT);
				fprintf (stderr, "console: tty%d de volta ao modo texto: %s\n", (int) st.v_active, r == 0 ? "ok" : strerror (errno));
			}
			if (fd >= 0 && m.mode == VT_PROCESS)
			{
				struct vt_mode a;
				int r;
				memset (&a, 0, sizeof(a));
				a.mode = VT_AUTO;
				r = ioctl (fd, VT_SETMODE, &a);
				fprintf (stderr, "console: tty%d com troca automatica: %s\n", (int) st.v_active, r == 0 ? "ok" : strerror (errno));
			}
			if (fd >= 0)
				close (fd);
		}
		ioctl (fd0, VT_UNLOCKSWITCH, 1);
		close (fd0);
	}

	memset (&sa, 0, sizeof(sa));
	sa.sa_handler = consoleKickHandler;	/* no SA_RESTART: the wait ends */
	sigaction (SIGRTMIN + 5, &sa, NULL);
	consoleMain = pthread_self ();
	consoleDone = 0;
	consoleT0 = now_s ();
	consoleWatching = (pthread_create (&consoleW, NULL, consoleWatcher, NULL) == 0);
}

void Onion_AfterScreen (void)
{
	int ms;

	if (!onion || !consoleWatching)
		return;
	consoleDone = 1;
	pthread_join (consoleW, NULL);
	consoleWatching = false;
	ms = (int) ((now_s () - consoleT0) * 1000.0);
	if (consoleKicks || ms > 1000)
		fprintf (stderr, "console: o SDL so abriu a tela em %d ms (%d avisos para destravar)\n", ms, (int) consoleKicks);
	else	fprintf (stderr, "console: tela aberta pelo SDL em %d ms\n", ms);
}
