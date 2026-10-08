/* d_vars.c - rasterization driver global variables
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
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

#include	"quakedef.h"
#include	"r_local.h"	/* MT_TLS */

#if	!id386

// all global and static refresh variables are collected in a contiguous block
// to avoid cache conflicts.

//-------------------------------------------------------
// global refresh variables
//-------------------------------------------------------

// FIXME: make into one big structure, like cl or sv
// FIXME: do separately for refresh engine and driver

MT_TLS float	d_sdivzstepu, d_tdivzstepu, d_zistepu;
MT_TLS float	d_sdivzstepv, d_tdivzstepv, d_zistepv;
MT_TLS float	d_sdivzorigin, d_tdivzorigin, d_ziorigin;

MT_TLS fixed16_t	sadjust, tadjust, bbextents, bbextentt;

MT_TLS pixel_t		*cacheblock;
MT_TLS int		cachewidth;

pixel_t		*d_viewbuffer;

short		*d_pzbuffer;
int		d_zrowbytes;
int		d_zwidth;

#endif	/* !id386 */
