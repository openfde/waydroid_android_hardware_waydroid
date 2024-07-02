/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <alsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>
#include "pa_volume_ctl.h"

/* Minimum granularity - Arbitrary but small value */
#define CODEC_BASE_FRAME_COUNT 32

#define CHANNEL_STEREO 2

#define PCM_OPEN_RETRIES 100
#define PCM_OPEN_WAIT_TIME_MS 20

/* Capture codec parameters */
/* Set up a capture period of 20 ms:
 * CAPTURE_PERIOD = PERIOD_SIZE / SAMPLE_RATE, so (20e-3) = PERIOD_SIZE / (16e3)
 * => PERIOD_SIZE = 320 frames, where each "frame" consists of 1 sample of every channel (here, 2ch) */
#define CAPTURE_PERIOD_MULTIPLIER 10
#define CAPTURE_PERIOD_SIZE (CODEC_BASE_FRAME_COUNT * CAPTURE_PERIOD_MULTIPLIER)
#define CAPTURE_PERIOD_COUNT 2
#define CAPTURE_PERIOD_START_THRESHOLD 0
#define CAPTURE_CODEC_SAMPLING_RATE 16000

/* Playback codec parameters */
/* number of base blocks in a short period (low latency) */
#define PLAYBACK_PERIOD_MULTIPLIER 32  /* 21 ms */
/* number of frames per short period (low latency) */
#define PLAYBACK_PERIOD_SIZE 2048
/* number of pseudo periods for low latency playback */
#define PLAYBACK_PERIOD_COUNT 4
#define PLAYBACK_PERIOD_START_THRESHOLD 2
#define PLAYBACK_CODEC_SAMPLING_RATE 48000
#define MIN_WRITE_SLEEP_US      2000

struct alsa_audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    int out_devices;
    int in_devices;
    struct alsa_stream_in *active_input;
    struct alsa_stream_out *active_output;
    bool mic_mute;
};

/* Configuration for a stream */
struct pcm_config {
    unsigned int channels;
    unsigned int rate;
    unsigned int period_size;
    unsigned int period_count;
    snd_pcm_format_t format;

    /* Values to use for the ALSA start, stop and silence thresholds, and
     * silence size.  Setting any one of these values to 0 will cause the
     * default tinyalsa values to be used instead.
     * Tinyalsa defaults are as follows.
     *
     * start_threshold   : period_count * period_size
     * stop_threshold    : period_count * period_size
     * silence_threshold : 0
     * silence_size      : 0
     */
    unsigned int start_threshold;
    unsigned int stop_threshold;
    unsigned int silence_threshold;
    unsigned int silence_size;

    /* Minimum number of frames available before pcm_mmap_write() will actually
     * write into the kernel buffer. Only used if the stream is opened in mmap mode
     * (pcm_open() called with PCM_MMAP flag set).   Use 0 for default.
     */
    int avail_min;
};

struct alsa_stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    struct pcm_config config;
    snd_pcm_t *pcm;
    bool unavailable;
    bool standby;
    struct alsa_audio_device *dev;
    int read_threshold;
    unsigned int read;
};

struct alsa_stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    struct pcm_config config;
    snd_pcm_t *pcm;
    bool unavailable;
    int standby;
    struct alsa_audio_device *dev;
    int write_threshold;
    unsigned int written;
};

static size_t out_get_buffer_size(const struct audio_stream *stream);

static void select_devices(struct alsa_audio_device *adev)
{
    int headphones_on;
    int speaker_on;
    int headset_mic_on;

    headphones_on = adev->out_devices & (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                                    AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    speaker_on = adev->out_devices & AUDIO_DEVICE_OUT_SPEAKER;
    headset_mic_on = adev->in_devices & AUDIO_DEVICE_IN_WIRED_HEADSET;

    ALOGV("hp=%c speaker=%c headset-mic=%c", headphones_on ? 'y' : 'n', speaker_on ? 'y' : 'n', headset_mic_on ? 'y' : 'n');
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct alsa_stream_out *out)
{
    struct alsa_audio_device *adev = out->dev;
    snd_pcm_hw_params_t *hwparams;
    int ret;

    /* default to low power: will be corrected in out_write if necessary before first write to
     * tinyalsa.
     */
    out->write_threshold = PLAYBACK_PERIOD_COUNT * PLAYBACK_PERIOD_SIZE;
    out->config.start_threshold = PLAYBACK_PERIOD_START_THRESHOLD * PLAYBACK_PERIOD_SIZE;
    out->config.avail_min = PLAYBACK_PERIOD_SIZE;
    out->unavailable = true;
    unsigned int pcm_retry_count = PCM_OPEN_RETRIES;
 
    while (1) {
        ret = snd_pcm_open(&out->pcm, "pulse", SND_PCM_STREAM_PLAYBACK, 0);
        if (ret < 0) {
            if (out->pcm != NULL) {
                snd_pcm_close(out->pcm);
                out->pcm = NULL;
            }
            if (--pcm_retry_count == 0) {
                ALOGE("Failed to open pcm_out after %d tries", PCM_OPEN_RETRIES);
                adev->active_output = NULL;
                return -ENODEV;
            }
            usleep(PCM_OPEN_WAIT_TIME_MS * 1000);
        } else {
            break;
        }
    }

    snd_pcm_hw_params_alloca(&hwparams);
    if (snd_pcm_hw_params_any(out->pcm, hwparams) < 0) {
        ALOGE("Can not configure this PCM device.");
        adev->active_output = NULL;
        return -ENODEV;
    }

    if (snd_pcm_hw_params_set_access(out->pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        ALOGE("Error setting access.\n");
        adev->active_output = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_format(out->pcm, hwparams, out->config.format) < 0) {
        ALOGE("Error setting format.\n");
        adev->active_output = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_rate_near(out->pcm, hwparams, &out->config.rate, 0) < 0) {
        ALOGE("Error setting rate.\n");
        adev->active_output = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_channels(out->pcm, hwparams, out->config.channels) < 0) {
        ALOGE("Error setting channels.\n");
        adev->active_output = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_periods(out->pcm, hwparams, out->config.period_count, 0) < 0) {
        ALOGE("Error setting periods.\n");
        adev->active_output = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_buffer_size(out->pcm, hwparams, out_get_buffer_size((struct audio_stream *)out)) < 0) {
        ALOGE("Error setting periods.\n");
        adev->active_output = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params(out->pcm, hwparams) < 0) {
        ALOGE("Error setting HW params.");
        adev->active_output = NULL;
        return -ENODEV;
    }

    if (snd_pcm_prepare(out->pcm) < 0) {
       ALOGE("Can not prepare this PCM device.");
        adev->active_output = NULL;
        return -ENODEV;
    }

    if (snd_pcm_state(out->pcm) != SND_PCM_STATE_PREPARED) {
        ALOGE("cannot open pcm_out driver");
        snd_pcm_close(out->pcm);
        adev->active_output = NULL;
        return -ENODEV;
    }

    out->unavailable = false;
    adev->active_output = out;
    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("out_set_sample_rate: %d", 0);
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    ALOGV("out_get_buffer_size: %d", 4096);

    /* return the closest majoring multiple of 16 frames, as
     * audioflinger expects audio buffers to be a multiple of 16 frames */
    size_t size = PLAYBACK_PERIOD_SIZE;
    size = ((size + 15) / 16) * 16;
    return size * audio_stream_out_frame_size((struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    ALOGV("out_get_channels");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return audio_channel_out_mask_from_count(out->config.channels);
}

/* Converts audio_format to pcm_format.
 * Parameters:
 *  format  the audio_format_t to convert
 *
 * Logs a fatal error if format is not a valid convertible audio_format_t.
 */
static inline snd_pcm_format_t pcm_format_from_audio_format(audio_format_t format)
{
    switch (format) {
#if HAVE_BIG_ENDIAN
    case AUDIO_FORMAT_PCM_16_BIT:
        return SND_PCM_FORMAT_S16_BE;
    case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        return SND_PCM_FORMAT_S24_3BE;
    case AUDIO_FORMAT_PCM_32_BIT:
        return SND_PCM_FORMAT_S32_BE;
    case AUDIO_FORMAT_PCM_8_24_BIT:
        return SND_PCM_FORMAT_S24_BE;
#else
    case AUDIO_FORMAT_PCM_16_BIT:
        return SND_PCM_FORMAT_S16_LE;
    case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        return SND_PCM_FORMAT_S24_3LE;
    case AUDIO_FORMAT_PCM_32_BIT:
        return SND_PCM_FORMAT_S32_LE;
    case AUDIO_FORMAT_PCM_8_24_BIT:
        return SND_PCM_FORMAT_S24_LE;
#endif
    case AUDIO_FORMAT_PCM_FLOAT:  /* there is no equivalent for float */
    default:
        LOG_ALWAYS_FATAL("pcm_format_from_audio_format: invalid audio format %#x", format);
        return 0;
    }
}

/* Converts pcm_format to audio_format.
 * Parameters:
 *  format  the pcm_format to convert
 *
 * Logs a fatal error if format is not a valid convertible pcm_format.
 */
static audio_format_t audio_format_from_pcm_format(snd_pcm_format_t format)
{
    switch (format) {
#if HAVE_BIG_ENDIAN
    case SND_PCM_FORMAT_S16_BE:
        return AUDIO_FORMAT_PCM_16_BIT;
    case SND_PCM_FORMAT_S24_3BE:
        return AUDIO_FORMAT_PCM_24_BIT_PACKED;
    case SND_PCM_FORMAT_S24_BE:
        return AUDIO_FORMAT_PCM_8_24_BIT;
    case SND_PCM_FORMAT_S32_BE:
        return AUDIO_FORMAT_PCM_32_BIT;
#else
    case SND_PCM_FORMAT_S16_LE:
        return AUDIO_FORMAT_PCM_16_BIT;
    case SND_PCM_FORMAT_S24_3LE:
        return AUDIO_FORMAT_PCM_24_BIT_PACKED;
    case SND_PCM_FORMAT_S24_LE:
        return AUDIO_FORMAT_PCM_8_24_BIT;
    case SND_PCM_FORMAT_S32_LE:
        return AUDIO_FORMAT_PCM_32_BIT;
#endif
    default:
        LOG_ALWAYS_FATAL("audio_format_from_pcm_format: invalid pcm format %#x", format);
        return 0;
    }
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    ALOGV("out_get_format");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return audio_format_from_pcm_format(out->config.format);
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    ALOGV("out_set_format: %d",format);
    return -ENOSYS;
}

static int do_output_standby(struct alsa_stream_out *out)
{
    struct alsa_audio_device *adev = out->dev;

    if (!out->standby) {
        snd_pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_output = NULL;
        out->standby = 1;
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    ALOGV("out_standby");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    int status;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    ALOGV("out_dump");
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("out_set_parameters");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    struct alsa_audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret, val = 0;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (((adev->out_devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
             adev->out_devices &= ~AUDIO_DEVICE_OUT_ALL;
             adev->out_devices |= val;
        }
        select_devices(adev);
        pthread_mutex_unlock(&out->lock);
        pthread_mutex_unlock(&adev->lock);
    }

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    ALOGV("out_get_parameters");
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    ALOGV("out_get_latency");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    return (PLAYBACK_PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * 1000) / out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
        float right)
{
    ALOGV("out_set_volume: Left:%f Right:%f", left, right);
    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
        size_t bytes)
{
    int ret;
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    struct alsa_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    size_t out_frames = bytes / frame_size;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = 0;
    }

    pthread_mutex_unlock(&adev->lock);

    ret = snd_pcm_writei(out->pcm, buffer, out_frames);
    if (ret == -EPIPE) {
	    snd_pcm_prepare(out->pcm);
	    ret = snd_pcm_writei(out->pcm, buffer, out_frames);
    }

    if (ret > 0) {
        out->written += ret;
    }
exit:
    pthread_mutex_unlock(&out->lock);

    if (ret < 0) {
        usleep((int64_t)bytes * 1000000 / audio_stream_out_frame_size(stream) /
                out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
        uint32_t *dsp_frames)
{
    *dsp_frames = 0;
    ALOGV("out_get_render_position: dsp_frames: %p", dsp_frames);
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_add_audio_effect: %p", effect);
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_remove_audio_effect: %p", effect);
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
        int64_t *timestamp)
{
    *timestamp = 0;
    ALOGV("out_get_next_write_timestamp: %ld", (long int)(*timestamp));
    return -EINVAL;
}

/** audio_stream_in implementation **/

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct alsa_stream_in *in)
{
    struct alsa_audio_device *adev = in->dev;
    snd_pcm_hw_params_t *hwparams;
    in->unavailable = true;
    unsigned int pcm_retry_count = PCM_OPEN_RETRIES;
    int ret;

    while (1) {
        ret = snd_pcm_open(&in->pcm, "pulse", SND_PCM_STREAM_CAPTURE, 0);
        if (ret < 0) {
            if (in->pcm != NULL) {
                snd_pcm_close(in->pcm);
                in->pcm = NULL;
            }
            if (--pcm_retry_count == 0) {
                ALOGE("Failed to open pcm_in after %d tries", PCM_OPEN_RETRIES);
                adev->active_input = NULL;
                return -ENODEV;
            }
            usleep(PCM_OPEN_WAIT_TIME_MS * 1000);
        } else {
            break;
        }
    }

    snd_pcm_hw_params_alloca(&hwparams);
    if (snd_pcm_hw_params_any(in->pcm, hwparams) < 0) {
        ALOGE("Can not configure this PCM device.");
        adev->active_input = NULL;
        return -ENODEV;
    }

    if (snd_pcm_hw_params_set_access(in->pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        ALOGE("Error setting access.\n");
        adev->active_input = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_format(in->pcm, hwparams, in->config.format) < 0) {
        ALOGE("Error setting format.\n");
        adev->active_input = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_rate_near(in->pcm, hwparams, &in->config.rate, 0) < 0) {
        ALOGE("Error setting rate.\n");
        adev->active_input = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params_set_channels(in->pcm, hwparams, in->config.channels) < 0) {
        ALOGE("Error setting channels.\n");
        adev->active_input = NULL;
  	    return -ENODEV;
    }

    if (snd_pcm_hw_params(in->pcm, hwparams) < 0) {
        ALOGE("Error setting HW params.");
        adev->active_input = NULL;
        return -ENODEV;
    }

    if (snd_pcm_prepare(in->pcm) < 0) {
       ALOGE("Can not prepare this PCM device.");
        adev->active_input = NULL;
        return -ENODEV;
    }

    if (snd_pcm_state(in->pcm) != SND_PCM_STATE_PREPARED) {
        ALOGE("cannot open pcm_in driver");
        snd_pcm_close(in->pcm);
        adev->active_input = NULL;
        return -ENODEV;
    }

    in->unavailable = false;
    adev->active_input = in;
    return 0;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct alsa_stream_in *in = (struct alsa_stream_in *)stream;
    return in->config.rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("in_set_sample_rate: %d", rate);
    return -ENOSYS;
}

static size_t get_input_buffer_size(audio_format_t format,
                                    audio_channel_mask_t channel_mask)
{
    /* return the closest majoring multiple of 16 frames, as
     * audioflinger expects audio buffers to be a multiple of 16 frames */
    size_t frames = CAPTURE_PERIOD_SIZE;
    frames = ((frames + 15) / 16) * 16;
    size_t bytes_per_frame = audio_channel_count_from_in_mask(channel_mask) *
                            audio_bytes_per_sample(format);
    size_t buffer_size = frames * bytes_per_frame;
    return buffer_size;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    struct alsa_stream_in *in = (struct alsa_stream_in *)stream;
    ALOGV("in_get_channels: %d", in->config.channels);
    return audio_channel_in_mask_from_count(in->config.channels);
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    struct alsa_stream_in *in = (struct alsa_stream_in *)stream;
    ALOGV("in_get_format: %d", in->config.format);
    return audio_format_from_pcm_format(in->config.format);
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{

    size_t buffer_size = get_input_buffer_size(stream->get_format(stream),
                            stream->get_channels(stream));
    ALOGV("in_get_buffer_size: %zu", buffer_size);
    return buffer_size;
}

static int do_input_standby(struct alsa_stream_in *in)
{
    struct alsa_audio_device *adev = in->dev;

    if (!in->standby) {
        snd_pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_input = NULL;
        in->standby = true;
    }
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct alsa_stream_in *in = (struct alsa_stream_in *)stream;
    int status;

    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&in->dev->lock);
    status = do_input_standby(in);
    pthread_mutex_unlock(&in->dev->lock);
    pthread_mutex_unlock(&in->lock);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
        const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
        		size_t bytes)
{
    ALOGV("in_read: bytes %zu", bytes);

    int ret;
    struct alsa_stream_in *in = (struct alsa_stream_in *)stream;
    struct alsa_audio_device *adev = in->dev;
    size_t frame_size = audio_stream_in_frame_size(stream);
    size_t in_frames = bytes / frame_size;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&adev->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            ALOGE("start_input_stream failed with code %d", ret);
            goto exit;
        }
        in->standby = 0;
    }

    pthread_mutex_unlock(&adev->lock);

    ret = snd_pcm_readi(in->pcm, buffer, in_frames);
    if (ret == -EPIPE) {
        snd_pcm_prepare(in->pcm);
        ret = snd_pcm_readi(in->pcm, buffer, in_frames);
    }

    if (ret > 0) {
        in->read += ret;
    }

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret >= 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    pthread_mutex_unlock(&in->lock);

    if (ret < 0) {
        in_standby(&in->stream.common);
        usleep((int64_t)bytes * 1000000 / audio_stream_in_frame_size(stream) /
                in_get_sample_rate(&stream->common));
    }

     return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
        audio_io_handle_t handle,
        audio_devices_t devices,
        audio_output_flags_t flags,
        struct audio_config *config,
        struct audio_stream_out **stream_out,
        const char *address __unused)
{
    ALOGV("adev_open_output_stream...");

    struct alsa_audio_device *ladev = (struct alsa_audio_device *)dev;
    struct alsa_stream_out *out;
    int ret = 0;

    out = (struct alsa_stream_out *)calloc(1, sizeof(struct alsa_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->config.channels = CHANNEL_STEREO;
    out->config.rate = PLAYBACK_CODEC_SAMPLING_RATE;
    out->config.format = SND_PCM_FORMAT_S16_LE;
    out->config.period_size = PLAYBACK_PERIOD_SIZE;
    out->config.period_count = PLAYBACK_PERIOD_COUNT;

    if (out->config.rate != config->sample_rate ||
           audio_channel_count_from_out_mask(config->channel_mask) != CHANNEL_STEREO ||
               out->config.format !=  pcm_format_from_audio_format(config->format) ) {
        config->sample_rate = out->config.rate;
        config->format = audio_format_from_pcm_format(out->config.format);
        config->channel_mask = audio_channel_out_mask_from_count(CHANNEL_STEREO);
        ret = -EINVAL;
    }

    ALOGI("adev_open_output_stream selects channels=%d rate=%d format=%d",
                out->config.channels, out->config.rate, out->config.format);

    out->dev = ladev;
    out->standby = 1;
    out->unavailable = false;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;

    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
        struct audio_stream_out *stream)
{
    ALOGV("adev_close_output_stream...");
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    ALOGV("adev_set_parameters");
    return -ENOSYS;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
        const char *keys)
{
    ALOGV("adev_get_parameters");
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    ALOGV("adev_init_check");
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_voice_volume: %f", volume);
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_master_volume: %f", volume);
    return pa_set_master_volume(volume);
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    return pa_get_master_volume(volume);
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    ALOGV("adev_set_master_mute: %d", muted);
    return pa_set_master_mute(muted);
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    return pa_get_master_mute(muted);
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    ALOGV("adev_set_mode: %d", mode);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    ALOGV("adev_set_mic_mute: %d",state);
    
    struct alsa_audio_device *adev = (struct alsa_audio_device *)dev;
    
    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    ALOGV("adev_get_mic_mute");

    struct alsa_audio_device *adev = (struct alsa_audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
        const struct audio_config *config)
{
    size_t buffer_size = get_input_buffer_size(config->format, config->channel_mask);
    ALOGV("adev_get_input_buffer_size: %zu", buffer_size);
    return buffer_size;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
        audio_io_handle_t handle,
        audio_devices_t devices,
        struct audio_config *config,
        struct audio_stream_in **stream_in,
        audio_input_flags_t flags __unused,
        const char *address __unused,
        audio_source_t source __unused)
{

    ALOGV("adev_open_input_stream...");

    struct alsa_audio_device *ladev = (struct alsa_audio_device *)dev;
    struct alsa_stream_in *in;
    int ret = 0;

    in = (struct alsa_stream_in *)calloc(1, sizeof(struct alsa_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->config.channels = CHANNEL_STEREO;
    in->config.rate = CAPTURE_CODEC_SAMPLING_RATE;
    in->config.format = SND_PCM_FORMAT_S32_LE;
    in->config.period_size = CAPTURE_PERIOD_SIZE;
    in->config.period_count = CAPTURE_PERIOD_COUNT;

    if (in->config.rate != config->sample_rate ||
           audio_channel_count_from_in_mask(config->channel_mask) != CHANNEL_STEREO ||
               in->config.format !=  pcm_format_from_audio_format(config->format) ) {
        ret = -EINVAL;
    }

    ALOGI("adev_open_input_stream selects channels=%d rate=%d format=%d",
                in->config.channels, in->config.rate, in->config.format);

    in->dev = ladev;
    in->standby = true;
    in->unavailable = false;

    config->format = in_get_format(&in->stream.common);
    config->channel_mask = in_get_channels(&in->stream.common);
    config->sample_rate = in_get_sample_rate(&in->stream.common);

    if (ret) {
        free(in);
    } else {
        *stream_in = &in->stream;
    }

    return ret;
 }

static void adev_close_input_stream(struct audio_hw_device *dev,
        			     struct audio_stream_in *in)
{
    ALOGV("adev_close_input_stream...");
    in_standby(&in->common);
    free(in);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    ALOGV("adev_dump");
    return 0;
}

static int adev_close(hw_device_t *device)
{
    ALOGV("adev_close");
    free(device);
    return 0;
}

static char *adev_get_devs(struct audio_hw_device *dev, bool input)
{
    ALOGV("%s: %d",__FUNCTION__, input);
    return input ? pa_get_input_devs() : pa_get_output_devs();
}

static int adev_set_dev_volume(struct audio_hw_device *dev, bool input, const char *dev_name, float volume)
{
    ALOGV("%s: %d-%s-%f", __FUNCTION__, input, dev_name, volume);
    return input ? pa_set_input_dev_volume(dev_name, volume) : pa_set_output_dev_volume(dev_name, volume);
}

static int adev_set_dev_mute(struct audio_hw_device *dev, bool input, const char *dev_name, bool mute)
{
    ALOGV("%s: %d-%s-%d", __FUNCTION__, input, dev_name, mute);
    return input ? pa_set_input_dev_mute(dev_name, mute) : pa_set_output_dev_mute(dev_name, mute);
}

static char *adev_set_default_dev(struct audio_hw_device *dev, bool input, const char *dev_name, bool needInfo)
{
    ALOGV("%s: %d-%s-%d", __FUNCTION__, input, dev_name, needInfo);
    return input ? pa_set_input_default_dev(dev_name, needInfo) 
	             : pa_set_output_default_dev(dev_name, needInfo);
}

static int adev_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    struct alsa_audio_device *adev;
    char property[PROPERTY_VALUE_MAX];

    if (property_get("waydroid.pulse_runtime_path", property, "/run/user/1000/pulse") > 0) {
        setenv("PULSE_RUNTIME_PATH", property, 1);
    }

    ALOGV("adev_open: %s", name);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct alsa_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_master_mute = adev_set_master_mute;
    adev->hw_device.get_master_mute = adev_get_master_mute;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;
    adev->hw_device.get_devs = adev_get_devs;
    adev->hw_device.set_dev_volume = adev_set_dev_volume;
    adev->hw_device.set_dev_mute = adev_set_dev_mute;
    adev->hw_device.set_default_dev = adev_set_default_dev;


    adev->out_devices = AUDIO_DEVICE_OUT_SPEAKER;
    adev->in_devices = AUDIO_DEVICE_IN_BUILTIN_MIC & ~AUDIO_DEVICE_BIT_IN;

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Audio HAL for Waydroid",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
