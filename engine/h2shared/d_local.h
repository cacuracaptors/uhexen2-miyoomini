/*
 * d_local.h -- private rasterization driver defs
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

#ifndef D_LOCAL_H
#define D_LOCAL_H

#include "r_shared.h"

//
// TODO: fine-tune this; it's based on providing some overage even if there
// is a 2k-wide scan, with subdivision every 8, for 256 spans of 12 bytes each
//
#define SCANBUFFERPAD		0x1000

#define R_SKY_SMASK	0x007F0000
#define R_SKY_TMASK	0x007F0000

#define DS_SPAN_LIST_END	-128

//#define SURFCACHE_SIZE_AT_320X200	600*1024
#define SURFCACHE_SIZE_AT_320X200	768*1024

typedef struct surfcache_s
{
	struct surfcache_s	*next;
	struct surfcache_s 	**owner;		// NULL is an empty chunk of memory
	int			lightadj[MAXLIGHTMAPS]; // checked for strobe flush
	int			dlight;
	int			size;		// including header
	unsigned int		width;
	unsigned int		height;		// DEBUG only needed for debug
	float			mipscale;
	struct texture_s	*texture;	// checked for animating textures
	int			drawflags;
	int			abslight;
	unsigned int		mtuse;		// Miyoo: second-core jobs using it are done when mt_tail reaches this
	byte			data[4];	// width*height elements
} surfcache_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct sspan_s
{
	int			u, v, count;
} sspan_t;

// TODO: put in span spilling to shrink list size
// !!! if this is changed, it must be changed in d_polysa.s too !!!
#define	DPS_MAXSPANS	(MAXHEIGHT + 1)
// 1 extra for spanpackage that marks end

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct {
	void		*pdest;
	short		*pz;
	int		count;
	byte		*ptex;
	int		sfrac, tfrac, light, zi;
} spanpackage_t;

typedef struct {
	int		isflattop;
	int		numleftedges;
	int		*pleftedgevert0;
	int		*pleftedgevert1;
	int		*pleftedgevert2;
	int		numrightedges;
	int		*prightedgevert0;
	int		*prightedgevert1;
	int		*prightedgevert2;
} edgetable_t;

extern float	scale_for_mip;

extern qboolean		d_roverwrapped;
extern surfcache_t	*sc_rover;
extern surfcache_t	*d_initial_rover;

#ifndef MT_TLS
/* Miyoo: the span drawing state is per thread, so that the second core can
 * draw part of the rows of a surface while the first core does the rest */
#define MT_TLS	__thread
#endif

ASM_LINKAGE_BEGIN

extern MT_TLS float	d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern MT_TLS float	d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern MT_TLS float	d_sdivzorigin, d_tdivzorigin, d_ziorigin;

extern MT_TLS fixed16_t	sadjust, tadjust;
extern MT_TLS fixed16_t	bbextents, bbextentt;

ASM_LINKAGE_END


extern void (*d_drawspans) (espan_t *pspan);

/* Miyoo: drawing on the second core (d_edge.c) */
void D_MT_Drain (void);		/* wait until the second core finished what it was given */
unsigned int D_MT_Mark (void);	/* mark of everything given to the second core so far */
unsigned int D_MT_Done (void);	/* a mark that is already finished */
qboolean D_MT_Pending (unsigned int mark);	/* the second core has not reached it yet */
void D_MT_WaitMark (unsigned int mark);	/* wait until it has (texture cache memory reused) */
void D_MT_EndFrame (void);	/* once per frame: balance the rows between the cores */
extern float	d_mt_share;	/* part of the rows drawn by the second core */
extern float	d_mt_share_tr;	/* the same, in the translucent pass */
extern int	(*d_mt_idlework)(void);	/* done by the second core between jobs; 1 = it did something */
extern int	(*d_mt_duework)(void);	/* the same, before its next job, when it is behind schedule */
void D_MT_Kick (void);		/* there is idle work now */
qboolean D_MT_Alive (void);	/* its thread is running */
qboolean D_MT_On (void);	/* jobs may be given to it now */

ASM_LINKAGE_BEGIN

void D_DrawSpans8 (espan_t *pspans);
void D_DrawSpans8T(espan_t *pspans);
void D_DrawZSpans (espan_t *pspans);
void D_DrawSingleZSpans (espan_t *pspans);

#if id386
void D_DrawSpans16 (espan_t *pspans);
void D_DrawSpans16T (espan_t *pspans);
void D_SpriteDrawSpans (sspan_t *pspan);
void D_SpriteDrawSpansT (sspan_t *pspan);
void D_SpriteDrawSpansT2 (sspan_t *pspan);
void D_DrawTurbulent8Span (void);
void D_DrawTurbulent8TSpan (void);
void D_DrawTurbulent8TQuickSpan (void);

void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage);
void D_PolysetDrawSpans8T (spanpackage_t *pspanpackage);
void D_PolysetDrawSpans8T2 (spanpackage_t *pspanpackage);
void D_PolysetDrawSpans8T3 (spanpackage_t *pspanpackage);
void D_PolysetDrawSpans8T5 (spanpackage_t *pspanpackage);

void D_Draw16StartT (void);
void D_Draw16EndT (void);
void D_DrawTurbulent8TSpanEnd (void);
void D_PolysetAff8Start (void);
void D_PolysetAff8StartT (void);
void D_PolysetAff8StartT2 (void);
void D_PolysetAff8StartT3 (void);
void D_PolysetAff8StartT5 (void);
void D_PolysetAff8End (void);
void D_PolysetAff8EndT (void);
void D_PolysetAff8EndT2 (void);
void D_PolysetAff8EndT3 (void);
void D_PolysetAff8EndT5 (void);
void D_SpriteSpansStartT (void);
void D_SpriteSpansEndT (void);
void D_SpriteSpansStartT2 (void);
void D_SpriteSpansEndT2 (void);

void D_Aff8Patch (void *pcolormap);
void D_Aff8PatchT (void *pcolormap);
void D_Aff8PatchT2 (void *pcolormap);
void D_Aff8PatchT3 (void *pcolormap);
void D_Aff8PatchT5 (void *pcolormap);

void R_TranPatch1 (void);
void R_TranPatch2 (void);
void R_TranPatch3 (void);
void R_TranPatch4 (void);
void R_TranPatch5 (void);
void R_TranPatch6 (void);
void R_TranPatch7 (void);
#endif /* id386 */

/* C funcs called from asm code: */
void D_PolysetSetEdgeTable (void);
void D_RasterizeAliasPolySmooth (void);

ASM_LINKAGE_END


void Turbulent8 (surf_t *s);

void D_DrawSkyScans8 (espan_t *pspan);
void D_DrawSkyScans16 (espan_t *pspan);

surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel);

void D_Patch (void);


ASM_LINKAGE_BEGIN

extern short	*d_pzbuffer;
extern int	d_zrowbytes, d_zwidth;

extern int	d_scantable[MAXHEIGHT];

extern int	d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;

extern int	d_y_aspect_shift, d_y_aspect_rshift, d_pix_min, d_pix_max, d_pix_shift;

extern pixel_t	*d_viewbuffer;

extern short	*zspantable[MAXHEIGHT];

#define	SCAN_SIZE		2048

extern byte	scanList[SCAN_SIZE];
extern int	ZScanCount;

extern MT_TLS int	d_aflatcolor;

ASM_LINKAGE_END

extern int	d_minmip;
extern float	d_scalemip[3];

#endif	/* D_LOCAL_H */

