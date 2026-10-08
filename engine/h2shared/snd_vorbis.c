/*
 * Ogg/Vorbis streaming music support, loosely based on several open source
 * Quake engine based projects with many modifications.
 *
 * Copyright (C) 2010-2012 O.Sezer <sezero@users.sourceforge.net>
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
 *
 */

#include "quakedef.h"

#if defined(USE_CODEC_VORBIS)
#include "snd_codec.h"
#include "snd_codeci.h"
#include "snd_vorbis.h"

#if defined(VORBIS_USE_STB)
/* Miyoo: Ogg Vorbis music without external libraries, with stb_vorbis
 * (public domain, single file). The whole file is read into memory when the
 * music starts, and a thread on the second core decodes it ahead into a
 * ring buffer, at the lowest priority, so that the game's own frame (on the
 * first core) only copies already decoded sound. */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"	/* harmless, in stb_vorbis */
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

#define VORBIS_SAMPLEBITS 16	/* signed 16 bit, host order */
#define VORBIS_SAMPLEWIDTH 2

#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define SV_RING		(44100 * 2 * 4)	/* samples (shorts): ~4 seconds of 44kHz stereo */
#define SV_CHUNK	2048		/* shorts decoded at a time */

typedef struct
{
	stb_vorbis	*v;
	byte		*data;		/* the whole file */
	int		channels;
	short		*ring;
	int		rd, wr, count;	/* in shorts */
	qboolean	eof, quit;
	pthread_t	thread;
	qboolean	thread_ok;
	pthread_mutex_t	lock;
	pthread_cond_t	cond;
} svstream_t;

/* decode one chunk into the ring; called with the lock held */
static void SV_DecodeChunk (svstream_t *sv)
{
	short	tmp[SV_CHUNK];
	int	n, i;

	n = stb_vorbis_get_samples_short_interleaved (sv->v, sv->channels, tmp, SV_CHUNK) * sv->channels;
	if (n <= 0)
	{
		sv->eof = true;
		return;
	}
	for (i = 0; i < n; i++)
	{
		sv->ring[sv->wr] = tmp[i];
		sv->wr = (sv->wr + 1) % SV_RING;
	}
	sv->count += n;
}

static void *SV_Thread (void *arg)
{
	svstream_t	*sv = (svstream_t *) arg;
	unsigned long	mask = 1UL << 1;

	syscall (SYS_sched_setaffinity, 0, sizeof(mask), &mask);	/* the second core */
	setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), 19);	/* only in its gaps */
	pthread_mutex_lock (&sv->lock);
	while (!sv->quit)
	{
		if (!sv->eof && SV_RING - sv->count >= SV_CHUNK)
		{
			SV_DecodeChunk (sv);
			/* let the reader in between chunks */
			pthread_mutex_unlock (&sv->lock);
			pthread_mutex_lock (&sv->lock);
			continue;
		}
		pthread_cond_wait (&sv->cond, &sv->lock);
	}
	pthread_mutex_unlock (&sv->lock);
	return NULL;
}

static qboolean S_VORBIS_CodecInitialize (void)
{
	return true;
}

static void S_VORBIS_CodecShutdown (void)
{
}

static qboolean S_VORBIS_CodecOpenStream (snd_stream_t *stream)
{
	svstream_t	*sv;
	stb_vorbis_info	info;
	int		err = 0;
	long		len = stream->fh.length;

	sv = (svstream_t *) calloc (1, sizeof(svstream_t));
	if (!sv)
		return false;
	sv->data = (byte *) malloc (len > 0 ? len : 1);
	sv->ring = (short *) malloc (SV_RING * sizeof(short));
	if (!sv->data || !sv->ring || FS_fread (sv->data, 1, len, &stream->fh) != (size_t) len)
	{
		Con_Printf("Couldn't read %s\n", stream->name);
		goto _fail;
	}
	sv->v = stb_vorbis_open_memory (sv->data, (int) len, &err, NULL);
	if (!sv->v)
	{
		Con_Printf("%s is not a valid Ogg Vorbis file (error %i).\n", stream->name, err);
		goto _fail;
	}
	info = stb_vorbis_get_info (sv->v);
	if (info.channels != 1 && info.channels != 2)
	{
		Con_Printf("Unsupported number of channels %d in %s\n", info.channels, stream->name);
		goto _fail;
	}
	sv->channels = info.channels;
	stream->info.rate = info.sample_rate;
	stream->info.channels = info.channels;
	stream->info.bits = VORBIS_SAMPLEBITS;
	stream->info.width = VORBIS_SAMPLEWIDTH;
	pthread_mutex_init (&sv->lock, NULL);
	pthread_cond_init (&sv->cond, NULL);
	sv->thread_ok = (pthread_create (&sv->thread, NULL, SV_Thread, sv) == 0);
	stream->priv = sv;
	return true;
_fail:
	if (sv->v)
		stb_vorbis_close (sv->v);
	free (sv->data);
	free (sv->ring);
	free (sv);
	return false;
}

static int S_VORBIS_CodecReadStream (snd_stream_t *stream, int bytes, void *buffer)
{
	svstream_t	*sv = (svstream_t *) stream->priv;
	short		*out = (short *) buffer;
	int		want = bytes / 2, n = 0;

	want -= want % sv->channels;
	pthread_mutex_lock (&sv->lock);
	/* the thread fell behind (or did not start): decode here */
	if (sv->count == 0 && !sv->eof)
		SV_DecodeChunk (sv);
	while (n < want && sv->count > 0)
	{
		out[n++] = sv->ring[sv->rd];
		sv->rd = (sv->rd + 1) % SV_RING;
		sv->count--;
	}
	pthread_cond_signal (&sv->cond);	/* room again */
	pthread_mutex_unlock (&sv->lock);
	return n * 2;	/* 0 only at the end of the file */
}

static void S_VORBIS_CodecCloseStream (snd_stream_t *stream)
{
	svstream_t	*sv = (svstream_t *) stream->priv;

	if (sv->thread_ok)
	{
		pthread_mutex_lock (&sv->lock);
		sv->quit = true;
		pthread_cond_signal (&sv->cond);
		pthread_mutex_unlock (&sv->lock);
		pthread_join (sv->thread, NULL);
	}
	stb_vorbis_close (sv->v);
	pthread_mutex_destroy (&sv->lock);
	pthread_cond_destroy (&sv->cond);
	free (sv->data);
	free (sv->ring);
	free (sv);
	S_CodecUtilClose(&stream);
}

static int S_VORBIS_CodecRewindStream (snd_stream_t *stream)
{
	svstream_t	*sv = (svstream_t *) stream->priv;
	int		ok;

	pthread_mutex_lock (&sv->lock);
	ok = stb_vorbis_seek_start (sv->v);
	sv->rd = sv->wr = sv->count = 0;
	sv->eof = false;
	pthread_cond_signal (&sv->cond);
	pthread_mutex_unlock (&sv->lock);
	return ok ? 0 : -1;
}

#else	/* !VORBIS_USE_STB: libvorbisfile or Tremor */

#define OV_EXCLUDE_STATIC_CALLBACKS
#if defined(VORBIS_USE_TREMOR)
/* for Tremor / Vorbisfile api differences,
 * see doc/diff.html in the Tremor package. */
#include <tremor/ivorbisfile.h>
#else
#include <vorbis/vorbisfile.h>
#endif

/* Vorbis codec can return the samples in a number of different
 * formats, we use the standard signed short format. */
#define VORBIS_SAMPLEBITS 16
#define VORBIS_SAMPLEWIDTH 2
#define VORBIS_SIGNED_DATA 1

/* CALLBACK FUNCTIONS: */

static int ovc_fclose (void *f)
{
	return 0;		/* we fclose() elsewhere. */
}

static int ovc_fseek (void *f, ogg_int64_t off, int whence)
{
	if (f == NULL) return (-1);
	return FS_fseek((fshandle_t *)f, (long) off, whence);
}

static ov_callbacks ovc_qfs =
{
	(size_t (*)(void *, size_t, size_t, void *))	FS_fread,
	(int (*)(void *, ogg_int64_t, int))		ovc_fseek,
	(int (*)(void *))				ovc_fclose,
	(long (*)(void *))				FS_ftell
};

static qboolean S_VORBIS_CodecInitialize (void)
{
	return true;
}

static void S_VORBIS_CodecShutdown (void)
{
}

static qboolean S_VORBIS_CodecOpenStream (snd_stream_t *stream)
{
	OggVorbis_File *ovFile;
	vorbis_info *ovf_info;
	long numstreams;
	int res;

	ovFile = (OggVorbis_File *) Z_Malloc(sizeof(OggVorbis_File), Z_MAINZONE);
	stream->priv = ovFile;
	res = ov_open_callbacks(&stream->fh, ovFile, NULL, 0, ovc_qfs);
	if (res != 0)
	{
		Con_Printf("%s is not a valid Ogg Vorbis file (error %i).\n",
				stream->name, res);
		goto _fail;
	}

	if (!ov_seekable(ovFile))
	{
		Con_Printf("Stream %s not seekable.\n", stream->name);
		goto _fail;
	}

	ovf_info = ov_info(ovFile, 0);
	if (!ovf_info)
	{
		Con_Printf("Unable to get stream info for %s.\n", stream->name);
		goto _fail;
	}

	/* FIXME: handle section changes */
	numstreams = ov_streams(ovFile);
	if (numstreams != 1)
	{
		Con_Printf("More than one (%ld) stream in %s.\n",
					numstreams, stream->name);
		goto _fail;
	}

	if (ovf_info->channels != 1 && ovf_info->channels != 2)
	{
		Con_Printf("Unsupported number of channels %d in %s\n",
					ovf_info->channels, stream->name);
		goto _fail;
	}

	stream->info.rate = ovf_info->rate;
	stream->info.channels = ovf_info->channels;
	stream->info.bits = VORBIS_SAMPLEBITS;
	stream->info.width = VORBIS_SAMPLEWIDTH;

	return true;
_fail:
	if (res == 0)
		ov_clear(ovFile);
	Z_Free(ovFile);
	return false;
}

static int S_VORBIS_CodecReadStream (snd_stream_t *stream, int bytes, void *buffer)
{
	int	section;	/* FIXME: handle section changes */
	int	cnt, res, rem;
	char *	ptr;

	cnt = 0; rem = bytes;
	ptr = (char *) buffer;
	while (1)
	{
	/* # ov_read() from libvorbisfile returns the decoded PCM audio
	 *   in requested endianness, signedness and word size.
	 * # ov_read() from Tremor (libvorbisidec) returns decoded audio
	 *   always in host-endian, signed 16 bit PCM format.
	 * # For both of the libraries, if the audio is multichannel,
	 *   the channels are interleaved in the output buffer.
	 */
		res = ov_read( (OggVorbis_File *)stream->priv, ptr, rem,
#ifndef VORBIS_USE_TREMOR
				host_bigendian,
				VORBIS_SAMPLEWIDTH,
				VORBIS_SIGNED_DATA,
#endif
				&section );
		if (res <= 0)
			break;
		rem -= res;
		cnt += res;
		if (rem <= 0)
			break;
		ptr += res;
	}

	if (res < 0)
		return res;
	return cnt;
}

static void S_VORBIS_CodecCloseStream (snd_stream_t *stream)
{
	ov_clear((OggVorbis_File *)stream->priv);
	Z_Free(stream->priv);
	S_CodecUtilClose(&stream);
}

static int S_VORBIS_CodecRewindStream (snd_stream_t *stream)
{
/* for libvorbisfile, the ov_time_seek() position argument
 * is seconds as doubles, whereas for Tremor libvorbisidec
 * it is milliseconds as 64 bit integers.
 */
	return ov_time_seek ((OggVorbis_File *)stream->priv, 0);
}

#endif	/* VORBIS_USE_STB */

snd_codec_t vorbis_codec =
{
	CODECTYPE_VORBIS,
	true,	/* always available. */
	"ogg",
	S_VORBIS_CodecInitialize,
	S_VORBIS_CodecShutdown,
	S_VORBIS_CodecOpenStream,
	S_VORBIS_CodecReadStream,
	S_VORBIS_CodecRewindStream,
	NULL, /* jump */
	S_VORBIS_CodecCloseStream,
	NULL
};

#endif	/* USE_CODEC_VORBIS */

