/* d_polyset.c - routines for drawing sets of polygons sharing the same
 * texture (used for Alias models.)
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * C versions of several asm functions:  Juraj Styk <jurajstyk@host.sk>
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
#include "r_local.h"
#include "d_local.h"

/* Miyoo: the model drawing state is per thread (MT_TLS): the second core
 * draws the lower rows of the models while this core draws the upper ones,
 * see D_PolyMT_* at the end of this file */
ASM_LINKAGE_BEGIN
MT_TLS int	r_p0[6], r_p1[6], r_p2[6];

MT_TLS byte	*d_pcolormap;

MT_TLS int	d_aflatcolor;
MT_TLS int	d_xdenom;

MT_TLS edgetable_t	*pedgetable;
/* the original table, with 0/1/2 for r_p0/r_p1/r_p2 (-1 = NULL): the
 * addresses of per-thread arrays are only known at run time */
static const signed char edgetable_idx[12][9] =
{
	{ 0, 1, 0, 2, -1, 2, 0, 1, 2 },
	{ 0, 2, 1, 0, 2, 1, 1, 2, -1 },
	{ 1, 1, 0, 2, -1, 1, 1, 2, -1 },
	{ 0, 1, 1, 0, -1, 2, 1, 2, 0 },
	{ 0, 2, 0, 2, 1, 1, 0, 1, -1 },
	{ 0, 1, 2, 1, -1, 1, 2, 0, -1 },
	{ 0, 1, 2, 1, -1, 2, 2, 0, 1 },
	{ 0, 2, 2, 1, 0, 1, 2, 0, -1 },
	{ 0, 1, 1, 0, -1, 1, 1, 2, -1 },
	{ 1, 1, 2, 1, -1, 1, 0, 1, -1 },
	{ 1, 1, 1, 0, -1, 1, 2, 0, -1 },
	{ 0, 1, 0, 2, -1, 1, 0, 1, -1 }
};
static MT_TLS edgetable_t	edgetables[12];
static MT_TLS int		edgetables_ready;

// FIXME: some of these can become statics
MT_TLS int	a_sstepxfrac, a_tstepxfrac, r_lstepx, a_ststepxwhole;
MT_TLS int	r_sstepx, r_tstepx, r_lstepy, r_sstepy, r_tstepy;
MT_TLS int	r_zistepx, r_zistepy;
MT_TLS int	d_aspancount, d_countextrastep;

MT_TLS spanpackage_t	*a_spans;
MT_TLS spanpackage_t	*d_pedgespanpackage;
MT_TLS byte	*d_pdest, *d_ptex;
MT_TLS short	*d_pz;
MT_TLS int	d_sfrac, d_tfrac, d_light, d_zi;
MT_TLS int	d_ptexextrastep, d_sfracextrastep;
MT_TLS int	d_tfracextrastep, d_lightextrastep, d_pdestextrastep;
MT_TLS int	d_lightbasestep, d_pdestbasestep, d_ptexbasestep;
MT_TLS int	d_sfracbasestep, d_tfracbasestep;
MT_TLS int	d_ziextrastep, d_zibasestep;
MT_TLS int	d_pzextrastep, d_pzbasestep;

MT_TLS byte	*skintable[MAX_SKIN_HEIGHT];
ASM_LINKAGE_END
static MT_TLS int	skinwidth;
static MT_TLS byte	*skinstart;

static MT_TLS int	ystart;

/* Miyoo: rows this thread draws (all of them unless the models are shared
 * between the cores), the row of the first span package of a DrawSpans call,
 * which of the five drawing kinds is in use, and the pixel count */
static MT_TLS int	d_poly_ymin = 0, d_poly_ymax = 0x7fffffff;
static MT_TLS int	d_spanrow;
static MT_TLS int	d_polykind;	/* 0 normal, 1 T, 2 T2, 3 T3, 5 T5 */
static MT_TLS int	d_poly_worker;	/* this is the second core */
static int		d_poly_pix_mt;	/* pixels counted by the second core (-perf) */
#define POLY_ROW_IN(y)	((unsigned int) ((y) - d_poly_ymin) < (unsigned int) (d_poly_ymax - d_poly_ymin))

static MT_TLS int	d_polymt_rec;	/* this core also records what it draws, for the other one */
/* -perf: one D_PolysetDraw* call in 8 on the first core is timed */
static int		d_poly_sample;
#define POLY_SAMPLE_START	double poly_t0_ = 0; int poly_smp_ = vid_perf && !d_poly_worker && ((++d_poly_sample & 7) == 0); if (poly_smp_) poly_t0_ = VID_PerfNow ();
#define POLY_SAMPLE_STOP	if (poly_smp_) VID_PerfAdd (PF_MRAST, 8.0 * (VID_PerfNow () - poly_t0_));
static void D_PolyMT_Record (int kind, const finalvert_t *a, const finalvert_t *b, const finalvert_t *c, int facesfront);
static void D_PolyMT_RecordTris (int kind);

/* a triangle with vertex rows a, b, c can touch this thread's rows */
static inline int D_PolyTriInRows (int a, int b, int c)
{
	int lo = a, hi = a;
	if (b < lo) lo = b;
	if (b > hi) hi = b;
	if (c < lo) lo = c;
	if (c > hi) hi = c;
	return !(hi < d_poly_ymin || lo >= d_poly_ymax);
}

typedef struct {
	int		quotient;
	int		remainder;
} adivtab_t;

ASM_LINKAGE_BEGIN
adivtab_t	adivtab[32*32] =
{
#include "adivtab.h"
};
ASM_LINKAGE_END

#if !id386
static MT_TLS spanpackage_t	spans[DPS_MAXSPANS + 1 + ((CACHE_SIZE - 1) / sizeof(spanpackage_t)) + 1];
						/* one extra because of cache line pretouching */
static void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage);
static void D_PolysetDrawSpans8T (spanpackage_t *pspanpackage);
static void D_PolysetDrawSpans8T2 (spanpackage_t *pspanpackage);
static void D_PolysetDrawSpans8T3 (spanpackage_t *pspanpackage);
static void D_PolysetDrawSpans8T5 (spanpackage_t *pspanpackage);
#endif


#if	!id386

/*
================
D_PolysetDrawFinalVerts
================
*/
static inline void do_PolysetDrawFinalVerts (finalvert_t *pv)
{
	int		z;
	short		*zbuf;

	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
	if (pv->v[0] < r_refdef.vrectright && pv->v[1] < r_refdef.vrectbottom && POLY_ROW_IN (pv->v[1]))
	{
		z = pv->v[5]>>16;
		zbuf = zspantable[pv->v[1]] + pv->v[0];
		if (z >= *zbuf)
		{
			unsigned int	pix;

			*zbuf = z;
			pix = skintable[pv->v[3]>>16][pv->v[2]>>16];
			pix = ((byte *)acolormap)[pix + (pv->v[4] & 0xFF00)];
			d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]] = pix;
		}
	}
}

static inline void do_PolysetDrawFinalVertsT (finalvert_t *pv)
{
	int		z;
	short		*zbuf;

	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
	if (pv->v[0] < r_refdef.vrectright && pv->v[1] < r_refdef.vrectbottom && POLY_ROW_IN (pv->v[1]))
	{
		z = pv->v[5]>>16;
		zbuf = zspantable[pv->v[1]] + pv->v[0];
		if (z >= *zbuf)
		{
			const byte color_map_idx = skintable[pv->v[3]>>16][pv->v[2]>>16];

			if (color_map_idx != 0)
			{
				unsigned int	pix, pix2;

				*zbuf = z;
				pix = ((byte *)acolormap)[color_map_idx + (pv->v[4] & 0xFF00)];
				pix2 = d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]];
				pix = mainTransTable[(pix<<8) + pix2];
				d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]] = pix;
			}
		}
	}
}

static inline void do_PolysetDrawFinalVertsT2 (finalvert_t *pv)
{
	int		z;
	short		*zbuf;

	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
	if (pv->v[0] < r_refdef.vrectright && pv->v[1] < r_refdef.vrectbottom && POLY_ROW_IN (pv->v[1]))
	{
		z = pv->v[5]>>16;
		zbuf = zspantable[pv->v[1]] + pv->v[0];
		if (z >= *zbuf)
		{
			const byte color_map_idx = skintable[pv->v[3]>>16][pv->v[2]>>16];

			if (color_map_idx != 0)
			{
				unsigned int	pix, pix2;

				*zbuf = z;
				pix = ((byte *)acolormap)[color_map_idx + (pv->v[4] & 0xFF00)];
				if (!(color_map_idx & 0x1))
				{
					d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]] = pix;
				}
				else
				{
					pix2 = d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]];
					pix = mainTransTable[(pix<<8) + pix2];
					d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]] = pix;
				}
			}
		}
	}
}

static inline void do_PolysetDrawFinalVertsT3 (finalvert_t *pv)
{
	int		z;
	short		*zbuf;

	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
	if (pv->v[0] < r_refdef.vrectright && pv->v[1] < r_refdef.vrectbottom && POLY_ROW_IN (pv->v[1]))
	{
		z = pv->v[5]>>16;
		zbuf = zspantable[pv->v[1]] + pv->v[0];
		if (z >= *zbuf)
		{
			const byte color_map_idx = skintable[pv->v[3]>>16][pv->v[2]>>16];

			if (color_map_idx != 0)
			{
				unsigned int	pix;

				*zbuf = z;
				pix = ((byte *)acolormap)[color_map_idx + (pv->v[4] & 0xFF00)];
				d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]] = pix;
			}
		}
	}
}

static inline void do_PolysetDrawFinalVertsT5 (finalvert_t *pv)
{
	int		z;
	short		*zbuf;

	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
	if (pv->v[0] < r_refdef.vrectright && pv->v[1] < r_refdef.vrectbottom && POLY_ROW_IN (pv->v[1]))
	{
		z = pv->v[5]>>16;
		zbuf = zspantable[pv->v[1]] + pv->v[0];
		if (z >= *zbuf)
		{
			const byte color_map_idx = skintable[pv->v[3]>>16][pv->v[2]>>16];

			if (color_map_idx != 0)
			{
				unsigned int	pix, pix2;

				*zbuf = z;
				pix2 = d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]];
				pix = transTable[(color_map_idx<<8) + pix2];
				d_viewbuffer[d_scantable[pv->v[1]] + pv->v[0]] = pix;
			}
		}
	}
}

void D_PolysetDrawFinalVerts (finalvert_t *pv1, finalvert_t *pv2, finalvert_t *pv3)
{
	if (d_polymt_rec)
		D_PolyMT_Record (8 + 0, pv1, pv2, pv3, 0);
	do_PolysetDrawFinalVerts (pv1);
	do_PolysetDrawFinalVerts (pv2);
	do_PolysetDrawFinalVerts (pv3);
}

void D_PolysetDrawFinalVertsT (finalvert_t *pv1, finalvert_t *pv2, finalvert_t *pv3)
{
	if (d_polymt_rec)
		D_PolyMT_Record (8 + 1, pv1, pv2, pv3, 0);
	do_PolysetDrawFinalVertsT (pv1);
	do_PolysetDrawFinalVertsT (pv2);
	do_PolysetDrawFinalVertsT (pv3);
}

void D_PolysetDrawFinalVertsT2 (finalvert_t *pv1, finalvert_t *pv2, finalvert_t *pv3)
{
	if (d_polymt_rec)
		D_PolyMT_Record (8 + 2, pv1, pv2, pv3, 0);
	do_PolysetDrawFinalVertsT2 (pv1);
	do_PolysetDrawFinalVertsT2 (pv2);
	do_PolysetDrawFinalVertsT2 (pv3);
}

void D_PolysetDrawFinalVertsT3 (finalvert_t *pv1, finalvert_t *pv2, finalvert_t *pv3)
{
	if (d_polymt_rec)
		D_PolyMT_Record (8 + 3, pv1, pv2, pv3, 0);
	do_PolysetDrawFinalVertsT3 (pv1);
	do_PolysetDrawFinalVertsT3 (pv2);
	do_PolysetDrawFinalVertsT3 (pv3);
}

void D_PolysetDrawFinalVertsT5 (finalvert_t *pv1, finalvert_t *pv2, finalvert_t *pv3)
{
	if (d_polymt_rec)
		D_PolyMT_Record (8 + 5, pv1, pv2, pv3, 0);
	do_PolysetDrawFinalVertsT5 (pv1);
	do_PolysetDrawFinalVertsT5 (pv2);
	do_PolysetDrawFinalVertsT5 (pv3);
}


/*
================
D_PolysetRecursiveTriangle
================
*/
static void D_PolysetRecursiveTriangle (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d;
	int		new_p[6];
	int		z;
	short		*zbuf;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return; // entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;
	if (!POLY_ROW_IN (new_p[1]))	/* Miyoo: a row of the other core */
		goto nodraw;

	z = new_p[5]>>16;
	zbuf = zspantable[new_p[1]] + new_p[0];
	if (z >= *zbuf)
	{
		unsigned int	pix;

		*zbuf = z;
		pix = d_pcolormap[skintable[new_p[3]>>16][new_p[2]>>16]];
		d_viewbuffer[d_scantable[new_p[1]] + new_p[0]] = pix;
	}

nodraw:
// recursively continue
	D_PolysetRecursiveTriangle (lp3, lp1, new_p);
	D_PolysetRecursiveTriangle (lp3, new_p, lp2);
}

static void D_PolysetRecursiveTriangleT (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d;
	int		new_p[6];
	int		z;
	short		*zbuf;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return; // entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;
	if (!POLY_ROW_IN (new_p[1]))	/* Miyoo: a row of the other core */
		goto nodraw;

	z = new_p[5]>>16;
	zbuf = zspantable[new_p[1]] + new_p[0];
	if (z >= *zbuf)
	{
		const byte color_map_idx = skintable[new_p[3]>>16][new_p[2]>>16];

		if (color_map_idx != 0)
		{
			unsigned int	pix, pix2;

			*zbuf = z;
			pix = d_pcolormap[color_map_idx];
			pix2 = d_viewbuffer[d_scantable[new_p[1]] + new_p[0]];
			pix = mainTransTable[(pix<<8) + pix2];
			d_viewbuffer[d_scantable[new_p[1]] + new_p[0]] = pix;
		}
	}

nodraw:
// recursively continue
	D_PolysetRecursiveTriangleT (lp3, lp1, new_p);
	D_PolysetRecursiveTriangleT (lp3, new_p, lp2);
}

static void D_PolysetRecursiveTriangleT2 (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d;
	int		new_p[6];
	int		z;
	short		*zbuf;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return; // entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;
	if (!POLY_ROW_IN (new_p[1]))	/* Miyoo: a row of the other core */
		goto nodraw;

	z = new_p[5]>>16;
	zbuf = zspantable[new_p[1]] + new_p[0];
	if (z >= *zbuf)
	{
		const byte color_map_idx = skintable[new_p[3]>>16][new_p[2]>>16];

		if (color_map_idx != 0)
		{
			unsigned int	pix, pix2;

			*zbuf = z;
			pix = d_pcolormap[color_map_idx];

			if (!(color_map_idx & 0x1))
			{
				d_viewbuffer[d_scantable[new_p[1]] + new_p[0]] = pix;
			}
			else
			{
				pix2 = d_viewbuffer[d_scantable[new_p[1]] + new_p[0]];
				pix = mainTransTable[(pix<<8) + pix2];
				d_viewbuffer[d_scantable[new_p[1]] + new_p[0]] = pix;
			}
		}
	}

nodraw:
// recursively continue
	D_PolysetRecursiveTriangleT2 (lp3, lp1, new_p);
	D_PolysetRecursiveTriangleT2 (lp3, new_p, lp2);
}

static void D_PolysetRecursiveTriangleT3 (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d;
	int		new_p[6];
	int		z;
	short		*zbuf;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return; // entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;
	if (!POLY_ROW_IN (new_p[1]))	/* Miyoo: a row of the other core */
		goto nodraw;

	z = new_p[5]>>16;
	zbuf = zspantable[new_p[1]] + new_p[0];
	if (z >= *zbuf)
	{
		const byte color_map_idx = skintable[new_p[3]>>16][new_p[2]>>16];

		if (color_map_idx != 0)
		{
			unsigned int	pix;

			*zbuf = z;
			pix = d_pcolormap[color_map_idx];
			d_viewbuffer[d_scantable[new_p[1]] + new_p[0]] = pix;
		}
	}

nodraw:
// recursively continue
	D_PolysetRecursiveTriangleT3 (lp3, lp1, new_p);
	D_PolysetRecursiveTriangleT3 (lp3, new_p, lp2);
}

static void D_PolysetRecursiveTriangleT5 (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d;
	int		new_p[6];
	int		z;
	short		*zbuf;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return; // entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;
	if (!POLY_ROW_IN (new_p[1]))	/* Miyoo: a row of the other core */
		goto nodraw;

	z = new_p[5]>>16;
	zbuf = zspantable[new_p[1]] + new_p[0];
	if (z >= *zbuf)
	{
		const byte color_map_idx = skintable[new_p[3]>>16][new_p[2]>>16];

		if (color_map_idx != 0)
		{
			unsigned int	pix, pix2;

			*zbuf = z;
			pix2 = d_viewbuffer[d_scantable[new_p[1]] + new_p[0]];
			pix = transTable[(color_map_idx<<8) + pix2];
			d_viewbuffer[d_scantable[new_p[1]] + new_p[0]] = pix;
		}
	}

nodraw:
// recursively continue
	D_PolysetRecursiveTriangleT5 (lp3, lp1, new_p);
	D_PolysetRecursiveTriangleT5 (lp3, new_p, lp2);
}

/*
================
D_DrawSubdiv
================
*/
static void D_DrawSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int			i, lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i = 0; i < lnumtriangles; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) * (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}
		if (!D_PolyTriInRows (index0->v[1], index1->v[1], index2->v[1]))
			continue;	/* Miyoo: only rows of the other core */

		d_pcolormap = &((byte *)acolormap)[index0->v[4] & 0xFF00];

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}

static void D_DrawSubdivT (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int			i, lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i = 0; i < lnumtriangles; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) * (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}
		if (!D_PolyTriInRows (index0->v[1], index1->v[1], index2->v[1]))
			continue;	/* Miyoo: only rows of the other core */

		d_pcolormap = &((byte *)acolormap)[index0->v[4] & 0xFF00];

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangleT(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangleT(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}

static void D_DrawSubdivT2 (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int			i, lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i = 0; i < lnumtriangles; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) * (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}
		if (!D_PolyTriInRows (index0->v[1], index1->v[1], index2->v[1]))
			continue;	/* Miyoo: only rows of the other core */

		d_pcolormap = &((byte *)acolormap)[index0->v[4] & 0xFF00];

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangleT2(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangleT2(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}

static void D_DrawSubdivT3 (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int			i, lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i = 0; i < lnumtriangles; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) * (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}
		if (!D_PolyTriInRows (index0->v[1], index1->v[1], index2->v[1]))
			continue;	/* Miyoo: only rows of the other core */

		d_pcolormap = &((byte *)acolormap)[index0->v[4] & 0xFF00];

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangleT3(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangleT3(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}

static void D_DrawSubdivT5 (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int			i, lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i = 0; i < lnumtriangles; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) * (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}
		if (!D_PolyTriInRows (index0->v[1], index1->v[1], index2->v[1]))
			continue;	/* Miyoo: only rows of the other core */

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangleT5(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangleT5(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}


/*
================
D_DrawNonSubdiv
================
*/
static void D_DrawNonSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int			i, lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i = 0; i < lnumtriangles; i++, ptri++)
	{
		index0 = pfv + ptri->vertindex[0];
		index1 = pfv + ptri->vertindex[1];
		index2 = pfv + ptri->vertindex[2];

		d_xdenom = (index0->v[1]-index1->v[1]) * (index0->v[0]-index2->v[0]) -
				(index0->v[0]-index1->v[0])*(index0->v[1]-index2->v[1]);

		if (d_xdenom >= 0)
		{
			continue;
		}
		if (!D_PolyTriInRows (index0->v[1], index1->v[1], index2->v[1]))
			continue;	/* Miyoo: only rows of the other core */

		r_p0[0] = index0->v[0];		// u
		r_p0[1] = index0->v[1];		// v
		r_p0[2] = index0->v[2];		// s
		r_p0[3] = index0->v[3];		// t
		r_p0[4] = index0->v[4];		// light
		r_p0[5] = index0->v[5];		// iz

		r_p1[0] = index1->v[0];
		r_p1[1] = index1->v[1];
		r_p1[2] = index1->v[2];
		r_p1[3] = index1->v[3];
		r_p1[4] = index1->v[4];
		r_p1[5] = index1->v[5];

		r_p2[0] = index2->v[0];
		r_p2[1] = index2->v[1];
		r_p2[2] = index2->v[2];
		r_p2[3] = index2->v[3];
		r_p2[4] = index2->v[4];
		r_p2[5] = index2->v[5];

		if (!ptri->facesfront)
		{
			if (index0->flags & ALIAS_ONSEAM)
				r_p0[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				r_p1[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				r_p2[2] += r_affinetridesc.seamfixupX16;
		}

		D_PolysetSetEdgeTable ();
		D_RasterizeAliasPolySmooth ();
	}
}


/*
================
D_PolysetDraw
================
*/
void D_PolysetDraw (void)
{
	POLY_SAMPLE_START
	if (vid_perf && !d_poly_worker)
		VID_PerfCount (PC_MTRIS, r_affinetridesc.numtriangles);
	if (d_polymt_rec)
		D_PolyMT_RecordTris (0);
	d_polykind = 0;
	a_spans = (spanpackage_t *)
			(((intptr_t)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdiv ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
	POLY_SAMPLE_STOP
}

void D_PolysetDrawT (void)
{
	POLY_SAMPLE_START
	if (vid_perf && !d_poly_worker)
		VID_PerfCount (PC_MTRIS, r_affinetridesc.numtriangles);
	if (d_polymt_rec)
		D_PolyMT_RecordTris (1);
	d_polykind = 1;
	a_spans = (spanpackage_t *)
			(((intptr_t)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdivT ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
	POLY_SAMPLE_STOP
}

void D_PolysetDrawT2 (void)
{
	POLY_SAMPLE_START
	if (vid_perf && !d_poly_worker)
		VID_PerfCount (PC_MTRIS, r_affinetridesc.numtriangles);
	if (d_polymt_rec)
		D_PolyMT_RecordTris (2);
	d_polykind = 2;
	a_spans = (spanpackage_t *)
			(((intptr_t)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdivT2 ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
	POLY_SAMPLE_STOP
}

void D_PolysetDrawT3 (void)
{
	POLY_SAMPLE_START
	if (vid_perf && !d_poly_worker)
		VID_PerfCount (PC_MTRIS, r_affinetridesc.numtriangles);
	if (d_polymt_rec)
		D_PolyMT_RecordTris (3);
	d_polykind = 3;
	a_spans = (spanpackage_t *)
			(((intptr_t)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdivT3 ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
	POLY_SAMPLE_STOP
}

void D_PolysetDrawT5 (void)
{
	POLY_SAMPLE_START
	if (vid_perf && !d_poly_worker)
		VID_PerfCount (PC_MTRIS, r_affinetridesc.numtriangles);
	if (d_polymt_rec)
		D_PolyMT_RecordTris (5);
	d_polykind = 5;
	a_spans = (spanpackage_t *)
			(((intptr_t)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdivT5 ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
	POLY_SAMPLE_STOP
}

#endif	/* !id386 */


/*
================
D_PolysetUpdateTables
================
*/
void D_PolysetUpdateTables (void)
{
	int		i;
	byte		*s;

	if (r_affinetridesc.skinwidth != skinwidth ||
		r_affinetridesc.pskin != skinstart)
	{
		skinwidth = r_affinetridesc.skinwidth;
		skinstart = (byte *) r_affinetridesc.pskin;
		s = skinstart;
		for (i = 0; i < MAX_SKIN_HEIGHT; i++, s += skinwidth)
			skintable[i] = s;
	}
}


#if	!id386

#define D_PolysetScanLeftEdgeT		D_PolysetScanLeftEdge
#define D_PolysetScanLeftEdgeT2		D_PolysetScanLeftEdge
#define D_PolysetScanLeftEdgeT3		D_PolysetScanLeftEdge
#define D_PolysetScanLeftEdgeT5		D_PolysetScanLeftEdge
/*
===================
D_PolysetScanLeftEdge
====================
*/
static void D_PolysetScanLeftEdge (int height)
{
	/* Miyoo: the same steps on local copies; the stores into the span
	 * packages forced every global to be reloaded at each line */
	spanpackage_t	*pk = d_pedgespanpackage;
	byte		*pdest = d_pdest, *ptex = d_ptex;
	short		*pz = d_pz;
	int		aspancount = d_aspancount, sfrac = d_sfrac, tfrac = d_tfrac;
	int		light = d_light, zi = d_zi, eterm = errorterm;
	const int	eup = erroradjustup, edown = erroradjustdown;
	const int	pdestextra = d_pdestextrastep, pzextra = d_pzextrastep;
	const int	countextra = d_countextrastep, ptexextra = d_ptexextrastep;
	const int	sfracextra = d_sfracextrastep, tfracextra = d_tfracextrastep;
	const int	lightextra = d_lightextrastep, ziextra = d_ziextrastep;
	const int	pdestbase = d_pdestbasestep, pzbase = d_pzbasestep;
	const int	countbase = ubasestep, ptexbase = d_ptexbasestep;
	const int	sfracbase = d_sfracbasestep, tfracbase = d_tfracbasestep;
	const int	lightbase = d_lightbasestep, zibase = d_zibasestep;
	const int	skinw = r_affinetridesc.skinwidth;

	do
	{
		pk->pdest = pdest;
		pk->pz = pz;
		pk->count = aspancount;
		pk->ptex = ptex;

		pk->sfrac = sfrac;
		pk->tfrac = tfrac;

	// FIXME: need to clamp l, s, t, at both ends?
		pk->light = light;
		pk->zi = zi;

		pk++;

		eterm += eup;
		if (eterm >= 0)
		{
			pdest += pdestextra;
			pz += pzextra;
			aspancount += countextra;
			ptex += ptexextra;
			sfrac += sfracextra;
			ptex += sfrac >> 16;

			sfrac &= 0xFFFF;
			tfrac += tfracextra;
			if (tfrac & 0x10000)
			{
				ptex += skinw;
				tfrac &= 0xFFFF;
			}
			light += lightextra;
			zi += ziextra;
			eterm -= edown;
		}
		else
		{
			pdest += pdestbase;
			pz += pzbase;
			aspancount += countbase;
			ptex += ptexbase;
			sfrac += sfracbase;
			ptex += sfrac >> 16;
			sfrac &= 0xFFFF;
			tfrac += tfracbase;
			if (tfrac & 0x10000)
			{
				ptex += skinw;
				tfrac &= 0xFFFF;
			}
			light += lightbase;
			zi += zibase;
		}
	} while (--height);

	d_pedgespanpackage = pk;
	d_pdest = pdest;	d_ptex = ptex;		d_pz = pz;
	d_aspancount = aspancount;	d_sfrac = sfrac;	d_tfrac = tfrac;
	d_light = light;	d_zi = zi;		errorterm = eterm;
}

#endif	/* !id386 */


/*
===================
D_PolysetSetUpForLineScan
====================
*/
static void D_PolysetSetUpForLineScan(fixed8_t startvertu, fixed8_t startvertv,
					fixed8_t endvertu, fixed8_t endvertv)
{
	double		dm, dn;
	int		tm, tn;
	adivtab_t	*ptemp;

// TODO: implement x86 version

	errorterm = -1;

	tm = endvertu - startvertu;
	tn = endvertv - startvertv;

	if (((tm <= 16) && (tm >= -15)) &&
		((tn <= 16) && (tn >= -15)))
	{
		ptemp = &adivtab[((tm+15) << 5) + (tn+15)];
		ubasestep = ptemp->quotient;
		erroradjustup = ptemp->remainder;
		erroradjustdown = tn;
	}
	else
	{
		if (tn > 0)
		{	/* Miyoo: FloorDivMod's result with integers (no double
			 * division and floor calls): both are whole numbers, and
			 * the quotient of numbers this size is exact in a double */
			int q = tm / tn, r = tm % tn;
			if (r < 0)
			{
				q--;
				r += tn;
			}
			ubasestep = q;
			erroradjustup = r;
			erroradjustdown = tn;
		}
		else
		{
			dm = (double)tm;
			dn = (double)tn;

			FloorDivMod (dm, dn, &ubasestep, &erroradjustup);

			erroradjustdown = dn;
		}
	}
}


#if	!id386

#define D_PolysetCalcGradientsT		D_PolysetCalcGradients
#define D_PolysetCalcGradientsT2	D_PolysetCalcGradients
#define D_PolysetCalcGradientsT3	D_PolysetCalcGradients
#define D_PolysetCalcGradientsT5	D_PolysetCalcGradients
/*
================
D_PolysetCalcGradients
================
*/
static void D_PolysetCalcGradients (int skin_width)
{
	float	xstepdenominv, ystepdenominv, t0, t1;
	float	p01_minus_p21, p11_minus_p21, p00_minus_p20, p10_minus_p20;

	p00_minus_p20 = r_p0[0] - r_p2[0];
	p01_minus_p21 = r_p0[1] - r_p2[1];
	p10_minus_p20 = r_p1[0] - r_p2[0];
	p11_minus_p21 = r_p1[1] - r_p2[1];

	xstepdenominv = 1.0 / (float)d_xdenom;

	ystepdenominv = -xstepdenominv;

// ceil () for light so positive steps are exaggerated, negative steps
// diminished,  pushing us away from underflow toward overflow. Underflow is
// very visible, overflow is very unlikely, because of ambient lighting
	t0 = r_p0[4] - r_p2[4];
	t1 = r_p1[4] - r_p2[4];
	r_lstepx = (int)ceil((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	r_lstepy = (int)ceil((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	t0 = r_p0[2] - r_p2[2];
	t1 = r_p1[2] - r_p2[2];
	r_sstepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	r_sstepy = (int)((t1 * p00_minus_p20 - t0* p10_minus_p20) * ystepdenominv);

	t0 = r_p0[3] - r_p2[3];
	t1 = r_p1[3] - r_p2[3];
	r_tstepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	r_tstepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	t0 = r_p0[5] - r_p2[5];
	t1 = r_p1[5] - r_p2[5];
	r_zistepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	r_zistepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

//	a_sstepxfrac = r_sstepx << 16;	// was #if id386 code
//	a_tstepxfrac = r_tstepx << 16;	// was #if id386 code
	a_sstepxfrac = r_sstepx & 0xFFFF;
	a_tstepxfrac = r_tstepx & 0xFFFF;

	a_ststepxwhole = skin_width * (r_tstepx >> 16) + (r_sstepx >> 16);
}

#endif	/* !id386 */


#if 0
byte gelmap[256];
void InitGel (byte *palette)
{
	int		i;
	int		r;

	for (i = 0; i < 256; i++)
	{
//		r = (palette[i*3]>>4);
		r = (palette[i*3] + palette[i*3+1] + palette[i*3+2])/(16*3);
		gelmap[i] = /* 64 */ 0 + r;
	}
}
#endif


#if	!id386

/*
================
D_PolysetDrawSpans8
================
*/
static void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage)
{
	int		lcount;
	byte		*lpdest;
	byte		*lptex;
	int		lsfrac, ltfrac;
	int		llight;
	int		lzi;
	short		*lpz;
	/* Miyoo: the loop stores bytes, which (with -fno-strict-aliasing) forces
	 * every global it reads to be reloaded at each pixel: work on copies */
	const int	zistepx = r_zistepx, ststepxwhole = a_ststepxwhole;
	const int	sstepxfrac = a_sstepxfrac, tstepxfrac = a_tstepxfrac;
	const int	skinwidth = r_affinetridesc.skinwidth;
	const int	lstepx = r_lstepx;
	const byte	*const cmap = (const byte *) acolormap;
	int		npix = 0;	/* Miyoo: counted for -perf */

	/* Miyoo: the right edge's stepping too (it was reloaded at each line) */
	int		laspancount = d_aspancount, lerrorterm = errorterm;
	const int	lerroradjustup = erroradjustup, lerroradjustdown = erroradjustdown;
	const int	lcountextrastep = d_countextrastep, lubasestep = ubasestep;
	/* Miyoo: rows of the other core are skipped (the edge still steps) */
	int		row = d_spanrow;
	const int	rowmin = d_poly_ymin;
	const unsigned int rowcount = (unsigned int) (d_poly_ymax - d_poly_ymin);

	do
	{
		lcount = laspancount - pspanpackage->count;
		if ((unsigned int) (row++ - rowmin) >= rowcount)
			lcount = 0;

		lerrorterm += lerroradjustup;
		if (lerrorterm >= 0)
		{
			laspancount += lcountextrastep;
			lerrorterm -= lerroradjustdown;
		}
		else
		{
			laspancount += lubasestep;
		}

		if (lcount)
		{
			npix += lcount;
			lpdest = (byte *) pspanpackage->pdest;
			lptex = pspanpackage->ptex;
			lpz = pspanpackage->pz;
			lsfrac = pspanpackage->sfrac;
			ltfrac = pspanpackage->tfrac;
			llight = pspanpackage->light;
			lzi = pspanpackage->zi;

			do
			{
				if ((lzi >> 16) >= *lpz)
				{
					*lpdest = cmap[*lptex + (llight & 0xFF00)];
				// gel mapping
				//	*lpdest = gelmap[*lpdest];
					*lpz = lzi >> 16;
				}
				lpdest++;
				lzi += zistepx;
				lpz++;
				llight += lstepx;
				lptex += ststepxwhole;
				lsfrac += sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}

		pspanpackage++;
	} while (pspanpackage->count != -999999);
	d_aspancount = laspancount;
	errorterm = lerrorterm;
	if (vid_perf)
	{
		if (d_poly_worker)
			__atomic_add_fetch (&d_poly_pix_mt, npix, __ATOMIC_RELAXED);
		else
			VID_PerfCount (PC_MPIX, npix);
	}
}

static void D_PolysetDrawSpans8T (spanpackage_t *pspanpackage)
{
	int		lcount;
	byte		*lpdest;
	byte		*lptex;
	int		lsfrac, ltfrac;
	int		llight;
	int		lzi;
	short		*lpz;
	byte		btemp;
	/* Miyoo: the loop stores bytes, which (with -fno-strict-aliasing) forces
	 * every global it reads to be reloaded at each pixel: work on copies */
	const int	zistepx = r_zistepx, ststepxwhole = a_ststepxwhole;
	const int	sstepxfrac = a_sstepxfrac, tstepxfrac = a_tstepxfrac;
	const int	skinwidth = r_affinetridesc.skinwidth;
	const int	lstepx = r_lstepx;
	const byte	*const cmap = (const byte *) acolormap;
	const byte	*const mtt = mainTransTable;
	int		npix = 0;	/* Miyoo: counted for -perf */

	/* Miyoo: the right edge's stepping too (it was reloaded at each line) */
	int		laspancount = d_aspancount, lerrorterm = errorterm;
	const int	lerroradjustup = erroradjustup, lerroradjustdown = erroradjustdown;
	const int	lcountextrastep = d_countextrastep, lubasestep = ubasestep;
	/* Miyoo: rows of the other core are skipped (the edge still steps) */
	int		row = d_spanrow;
	const int	rowmin = d_poly_ymin;
	const unsigned int rowcount = (unsigned int) (d_poly_ymax - d_poly_ymin);

	do
	{
		lcount = laspancount - pspanpackage->count;
		if ((unsigned int) (row++ - rowmin) >= rowcount)
			lcount = 0;

		lerrorterm += lerroradjustup;
		if (lerrorterm >= 0)
		{
			laspancount += lcountextrastep;
			lerrorterm -= lerroradjustdown;
		}
		else
		{
			laspancount += lubasestep;
		}

		if (lcount)
		{
			npix += lcount;
			lpdest = (byte *) pspanpackage->pdest;
			lptex = pspanpackage->ptex;
			lpz = pspanpackage->pz;
			lsfrac = pspanpackage->sfrac;
			ltfrac = pspanpackage->tfrac;
			llight = pspanpackage->light;
			lzi = pspanpackage->zi;

			do
			{
				const byte color_map_idx = lptex[0];
				if (color_map_idx != 0)
				{
					if ((lzi >> 16) >= *lpz)
					{
						btemp = cmap[color_map_idx + (llight & 0xFF00)];
						*lpdest = mtt[(btemp<<8) + (*lpdest)];
						*lpz = lzi >> 16;
					}
				}
				lpdest++;
				lzi += zistepx;
				lpz++;
				llight += lstepx;
				lptex += ststepxwhole;
				lsfrac += sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}

		pspanpackage++;
	} while (pspanpackage->count != -999999);
	d_aspancount = laspancount;
	errorterm = lerrorterm;
	if (vid_perf)
	{
		if (d_poly_worker)
			__atomic_add_fetch (&d_poly_pix_mt, npix, __ATOMIC_RELAXED);
		else
			VID_PerfCount (PC_MPIX, npix);
	}
}

static void D_PolysetDrawSpans8T2 (spanpackage_t *pspanpackage)
{
	int		lcount;
	byte		*lpdest;
	byte		*lptex;
	int		lsfrac, ltfrac;
	int		llight;
	int		lzi;
	short		*lpz;
	byte		btemp;
	/* Miyoo: the loop stores bytes, which (with -fno-strict-aliasing) forces
	 * every global it reads to be reloaded at each pixel: work on copies */
	const int	zistepx = r_zistepx, ststepxwhole = a_ststepxwhole;
	const int	sstepxfrac = a_sstepxfrac, tstepxfrac = a_tstepxfrac;
	const int	skinwidth = r_affinetridesc.skinwidth;
	const int	lstepx = r_lstepx;
	const byte	*const cmap = (const byte *) acolormap;
	const byte	*const mtt = mainTransTable;
	int		npix = 0;	/* Miyoo: counted for -perf */

	/* Miyoo: the right edge's stepping too (it was reloaded at each line) */
	int		laspancount = d_aspancount, lerrorterm = errorterm;
	const int	lerroradjustup = erroradjustup, lerroradjustdown = erroradjustdown;
	const int	lcountextrastep = d_countextrastep, lubasestep = ubasestep;
	/* Miyoo: rows of the other core are skipped (the edge still steps) */
	int		row = d_spanrow;
	const int	rowmin = d_poly_ymin;
	const unsigned int rowcount = (unsigned int) (d_poly_ymax - d_poly_ymin);

	do
	{
		lcount = laspancount - pspanpackage->count;
		if ((unsigned int) (row++ - rowmin) >= rowcount)
			lcount = 0;

		lerrorterm += lerroradjustup;
		if (lerrorterm >= 0)
		{
			laspancount += lcountextrastep;
			lerrorterm -= lerroradjustdown;
		}
		else
		{
			laspancount += lubasestep;
		}

		if (lcount)
		{
			npix += lcount;
			lpdest = (byte *) pspanpackage->pdest;
			lptex = pspanpackage->ptex;
			lpz = pspanpackage->pz;
			lsfrac = pspanpackage->sfrac;
			ltfrac = pspanpackage->tfrac;
			llight = pspanpackage->light;
			lzi = pspanpackage->zi;

			do
			{
				const byte color_map_idx = lptex[0];
				if (color_map_idx != 0)
				{
					if ((lzi >> 16) >= *lpz)
					{
						btemp = cmap[color_map_idx + (llight & 0xFF00)];
						*lpdest = (color_map_idx & 0x1) ? mtt[(btemp<<8) + (*lpdest)] : btemp;
						*lpz = lzi >> 16;
					}
				}
				lpdest++;
				lzi += zistepx;
				lpz++;
				llight += lstepx;
				lptex += ststepxwhole;
				lsfrac += sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}

		pspanpackage++;
	} while (pspanpackage->count != -999999);
	d_aspancount = laspancount;
	errorterm = lerrorterm;
	if (vid_perf)
	{
		if (d_poly_worker)
			__atomic_add_fetch (&d_poly_pix_mt, npix, __ATOMIC_RELAXED);
		else
			VID_PerfCount (PC_MPIX, npix);
	}
}

static void D_PolysetDrawSpans8T3 (spanpackage_t *pspanpackage)
{
	int		lcount;
	byte		*lpdest;
	byte		*lptex;
	int		lsfrac, ltfrac;
	int		llight;
	int		lzi;
	short		*lpz;
	/* Miyoo: the loop stores bytes, which (with -fno-strict-aliasing) forces
	 * every global it reads to be reloaded at each pixel: work on copies */
	const int	zistepx = r_zistepx, ststepxwhole = a_ststepxwhole;
	const int	sstepxfrac = a_sstepxfrac, tstepxfrac = a_tstepxfrac;
	const int	skinwidth = r_affinetridesc.skinwidth;
	const int	lstepx = r_lstepx;
	const byte	*const cmap = (const byte *) acolormap;
	int		npix = 0;	/* Miyoo: counted for -perf */

	/* Miyoo: the right edge's stepping too (it was reloaded at each line) */
	int		laspancount = d_aspancount, lerrorterm = errorterm;
	const int	lerroradjustup = erroradjustup, lerroradjustdown = erroradjustdown;
	const int	lcountextrastep = d_countextrastep, lubasestep = ubasestep;
	/* Miyoo: rows of the other core are skipped (the edge still steps) */
	int		row = d_spanrow;
	const int	rowmin = d_poly_ymin;
	const unsigned int rowcount = (unsigned int) (d_poly_ymax - d_poly_ymin);

	do
	{
		lcount = laspancount - pspanpackage->count;
		if ((unsigned int) (row++ - rowmin) >= rowcount)
			lcount = 0;

		lerrorterm += lerroradjustup;
		if (lerrorterm >= 0)
		{
			laspancount += lcountextrastep;
			lerrorterm -= lerroradjustdown;
		}
		else
		{
			laspancount += lubasestep;
		}

		if (lcount)
		{
			npix += lcount;
			lpdest = (byte *) pspanpackage->pdest;
			lptex = pspanpackage->ptex;
			lpz = pspanpackage->pz;
			lsfrac = pspanpackage->sfrac;
			ltfrac = pspanpackage->tfrac;
			llight = pspanpackage->light;
			lzi = pspanpackage->zi;

			do
			{
				const byte color_map_idx = lptex[0];
				if (color_map_idx != 0)
				{
					if ((lzi >> 16) >= *lpz)
					{
						*lpdest = cmap[color_map_idx + (llight & 0xFF00)];
						*lpz = lzi >> 16;
					}
				}
				lpdest++;
				lzi += zistepx;
				lpz++;
				llight += lstepx;
				lptex += ststepxwhole;
				lsfrac += sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}

		pspanpackage++;
	} while (pspanpackage->count != -999999);
	d_aspancount = laspancount;
	errorterm = lerrorterm;
	if (vid_perf)
	{
		if (d_poly_worker)
			__atomic_add_fetch (&d_poly_pix_mt, npix, __ATOMIC_RELAXED);
		else
			VID_PerfCount (PC_MPIX, npix);
	}
}

static void D_PolysetDrawSpans8T5 (spanpackage_t *pspanpackage)
{
	int		lcount;
	byte		*lpdest;
	byte		*lptex;
	int		lsfrac, ltfrac;
	int		lzi;
	short		*lpz;
	/* Miyoo: the loop stores bytes, which (with -fno-strict-aliasing) forces
	 * every global it reads to be reloaded at each pixel: work on copies */
	const int	zistepx = r_zistepx, ststepxwhole = a_ststepxwhole;
	const int	sstepxfrac = a_sstepxfrac, tstepxfrac = a_tstepxfrac;
	const int	skinwidth = r_affinetridesc.skinwidth;
	const byte	*const ttab = transTable;
	int		npix = 0;	/* Miyoo: counted for -perf */

	/* Miyoo: the right edge's stepping too (it was reloaded at each line) */
	int		laspancount = d_aspancount, lerrorterm = errorterm;
	const int	lerroradjustup = erroradjustup, lerroradjustdown = erroradjustdown;
	const int	lcountextrastep = d_countextrastep, lubasestep = ubasestep;
	/* Miyoo: rows of the other core are skipped (the edge still steps) */
	int		row = d_spanrow;
	const int	rowmin = d_poly_ymin;
	const unsigned int rowcount = (unsigned int) (d_poly_ymax - d_poly_ymin);

	do
	{
		lcount = laspancount - pspanpackage->count;
		if ((unsigned int) (row++ - rowmin) >= rowcount)
			lcount = 0;

		lerrorterm += lerroradjustup;
		if (lerrorterm >= 0)
		{
			laspancount += lcountextrastep;
			lerrorterm -= lerroradjustdown;
		}
		else
		{
			laspancount += lubasestep;
		}

		if (lcount)
		{
			npix += lcount;
			lpdest = (byte *) pspanpackage->pdest;
			lptex = pspanpackage->ptex;
			lpz = pspanpackage->pz;
			lsfrac = pspanpackage->sfrac;
			ltfrac = pspanpackage->tfrac;
			lzi = pspanpackage->zi;

			do
			{
				const byte color_map_idx = lptex[0];
				if (color_map_idx != 0)
				{
					if ((lzi >> 16) >= *lpz)
					{
						*lpdest = ttab[(color_map_idx<<8) + (*lpdest)];
						*lpz = lzi >> 16;
					}
				}
				lpdest++;
				lzi += zistepx;
				lpz++;
				lptex += ststepxwhole;
				lsfrac += sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}

		pspanpackage++;
	} while (pspanpackage->count != -999999);
	d_aspancount = laspancount;
	errorterm = lerrorterm;
	if (vid_perf)
	{
		if (d_poly_worker)
			__atomic_add_fetch (&d_poly_pix_mt, npix, __ATOMIC_RELAXED);
		else
			VID_PerfCount (PC_MPIX, npix);
	}
}

#endif	/* !id386 */


/*
================
D_PolysetFillSpans8
================
*/
void D_PolysetFillSpans8 (spanpackage_t *pspanpackage)
{
	int		color;

// FIXME: do z buffering

	color = d_aflatcolor++;

	while (1)
	{
		int		lcount;
		byte	*lpdest;

		lcount = pspanpackage->count;

		if (lcount == -1)
			return;

		if (lcount)
		{
			lpdest = (byte *) pspanpackage->pdest;

			do
			{
				*lpdest++ = color;
			} while (--lcount);
		}

		pspanpackage++;
	}
}

/*
================
D_RasterizeAliasPolySmooth
================
*/
void D_RasterizeAliasPolySmooth (void)
{
	int		initialleftheight, initialrightheight;
	int		*plefttop, *prighttop, *pleftbottom, *prightbottom;
	int		working_lstepx, originalcount;
	int		toprow;

	plefttop = pedgetable->pleftedgevert0;
	prighttop = pedgetable->prightedgevert0;

	pleftbottom = pedgetable->pleftedgevert1;
	prightbottom = pedgetable->prightedgevert1;

	initialleftheight = pleftbottom[1] - plefttop[1];
	initialrightheight = prightbottom[1] - prighttop[1];
	toprow = plefttop[1];	/* Miyoo: the row of a_spans[0] */

//
// set the s, t, and light gradients, which are consistent across the triangle
// because being a triangle, things are affine
//
	/* Miyoo: all five are the same function here (no x86 assembly) */
	D_PolysetCalcGradients (r_affinetridesc.skinwidth);

//
// rasterize the polygon
//

//
// scan out the top (and possibly only) part of the left edge
//
	d_pedgespanpackage = a_spans;

	ystart = plefttop[1];
	d_aspancount = plefttop[0] - prighttop[0];

	d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
			(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
#if	id386
	d_sfrac = (plefttop[2] & 0xFFFF) << 16;
	d_tfrac = (plefttop[3] & 0xFFFF) << 16;
#else
	d_sfrac = plefttop[2] & 0xFFFF;
	d_tfrac = plefttop[3] & 0xFFFF;
#endif
	d_light = plefttop[4];
	d_zi = plefttop[5];

	d_pdest = (byte *)d_viewbuffer + ystart * screenwidth + plefttop[0];
	d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

	if (initialleftheight == 1)
	{
		d_pedgespanpackage->pdest = d_pdest;
		d_pedgespanpackage->pz = d_pz;
		d_pedgespanpackage->count = d_aspancount;
		d_pedgespanpackage->ptex = d_ptex;

		d_pedgespanpackage->sfrac = d_sfrac;
		d_pedgespanpackage->tfrac = d_tfrac;

	// FIXME: need to clamp l, s, t, at both ends?
		d_pedgespanpackage->light = d_light;
		d_pedgespanpackage->zi = d_zi;

		d_pedgespanpackage++;
	}
	else
	{
		D_PolysetSetUpForLineScan(plefttop[0], plefttop[1], pleftbottom[0], pleftbottom[1]);

#if	id386
		d_pzbasestep = (d_zwidth + ubasestep) << 1;
		d_pzextrastep = d_pzbasestep + 2;
#else
		d_pzbasestep = d_zwidth + ubasestep;
		d_pzextrastep = d_pzbasestep + 1;
#endif

		d_pdestbasestep = screenwidth + ubasestep;
		d_pdestextrastep = d_pdestbasestep + 1;

	// TODO: can reuse partial expressions here

	// for negative steps in x along left edge, bias toward overflow rather than
	// underflow (sort of turning the floor () we did in the gradient calcs into
	// ceil (), but plus a little bit)
		if (ubasestep < 0)
			working_lstepx = r_lstepx - 1;
		else
			working_lstepx = r_lstepx;

		d_countextrastep = ubasestep + 1;
		d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
				((r_tstepy + r_tstepx * ubasestep) >> 16) * r_affinetridesc.skinwidth;
#if	id386
		d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) << 16;
		d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) << 16;
#else
		d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
		d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
#endif
		d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
		d_zibasestep = r_zistepy + r_zistepx * ubasestep;

		d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
				((r_tstepy + r_tstepx * d_countextrastep) >> 16) * r_affinetridesc.skinwidth;
#if	id386
		d_sfracextrastep = (r_sstepy + r_sstepx*d_countextrastep) << 16;
		d_tfracextrastep = (r_tstepy + r_tstepx*d_countextrastep) << 16;
#else
		d_sfracextrastep = (r_sstepy + r_sstepx*d_countextrastep) & 0xFFFF;
		d_tfracextrastep = (r_tstepy + r_tstepx*d_countextrastep) & 0xFFFF;
#endif
		d_lightextrastep = d_lightbasestep + working_lstepx;
		d_ziextrastep = d_zibasestep + r_zistepx;

		D_PolysetScanLeftEdge (initialleftheight);	/* the five are the same */
	}

//
// scan out the bottom part of the left edge, if it exists
//
	if (pedgetable->numleftedges == 2)
	{
		int		height;

		plefttop = pleftbottom;
		pleftbottom = pedgetable->pleftedgevert2;

		height = pleftbottom[1] - plefttop[1];

// TODO: make this a function; modularize this function in general

		ystart = plefttop[1];
		d_aspancount = plefttop[0] - prighttop[0];
		d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
				(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
		d_sfrac = 0;
		d_tfrac = 0;
		d_light = plefttop[4];
		d_zi = plefttop[5];

		d_pdest = (byte *)d_viewbuffer + ystart * screenwidth + plefttop[0];
		d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

		if (height == 1)
		{
			d_pedgespanpackage->pdest = d_pdest;
			d_pedgespanpackage->pz = d_pz;
			d_pedgespanpackage->count = d_aspancount;
			d_pedgespanpackage->ptex = d_ptex;

			d_pedgespanpackage->sfrac = d_sfrac;
			d_pedgespanpackage->tfrac = d_tfrac;

		// FIXME: need to clamp l, s, t, at both ends?
			d_pedgespanpackage->light = d_light;
			d_pedgespanpackage->zi = d_zi;

			d_pedgespanpackage++;
		}
		else
		{
			D_PolysetSetUpForLineScan(plefttop[0], plefttop[1], pleftbottom[0], pleftbottom[1]);

			d_pdestbasestep = screenwidth + ubasestep;
			d_pdestextrastep = d_pdestbasestep + 1;

#if	id386
			d_pzbasestep = (d_zwidth + ubasestep) << 1;
			d_pzextrastep = d_pzbasestep + 2;
#else
			d_pzbasestep = d_zwidth + ubasestep;
			d_pzextrastep = d_pzbasestep + 1;
#endif

			if (ubasestep < 0)
				working_lstepx = r_lstepx - 1;
			else
				working_lstepx = r_lstepx;

			d_countextrastep = ubasestep + 1;
			d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
					((r_tstepy + r_tstepx * ubasestep) >> 16) * r_affinetridesc.skinwidth;
#if	id386
			d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) << 16;
			d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) << 16;
#else
			d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
			d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
#endif
			d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
			d_zibasestep = r_zistepy + r_zistepx * ubasestep;

			d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
					((r_tstepy + r_tstepx * d_countextrastep) >> 16) * r_affinetridesc.skinwidth;
#if	id386
			d_sfracextrastep = ((r_sstepy+r_sstepx*d_countextrastep) & 0xFFFF)<<16;
			d_tfracextrastep = ((r_tstepy+r_tstepx*d_countextrastep) & 0xFFFF)<<16;
#else
			d_sfracextrastep = (r_sstepy+r_sstepx*d_countextrastep) & 0xFFFF;
			d_tfracextrastep = (r_tstepy+r_tstepx*d_countextrastep) & 0xFFFF;
#endif
			d_lightextrastep = d_lightbasestep + working_lstepx;
			d_ziextrastep = d_zibasestep + r_zistepx;

			D_PolysetScanLeftEdge (height);	/* the five are the same */
		}
	}

// scan out the top (and possibly only) part of the right edge, updating the
// count field
	d_pedgespanpackage = a_spans;

	D_PolysetSetUpForLineScan(prighttop[0], prighttop[1], prightbottom[0], prightbottom[1]);
	d_aspancount = 0;
	d_countextrastep = ubasestep + 1;
	originalcount = a_spans[initialrightheight].count;
	a_spans[initialrightheight].count = -999999; // mark end of the spanpackages

	d_spanrow = toprow;
	switch (d_polykind)	/* Miyoo: set by the D_PolysetDraw* that was called */
	{
	case 5: D_PolysetDrawSpans8T5 (a_spans); break;
	case 1: D_PolysetDrawSpans8T (a_spans); break;
	case 2: D_PolysetDrawSpans8T2 (a_spans); break;
	case 3: D_PolysetDrawSpans8T3 (a_spans); break;
	default: D_PolysetDrawSpans8 (a_spans); break;
	}

// scan out the bottom part of the right edge, if it exists
	if (pedgetable->numrightedges == 2)
	{
		int				height;
		spanpackage_t	*pstart;

		pstart = a_spans + initialrightheight;
		pstart->count = originalcount;

		d_aspancount = prightbottom[0] - prighttop[0];

		prighttop = prightbottom;
		prightbottom = pedgetable->prightedgevert2;

		height = prightbottom[1] - prighttop[1];

		D_PolysetSetUpForLineScan(prighttop[0], prighttop[1], prightbottom[0], prightbottom[1]);

		d_countextrastep = ubasestep + 1;
		a_spans[initialrightheight + height].count = -999999;
											// mark end of the spanpackages
		d_spanrow = toprow + initialrightheight;
		switch (d_polykind)	/* Miyoo: set by the D_PolysetDraw* that was called */
		{
		case 5: D_PolysetDrawSpans8T5 (pstart); break;
		case 1: D_PolysetDrawSpans8T (pstart); break;
		case 2: D_PolysetDrawSpans8T2 (pstart); break;
		case 3: D_PolysetDrawSpans8T3 (pstart); break;
		default: D_PolysetDrawSpans8 (pstart); break;
		}
	}
}


/*
================
D_PolysetSetEdgeTable
================
*/
void D_PolysetSetEdgeTable (void)
{
	int			edgetableindex;

	if (!edgetables_ready)
	{	/* Miyoo: this thread's table, pointing to its own r_p0/1/2 */
		int	*pv[3], i;
		pv[0] = r_p0; pv[1] = r_p1; pv[2] = r_p2;
		for (i = 0; i < 12; i++)
		{
			const signed char *e = edgetable_idx[i];
			edgetables[i].isflattop = e[0];
			edgetables[i].numleftedges = e[1];
			edgetables[i].pleftedgevert0 = (e[2] < 0) ? NULL : pv[(int) e[2]];
			edgetables[i].pleftedgevert1 = (e[3] < 0) ? NULL : pv[(int) e[3]];
			edgetables[i].pleftedgevert2 = (e[4] < 0) ? NULL : pv[(int) e[4]];
			edgetables[i].numrightedges = e[5];
			edgetables[i].prightedgevert0 = (e[6] < 0) ? NULL : pv[(int) e[6]];
			edgetables[i].prightedgevert1 = (e[7] < 0) ? NULL : pv[(int) e[7]];
			edgetables[i].prightedgevert2 = (e[8] < 0) ? NULL : pv[(int) e[8]];
		}
		edgetables_ready = 1;
	}

	edgetableindex = 0;	// assume the vertices are already in
						//  top to bottom order

//
// determine which edges are right & left, and the order in which
// to rasterize them
//
	if (r_p0[1] >= r_p1[1])
	{
		if (r_p0[1] == r_p1[1])
		{
			if (r_p0[1] < r_p2[1])
				pedgetable = &edgetables[2];
			else
				pedgetable = &edgetables[5];

			return;
		}
		else
		{
			edgetableindex = 1;
		}
	}

	if (r_p0[1] == r_p2[1])
	{
		if (edgetableindex)
			pedgetable = &edgetables[8];
		else
			pedgetable = &edgetables[9];

		return;
	}
	else if (r_p1[1] == r_p2[1])
	{
		if (edgetableindex)
			pedgetable = &edgetables[10];
		else
			pedgetable = &edgetables[11];

		return;
	}

	if (r_p0[1] > r_p2[1])
		edgetableindex += 2;

	if (r_p1[1] > r_p2[1])
		edgetableindex += 4;

	pedgetable = &edgetables[edgetableindex];
}


#if 0

void D_PolysetRecursiveDrawLine (int *lp1, int *lp2)
{
	int		d;
	int		new_p[6];
	int		ofs;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	return;	// line is completed

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[4] = (lp1[4] + lp2[4]) >> 1;

// draw the point
	ofs = d_scantable[new_p[1]] + new_p[0];
	if (new_p[5] > d_pzbuffer[ofs])
	{
		unsigned int	pix;

		d_pzbuffer[ofs] = new_p[5];
		pix = skintable[new_p[3]>>16][new_p[2]>>16];
//		pix = ((byte *)acolormap)[pix + (new_p[4] & 0xFF00)];
		d_viewbuffer[ofs] = pix;
	}

// recursively continue
	D_PolysetRecursiveDrawLine (lp1, new_p);
	D_PolysetRecursiveDrawLine (new_p, lp2);
}

void D_PolysetRecursiveTriangle2 (int *lp1, int *lp2, int *lp3)
{
	int		d;
	int		new_p[6];

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;
	return;

split:
// split this edge
	new_p[0] = (lp1[0] + lp2[0]) >> 1;
	new_p[1] = (lp1[1] + lp2[1]) >> 1;
	new_p[5] = (lp1[5] + lp2[5]) >> 1;
	new_p[2] = (lp1[2] + lp2[2]) >> 1;
	new_p[3] = (lp1[3] + lp2[3]) >> 1;
	new_p[4] = (lp1[4] + lp2[4]) >> 1;

	D_PolysetRecursiveDrawLine (new, lp3);

// recursively continue
	D_PolysetRecursiveTriangle (lp1, new, lp3);
	D_PolysetRecursiveTriangle (new, lp2, lp3);
}

#endif



/*
==============================================================================
Miyoo: alias models drawn by both cores

While the models (and then the weapon) are drawn, this core keeps only the
rows above a split line and records every triangle it is given, with copies
of its three vertices and the drawing state; the second core replays the
records, in the same order, for the rows below the line. Each pixel is
drawn by one core only, in the original order, so the picture is the same.
The line moves so that neither core waits much for the other.
==============================================================================
*/
typedef struct
{
	void		*pskin;
	int		skinwidth, skinheight, seamfixupX16, drawtype;
	void		*colormap;
} polystate_t;

typedef struct
{
	finalvert_t	fv[3];
	short		kind;		/* 0,1,2,3,5 = D_PolysetDraw..T5; +8 = D_PolysetDrawFinalVerts.. */
	short		facesfront;
	int		state;
} polyrec_t;

typedef struct
{
	int		r0, r1, split;
} polybatch_t;

#define	POLY_MAXREC	8192
#define	POLY_MAXSTATE	512
#define	POLY_BATCH	32
static polyrec_t	poly_rec[POLY_MAXREC];
static polystate_t	poly_state[POLY_MAXSTATE];
static polybatch_t	poly_batch[POLY_MAXREC / POLY_BATCH + 4];
static int		poly_nrec, poly_nstate, poly_posted, poly_nbatch;
static int		poly_split;		/* first row of the second core */
static int		poly_phase;		/* 0 models, 1 weapon */
static float		poly_frac[2] = { 0.5f, 0.5f };	/* where the line is, in the 3D view */
static double		poly_wait;		/* ms waited for the second core this frame */
static double		poly_t_begin, poly_flushwait;	/* this phase: start, waits inside it */
static long long	poly_busy_us;		/* the second core's time on this phase's records */

static void D_PolyMT_Replay (void *arg)
{
	const polybatch_t *b = (const polybatch_t *) arg;
	mtriangle_t	tri;
	int		i, last = -1;
	double		t0 = VID_PerfNow ();

	d_poly_worker = 1;
	d_poly_ymin = b->split;
	d_poly_ymax = 0x7fffffff;
	memset (&tri, 0, sizeof(tri));
	tri.vertindex[0] = 0;
	tri.vertindex[1] = 1;
	tri.vertindex[2] = 2;
	for (i = b->r0; i < b->r1; i++)
	{
		polyrec_t	*r = &poly_rec[i];

		if (r->state != last)
		{
			const polystate_t *st = &poly_state[r->state];
			r_affinetridesc.pskin = st->pskin;
			r_affinetridesc.skinwidth = st->skinwidth;
			r_affinetridesc.skinheight = st->skinheight;
			r_affinetridesc.seamfixupX16 = st->seamfixupX16;
			r_affinetridesc.drawtype = st->drawtype;
			acolormap = st->colormap;
			if (st->drawtype)
				D_PolysetUpdateTables ();
			last = r->state;
		}
		if (r->kind >= 8)
		{
			switch (r->kind - 8)
			{
			case 1: D_PolysetDrawFinalVertsT (&r->fv[0], &r->fv[1], &r->fv[2]); break;
			case 2: D_PolysetDrawFinalVertsT2 (&r->fv[0], &r->fv[1], &r->fv[2]); break;
			case 3: D_PolysetDrawFinalVertsT3 (&r->fv[0], &r->fv[1], &r->fv[2]); break;
			case 5: D_PolysetDrawFinalVertsT5 (&r->fv[0], &r->fv[1], &r->fv[2]); break;
			default: D_PolysetDrawFinalVerts (&r->fv[0], &r->fv[1], &r->fv[2]); break;
			}
			continue;
		}
		tri.facesfront = r->facesfront;
		r_affinetridesc.pfinalverts = r->fv;
		r_affinetridesc.ptriangles = &tri;
		r_affinetridesc.numtriangles = 1;
		switch (r->kind)
		{
		case 1: D_PolysetDrawT (); break;
		case 2: D_PolysetDrawT2 (); break;
		case 3: D_PolysetDrawT3 (); break;
		case 5: D_PolysetDrawT5 (); break;
		default: D_PolysetDraw (); break;
		}
	}
	d_poly_ymin = 0;
	d_poly_ymax = 0x7fffffff;
	__atomic_add_fetch (&poly_busy_us, (long long) ((VID_PerfNow () - t0) * 1000.0), __ATOMIC_RELAXED);
}

static void D_PolyMT_Post (void)
{
	polybatch_t	*b;

	if (poly_nrec == poly_posted)
		return;
	b = &poly_batch[poly_nbatch++];
	b->r0 = poly_posted;
	b->r1 = poly_nrec;
	b->split = poly_split;
	poly_posted = poly_nrec;
	D_MT_PostCall (D_PolyMT_Replay, b);
}

static void D_PolyMT_Reset (void)
{
	poly_nrec = poly_nstate = poly_posted = poly_nbatch = 0;
}

/* the second core must be done with everything recorded so far: something
 * else is about to draw, or memory the records point to may change */
void D_PolyMT_Flush (void)
{
	double	t0;

	if (!d_polymt_rec)
		return;
	D_PolyMT_Post ();
	t0 = VID_PerfNow ();
	D_MT_Drain ();
	t0 = VID_PerfNow () - t0;
	poly_wait += t0;
	poly_flushwait += t0;
	D_PolyMT_Reset ();
}

qboolean D_PolyMT_Active (void)
{
	return d_polymt_rec;
}

static void D_PolyMT_Record (int kind, const finalvert_t *a, const finalvert_t *b, const finalvert_t *c, int facesfront)
{
	polyrec_t	*r;
	polystate_t	*st;

	if (poly_nrec == POLY_MAXREC || poly_nbatch >= (int) (sizeof(poly_batch) / sizeof(poly_batch[0])) - 1)
		D_PolyMT_Flush ();
	st = poly_nstate ? &poly_state[poly_nstate - 1] : NULL;
	if (!st || st->pskin != r_affinetridesc.pskin || st->skinwidth != r_affinetridesc.skinwidth ||
	    st->skinheight != r_affinetridesc.skinheight || st->seamfixupX16 != r_affinetridesc.seamfixupX16 ||
	    st->drawtype != r_affinetridesc.drawtype || st->colormap != acolormap)
	{
		if (poly_nstate == POLY_MAXSTATE)
			D_PolyMT_Flush ();
		st = &poly_state[poly_nstate++];
		st->pskin = r_affinetridesc.pskin;
		st->skinwidth = r_affinetridesc.skinwidth;
		st->skinheight = r_affinetridesc.skinheight;
		st->seamfixupX16 = r_affinetridesc.seamfixupX16;
		st->drawtype = r_affinetridesc.drawtype;
		st->colormap = acolormap;
	}
	r = &poly_rec[poly_nrec++];
	r->fv[0] = *a;
	r->fv[1] = *b;
	r->fv[2] = *c;
	r->kind = (short) kind;
	r->facesfront = (short) facesfront;
	r->state = (int) (st - poly_state);
	if (poly_nrec - poly_posted >= POLY_BATCH)
		D_PolyMT_Post ();
}

static void D_PolyMT_RecordTris (int kind)
{
	const finalvert_t	*pfv = r_affinetridesc.pfinalverts;
	const mtriangle_t	*ptri = r_affinetridesc.ptriangles;
	int			i;

	for (i = 0; i < r_affinetridesc.numtriangles; i++, ptri++)
	{
		const finalvert_t *a = pfv + ptri->vertindex[0];
		const finalvert_t *b = pfv + ptri->vertindex[1];
		const finalvert_t *c = pfv + ptri->vertindex[2];
		int lo = a->v[1], hi = a->v[1];
		if (b->v[1] < lo) lo = b->v[1];
		if (b->v[1] > hi) hi = b->v[1];
		if (c->v[1] < lo) lo = c->v[1];
		if (c->v[1] > hi) hi = c->v[1];
		if (hi < poly_split)
			continue;	/* above the line: nothing for the second core */
		D_PolyMT_Record (kind, a, b, c, ptri->facesfront);
	}
}

/* phase 0: the models, 1: the weapon */
void D_PolyMT_Begin (int phase)
{
	int	y0, h;

	if (!r_mt.integer || !r_fastloops.integer || !D_MT_On ())
		return;
	y0 = r_refdef.vrect.y;
	h = r_refdef.vrect.height;
	poly_phase = phase;
	poly_split = y0 + (int) (poly_frac[phase] * h);
	D_PolyMT_Reset ();
	d_polymt_rec = 1;
	d_poly_ymin = 0;
	d_poly_ymax = poly_split;
	poly_t_begin = VID_PerfNow ();
	poly_flushwait = 0;
	__atomic_store_n (&poly_busy_us, 0, __ATOMIC_RELAXED);
}

void D_PolyMT_End (void)
{
	double	t0, w, mine, theirs, d;

	if (!d_polymt_rec)
		return;
	D_PolyMT_Post ();
	t0 = VID_PerfNow ();
	D_MT_Drain ();
	w = VID_PerfNow () - t0;
	poly_wait += w;
	D_PolyMT_Reset ();
	d_polymt_rec = 0;
	d_poly_ymin = 0;
	d_poly_ymax = 0x7fffffff;
	/* Miyoo: move the line so that both cores work about as long: this
	 * core's time in the phase (without its waits) against the second
	 * core's time on the records. (Comparing waits alone was biased: the
	 * last records always reach the second core at the very end.) */
	mine = t0 - poly_t_begin - poly_flushwait;
	theirs = __atomic_load_n (&poly_busy_us, __ATOMIC_RELAXED) / 1000.0;
	if (mine + theirs > 0.05)
	{
		d = 0.4 * (theirs - mine) / (mine + theirs);
		if (d > 0.05)
			d = 0.05;
		if (d < -0.05)
			d = -0.05;
		poly_frac[poly_phase] += (float) d;
	}
	if (poly_frac[poly_phase] < 0.15f)
		poly_frac[poly_phase] = 0.15f;
	if (poly_frac[poly_phase] > 0.9f)
		poly_frac[poly_phase] = 0.9f;
	if (vid_perf)
	{
		VID_PerfAdd (PF_MPWAIT, poly_wait);
		poly_wait = 0;
		VID_PerfCount (PC_MPIX, __atomic_exchange_n (&d_poly_pix_mt, 0, __ATOMIC_RELAXED));
		VID_PerfCount (poly_phase ? PC_MSPLITW : PC_MSPLIT, (int) (poly_frac[poly_phase] * 100.0f + 0.5f));
	}
}
