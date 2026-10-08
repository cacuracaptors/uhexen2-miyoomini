/*
 * d_iface.h -- interface header file for rasterization driver modules
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

#ifndef D_IFACE_H
#define D_IFACE_H

#define WARP_WIDTH		320
#define WARP_HEIGHT		200

#define MAX_SKIN_HEIGHT		480

typedef struct
{
	float	u, v;
	float	s, t;
	float	zi;
} emitpoint_t;

/* particle enums and types: note that hexen2 and
   hexenworld versions of these are different!! */
#include "particle.h"

typedef struct polyvert_s {
	float	u, v, zi, s, t;
} polyvert_t;

typedef struct polydesc_s {
	int			numverts;
	float		nearzi;
	msurface_t	*pcurrentface;
	polyvert_t	*pverts;
} polydesc_t;

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
typedef struct finalvert_s {
	int		v[6];		// u, v, s, t, l, 1/z
	int		flags;
	float	reserved;
} finalvert_t;

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
typedef struct
{
	void			*pskin;
	maliasskindesc_t	*pskindesc;
	int			skinwidth;
	int			skinheight;
	mtriangle_t		*ptriangles;
	finalvert_t		*pfinalverts;
	int			numtriangles;
	int			drawtype;
	int			seamfixupX16;
} affinetridesc_t;

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
typedef struct {
	float	u, v, zi, color;
} screenpart_t;

typedef struct
{
	int		nump;
	emitpoint_t	*pverts;	// there's room for an extra element at [nump], 
					//  if the driver wants to duplicate element [0] at
					//  element [nump] to avoid dealing with wrapping
	mspriteframe_t	*pspriteframe;
	vec3_t		vup, vright, vpn;	// in worldspace
	float		nearzi;
} spritedesc_t;

typedef struct
{
	int		u, v;
	float		zi;
	int		color;
} zpointdesc_t;

extern cvar_t	r_drawflat;
extern int		d_spanpixcount;
ASM_LINKAGE_BEGIN
extern int		r_framecount;	// sequence # of current frame since Quake
					//  started
ASM_LINKAGE_END
extern qboolean	r_recursiveaffinetriangles;	// true if a driver wants to use
						//  recursive triangular subdivison
						//  and vertex drawing via
						//  D_PolysetDrawFinalVerts() past
						//  a certain distance (normally 
						//  only used by the software
						//  driver)
extern float	r_aliasuvscale;		// scale-up factor for screen u and v
					//  on Alias vertices passed to driver
extern int	r_pixbytes;
extern qboolean	r_dowarp;

extern int	d_con_indirect;		// if 0, Quake will draw console directly
					//  to vid.buffer; if 1, Quake will
					//  draw console via D_DrawRect. Must be
					//  defined by the driver (vid_*.c)

#ifndef MT_TLS
#define MT_TLS	__thread	/* Miyoo: per-thread drawing state (second core) */
#endif
ASM_LINKAGE_BEGIN
extern MT_TLS affinetridesc_t	r_affinetridesc;
extern spritedesc_t	r_spritedesc;
extern zpointdesc_t	r_zpointdesc;
extern polydesc_t	r_polydesc;

extern vec3_t	r_pright, r_pup, r_ppn;
ASM_LINKAGE_END


void D_DrawSprite (void);
void D_DrawSurfaces (qboolean Translucent);


ASM_LINKAGE_BEGIN

void D_PolysetDraw (void);
void D_PolysetDrawT (void);
void D_PolysetDrawT2 (void);
void D_PolysetDrawT3 (void);
void D_PolysetDrawT5 (void);

void D_PolysetDrawFinalVerts (finalvert_t *p1, finalvert_t *p2, finalvert_t *p3);
void D_PolysetDrawFinalVertsT (finalvert_t *p1, finalvert_t *p2, finalvert_t *p3);
void D_PolysetDrawFinalVertsT2 (finalvert_t *p1, finalvert_t *p2, finalvert_t *p3);
void D_PolysetDrawFinalVertsT3 (finalvert_t *p1, finalvert_t *p2, finalvert_t *p3);
void D_PolysetDrawFinalVertsT5 (finalvert_t *p1, finalvert_t *p2, finalvert_t *p3);

#if id386
void D_DrawNonSubdiv (void);
void D_PolysetCalcGradients (int skinwidth);
void D_PolysetCalcGradientsT (int skinwidth);
void D_PolysetCalcGradientsT2 (int skinwidth);
void D_PolysetCalcGradientsT3 (int skinwidth);
void D_PolysetCalcGradientsT5 (int skinwidth);
void D_PolysetRecursiveTriangle (int *p1, int *p2, int *p3);
void D_PolysetScanLeftEdge (int height);
void D_PolysetScanLeftEdgeT (int height);
void D_PolysetScanLeftEdgeT2 (int height);
void D_PolysetScanLeftEdgeT3 (int height);
void D_PolysetScanLeftEdgeT5 (int height);
void D_DrawParticle1x1b (particle_t *pparticle);
#endif

void D_DrawParticle (particle_t *pparticle);

ASM_LINKAGE_END


void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height);
void D_EndDirectRect (int x, int y, int width, int height);
void D_EnableBackBufferAccess (void);
void D_DisableBackBufferAccess (void);

void D_DrawZPoint (void);
void D_Init (void);
void D_ViewChanged (void);
void D_SetupFrame (void);
void D_TurnZOn (void);
void D_WarpScreen (void);

void D_FillRect (vrect_t *vrect, int color);
void D_DrawRect (void);
void D_UpdateRects (vrect_t *prect);

void D_StartParticles (void);
void D_EndParticles (void);

// currently for internal use only, and should be a do-nothing function in
// hardware drivers
// FIXME: this should go away
void D_PolysetUpdateTables (void);

// these are currently for internal use only, and should not be used by drivers
extern byte				*r_skysource;

// !!! must be kept the same as in quakeasm.h !!!
#define TRANSPARENT_COLOR	0xFF

ASM_LINKAGE_BEGIN
extern MT_TLS void *acolormap;	// FIXME: should go away
/* Miyoo: alias models drawn by both cores (d_polyse.c) */
void D_PolyMT_Begin (int phase);	/* 0 the models, 1 the weapon */
void D_PolyMT_End (void);
void D_PolyMT_Flush (void);	/* before anything else draws, or model memory may move */
qboolean D_PolyMT_Active (void);
ASM_LINKAGE_END

//=======================================================================//

// callbacks to Quake

typedef struct
{
	pixel_t		*surfdat;	// destination for generated surface
	int		rowbytes;	// destination logical width in bytes
	msurface_t	*surf;		// description for surface to generate
	fixed8_t	lightadj[MAXLIGHTMAPS];
						// adjust for lightmap levels for dynamic lighting
	texture_t	*texture;	// corrected for animating textures
	int		surfmip;	// mipmapped ratio of surface texels / world pixels
	int		surfwidth;	// in mipmapped texels
	int		surfheight;	// in mipmapped texels
} drawsurf_t;

extern drawsurf_t	r_drawsurf;

void R_DrawSurface (void);
void R_GenTile (msurface_t *psurf, void *pdest);


// !!! if this is changed, it must be changed in d_ifacea.h too !!!
#define TURB_TEX_SIZE	64		// base turbulent texture size

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
#define	CYCLE		128		// turbulent cycle size

#define TILE_SIZE	128		// size of textures generated by R_GenTiledSurf

#define SKYSHIFT	7
#define	SKYSIZE		(1 << SKYSHIFT)
#define SKYMASK		(SKYSIZE - 1)

extern float	skyspeed, skyspeed2;
extern float	skytime;

extern int	c_surf;
extern vrect_t	scr_vrect;
extern vrect_t	scr_vrect3d;	/* the 3D view area in pixels of the 3D picture (== scr_vrect unless vid_3dscale > 1) */
extern int	vid_3dscale;	/* 1; 2 when the 3D view is rendered at twice the size of the 2D picture (Miyoo -ui320) */
extern int	vid_fade3d;	/* set when a menu fade must darken the 3D picture too (split mode) */
void VID_Use3D (void);		/* vid.width/height/buffer describe the 3D picture; calls nest; no-op if not split */
void VID_Use2D (void);
void VID_ClearUIView (void);

/* ---- per-phase timing, enabled by -perf (see vid_miyoo.c) ---- */
extern int	vid_perf;
double VID_PerfNow (void);
void VID_PerfAdd (int id, double ms);
void VID_PerfCount (int id, int n);
#define PERF_START()		(vid_perf ? VID_PerfNow () : 0.0)
#define PERF_STOP(t0, id)	do { if (vid_perf) VID_PerfAdd ((id), VID_PerfNow () - (t0)); } while (0)
enum { PF_INPUT, PF_SERVER, PF_CLIENT, PF_SCREEN, PF_SOUND, PF_VIEW3D,
	PF_SETUP,
	PF_WBUILD,	/* world: edges and surfaces of the visible world and brush models */
	PF_WSAVE,	/* world: copy kept for the translucent pass */
	PF_WSCAN,	/* world: sorting the edges line by line, drawing included */
	PF_WSPANS,	/*   part of PF_WSCAN: drawing the world's textures (D_DrawSurfaces) */
	PF_MODELS,	/* alias models and sprites */
	PF_TRANS,	/* water, glass, translucent brush models */
	PF_TSPANS,	/*   part of PF_TRANS: drawing them */
	PF_VMODEL, PF_PARTS, PF_WARP, PF_SNDX,
	PF_MTWAIT,	/* the first core waiting for the second one */
	PF_SBUILD,	/*   part of PF_WSPANS: rebuilding lit textures (both cores) */
	PF_MTBUSY,	/* the second core working on drawing jobs */
	PF_MTWAITC,	/*   part of PF_MTWAIT: texture cache memory still being read */
	PF_MTWAITU,	/*   part of PF_MTWAIT: waiting for the half of a texture it helps with */
	PF_MTWAITT,	/*   part of PF_MTWAIT: at the end of the translucent pass */
	PF_MSETUP,	/* alias models (weapon included): skin, light, frame */
	PF_MVERTS,	/*   vertices: transform and projection */
	PF_MTRI,	/*   triangles: clipping and drawing */
	PF_MPWAIT,	/* models: waiting for the second core's rows */
	PF_MTRIC,	/*   part of PF_MTRI: models partly off the view (clipped triangles) */
	PF_MTRIS,	/*   part of PF_MTRI: small far models (point by point subdivision) */
	PF_MRAST,	/*   part of PF_MTRI: inside D_PolysetDraw* on the first core (sampled 1 in 8) */
	PF_MTSTEAL,	/* the first core doing queued jobs it took back */
	PF_N };
enum { PC_WPOLY, PC_EPOLY, PC_AMODELS, PC_SURF, PC_TLINES, PC_FALLBACK, PC_MTJOBS, PC_MTRIS, PC_MPIX, PC_LITDEF, PC_LITCAP, PC_SHARETR, PC_MSPLIT, PC_MSPLITW, PC_MTU, PC_MTC, PC_MTS, PC_MTSTEAL, PC_THRASH, PC_MOCCL, PC_N };	/* split mode: make the 3D view area of the 2D picture transparent */

extern byte	*r_warpbuffer;

#endif	/* D_IFACE_H */

