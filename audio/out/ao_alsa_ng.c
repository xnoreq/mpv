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

#include "config.h"
#include "options/options.h"
#include "options/m_option.h"
#include "common/msg.h"

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API

#include <alsa/asoundlib.h>

#include "ao.h"
#include "audio/format.h"
#include "audio/chmap.h"

#include "ao_alsa_ng.h"

static int init(struct ao *ao)
{
    int err;
    struct priv *p = ao->priv;
    const char *device;

    snd_pcm_hw_params_t *hwparams;
    snd_pcm_sw_params_t *swparams;

    if (p->device && p->device[0])
        device = p->device;

    p->delay_before_pause = 0;
    p->prepause_frames = 0;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);

    ALSA("failed to open audio device",
         snd_pcm_open(&p->pcm, device, SND_PCM_STREAM_PLAYBACK, 0));

    ALSA("no usable playback configuration found",
         snd_pcm_hw_params_any(p->pcm, hwparams));

    ALSA("resampling setup failed",
         snd_pcm_hw_params_set_rate_resample(p->pcm, hwparams, p->resample));

    snd_pcm_access_t access = af_fmt_is_planar(ao->format)
                              ? SND_PCM_ACCESS_RW_NONINTERLEAVED
                              : SND_PCM_ACCESS_RW_INTERLEAVED;

    err = snd_pcm_hw_params_set_access(p->pcm, hwparams, access);
    if (err < 0 && access == SND_PCM_ACCESS_RW_NONINTERLEAVED) {
        MP_INFO(ao, "non-interleaved access not available\n");
        access = SND_PCM_ACCESS_RW_INTERLEAVED;
        err = snd_pcm_hw_params_set_access(p->pcm, hwparams, access);
    }
    ALSA("access type setup failed", err);

    p->format = find_alsa_format(ao->format);
    if (p->format == SND_PCM_FORMAT_UNKNOWN) {
        MP_INFO(ao, "format %s is not known to ALSA, trying default",
                af_fmt_to_str(ao->format));

        p->format = SND_PCM_FORMAT_S16;
        ao->format = AF_FORMAT_S16;
    }

    err = snd_pcm_hw_params_test_format(p->pcm, hwparams, p->format);
    if (err < 0) {
        MP_INFO(ao, "format %s is not supported by hardware, trying default",
                af_fmt_to_str(ao->format));
        p->format = SND_PCM_FORMAT_S16_LE;

        ao->format = AF_FORMAT_S16_LE;
    }
    ALSA("format setup failed",
         snd_pcm_hw_params_set_format(p->pcm, hwparams, p->format));

    if (!query_chmaps(ao)) {
        MP_ERR(ao, "querying channel maps failed\n");
        goto bail;
    }

    ALSA("channel count setup failed",
         snd_pcm_hw_params_set_channels(p->pcm, hwparams, ao->channels.num));

    ALSA("samplerate setup failed",
         snd_pcm_hw_params_set_rate_near(p->pcm, hwparams,
                                         &ao->samplerate, NULL));

    ALSA("unable to set hardware parameters",
         snd_pcm_hw_params(p->pcm, hwparams));


    snd_pcm_chmap_t *alsa_chmap = snd_pcm_get_chmap(p->pcm);
    for (int c = 0; c < ao->channels.num; c++) {
        alsa_chmap->pos[c] = find_alsa_channel(ao->channels.speaker[c]);
    }

    err = snd_pcm_set_chmap(p->pcm, alsa_chmap);

    if (err == -ENXIO) {
        MP_WARN(ao, "setting channel map not supported, hoping for the best\n");
    } else {
        ALSA("channel map setup failed", err);
    }

    ALSA("unable to get buffer size",
         snd_pcm_hw_params_get_buffer_size(hwparams, &p->buffer_size));

    ALSA("unable to get period size",
         snd_pcm_hw_params_get_period_size(hwparams, &p->period_size, NULL));

    p->can_pause = snd_pcm_hw_params_can_pause(hwparams);

    return 0;

bail:
    uninit(ao, true);
    return -1;
}

static void uninit(struct ao *ao, bool immed)
{
    struct priv *p = ao->priv;

    if (p->pcm) {
        if (immed) {
            ALSA("cannot drop audio data", snd_pcm_drop(p->pcm));
        } else {
            ALSA("cannot drain audio data", snd_pcm_drain(p->pcm));
        }

        ALSA("cannot close audio device", snd_pcm_close(p->pcm));

        MP_VERBOSE(ao, "uninit finished\n");
    }

bail:
    p->pcm = NULL;
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->prepause_frames = 0;
    p->delay_before_pause = 0;
    ALSA("cannot drop audio data", snd_pcm_drop(p->pcm));
    ALSA("cannot prepare audio device", snd_pcm_prepare(p->pcm));

bail: ;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    snd_mixer_t *mixer = NULL;

    switch (cmd) {
        case AOCONTROL_GET_MUTE:
        case AOCONTROL_SET_MUTE:
        case AOCONTROL_GET_VOLUME:
        case AOCONTROL_SET_VOLUME:
        {
            snd_mixer_elem_t *elem;
            snd_mixer_selem_id_t *sid;

            long pmin, pmax;
            float multi;

            snd_mixer_selem_id_alloca(&sid);
            snd_mixer_selem_id_set_index(sid, p->mixer_index);
            snd_mixer_selem_id_set_name(sid, p->mixer_name);

            ALSA("cannot open mixer", snd_mixer_open(&mixer, 0));
            ALSA("cannot attach mixer",
                 snd_mixer_attach(mixer, p->mixer_device));
            ALSA("cannot register mixer",
                 snd_mixer_selem_register(mixer, NULL, NULL));
            ALSA("cannot load mixer", snd_mixer_load(mixer));

            elem = snd_mixer_find_selem(mixer, sid);
            if (!elem) {
                MP_VERBOSE(ao, "unable to find simple mixer control '%s' (index %i)\n",
                        snd_mixer_selem_id_get_name(sid),
                        snd_mixer_selem_id_get_index(sid));
                goto bail;
            }

            snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
            multi = (100 / (float)(pmax - pmin));

            switch (cmd) {
                case AOCONTROL_GET_MUTE: {
                    if (!snd_mixer_selem_has_playback_switch(elem))
                        goto bail;

                    bool *mute = arg;
                    int tmp = 1;

                    snd_mixer_selem_get_playback_switch(elem,
                                                        SND_MIXER_SCHN_MONO,
                                                        &tmp);

                    *mute = !tmp;
                    break;
                }
                case AOCONTROL_SET_MUTE: {
                    if (!snd_mixer_selem_has_playback_switch(elem))
                        goto bail;

                    bool *mute = arg;

                    snd_mixer_selem_set_playback_switch_all(elem, !*mute);
                    break;
                }
                case AOCONTROL_GET_VOLUME: {
                    ao_control_vol_t *vol = arg;
                    long alsa_vol;
                    snd_mixer_selem_get_playback_volume
                        (elem, SND_MIXER_SCHN_FRONT_LEFT, &alsa_vol);
                    vol->left = (alsa_vol - pmin) * multi;
                    snd_mixer_selem_get_playback_volume
                        (elem, SND_MIXER_SCHN_FRONT_RIGHT, &alsa_vol);
                    vol->right = (alsa_vol - pmin) * multi;
                    break;
                }
                case AOCONTROL_SET_VOLUME: {
                    ao_control_vol_t *vol = arg;

                    long alsa_vol = vol->left / multi + pmin + 0.5;
                    ALSA("cannot set left channel volume",
                         snd_mixer_selem_set_playback_volume
                            (elem, SND_MIXER_SCHN_FRONT_LEFT, alsa_vol));

                    alsa_vol = vol->right / multi + pmin + 0.5;
                    ALSA("cannot set right channel volume",
                         snd_mixer_selem_set_playback_volume
                            (elem, SND_MIXER_SCHN_FRONT_RIGHT, alsa_vol));
                    break;
                }
            }

            snd_mixer_close(mixer);
            return CONTROL_OK;
        }
    }

    return CONTROL_UNKNOWN;

bail:
    if (mixer)
        snd_mixer_close(mixer);

    return CONTROL_ERROR;
}

static int play(struct ao *ao, void **data, int samples, int flags)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t res = 0;

    if (!(flags & AOPLAY_FINAL_CHUNK))
        samples = samples / p->period_size * p->period_size;

    if (samples == 0)
        return 0;

    do {
        int recovered = 0;
retry:
        if (af_fmt_is_planar(ao->format)) {
            res = snd_pcm_writen(p->pcm, data, samples);
        } else {
            res = snd_pcm_writei(p->pcm, data[0], samples);
        }

        if (res < 0) {
            switch (res) {
                case -EINTR:
                case -EPIPE:
                case -ESTRPIPE:
                    if (!recovered) {
                        recovered++;
                        MP_WARN(ao, "write failed: %s; trying to recover\n",
                                snd_strerror(res));

                        res = snd_pcm_recover(p->pcm, res, 1);
                        if (!res || res == -EAGAIN)
                            goto retry;
                    }
                    break;
            }
        }
    } while (res == 0);

    return res < 0 ? -1 : res;
}

static void audio_pause(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->can_pause) {
        snd_pcm_state_t state = snd_pcm_state(p->pcm);

        switch (state) {
            case SND_PCM_STATE_PREPARED:
                break;
            case SND_PCM_STATE_RUNNING:
                ALSA("device not ready", snd_pcm_wait(p->pcm, -1));
                p->delay_before_pause = get_delay(ao);
                ALSA("pause failed", snd_pcm_pause(p->pcm, 1));
                break;
            default:
                MP_ERR(ao, "device in bad state while pausing\n");
                goto bail;
        }
    } else {
        MP_VERBOSE(ao, "pause not supported by hardware\n");

        if (snd_pcm_delay(p->pcm, &p->prepause_frames) < 0
            || p->prepause_frames < 0) {

            p->prepause_frames = 0;
        }

        ALSA("cannot drop audio data", snd_pcm_drop(p->pcm));
    }

bail: ;
}

static void audio_resume(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->can_pause) {
        snd_pcm_state_t state = snd_pcm_state(p->pcm);

        switch(state) {
            case SND_PCM_STATE_PREPARED:
                break;
            case SND_PCM_STATE_PAUSED:
                ALSA("device not ready", snd_pcm_wait(p->pcm, -1));
                ALSA("unpause failed", snd_pcm_pause(p->pcm, 0));
                break;
            default:
                MP_ERR(ao, "device in bad state while unpausing\n");
                goto bail;
        }
    } else {
        MP_VERBOSE(ao, "unpause not supported by hardware\n");
        ALSA("cannot prepare audio device for playback",
             snd_pcm_prepare(p->pcm));

        if (p->prepause_frames)
            ao_play_silence(ao, p->prepause_frames);
    }

bail: ;
}

static int get_space(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_status_t *status;

    snd_pcm_status_alloca(&status);
    ALSA("cannot get pcm status", snd_pcm_status(p->pcm, status));

    snd_pcm_sframes_t space = snd_pcm_status_get_avail(status);
    if (space > p->buffer_size)
        space = p->buffer_size;

    return space;

bail:
    return 0;
}

static float get_delay(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t delay;

    if (snd_pcm_state(p->pcm) == SND_PCM_STATE_PAUSED)
        return p->delay_before_pause;

    if (snd_pcm_delay(p->pcm, &delay) < 0)
        return 0;

    if (delay < 0) {
        snd_pcm_forward(p->pcm, -delay);
        delay = 0;
    }

    return (float)delay / (float)ao->samplerate;
}

static int find_mp_channel(int alsa_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SP(UNKNOWN_LAST); i++) {
        if (alsa_to_mp_channels[i][0] == alsa_channel)
            return alsa_to_mp_channels[i][1];
    }

    return MP_SP(UNKNOWN_LAST);
}

static int find_alsa_channel(int mp_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SP(UNKNOWN_LAST); i++) {
        if (alsa_to_mp_channels[i][1] == mp_channel)
            return alsa_to_mp_channels[i][0];
    }

    return SND_CHMAP_UNKNOWN;
}

static int query_chmaps(struct ao *ao)
{
    struct priv *p = ao->priv;
    struct mp_chmap_sel chmap_sel = {0};

    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps(p->pcm);

    for (int i = 0; maps[i] != NULL; i++) {
        struct mp_chmap chmap = {0};

        chmap.num = maps[i]->map.channels;
        for (int c = 0; c < chmap.num; c++) {
            chmap.speaker[c] = find_mp_channel(maps[i]->map.pos[c]);
        }

        char *chmap_str = mp_chmap_to_str(&chmap);
        MP_DBG(ao, "got supported channel map: %s (type %s)\n",
               chmap_str,
               snd_pcm_chmap_type_name(maps[i]->type));
        talloc_free(chmap_str);

        mp_chmap_sel_add_map(&chmap_sel, &chmap);
    }

    snd_pcm_free_chmaps(maps);

    return ao_chmap_sel_adjust(ao, &chmap_sel, &ao->channels);
}

static int find_alsa_format(int af_format)
{
    af_format = af_fmt_from_planar(af_format);

    for (int n = 0; mp_to_alsa_format[n][0] != AF_FORMAT_UNKNOWN; n++) {
        if (mp_to_alsa_format[n][0] == af_format)
            return mp_to_alsa_format[n][1];
    }

    return SND_PCM_FORMAT_UNKNOWN;
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_alsa_ng = {
    .description    = "ALSA audio output",
    .name           = "alsa_ng",
    .init           = init,
    .uninit         = uninit,
    .reset          = reset,
    .control        = control,
    .play           = play,
    .pause          = audio_pause,
    .resume         = audio_resume,
    .get_space      = get_space,
    .get_delay      = get_delay,
    .priv_size      = sizeof(struct priv),
    .priv_defaults  = &(const struct priv) {
        .device = "default",
        .mixer_device = "default",
        .mixer_name = "Master",
        .mixer_index = 0
    },
    .options = (const struct m_option[]) {
        OPT_STRING("device", device, 0),
        OPT_STRING("mixer-device", mixer_device, 0),
        OPT_STRING("mixer-name", mixer_name, 0),
        OPT_INTRANGE("mixer-index", mixer_index, 0, 0, 99),
        OPT_FLAG("resample", resample, 0),
        {0}
    }
};
