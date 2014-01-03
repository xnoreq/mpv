/*
 * This file is part of mpv.
 *
 * Original author: Martin Herkt <lachs0r@srsfckn.biz>
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#define ALSA(msg, err) \
{ \
    if (err < 0) { \
        MP_ERR(ao, "%s: %s\n", (msg), snd_strerror(err)); \
        goto bail; \
    } \
}

struct priv {
    snd_pcm_t *pcm;
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    int can_pause;
    float delay_before_pause;
    snd_pcm_sframes_t prepause_frames;

    char *device;
    char *mixer_device;
    char *mixer_name;
    int mixer_index;
    int resample;
    snd_pcm_format_t format;
};

static int init(struct ao *ao);
static void uninit(struct ao *ao, bool immed);
static void reset(struct ao *ao);
static int control(struct ao *ao, enum aocontrol cmd, void *arg);
static int play(struct ao *ao, void **data, int samples, int flags);
static void audio_pause(struct ao *ao);
static void audio_resume(struct ao *ao);
static int get_space(struct ao *ao);
static float get_delay(struct ao *ao);

static const int alsa_to_mp_channels[][2] = {
    {SND_CHMAP_MONO,  MP_SP(FC)},
    {SND_CHMAP_FL,    MP_SP(FL)},
    {SND_CHMAP_FR,    MP_SP(FR)},
    {SND_CHMAP_RL,    MP_SP(BL)},
    {SND_CHMAP_RR,    MP_SP(BR)},
    {SND_CHMAP_FC,    MP_SP(FC)},
    {SND_CHMAP_LFE,   MP_SP(LFE)},
    {SND_CHMAP_SL,    MP_SP(SL)},
    {SND_CHMAP_SR,    MP_SP(SR)},
    {SND_CHMAP_RC,    MP_SP(BC)},
    {SND_CHMAP_FLC,   MP_SP(FLC)},
    {SND_CHMAP_FRC,   MP_SP(FRC)},
    {SND_CHMAP_FLW,   MP_SP(WL)},
    {SND_CHMAP_FRW,   MP_SP(WR)},
    {SND_CHMAP_TC,    MP_SP(TC)},
    {SND_CHMAP_TFL,   MP_SP(TFL)},
    {SND_CHMAP_TFR,   MP_SP(TFR)},
    {SND_CHMAP_TFC,   MP_SP(TFC)},
    {SND_CHMAP_TRL,   MP_SP(TBL)},
    {SND_CHMAP_TRR,   MP_SP(TBR)},
    {SND_CHMAP_TRC,   MP_SP(TBC)},
    {SND_CHMAP_LAST,  MP_SP(UNKNOWN_LAST)}
};
static int find_mp_channel(int alsa_channel);
static int find_alsa_channel(int mp_channel);
static int query_chmaps(struct ao *ao);

static const int mp_to_alsa_format[][2] = {
    {AF_FORMAT_S8,          SND_PCM_FORMAT_S8},
    {AF_FORMAT_U8,          SND_PCM_FORMAT_U8},
    {AF_FORMAT_U16_LE,      SND_PCM_FORMAT_U16_LE},
    {AF_FORMAT_U16_BE,      SND_PCM_FORMAT_U16_BE},
    {AF_FORMAT_S16_LE,      SND_PCM_FORMAT_S16_LE},
    {AF_FORMAT_S16_BE,      SND_PCM_FORMAT_S16_BE},
    {AF_FORMAT_U32_LE,      SND_PCM_FORMAT_U32_LE},
    {AF_FORMAT_U32_BE,      SND_PCM_FORMAT_U32_BE},
    {AF_FORMAT_S32_LE,      SND_PCM_FORMAT_S32_LE},
    {AF_FORMAT_S32_BE,      SND_PCM_FORMAT_S32_BE},
    {AF_FORMAT_U24_LE,      SND_PCM_FORMAT_U24_3LE},
    {AF_FORMAT_U24_BE,      SND_PCM_FORMAT_U24_3BE},
    {AF_FORMAT_S24_LE,      SND_PCM_FORMAT_S24_3LE},
    {AF_FORMAT_S24_BE,      SND_PCM_FORMAT_S24_3BE},
    {AF_FORMAT_FLOAT_LE,    SND_PCM_FORMAT_FLOAT_LE},
    {AF_FORMAT_FLOAT_BE,    SND_PCM_FORMAT_FLOAT_BE},
    {AF_FORMAT_AC3_LE,      SND_PCM_FORMAT_S16_LE},
    {AF_FORMAT_AC3_BE,      SND_PCM_FORMAT_S16_BE},
    {AF_FORMAT_IEC61937_LE, SND_PCM_FORMAT_S16_LE},
    {AF_FORMAT_IEC61937_BE, SND_PCM_FORMAT_S16_BE},
    {AF_FORMAT_MPEG2,       SND_PCM_FORMAT_MPEG},
    {AF_FORMAT_UNKNOWN,     SND_PCM_FORMAT_UNKNOWN}
};
static int find_alsa_format(int af_format);
