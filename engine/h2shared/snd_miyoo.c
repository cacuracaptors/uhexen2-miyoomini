/*
 * snd_miyoo.c -- sound driver for the Miyoo Mini (Plus), through the SoC's
 * own audio output (MI_AO). It takes the place of snd_sdl.c: it defines the
 * same "snddrv_sdl" driver, so the rest of the engine does not change.
 *
 * Why not SDL audio: on this device the ALSA path accepts parameter sets it
 * does not honour (the sound came out pitched down and distorted), while
 * MI_AO plays 44.1 kHz stereo 16-bit exactly as given. This follows the
 * working MI_AO code of the OpenLara port.
 *
 * The engine mixes into a ring buffer (the "DMA buffer") at desired_speed
 * (22050 Hz by default). A thread of our own reads that ring, raises the
 * rate to 44100 Hz with linear interpolation, and sends blocks to MI_AO.
 * It advances shm->samplepos as the engine expects from a sound card.
 *
 * Based on snd_sdl.c (Hammer of Thyrion), GPL v2 or later.
 */

#include "quakedef.h"
#include "snd_sys.h"

#if HAVE_SDL_SOUND

#include "snd_sdl.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <mi_sys.h>
#include <mi_ao.h>

#define OUT_RATE	44100
#define OUT_FRAMES	1024		/* frames per block sent to MI_AO (~23 ms) */
#define OUT_RING	8		/* blocks, so a block is never rewritten while MI_AO may read it */

typedef struct { int16_t l, r; } outframe_t;

static MI_AUDIO_DEV	ao_dev = 0;
static MI_AO_CHN	ao_chn = 0;
static MI_AUDIO_Attr_t	ao_attr;

static pthread_t	ao_thread;
static pthread_mutex_t	dma_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int	ao_running;
static volatile int	ao_blocked;
static int		ao_started;

static outframe_t	ao_blocks[OUT_RING][OUT_FRAMES];

static int	buffersize;	/* bytes in the DMA ring */
static uint32_t	rate_step;	/* source frames per output frame, 16.16 */
static uint32_t	rate_phase;	/* fractional position inside the current source frame */

/*
 * Fill one output block from the DMA ring (16-bit stereo, interleaved),
 * advancing shm->samplepos. Called with dma_mutex held.
 */
static void fill_block (outframe_t *out)
{
	const int16_t	*ring;
	int		nframes, f0, f1, i;

	if (!shm || ao_blocked)
	{
		memset (out, 0, OUT_FRAMES * sizeof(outframe_t));
		return;
	}

	ring = (const int16_t *) shm->buffer;
	nframes = buffersize / (int)sizeof(outframe_t);

	f0 = shm->samplepos / 2;	/* samplepos counts mono samples */
	if (f0 >= nframes)
		f0 = 0;

	for (i = 0; i < OUT_FRAMES; i++)
	{
		int32_t	frac = (int32_t)(rate_phase & 0xFFFF);

		f1 = f0 + 1;
		if (f1 >= nframes)
			f1 = 0;

		out[i].l = (int16_t)(ring[f0*2]   + (((int32_t)ring[f1*2]   - ring[f0*2])   * frac >> 16));
		out[i].r = (int16_t)(ring[f0*2+1] + (((int32_t)ring[f1*2+1] - ring[f0*2+1]) * frac >> 16));

		rate_phase += rate_step;
		while (rate_phase >= 0x10000)
		{
			rate_phase -= 0x10000;
			f0 = f1;
			f1 = f0 + 1;
			if (f1 >= nframes)
				f1 = 0;
		}
	}

	shm->samplepos = f0 * 2;
}

static void *ao_loop (void *arg)
{
	int	slot = 0;
	int	logs = 0;
	(void) arg;

	while (ao_running)
	{
		outframe_t		*buf = ao_blocks[slot];
		MI_AUDIO_Frame_t	frame;

		slot = (slot + 1) % OUT_RING;

		pthread_mutex_lock (&dma_mutex);
		fill_block (buf);
		pthread_mutex_unlock (&dma_mutex);

		memset (&frame, 0, sizeof(frame));
		frame.eBitwidth    = ao_attr.eBitwidth;
		frame.eSoundmode   = ao_attr.eSoundmode;
		frame.u32Len       = OUT_FRAMES * sizeof(outframe_t);
		frame.apVirAddr[0] = buf;
		frame.apVirAddr[1] = NULL;

		/* Wait for real room in the output queue, then send exactly once.
		 * Keep only ~3 blocks (~70 ms) queued: filling the queue to the
		 * brim made MI_AO overwrite audio not yet played on this device. */
		while (ao_running)
		{
			MI_AO_ChnState_t	st;
			MI_U32			total, target;

			memset (&st, 0, sizeof(st));
			if (MI_AO_QueryChnStat (ao_dev, ao_chn, &st) != MI_SUCCESS)
				break;
			total  = st.u32ChnFreeNum + st.u32ChnBusyNum;
			target = (total > 64) ? frame.u32Len * 3 : 3;	/* bytes or blocks */
			if (logs < 5)
			{
				fprintf (stderr, "sound: queue free=%u busy=%u\n",
					(unsigned) st.u32ChnFreeNum, (unsigned) st.u32ChnBusyNum);
				logs++;
			}
			if (st.u32ChnBusyNum <= target)
				break;
			usleep (2000);
		}

		if (MI_AO_SendFrame (ao_dev, ao_chn, &frame, 20) != MI_SUCCESS && logs < 10)
		{
			fprintf (stderr, "sound: SendFrame rejected a block\n");
			logs++;
		}
	}
	return NULL;
}

static char s_miyoo_driver[] = "MI_AO";

static qboolean S_MIYOO_Init (dma_t *dma)
{
	MI_AUDIO_Attr_t	attr;
	MI_SYS_ChnPort_t	port;
	int		speed, tmp, val, samples;

	/* the mixer works in 11025, 22050 or 44100 Hz here; always 16-bit stereo */
	speed = desired_speed;
	if (speed != 11025 && speed != 22050 && speed != 44100)
		speed = 22050;
	if (desired_bits != 16 || desired_channels != 2)
		Con_Printf ("MI_AO: using 16-bit stereo\n");

	memset (&attr, 0, sizeof(attr));
	attr.eBitwidth      = E_MI_AUDIO_BIT_WIDTH_16;
	attr.eWorkmode      = E_MI_AUDIO_MODE_I2S_MASTER;
	attr.u32FrmNum      = 6;
	attr.u32PtNumPerFrm = OUT_FRAMES;
	attr.u32ChnCnt      = 2;
	attr.eSoundmode     = E_MI_AUDIO_SOUND_MODE_STEREO;
	attr.eSamplerate    = (MI_AUDIO_SampleRate_e) OUT_RATE;

	if (MI_AO_SetPubAttr (ao_dev, &attr) != MI_SUCCESS)
	{
		Con_Printf ("MI_AO_SetPubAttr failed\n");
		return false;
	}
	if (MI_AO_GetPubAttr (ao_dev, &ao_attr) != MI_SUCCESS)
	{
		Con_Printf ("MI_AO_GetPubAttr failed\n");
		return false;
	}
	if (MI_AO_Enable (ao_dev) != MI_SUCCESS)
	{
		Con_Printf ("MI_AO_Enable failed\n");
		return false;
	}
	if (MI_AO_EnableChn (ao_dev, ao_chn) != MI_SUCCESS)
	{
		Con_Printf ("MI_AO_EnableChn failed\n");
		MI_AO_Disable (ao_dev);
		return false;
	}
	MI_AO_SetVolume (ao_dev, 0);

	memset (&port, 0, sizeof(port));
	port.eModId    = E_MI_MODULE_ID_AO;
	port.u32DevId  = ao_dev;
	port.u32ChnId  = ao_chn;
	port.u32PortId = 0;
	MI_SYS_SetChnOutputPortDepth (&port, 12, 13);

	memset ((void *) dma, 0, sizeof(dma_t));
	shm = dma;

	shm->samplebits = 16;
	shm->signed8 = 0;
	shm->speed = speed;
	shm->channels = 2;
	/* same sizing the SDL driver uses: ten blocks of the usual SDL size,
	 * rounded up to a power of two */
	samples = (speed <= 11025) ? 256 : (speed <= 22050) ? 512 : 1024;
	tmp = (samples * shm->channels) * 10;
	if (tmp & (tmp - 1))
	{
		val = 1;
		while (val < tmp)
			val <<= 1;
		tmp = val;
	}
	shm->samples = tmp;
	shm->samplepos = 0;
	shm->submission_chunk = 1;

	buffersize = shm->samples * (shm->samplebits / 8);
	shm->buffer = (unsigned char *) calloc (1, buffersize);
	if (!shm->buffer)
	{
		MI_AO_DisableChn (ao_dev, ao_chn);
		MI_AO_Disable (ao_dev);
		shm = NULL;
		Con_Printf ("Failed allocating memory for the MI_AO sound buffer\n");
		return false;
	}

	rate_step = (uint32_t)(((uint64_t) speed << 16) / OUT_RATE);
	rate_phase = 0;
	ao_blocked = 0;

	ao_running = 1;
	if (pthread_create (&ao_thread, NULL, ao_loop, NULL) != 0)
	{
		ao_running = 0;
		free (shm->buffer);
		shm->buffer = NULL;
		MI_AO_DisableChn (ao_dev, ao_chn);
		MI_AO_Disable (ao_dev);
		shm = NULL;
		Con_Printf ("Couldn't start the MI_AO sound thread\n");
		return false;
	}
	ao_started = 1;

	Con_Printf ("MI_AO audio    : %d Hz mixer -> %d Hz output, %d samples ring\n",
			shm->speed, OUT_RATE, shm->samples);
	return true;
}

static int S_MIYOO_GetDMAPos (void)
{
	return shm->samplepos;
}

static void S_MIYOO_Shutdown (void)
{
	if (shm)
	{
		Con_Printf ("Shutting down MI_AO sound\n");
		if (ao_started)
		{
			ao_running = 0;
			pthread_join (ao_thread, NULL);
			ao_started = 0;
		}
		MI_AO_DisableChn (ao_dev, ao_chn);
		MI_AO_Disable (ao_dev);
		if (shm->buffer)
			free (shm->buffer);
		shm->buffer = NULL;
		shm = NULL;
	}
}

static void S_MIYOO_LockBuffer (void)
{
	pthread_mutex_lock (&dma_mutex);
}

static void S_MIYOO_Submit (void)
{
	pthread_mutex_unlock (&dma_mutex);
}

static void S_MIYOO_BlockSound (void)
{
	ao_blocked = 1;
}

static void S_MIYOO_UnblockSound (void)
{
	ao_blocked = 0;
}

snd_driver_t snddrv_sdl =
{
	S_MIYOO_Init,
	S_MIYOO_Shutdown,
	S_MIYOO_GetDMAPos,
	S_MIYOO_LockBuffer,
	S_MIYOO_Submit,
	S_MIYOO_BlockSound,
	S_MIYOO_UnblockSound,
	s_miyoo_driver,
	SNDDRV_ID_SDL,
	false,
	NULL
};

#endif	/* HAVE_SDL_SOUND */
