/*
 *  PCM - Mu-Law conversion
 *  Copyright (c) 2000 by Abramo Bagnara <abramo@alsa-project.org>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#include <byteswap.h>
#include "pcm_local.h"
#include "pcm_plugin.h"

typedef void (*mulaw_f)(snd_pcm_channel_area_t *src_areas,
			size_t src_offset,
			snd_pcm_channel_area_t *dst_areas,
			size_t dst_offset,
			size_t frames, size_t channels, int getputidx);

typedef struct {
	/* This field need to be the first */
	snd_pcm_plugin_t plug;
	int getput_idx;
	mulaw_f func;
	int sformat;
} snd_pcm_mulaw_t;

static inline int val_seg(int val)
{
	int r = 0;
	val >>= 7;
	if (val & 0xf0) {
		val >>= 4;
		r += 4;
	}
	if (val & 0x0c) {
		val >>= 2;
		r += 2;
	}
	if (val & 0x02)
		r += 1;
	return r;
}

/*
 * s16_to_ulaw() - Convert a linear PCM value to u-law
 *
 * In order to simplify the encoding process, the original linear magnitude
 * is biased by adding 33 which shifts the encoding range from (0 - 8158) to
 * (33 - 8191). The result can be seen in the following encoding table:
 *
 *	Biased Linear Input Code	Compressed Code
 *	------------------------	---------------
 *	00000001wxyza			000wxyz
 *	0000001wxyzab			001wxyz
 *	000001wxyzabc			010wxyz
 *	00001wxyzabcd			011wxyz
 *	0001wxyzabcde			100wxyz
 *	001wxyzabcdef			101wxyz
 *	01wxyzabcdefg			110wxyz
 *	1wxyzabcdefgh			111wxyz
 *
 * Each biased linear code has a leading 1 which identifies the segment
 * number. The value of the segment number is equal to 7 minus the number
 * of leading 0's. The quantization interval is directly available as the
 * four bits wxyz.  * The trailing bits (a - h) are ignored.
 *
 * Ordinarily the complement of the resulting code word is used for
 * transmission, and so the code word is complemented before it is returned.
 *
 * For further information see John C. Bellamy's Digital Telephony, 1982,
 * John Wiley & Sons, pps 98-111 and 472-476.
 */

static unsigned char s16_to_ulaw(int pcm_val)	/* 2's complement (16-bit range) */
{
	int mask;
	int seg;
	unsigned char uval;

	if (pcm_val < 0) {
		pcm_val = -pcm_val + 0x84;
		mask = 0x7f;
	} else {
		pcm_val += 0x84;
		mask = 0xff;
	}
	if (pcm_val > 0x7fff)
		pcm_val = 0x7fff;

	/* Convert the scaled magnitude to segment number. */
	seg = val_seg(pcm_val);

	/*
	 * Combine the sign, segment, quantization bits;
	 * and complement the code word.
	 */
	uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0f);
	return uval ^ mask;
}

/*
 * ulaw_to_s16() - Convert a u-law value to 16-bit linear PCM
 *
 * First, a biased linear code is derived from the code word. An unbiased
 * output can then be obtained by subtracting 33 from the biased code.
 *
 * Note that this function expects to be passed the complement of the
 * original code word. This is in keeping with ISDN conventions.
 */
static int ulaw_to_s16(unsigned char u_val)
{
	int t;

	/* Complement to obtain normal u-law value. */
	u_val = ~u_val;

	/*
	 * Extract and bias the quantization bits. Then
	 * shift up by the segment number and subtract out the bias.
	 */
	t = ((u_val & 0x0f) << 3) + 0x84;
	t <<= (u_val & 0x70) >> 4;

	return ((u_val & 0x80) ? (0x84 - t) : (t - 0x84));
}

static void mulaw_decode(snd_pcm_channel_area_t *src_areas,
			 size_t src_offset,
			 snd_pcm_channel_area_t *dst_areas,
			 size_t dst_offset,
			 size_t frames, size_t channels, int putidx)
{
#define PUT16_LABELS
#include "plugin_ops.h"
#undef PUT16_LABELS
	void *put = put16_labels[putidx];
	size_t channel;
	for (channel = 0; channel < channels; ++channel) {
		char *src;
		char *dst;
		int src_step, dst_step;
		size_t frames1;
		snd_pcm_channel_area_t *src_area = &src_areas[channel];
		snd_pcm_channel_area_t *dst_area = &dst_areas[channel];
#if 0
		if (!src_area->enabled) {
			if (dst_area->wanted)
				snd_pcm_area_silence(&dst_areas[channel], dst_offset, frames, dst_sfmt);
			dst_area->enabled = 0;
			continue;
		}
		dst_area->enabled = 1;
#endif
		src = snd_pcm_channel_area_addr(src_area, src_offset);
		dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
		src_step = snd_pcm_channel_area_step(src_area);
		dst_step = snd_pcm_channel_area_step(dst_area);
		frames1 = frames;
		while (frames1-- > 0) {
			int16_t sample = ulaw_to_s16(*src);
			goto *put;
#define PUT16_END after
#include "plugin_ops.h"
#undef PUT16_END
		after:
			src += src_step;
			dst += dst_step;
		}
	}
}

static void mulaw_encode(snd_pcm_channel_area_t *src_areas,
			 size_t src_offset,
			 snd_pcm_channel_area_t *dst_areas,
			 size_t dst_offset,
			 size_t frames, size_t channels, int getidx)
{
#define GET16_LABELS
#include "plugin_ops.h"
#undef GET16_LABELS
	void *get = get16_labels[getidx];
	size_t channel;
	int16_t sample = 0;
	for (channel = 0; channel < channels; ++channel) {
		char *src;
		char *dst;
		int src_step, dst_step;
		size_t frames1;
		snd_pcm_channel_area_t *src_area = &src_areas[channel];
		snd_pcm_channel_area_t *dst_area = &dst_areas[channel];
#if 0
		if (!src_area->enabled) {
			if (dst_area->wanted)
				snd_pcm_area_silence(&dst_area->area, 0, frames, dst_sfmt);
			dst_area->enabled = 0;
			continue;
		}
		dst_area->enabled = 1;
#endif
		src = snd_pcm_channel_area_addr(src_area, src_offset);
		dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
		src_step = snd_pcm_channel_area_step(src_area);
		dst_step = snd_pcm_channel_area_step(dst_area);
		frames1 = frames;
		while (frames1-- > 0) {
			goto *get;
#define GET16_END after
#include "plugin_ops.h"
#undef GET16_END
		after:
			*dst = s16_to_ulaw(sample);
			src += src_step;
			dst += dst_step;
		}
	}
}

static int snd_pcm_mulaw_hw_info(snd_pcm_t *pcm, snd_pcm_hw_info_t * info)
{
	snd_pcm_mulaw_t *mulaw = pcm->private;
	unsigned int format_mask, access_mask;
	int err;
	info->access_mask &= (SND_PCM_ACCBIT_MMAP_INTERLEAVED | 
			      SND_PCM_ACCBIT_RW_INTERLEAVED |
			      SND_PCM_ACCBIT_MMAP_NONINTERLEAVED | 
			      SND_PCM_ACCBIT_RW_NONINTERLEAVED);
	access_mask = info->access_mask;
	if (access_mask == 0)
		return -EINVAL;
	if (mulaw->sformat == SND_PCM_FORMAT_MU_LAW)
		info->format_mask &= SND_PCM_FMTBIT_LINEAR;
	else
		info->format_mask &= SND_PCM_FMTBIT_MU_LAW;
	format_mask = info->format_mask;
	if (format_mask == 0)
		return -EINVAL;

	info->format_mask = 1U << mulaw->sformat;
	info->access_mask = SND_PCM_ACCBIT_MMAP;
	err = snd_pcm_hw_info(mulaw->plug.slave, info);
	info->format_mask = format_mask;
	info->access_mask = access_mask;
	if (err < 0)
		return err;
	info->info &= ~(SND_PCM_INFO_MMAP | SND_PCM_INFO_MMAP_VALID);
	snd_pcm_hw_info_complete(info);
	return 0;
}

static int snd_pcm_mulaw_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t * params)
{
	snd_pcm_mulaw_t *mulaw = pcm->private;
	snd_pcm_t *slave = mulaw->plug.slave;
	snd_pcm_hw_info_t sinfo;
	snd_pcm_hw_params_t sparams;
	int err;
	snd_pcm_hw_params_to_info(params, &sinfo);
	sinfo.format_mask = 1 << mulaw->sformat;
	sinfo.access_mask = SND_PCM_ACCBIT_MMAP;
	err = snd_pcm_hw_params_info(slave, &sparams, &sinfo);
	params->fail_mask = sparams.fail_mask;
	if (err < 0)
		return err;
	if (pcm->stream == SND_PCM_STREAM_PLAYBACK) {
		if (mulaw->sformat == SND_PCM_FORMAT_MU_LAW) {
			mulaw->getput_idx = get_index(params->format, SND_PCM_FORMAT_S16);
			mulaw->func = mulaw_encode;
		} else {
			mulaw->getput_idx = put_index(SND_PCM_FORMAT_S16, mulaw->sformat);
			mulaw->func = mulaw_decode;
		}
	} else {
		if (mulaw->sformat == SND_PCM_FORMAT_MU_LAW) {
			mulaw->getput_idx = put_index(SND_PCM_FORMAT_S16, params->format);
			mulaw->func = mulaw_decode;
		} else {
			mulaw->getput_idx = get_index(mulaw->sformat, SND_PCM_FORMAT_S16);
			mulaw->func = mulaw_encode;
		}
	}
	return 0;
}

static ssize_t snd_pcm_mulaw_write_areas(snd_pcm_t *pcm,
					 snd_pcm_channel_area_t *areas,
					 size_t offset,
					 size_t size,
					 size_t *slave_sizep)
{
	snd_pcm_mulaw_t *mulaw = pcm->private;
	snd_pcm_t *slave = mulaw->plug.slave;
	size_t xfer = 0;
	ssize_t err = 0;
	if (slave_sizep && *slave_sizep < size)
		size = *slave_sizep;
	assert(size > 0);
	while (xfer < size) {
		size_t frames = snd_pcm_mmap_playback_xfer(slave, size - xfer);
		mulaw->func(areas, offset, 
			    snd_pcm_mmap_areas(slave), snd_pcm_mmap_offset(slave),
			    frames, pcm->channels,
			    mulaw->getput_idx);
		err = snd_pcm_mmap_forward(slave, frames);
		if (err < 0)
			break;
		assert((size_t)err == frames);
		offset += err;
		xfer += err;
		snd_pcm_mmap_hw_forward(pcm, err);
	}
	if (xfer > 0) {
		if (slave_sizep)
			*slave_sizep = xfer;
		return xfer;
	}
	return err;
}

static ssize_t snd_pcm_mulaw_read_areas(snd_pcm_t *pcm,
					snd_pcm_channel_area_t *areas,
					size_t offset,
					size_t size,
					size_t *slave_sizep)
{
	snd_pcm_mulaw_t *mulaw = pcm->private;
	snd_pcm_t *slave = mulaw->plug.slave;
	size_t xfer = 0;
	ssize_t err = 0;
	if (slave_sizep && *slave_sizep < size)
		size = *slave_sizep;
	assert(size > 0);
	while (xfer < size) {
		size_t frames = snd_pcm_mmap_capture_xfer(slave, size - xfer);
		mulaw->func(snd_pcm_mmap_areas(slave), snd_pcm_mmap_offset(slave),
			    areas, offset, 
			    frames, pcm->channels,
			    mulaw->getput_idx);
		err = snd_pcm_mmap_forward(slave, frames);
		if (err < 0)
			break;
		assert((size_t)err == frames);
		offset += err;
		xfer += err;
		snd_pcm_mmap_hw_forward(pcm, err);
	}
	if (xfer > 0) {
		if (slave_sizep)
			*slave_sizep = xfer;
		return xfer;
	}
	return err;
}

static void snd_pcm_mulaw_dump(snd_pcm_t *pcm, FILE *fp)
{
	snd_pcm_mulaw_t *mulaw = pcm->private;
	fprintf(fp, "Mu-Law conversion PCM (%s)\n", 
		snd_pcm_format_name(mulaw->sformat));
	if (pcm->setup) {
		fprintf(fp, "Its setup is:\n");
		snd_pcm_dump_setup(pcm, fp);
	}
	fprintf(fp, "Slave: ");
	snd_pcm_dump(mulaw->plug.slave, fp);
}

snd_pcm_ops_t snd_pcm_mulaw_ops = {
	close: snd_pcm_plugin_close,
	info: snd_pcm_plugin_info,
	hw_info: snd_pcm_mulaw_hw_info,
	hw_params: snd_pcm_mulaw_hw_params,
	sw_params: snd_pcm_plugin_sw_params,
	dig_info: snd_pcm_plugin_dig_info,
	dig_params: snd_pcm_plugin_dig_params,
	channel_info: snd_pcm_plugin_channel_info,
	dump: snd_pcm_mulaw_dump,
	nonblock: snd_pcm_plugin_nonblock,
	async: snd_pcm_plugin_async,
	mmap: snd_pcm_plugin_mmap,
	munmap: snd_pcm_plugin_munmap,
};

int snd_pcm_mulaw_open(snd_pcm_t **pcmp, char *name, int sformat, snd_pcm_t *slave, int close_slave)
{
	snd_pcm_t *pcm;
	snd_pcm_mulaw_t *mulaw;
	assert(pcmp && slave);
	if (snd_pcm_format_linear(sformat) != 1 &&
	    sformat != SND_PCM_FORMAT_MU_LAW)
		return -EINVAL;
	mulaw = calloc(1, sizeof(snd_pcm_mulaw_t));
	if (!mulaw) {
		return -ENOMEM;
	}
	mulaw->sformat = sformat;
	mulaw->plug.read = snd_pcm_mulaw_read_areas;
	mulaw->plug.write = snd_pcm_mulaw_write_areas;
	mulaw->plug.slave = slave;
	mulaw->plug.close_slave = close_slave;

	pcm = calloc(1, sizeof(snd_pcm_t));
	if (!pcm) {
		free(mulaw);
		return -ENOMEM;
	}
	if (name)
		pcm->name = strdup(name);
	pcm->type = SND_PCM_TYPE_MULAW;
	pcm->stream = slave->stream;
	pcm->mode = slave->mode;
	pcm->ops = &snd_pcm_mulaw_ops;
	pcm->op_arg = pcm;
	pcm->fast_ops = &snd_pcm_plugin_fast_ops;
	pcm->fast_op_arg = pcm;
	pcm->private = mulaw;
	pcm->poll_fd = slave->poll_fd;
	pcm->hw_ptr = &mulaw->plug.hw_ptr;
	pcm->appl_ptr = &mulaw->plug.appl_ptr;
	*pcmp = pcm;

	return 0;
}

int _snd_pcm_mulaw_open(snd_pcm_t **pcmp, char *name,
			 snd_config_t *conf, 
			 int stream, int mode)
{
	snd_config_iterator_t i;
	char *sname = NULL;
	int err;
	snd_pcm_t *spcm;
	int sformat = -1;
	snd_config_foreach(i, conf) {
		snd_config_t *n = snd_config_entry(i);
		if (strcmp(n->id, "comment") == 0)
			continue;
		if (strcmp(n->id, "type") == 0)
			continue;
		if (strcmp(n->id, "stream") == 0)
			continue;
		if (strcmp(n->id, "sname") == 0) {
			err = snd_config_string_get(n, &sname);
			if (err < 0) {
				ERR("Invalid type for %s", n->id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(n->id, "sformat") == 0) {
			char *f;
			err = snd_config_string_get(n, &f);
			if (err < 0) {
				ERR("Invalid type for %s", n->id);
				return -EINVAL;
			}
			sformat = snd_pcm_format_value(f);
			if (sformat < 0) {
				ERR("Unknown sformat");
				return -EINVAL;
			}
			if (snd_pcm_format_linear(sformat) != 1 &&
			    sformat != SND_PCM_FORMAT_MU_LAW) {
				ERR("Invalid sformat");
				return -EINVAL;
			}
			continue;
		}
		ERR("Unknown field %s", n->id);
		return -EINVAL;
	}
	if (!sname) {
		ERR("sname is not defined");
		return -EINVAL;
	}
	if (sformat < 0) {
		ERR("sformat is not defined");
		return -EINVAL;
	}
	/* This is needed cause snd_config_update may destroy config */
	sname = strdup(sname);
	if (!sname)
		return  -ENOMEM;
	err = snd_pcm_open(&spcm, sname, stream, mode);
	free(sname);
	if (err < 0)
		return err;
	err = snd_pcm_mulaw_open(pcmp, name, sformat, spcm, 1);
	if (err < 0)
		snd_pcm_close(spcm);
	return err;
}
				

