/* d_edge.c
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "d_local.h"
#include "r_local.h"

static int	miplevel;

float		scale_for_mip;
MT_TLS int	ubasestep, errorterm, erroradjustup, erroradjustdown;	/* Miyoo: per thread, see d_polyse.c */
int		vstartscan;

// FIXME: should go away
extern void	R_RotateBmodel (void);
extern void	R_TransformFrustum (void);

vec3_t		transformed_modelorg;

/*
==============================================================================

Miyoo: drawing on the second core

The rows of the screen are split in bands of 4 lines. For each surface the
first core does the setup as before (texture cache, gradients), then draws
the spans in its own bands and hands the spans in the other bands to a
thread on the second core, with a copy of the drawing state (these variables
are per thread, see MT_TLS). The two cores never touch the same pixel. The
first core waits for the second one before a span list is reused (end of
D_DrawSurfaces) and before a texture cache block is rebuilt or allocated.
"r_mt 0" draws everything on the first core.

==============================================================================
*/
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>

cvar_t	r_mt = {"r_mt", "1", CVAR_NONE};
extern qboolean	r_cache_thrash;	/* d_surf.c */
cvar_t	r_mtsteal = {"r_mtsteal", "1", CVAR_NONE};	/* Miyoo: this core takes back queued jobs instead of waiting */

enum { MTJ_SPANS, MTJ_TURB, MTJ_TURBT, MTJ_SPANST, MTJ_CALL, MTJ_LITSURF };

typedef struct
{
	float		sdivzstepu, tdivzstepu, zistepu;
	float		sdivzstepv, tdivzstepv, zistepv;
	float		sdivzorigin, tdivzorigin, ziorigin;
	fixed16_t	sadjust, tadjust, bbextents, bbextentt;
	pixel_t		*cacheblock;
	int		cachewidth;
	int		type, flags;
	espan_t		*spans;
	void		(*fn)(void *);	/* MTJ_CALL */
	void		*arg;
	int		state;		/* Miyoo: 0 waiting, 1 taken, 2 done (by either core) */
	int		steal;		/* depends on no earlier job: the first core may take it */
} mtjob_t;

#define	MT_QSIZE	1024		/* power of two */
#define	MT_BANDSHIFT	2		/* bands of 4 rows */
static mtjob_t		mt_q[MT_QSIZE];
static unsigned int	mt_head, mt_tail;	/* written by the first / the second core */
static int		mt_sleeping;
static int		mt_started, mt_failed;
static pthread_t	mt_thread;
static pthread_mutex_t	mt_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	mt_cond = PTHREAD_COND_INITIALIZER;
static byte		mt_band[(MAXHEIGHT >> MT_BANDSHIFT) + 1];
float			d_mt_share = 0.5f;	/* world pass */
float			d_mt_cut = 1.0f;	/* Miyoo: part of the surface list during which the second core gets work */
float			d_mt_share_tr = 0.45f;	/* Miyoo: translucent pass (water, glass): balanced apart */
int			d_mt_litcap = 8;	/* whole lit textures the second core may have waiting */
static float		*mt_pass_share = &d_mt_share;
static float		mt_band_share = -1.0f;
static double		mt_wait_trans;		/* ms waited at the end of the translucent pass */
static int		mt_trans_frame;		/* there was one this frame */
static int		mt_litcap_up;		/* frames without waiting, before trying one more */
static unsigned int	mt_lit_posted, mt_lit_done;	/* whole lit textures given / finished */
int			(*d_mt_idlework)(void);	/* Miyoo: done when there is no drawing job (vid_miyoo.c) */
int			(*d_mt_duework)(void);	/* done before the next job when it is late (vid_miyoo.c) */
static int		mt_kick;		/* there may be idle work: look before sleeping */
static double		mt_wait_frame;		/* ms the first core waited for its share of rows this frame */
static double		mt_wait_other;		/* ms it waited for other reasons */
static double		mt_wait_cache;		/* ms it waited before reusing texture cache memory */
static double		mt_wait_urgent;		/* ms it waited for the half texture given to the second core */
static unsigned int	mt_lit_mark;		/* the last whole lit texture given to the second core */
/* one urgent call (half of a lit texture) taken before the next queued job */
static void		(*mt_prio_fn)(void *);
static void		*mt_prio_arg;
static int		mt_prio_state;		/* 0 free, 1 posted, 2 done */
static long long	mt_busy_us;		/* time the second core spent on jobs (its counter) */
static long long	mt_busy_seen;

static double D_MT_Now (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* draws one job on the calling thread (either core) */
static void D_MT_Run (int type, espan_t *spans, int flags)
{
	surf_t	fs;

	switch (type)
	{
	case MTJ_SPANS:
		(*d_drawspans) (spans);
		D_DrawZSpans (spans);
		break;
	case MTJ_TURB:
	case MTJ_TURBT:
		memset (&fs, 0, sizeof(fs));
		fs.spans = spans;
		fs.flags = flags;
		Turbulent8 (&fs);
		if (type == MTJ_TURB)
			D_DrawZSpans (spans);
		break;
	case MTJ_SPANST:
		D_DrawSpans8T (spans);
		break;
	}
}

/* Miyoo: a job is run by whichever core takes it first */
static qboolean D_MT_Claim (mtjob_t *j)
{
	int expected = 0;
	return __atomic_compare_exchange_n (&j->state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void D_MT_Exec (mtjob_t *j)
{
	d_sdivzstepu = j->sdivzstepu;	d_tdivzstepu = j->tdivzstepu;	d_zistepu = j->zistepu;
	d_sdivzstepv = j->sdivzstepv;	d_tdivzstepv = j->tdivzstepv;	d_zistepv = j->zistepv;
	d_sdivzorigin = j->sdivzorigin;	d_tdivzorigin = j->tdivzorigin;	d_ziorigin = j->ziorigin;
	sadjust = j->sadjust;		tadjust = j->tadjust;
	bbextents = j->bbextents;	bbextentt = j->bbextentt;
	cacheblock = j->cacheblock;	cachewidth = j->cachewidth;
	if (j->type == MTJ_CALL)
		j->fn (j->arg);
	else if (j->type == MTJ_LITSURF)
	{	/* build the lit texture, then draw the whole surface */
		j->fn (j->arg);
		D_MT_Run (MTJ_SPANS, j->spans, j->flags);
		__atomic_add_fetch (&mt_lit_done, 1, __ATOMIC_RELEASE);
	}
	else
		D_MT_Run (j->type, j->spans, j->flags);
}

static void *D_MT_Thread (void *arg)
{
	unsigned long	mask = 1UL << 1;	/* the second core */
	unsigned int	t, h;
	int		spin;
	mtjob_t		*j;

	(void) arg;
	syscall (SYS_sched_setaffinity, 0, sizeof(mask), &mask);
	setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), -5);	/* allowed to fail */

	for (;;)
	{
		double	b0;

		if (__atomic_load_n (&mt_prio_state, __ATOMIC_ACQUIRE) == 1)
		{
			b0 = vid_perf ? D_MT_Now () : 0;
			mt_prio_fn (mt_prio_arg);
			if (vid_perf)
				__atomic_add_fetch (&mt_busy_us, (long long) ((D_MT_Now () - b0) * 1000.0), __ATOMIC_RELAXED);
			__atomic_store_n (&mt_prio_state, 2, __ATOMIC_RELEASE);
			continue;
		}
		t = mt_tail;
		h = __atomic_load_n (&mt_head, __ATOMIC_ACQUIRE);
		if (t != h)
		{
			/* Miyoo: the picture going to the screen can only be made on
			 * this core, while most queued jobs can be taken back by the
			 * first core: when the picture is behind its schedule, a piece
			 * of it goes before the next job (vid_miyoo.c, conv_due) */
			int (*due)(void) = __atomic_load_n (&d_mt_duework, __ATOMIC_ACQUIRE);
			if (due && due ())
				continue;
		}
		if (t != h)
		{
			b0 = vid_perf ? D_MT_Now () : 0;
			j = &mt_q[t & (MT_QSIZE - 1)];
			if (D_MT_Claim (j))
			{
				D_MT_Exec (j);
				__atomic_store_n (&j->state, 2, __ATOMIC_RELEASE);
			}
			else
			{	/* the first core took it: the jobs after it may need it done */
				int n = 0;
				while (__atomic_load_n (&j->state, __ATOMIC_ACQUIRE) != 2)
				{
					if (++n > 100)
					{
						sched_yield ();
						n = 0;
					}
				}
			}
			if (vid_perf)
				__atomic_add_fetch (&mt_busy_us, (long long) ((D_MT_Now () - b0) * 1000.0), __ATOMIC_RELAXED);
			__atomic_store_n (&mt_tail, t + 1, __ATOMIC_RELEASE);
			continue;
		}
	// no drawing job: a piece of the picture going to the screen, if one
	// waits (it is done in these gaps instead of interrupting the jobs)
		__atomic_store_n (&mt_kick, 0, __ATOMIC_SEQ_CST);
		{
			int (*idle)(void) = __atomic_load_n (&d_mt_idlework, __ATOMIC_ACQUIRE);
			if (idle && idle ())
				continue;
		}
	// nothing to do: look again for a little while, then sleep
		for (spin = 0; spin < 2000; spin++)
		{
			if (__atomic_load_n (&mt_head, __ATOMIC_ACQUIRE) != mt_tail ||
			    __atomic_load_n (&mt_prio_state, __ATOMIC_ACQUIRE) == 1 ||
			    __atomic_load_n (&mt_kick, __ATOMIC_ACQUIRE))
				break;
		}
		if (spin < 2000)
			continue;
		pthread_mutex_lock (&mt_mutex);
		__atomic_store_n (&mt_sleeping, 1, __ATOMIC_SEQ_CST);
		while (__atomic_load_n (&mt_head, __ATOMIC_SEQ_CST) == mt_tail &&
		       __atomic_load_n (&mt_prio_state, __ATOMIC_SEQ_CST) != 1 &&
		       !__atomic_load_n (&mt_kick, __ATOMIC_SEQ_CST))
			pthread_cond_wait (&mt_cond, &mt_mutex);
		__atomic_store_n (&mt_sleeping, 0, __ATOMIC_SEQ_CST);
		pthread_mutex_unlock (&mt_mutex);
	}
	return NULL;
}

static qboolean D_MT_Ready (void)
{
	if (!r_mt.integer || mt_failed)
		return false;
	if (!mt_started)
	{
		if (pthread_create (&mt_thread, NULL, D_MT_Thread, NULL) != 0)
		{
			mt_failed = 1;
			Con_Printf ("second core drawing: thread not started\n");
			return false;
		}
		mt_started = 1;
	}
	if (mt_band_share != *mt_pass_share)
	{	/* spread the second core's bands evenly over the screen */
		float	acc = 0, share = *mt_pass_share;
		int	b;
		for (b = 0; b < (int) sizeof(mt_band); b++)
		{
			acc += share;
			mt_band[b] = (acc >= 1.0f);
			if (acc >= 1.0f)
				acc -= 1.0f;
		}
		mt_band_share = share;
	}
	return true;
}

static int		mt_next_steal;		/* the next posted job may be taken back */
static double		mt_steal_ms;		/* ms this core spent on jobs it took back */
static int		mt_steal_n;

/* Miyoo: instead of waiting idle, this core takes back a queued job that
 * depends on no earlier one, the newest first (the second core works from
 * the oldest). Its own drawing state is kept aside meanwhile. 1 = it did. */
static int D_MT_StealOne (void)
{
	unsigned int	t = __atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE), k;
	mtjob_t		*j, save;
	double		t0;

	for (k = mt_head; (int) (k - t) > 0; )
	{
		k--;
		j = &mt_q[k & (MT_QSIZE - 1)];
		if (!j->steal || __atomic_load_n (&j->state, __ATOMIC_ACQUIRE) != 0)
			continue;
		if (!D_MT_Claim (j))
			continue;
		t0 = D_MT_Now ();
		save.sdivzstepu = d_sdivzstepu;	save.tdivzstepu = d_tdivzstepu;	save.zistepu = d_zistepu;
		save.sdivzstepv = d_sdivzstepv;	save.tdivzstepv = d_tdivzstepv;	save.zistepv = d_zistepv;
		save.sdivzorigin = d_sdivzorigin; save.tdivzorigin = d_tdivzorigin; save.ziorigin = d_ziorigin;
		save.sadjust = sadjust;		save.tadjust = tadjust;
		save.bbextents = bbextents;	save.bbextentt = bbextentt;
		save.cacheblock = cacheblock;	save.cachewidth = cachewidth;
		D_MT_Exec (j);
		__atomic_store_n (&j->state, 2, __ATOMIC_RELEASE);
		d_sdivzstepu = save.sdivzstepu;	d_tdivzstepu = save.tdivzstepu;	d_zistepu = save.zistepu;
		d_sdivzstepv = save.sdivzstepv;	d_tdivzstepv = save.tdivzstepv;	d_zistepv = save.zistepv;
		d_sdivzorigin = save.sdivzorigin; d_tdivzorigin = save.tdivzorigin; d_ziorigin = save.ziorigin;
		sadjust = save.sadjust;		tadjust = save.tadjust;
		bbextents = save.bbextents;	bbextentt = save.bbextentt;
		cacheblock = save.cacheblock;	cachewidth = save.cachewidth;
		mt_steal_ms += D_MT_Now () - t0;
		mt_steal_n++;
		return 1;
	}
	return 0;
}

static void D_MT_WaitAll (double *acc)
{
	double	t0, s0;
	int	n = 0;

	if (!mt_started || __atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE) == mt_head)
		return;
	t0 = D_MT_Now ();
	s0 = mt_steal_ms;
	while (__atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE) != mt_head)
	{
		if (r_mtsteal.integer && D_MT_StealOne ())
			continue;
		if (++n > 200)
		{
			sched_yield ();
			n = 0;
		}
	}
	*acc += D_MT_Now () - t0 - (mt_steal_ms - s0);	/* only the time really waited */
}

/* waiting that has nothing to do with how the rows are shared: it must not
 * make the balance give the second core fewer rows */
void D_MT_Drain (void)
{
	D_MT_WaitAll (&mt_wait_other);
}

static void D_MT_Post (int type, espan_t *spans, int flags, void (*fn)(void *), void *arg);

/* Miyoo: jobs are done in order, so "everything posted until now" is just the
 * value of mt_head; it is finished once mt_tail has reached it. A texture
 * cache block keeps the mark of the last job that reads or writes it, so the
 * first core only waits when it really reuses that memory, not every time */
unsigned int D_MT_Mark (void)
{
	return mt_head;
}

unsigned int D_MT_Done (void)
{
	return __atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE);
}

qboolean D_MT_Pending (unsigned int mark)
{
	unsigned int t = __atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE);
	/* an empty queue has nothing pending, whatever an old mark says */
	return t != mt_head && (int) (mark - t) > 0;
}

void D_MT_WaitMark (unsigned int mark)
{
	double	t0;
	int	n = 0;

	double	s0;

	if (!D_MT_Pending (mark))
		return;
	t0 = D_MT_Now ();
	s0 = mt_steal_ms;
	while (D_MT_Pending (mark))
	{
		if (r_mtsteal.integer && D_MT_StealOne ())
			continue;
		if (++n > 200)
		{
			sched_yield ();
			n = 0;
		}
	}
	mt_wait_cache += D_MT_Now () - t0 - (mt_steal_ms - s0);
}

/* Miyoo: idle work is waiting (d_mt_idlework): wake the second core if it sleeps */
void D_MT_Kick (void)
{
	__atomic_store_n (&mt_kick, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n (&mt_sleeping, __ATOMIC_SEQ_CST))
	{
		pthread_mutex_lock (&mt_mutex);
		pthread_cond_signal (&mt_cond);
		pthread_mutex_unlock (&mt_mutex);
	}
}

/* the second core may be given jobs now (starts it if needed) */
qboolean D_MT_On (void)
{
	return D_MT_Ready ();
}

/* the second core's thread is running (it may be idle) */
qboolean D_MT_Alive (void)
{
	return mt_started && !mt_failed;
}

/* the second core can help with half a texture now: not while it still has
 * a whole lit texture in its queue, which would keep the first core waiting */
qboolean D_MT_Available (void)
{
	return D_MT_Ready () && !D_MT_Pending (mt_lit_mark);
}

/* fn(arg) runs on the second core as soon as it finishes the job in hand,
 * ahead of the queued ones; D_MT_WaitUrgent waits for it */
void D_MT_PostUrgent (void (*fn)(void *), void *arg)
{
	mt_prio_fn = fn;
	mt_prio_arg = arg;
	__atomic_store_n (&mt_prio_state, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n (&mt_sleeping, __ATOMIC_SEQ_CST))
	{
		pthread_mutex_lock (&mt_mutex);
		pthread_cond_signal (&mt_cond);
		pthread_mutex_unlock (&mt_mutex);
	}
}

void D_MT_WaitUrgent (void)
{
	double	t0;
	int	n = 0;

	if (__atomic_load_n (&mt_prio_state, __ATOMIC_ACQUIRE) == 2)
	{
		__atomic_store_n (&mt_prio_state, 0, __ATOMIC_RELEASE);
		return;
	}
	t0 = D_MT_Now ();
	while (__atomic_load_n (&mt_prio_state, __ATOMIC_ACQUIRE) != 2)
	{
		if (++n > 200)
		{
			sched_yield ();
			n = 0;
		}
	}
	__atomic_store_n (&mt_prio_state, 0, __ATOMIC_RELEASE);
	mt_wait_urgent += D_MT_Now () - t0;
}

void D_MT_PostCall (void (*fn)(void *), void *arg)
{
	D_MT_Post (MTJ_CALL, NULL, 0, fn, arg);
}

static void D_MT_Post (int type, espan_t *spans, int flags, void (*fn)(void *), void *arg)
{
	mtjob_t		*j;
	unsigned int	h = mt_head;

	while (h - __atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE) >= MT_QSIZE)
		;	/* full (should not happen: drained at the end of each D_DrawSurfaces) */
	j = &mt_q[h & (MT_QSIZE - 1)];
	j->sdivzstepu = d_sdivzstepu;	j->tdivzstepu = d_tdivzstepu;	j->zistepu = d_zistepu;
	j->sdivzstepv = d_sdivzstepv;	j->tdivzstepv = d_tdivzstepv;	j->zistepv = d_zistepv;
	j->sdivzorigin = d_sdivzorigin;	j->tdivzorigin = d_tdivzorigin;	j->ziorigin = d_ziorigin;
	j->sadjust = sadjust;		j->tadjust = tadjust;
	j->bbextents = bbextents;	j->bbextentt = bbextentt;
	j->cacheblock = cacheblock;	j->cachewidth = cachewidth;
	j->type = type;
	j->flags = flags;
	j->spans = spans;
	j->fn = fn;
	j->arg = arg;
	j->steal = mt_next_steal;	/* set just before by the caller, for this job only */
	mt_next_steal = 0;
	j->state = 0;
	__atomic_store_n (&mt_head, h + 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n (&mt_sleeping, __ATOMIC_SEQ_CST))
	{
		pthread_mutex_lock (&mt_mutex);
		pthread_cond_signal (&mt_cond);
		pthread_mutex_unlock (&mt_mutex);
	}
	if (vid_perf)
		VID_PerfCount (PC_MTJOBS, 1);
}

/* draws a surface's spans: the second core's bands go to it, the rest here */
static void D_MT_Draw (surf_t *s, int type, qboolean mt)
{
	espan_t	*mine = NULL, *theirs = NULL, **pm = &mine, **pt = &theirs, *sp, *next;

	if (!mt)
	{
		D_MT_Run (type, s->spans, s->flags);
		return;
	}
	for (sp = s->spans; sp; sp = next)
	{
		next = sp->pnext;
		if (mt_band[sp->v >> MT_BANDSHIFT])
		{
			*pt = sp;
			pt = &sp->pnext;
		}
		else
		{
			*pm = sp;
			pm = &sp->pnext;
		}
	}
	*pm = NULL;
	*pt = NULL;
	if (theirs)
	{
		mt_next_steal = 1;	/* rows of a ready texture: independent */
		D_MT_Post (type, theirs, s->flags, NULL, NULL);
	}
	if (mine)
		D_MT_Run (type, mine, s->flags);
	s->spans = mine;	/* the other part belongs to the second core now */
}

void D_MT_EndFrame (void)
{
	if (vid_perf)
	{
		long long b = __atomic_load_n (&mt_busy_us, __ATOMIC_RELAXED);
		VID_PerfAdd (PF_MTWAIT, mt_wait_frame + mt_wait_trans + mt_wait_other + mt_wait_cache + mt_wait_urgent);
		VID_PerfAdd (PF_MTWAITC, mt_wait_cache);
		VID_PerfAdd (PF_MTWAITU, mt_wait_urgent);
		VID_PerfAdd (PF_MTBUSY, (b - mt_busy_seen) / 1000.0);
		mt_busy_seen = b;
	}
	if (mt_started && r_mt.integer)
	{	/* the first core waited at the end of the world: give the second
		 * one less (fewer whole textures waiting, fewer rows), else a little
		 * more. The translucent pass has its own share, as it has no
		 * textures to build and would else get the world's leftovers. */
		/* Miyoo: what makes the first core wait at the end is work given
		 * late, which the second core cannot finish in time. So the second
		 * core gets work (rows and whole textures) only during the first
		 * part of the surface list (d_mt_cut); the first core does the rest
		 * alone while the second one empties its queue. When it waited:
		 * fewer rows if it has more than half, else an earlier cut; when
		 * it did not: a later cut, and once the cut is at the end, more rows. */
		/* Miyoo (later): this core now takes back queued jobs instead of
		 * waiting at the end (D_MT_StealOne), so the second core can be
		 * given plenty; what is left to wait for is the job it is in the
		 * middle of. The cut stays at the end of the list. */
		if (!r_mtsteal.integer)
		{
			if (mt_wait_frame > 0.25)
			{
				if (d_mt_share > 0.5f)
					d_mt_share -= 0.01f;
				else
					d_mt_cut -= 0.02f;
			}
			else
			{
				if (d_mt_cut < 1.0f)
					d_mt_cut += 0.01f;
				else
					d_mt_share += 0.005f;
			}
		}
		else
		{
			d_mt_cut = 1.0f;
			if (mt_wait_frame > 0.4)
				d_mt_share -= 0.01f;
			else
				d_mt_share += 0.005f;
			if (d_mt_share < 0.40f)
				d_mt_share = 0.40f;
		}
		if (d_mt_cut < 0.2f)
			d_mt_cut = 0.2f;
		if (d_mt_cut > 1.0f)
			d_mt_cut = 1.0f;
		if (d_mt_share < 0.10f)
			d_mt_share = 0.10f;
		if (d_mt_share > 0.75f)
			d_mt_share = 0.75f;
		(void) mt_litcap_up;
		if (mt_trans_frame)
		{
			if (mt_wait_trans > 0.15)
				d_mt_share_tr -= 0.01f;
			else
				d_mt_share_tr += 0.005f;
			if (d_mt_share_tr < 0.10f)
				d_mt_share_tr = 0.10f;
			if (d_mt_share_tr > 0.75f)
				d_mt_share_tr = 0.75f;
		}
	}
	if (vid_perf)
	{
		VID_PerfAdd (PF_MTWAITT, mt_wait_trans);
		VID_PerfAdd (PF_MTSTEAL, mt_steal_ms);
		VID_PerfCount (PC_MTSTEAL, mt_steal_n);
		if (r_cache_thrash)
			VID_PerfCount (PC_THRASH, 1);
		VID_PerfCount (PC_LITCAP, (int) (d_mt_cut * 100.0f + 0.5f));
		VID_PerfCount (PC_SHARETR, (int) (d_mt_share_tr * 100.0f + 0.5f));
	}
	mt_wait_trans = 0;
	mt_steal_ms = 0;
	mt_steal_n = 0;
	mt_trans_frame = 0;
	mt_wait_frame = 0;
	mt_wait_other = 0;
	mt_wait_cache = 0;
	mt_wait_urgent = 0;
}



/*
=============
D_MipLevelForScale
=============
*/
static int D_MipLevelForScale (float scale)
{
	int		lmiplevel;

	if (scale >= d_scalemip[0] )
		lmiplevel = 0;
	else if (scale >= d_scalemip[1] )
		lmiplevel = 1;
	else if (scale >= d_scalemip[2] )
		lmiplevel = 2;
	else
		lmiplevel = 3;

	if (lmiplevel < d_minmip)
		lmiplevel = d_minmip;

	return lmiplevel;
}


/*
==============
D_DrawSolidSurface

// FIXME: clean this up
==============
*/
static void D_DrawSolidSurface (surf_t *surf, int color)
{
	espan_t	*span;
	byte	*pdest;
	int		u, u2, pix;

	pix = (color<<24) | (color<<16) | (color<<8) | color;
	for (span = surf->spans ; span ; span = span->pnext)
	{
		pdest = (byte *)d_viewbuffer + screenwidth*span->v;
		u = span->u;
		u2 = span->u + span->count - 1;
		((byte *)pdest)[u] = pix;

		if (u2 - u < 8)
		{
			for (u++ ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
		else
		{
			for (u++ ; u & 3 ; u++)
				((byte *)pdest)[u] = pix;

			u2 -= 4;
			for ( ; u <= u2 ; u += 4)
				*(int *)((byte *)pdest + u) = pix;
			u2 += 4;
			for ( ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
	}
}


/*
==============
D_DrawSolidSurfaceT
==============
*/
static void D_DrawSolidSurfaceT (surf_t *surf, int color)
{
	espan_t	*pspan;
	byte	*pdest;
	short	*pz;
	int		izi, izistep, count;
	float	zi, dv, du;

	izistep = (int)(d_zistepu * 0x8000 * 0x10000);
	for (pspan = surf->spans ; pspan ; pspan = pspan->pnext)
	{
		pdest = (byte *)d_viewbuffer + screenwidth*pspan->v + pspan->u;
		pz = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;
		count = pspan->count;
		du = (float)pspan->u;
		dv = (float)pspan->v;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		izi = (int)(zi * 0x8000 * 0x10000);
		do
		{
			if (*pz <= (izi >> 16))
			{
				*pdest = mainTransTable[(color<<8) + (*pdest)];
			}
			izi += izistep;
			pdest++;
			pz++;
		} while (--count > 0);
	}
}


/*
==============
D_CalcGradients
==============
*/
static void D_CalcGradients (msurface_t *pface)
{
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	mipscale = 1.0 / (float)(1 << miplevel);

	TransformVector (pface->texinfo->vecs[0], p_saxis);
	TransformVector (pface->texinfo->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	d_sdivzstepu = p_saxis[0] * t;
	d_tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	d_sdivzstepv = -p_saxis[1] * t;
	d_tdivzstepv = -p_taxis[1] * t;

	d_sdivzorigin = p_saxis[2] * mipscale - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] * mipscale - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;

	VectorScale (transformed_modelorg, mipscale, p_temp1);

	t = 0x10000*mipscale;
	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5)) -
			((pface->texturemins[0] << 16) >> miplevel)
			+ pface->texinfo->vecs[0][3]*t;
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5)) -
			((pface->texturemins[1] << 16) >> miplevel)
			+ pface->texinfo->vecs[1][3]*t;

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
	bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
}


#if defined (H2W)
//color for sky given a certain light level 0 - 25
static const int SiegeFlatSkyFadeTable[25] =
{
	0,	// 1
	0,
	0,
	1,
	2,	// 5
	2,
	3,
	3,
	3,
	4,	// 10
	4,
	4,
	32,
	32,
	32,	// 15
	33,
	33,
	33,
	34,
	34,	// 20
	35,
	35,
	36,
	36,
	37	// 25
};
#endif	/* H2W */


/*
==============
D_DrawSurfaces
==============
*/
void D_DrawSurfaces (qboolean Translucent)
{
	surf_t			*s;
	msurface_t		*pface;
	surfcache_t		*pcurrentcache;
	vec3_t			world_transformed_modelorg;
	vec3_t			local_modelorg;
	int			count;
	qboolean		mt, zdone;

	mt_pass_share = Translucent ? &d_mt_share_tr : &d_mt_share;
	if (Translucent)
		mt_trans_frame = 1;
	mt = D_MT_Ready () && !r_drawflat.integer;

	// Restore the settings
	currententity = &r_worldentity;
//	VectorCopy (world_transformed_modelorg,
//				transformed_modelorg);
	VectorCopy (base_vpn, vpn);
	VectorCopy (base_vup, vup);
	VectorCopy (base_vright, vright);
	VectorCopy (base_modelorg, modelorg);
	R_TransformFrustum ();

	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);

// TODO: could preset a lot of this at mode set time
	if (r_drawflat.integer)
	{
		if (Translucent)
			return;

		for (s = &surfaces[1] ; s < surface_p ; s++)
		{
			if (!s->spans)
				continue;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			D_DrawSolidSurface (s, (intptr_t)s->data & 0xFF);
			D_DrawZSpans (s->spans);
		}
	}
	else
	{
		if (!Translucent)
		{
			/* Miyoo: work for the second core only before this surface */
			surf_t	*mt_cut = &surfaces[1] + (int) (d_mt_cut * (float) (surface_p - &surfaces[1]) + 0.5f);

			for (s = &surfaces[1] ; s < surface_p ; s++)
			{
				qboolean	mts;

				if (!s->spans)
					continue;

				r_drawnpolycount++;

				if (s->flags & SURF_TRANSLUCENT)
					continue;

				mts = mt && s < mt_cut;

				zdone = false;

				d_zistepu = s->d_zistepu;
				d_zistepv = s->d_zistepv;
				d_ziorigin = s->d_ziorigin;

			//	if (!strncmp(pface->texinfo->texture->name,"*BLACK",6))
				if (s->flags & SURF_DRAWBLACK)	// black vis-breaker, no turb
				{
					#if defined (H2W)
					if (cl_siege)
						D_DrawSolidSurface (s, SiegeFlatSkyFadeTable[(int)floor(d_lightstylevalue[0]/22)]);
					else
					#endif
						D_DrawSolidSurface (s, 0);
					D_DrawZSpans (s->spans);
					continue;
				}
				if (s->flags & SURF_DRAWSKY)
				{
					if (!r_skymade)
					{
						R_MakeSky ();
					}

					D_DrawSkyScans8 (s->spans);
					D_DrawZSpans (s->spans);
				}
				else if (s->flags & SURF_DRAWBACKGROUND)
				{
				// set up a gradient for the background surface that places it
				// effectively at infinity distance from the viewpoint
					d_zistepu = 0;
					d_zistepv = 0;
					d_ziorigin = -0.9;

					D_DrawSolidSurface (s, r_clearcolor.integer & 0xFF);
					D_DrawZSpans (s->spans);
				}
				else if (s->flags & SURF_DRAWTURB)
				{
					pface = (msurface_t *) s->data;
					miplevel = 0;
					cacheblock = (pixel_t *)
							((byte *)pface->texinfo->texture +
							pface->texinfo->texture->offsets[0]);
					cachewidth = 64;

					if (s->insubmodel)
					{
					// FIXME: we don't want to do all this for every polygon!
					// TODO: store once at start of frame
						currententity = s->entity;	//FIXME: make this passed in to
													// R_RotateBmodel ()
						VectorSubtract (r_origin, currententity->origin,
								local_modelorg);
						TransformVector (local_modelorg, transformed_modelorg);

						R_RotateBmodel ();	// FIXME: don't mess with the frustum,
											// make entity passed in
					}

					D_CalcGradients (pface);
					D_MT_Draw (s, MTJ_TURB, mts);	/* Turbulent8 + D_DrawZSpans */

					if (s->insubmodel)
					{
					//
					// restore the old drawing state
					// FIXME: we don't want to do this every time!
					// TODO: speed up
					//
						currententity = &r_worldentity;
						VectorCopy (world_transformed_modelorg,
									transformed_modelorg);
						VectorCopy (base_vpn, vpn);
						VectorCopy (base_vup, vup);
						VectorCopy (base_vright, vright);
						VectorCopy (base_modelorg, modelorg);
						R_TransformFrustum ();
					}
				}
				else
				{
					if (s->insubmodel)
					{
					// FIXME: we don't want to do all this for every polygon!
					// TODO: store once at start of frame
						currententity = s->entity;	//FIXME: make this passed in to
													// R_RotateBmodel ()
						VectorSubtract (r_origin, currententity->origin, local_modelorg);
						TransformVector (local_modelorg, transformed_modelorg);

						R_RotateBmodel ();	// FIXME: don't mess with the frustum,
											// make entity passed in
					}

					pface = (msurface_t *) s->data;
					if ((s->flags & SURF_DRAWSOLID) &&
						((s->entity->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT || !pface->samples))
					{
						byte *pixels = ((byte *)pface->texinfo->texture +
							pface->texinfo->texture->offsets[0]);
						int light;
						if (!r_fullbright.integer)
						{
							if ((s->entity->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
								light = s->entity->abslight;
							else
								light = r_refdef.ambientlight;
						}
						else
							light = 255;
						D_DrawSolidSurface (s, ((unsigned char *)vid.colormap)[(((255-light)<<VID_CBITS) & 0xFF00) + pixels[0]]);
					}
					else
					{
						miplevel = D_MipLevelForScale (s->nearzi * scale_for_mip
										* pface->texinfo->mipadjust);

					// FIXME: make this passed in to D_CacheSurface
					// Miyoo: a lit texture may be left to the second core,
					// when it is not too busy (see R_DrawSurface)
						r_mt_defer_ok = mts && r_fastloops.integer &&
								(mt_head - __atomic_load_n (&mt_tail, __ATOMIC_ACQUIRE)) < 64 &&
								(int) (mt_lit_posted - __atomic_load_n (&mt_lit_done, __ATOMIC_ACQUIRE)) < d_mt_litcap;
						r_mt_deferred = NULL;
						pcurrentcache = D_CacheSurface (pface, miplevel);
						r_mt_defer_ok = false;

						cacheblock = (pixel_t *)pcurrentcache->data;
						cachewidth = pcurrentcache->width;

						D_CalcGradients (pface);

						if (r_mt_deferred)
						{	/* texture and all the spans go to the second core */
							/* a texture rebuilt in place while earlier jobs
							 * still read it must stay after them */
							mt_next_steal = !r_mt_deferred_dep;
							D_MT_Post (MTJ_LITSURF, s->spans, s->flags, R_BuildDeferred, r_mt_deferred);
							r_mt_deferred = NULL;
							mt_lit_mark = mt_head;
							mt_lit_posted++;
							if (vid_perf)
								VID_PerfCount (PC_LITDEF, 1);
						}
						else if (mt && D_MT_Pending (pcurrentcache->mtuse))
						{	/* the second core may still be building this texture
							 * (another piece of the same surface): after it, in
							 * its queue, these spans are drawn in the right order */
							D_MT_Post (MTJ_SPANS, s->spans, s->flags, NULL, NULL);
						}
						else
							D_MT_Draw (s, MTJ_SPANS, mts);	/* d_drawspans + D_DrawZSpans */
						if (mt)
							pcurrentcache->mtuse = mt_head;
						zdone = true;
					}

					if (!zdone)
						D_DrawZSpans (s->spans);

					if (s->insubmodel)
					{
					//
					// restore the old drawing state
					// FIXME: we don't want to do this every time!
					// TODO: speed up
					//
						currententity = &r_worldentity;
						VectorCopy (world_transformed_modelorg,
									transformed_modelorg);
						VectorCopy (base_vpn, vpn);
						VectorCopy (base_vup, vup);
						VectorCopy (base_vright, vright);
						VectorCopy (base_modelorg, modelorg);
						R_TransformFrustum ();
					}
				}
			}
		}
		else
		{
			count = 0;

			for (s = &surfaces[1] ; s < surface_p ; s++)
			{
				if (!s->spans || !(s->flags & SURF_TRANSLUCENT))
					continue;

				count++;

				d_zistepu = s->d_zistepu;
				d_zistepv = s->d_zistepv;
				d_ziorigin = s->d_ziorigin;

			//	if (!strncmp(pface->texinfo->texture->name, "*BLACK", 6))
				if (s->flags & SURF_DRAWBLACK)	// black vis-breaker, no turb
				{
					#if defined (H2W)
					if (cl_siege)
						D_DrawSolidSurface (s, SiegeFlatSkyFadeTable[(int)floor(d_lightstylevalue[0]/22)]);
					else
					#endif
						D_DrawSolidSurface (s, 0);
					D_DrawZSpans (s->spans);
					continue;
				}
				if (s->flags & SURF_DRAWTURB)
				{
					pface = (msurface_t *) s->data;
					miplevel = 0;
					cacheblock = (pixel_t *)
							((byte *)pface->texinfo->texture +
							pface->texinfo->texture->offsets[0]);
					cachewidth = 64;

					if (s->insubmodel)
					{
					// FIXME: we don't want to do all this for every polygon!
					// TODO: store once at start of frame
						currententity = s->entity;	//FIXME: make this passed in to
													// R_RotateBmodel ()
						VectorSubtract (r_origin, currententity->origin,
								local_modelorg);
						TransformVector (local_modelorg, transformed_modelorg);

						R_RotateBmodel ();	// FIXME: don't mess with the frustum,
											// make entity passed in
					}

					D_CalcGradients (pface);
				// the original translucent water loop uses global scratch
				// memory, so it only runs on the first core
					D_MT_Draw (s, MTJ_TURBT, mt && r_fastloops.integer);
				//	D_DrawZSpans (s->spans);

					if (s->insubmodel)
					{
					//
					// restore the old drawing state
					// FIXME: we don't want to do this every time!
					// TODO: speed up
					//
						currententity = &r_worldentity;
						VectorCopy (world_transformed_modelorg,
									transformed_modelorg);
						VectorCopy (base_vpn, vpn);
						VectorCopy (base_vup, vup);
						VectorCopy (base_vright, vright);
						VectorCopy (base_modelorg, modelorg);
						R_TransformFrustum ();
					}
				}
				else
				{
					if (s->insubmodel)
					{
					// FIXME: we don't want to do all this for every polygon!
					// TODO: store once at start of frame
						currententity = s->entity;	//FIXME: make this passed in to
													// R_RotateBmodel ()
						VectorSubtract (r_origin, currententity->origin, local_modelorg);
						TransformVector (local_modelorg, transformed_modelorg);

						R_RotateBmodel ();	// FIXME: don't mess with the frustum,
											// make entity passed in
					}

					pface = (msurface_t *) s->data;
					if ((s->flags & SURF_DRAWSOLID) &&
						((s->entity->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT || !pface->samples))
					{
						byte *pixels = ((byte *)pface->texinfo->texture +
							pface->texinfo->texture->offsets[0]);
						int light;
						if (!r_fullbright.integer)
						{
							if ((s->entity->drawflags & MLS_ABSLIGHT) == MLS_ABSLIGHT)
								light = s->entity->abslight;
							else
								light = r_refdef.ambientlight;
						}
						else
							light = 255;
						D_DrawSolidSurfaceT (s, ((unsigned char *)vid.colormap)[(((255-light)<<VID_CBITS) & 0xFF00) + pixels[0]]);
					}
					else
					{
						miplevel = D_MipLevelForScale (s->nearzi * scale_for_mip
										* pface->texinfo->mipadjust);

					// FIXME: make this passed in to D_CacheSurface
						pcurrentcache = D_CacheSurface (pface, miplevel);

						cacheblock = (pixel_t *)pcurrentcache->data;
						cachewidth = pcurrentcache->width;

						D_CalcGradients (pface);

					//	(*d_drawspans) (s->spans);
#if id386
						D_DrawSpans16T(s->spans);
#else
						D_MT_Draw (s, MTJ_SPANST, mt);	/* D_DrawSpans8T */
						if (mt)
							pcurrentcache->mtuse = mt_head;
#endif
					}

				//	D_DrawZSpans (s->spans);

					if (s->insubmodel)
					{
					//
					// restore the old drawing state
					// FIXME: we don't want to do this every time!
					// TODO: speed up
					//
						currententity = &r_worldentity;
						VectorCopy (world_transformed_modelorg,
									transformed_modelorg);
						VectorCopy (base_vpn, vpn);
						VectorCopy (base_vup, vup);
						VectorCopy (base_vright, vright);
						VectorCopy (base_modelorg, modelorg);
						R_TransformFrustum ();
					}
				}
			}
		//	Con_Printf("   Surf is %d\n", count);
		}
	}

	D_MT_WaitAll (Translucent ? &mt_wait_trans : &mt_wait_frame);	/* the span lists are reused after this */
	R_LitPoolReset ();
}

