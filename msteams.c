teams_audio_core.c
/*
 * Teams Audio Core
 *
 * Native real-time audio processing prototype.
 *
 * C11
 *
 * Designed around:
 *
 *   microphone
 *       |
 *       v
 *   capture buffer
 *       |
 *       v
 *   preprocessing
 *       |
 *       +--> DC filter
 *       +--> high-pass
 *       +--> noise gate
 *       +--> AGC
 *       +--> VAD
 *       |
 *       v
 *   participant streams
 *       |
 *       v
 *   mixer
 *       |
 *       +--> master gain
 *       +--> limiter
 *       |
 *       v
 *   output buffer
 *
 * No malloc/free should occur on the real-time processing path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================
 * CONFIGURATION
 * ============================================================ */

#define TA_SAMPLE_RATE              48000
#define TA_CHANNELS                 2
#define TA_MAX_PARTICIPANTS         32
#define TA_MAX_BUFFER_FRAMES        48000
#define TA_AUDIO_BLOCK              480
#define TA_MAX_LATENCY_MS           100

#define TA_PI                       ((float)M_PI)

#define TA_EPSILON                  0.000001f

/* ============================================================
 * BASIC TYPES
 * ============================================================ */

typedef float ta_sample_t;

typedef struct
{
    ta_sample_t left;
    ta_sample_t right;
} ta_stereo_sample;

/* ============================================================
 * AUDIO RING BUFFER
 *
 * Single producer / single consumer design.
 *
 * Producer:
 *     capture/audio backend
 *
 * Consumer:
 *     processing thread
 *
 * The indexes are atomic so the producer and consumer don't
 * need a mutex for the normal audio path.
 * ============================================================ */

typedef struct
{
    ta_sample_t *buffer;

    size_t capacity;

    atomic_size_t read_index;
    atomic_size_t write_index;

    atomic_uint_fast64_t underruns;
    atomic_uint_fast64_t overruns;

} ta_ring_buffer;

/* ------------------------------------------------------------
 * Ring initialization
 * ------------------------------------------------------------ */

static bool
ta_ring_init(ta_ring_buffer *rb, size_t capacity)
{
    if (!rb)
        return false;

    rb->buffer = calloc(capacity, sizeof(ta_sample_t));

    if (!rb->buffer)
        return false;

    rb->capacity = capacity;

    atomic_init(&rb->read_index, 0);
    atomic_init(&rb->write_index, 0);

    atomic_init(&rb->underruns, 0);
    atomic_init(&rb->overruns, 0);

    return true;
}

/* ------------------------------------------------------------
 * Ring destruction
 * ------------------------------------------------------------ */

static void
ta_ring_destroy(ta_ring_buffer *rb)
{
    if (!rb)
        return;

    free(rb->buffer);

    rb->buffer = NULL;
    rb->capacity = 0;
}

/* ------------------------------------------------------------
 * Number of frames available
 * ------------------------------------------------------------ */

static size_t
ta_ring_available(const ta_ring_buffer *rb)
{
    size_t r;
    size_t w;

    r = atomic_load_explicit(
        &rb->read_index,
        memory_order_acquire
    );

    w = atomic_load_explicit(
        &rb->write_index,
        memory_order_acquire
    );

    return w - r;
}

/* ------------------------------------------------------------
 * Free capacity
 * ------------------------------------------------------------ */

static size_t
ta_ring_free_space(const ta_ring_buffer *rb)
{
    size_t available = ta_ring_available(rb);

    if (available >= rb->capacity)
        return 0;

    return rb->capacity - available;
}

/* ------------------------------------------------------------
 * Write samples
 * ------------------------------------------------------------ */

static size_t
ta_ring_write(
    ta_ring_buffer *rb,
    const ta_sample_t *samples,
    size_t count
)
{
    size_t r;
    size_t w;
    size_t available;

    if (!rb || !samples || count == 0)
        return 0;

    r = atomic_load_explicit(
        &rb->read_index,
        memory_order_acquire
    );

    w = atomic_load_explicit(
        &rb->write_index,
        memory_order_relaxed
    );

    available = w - r;

    if (count > rb->capacity - available)
    {
        atomic_fetch_add(
            &rb->overruns,
            1
        );

        count = rb->capacity - available;
    }

    for (size_t i = 0; i < count; ++i)
    {
        rb->buffer[
            (w + i) % rb->capacity
        ] = samples[i];
    }

    atomic_store_explicit(
        &rb->write_index,
        w + count,
        memory_order_release
    );

    return count;
}

/* ------------------------------------------------------------
 * Read samples
 * ------------------------------------------------------------ */

static size_t
ta_ring_read(
    ta_ring_buffer *rb,
    ta_sample_t *samples,
    size_t count
)
{
    size_t r;
    size_t w;
    size_t available;

    if (!rb || !samples || count == 0)
        return 0;

    r = atomic_load_explicit(
        &rb->read_index,
        memory_order_relaxed
    );

    w = atomic_load_explicit(
        &rb->write_index,
        memory_order_acquire
    );

    available = w - r;

    if (count > available)
    {
        atomic_fetch_add(
            &rb->underruns,
            1
        );

        count = available;
    }

    for (size_t i = 0; i < count; ++i)
    {
        samples[i] =
            rb->buffer[
                (r + i) % rb->capacity
            ];
    }

    atomic_store_explicit(
        &rb->read_index,
        r + count,
        memory_order_release
    );

    return count;
}

/* ============================================================
 * FILTERS
 * ============================================================ */

/*
 * One-pole DC blocking filter.
 *
 * y[n] = x[n] - x[n-1] + R*y[n-1]
 */

typedef struct
{
    float previous_input;
    float previous_output;

    float coefficient;

} ta_dc_filter;

/* ------------------------------------------------------------ */

static void
ta_dc_filter_init(
    ta_dc_filter *filter,
    float coefficient
)
{
    filter->previous_input = 0.0f;
    filter->previous_output = 0.0f;

    filter->coefficient = coefficient;
}

/* ------------------------------------------------------------ */

static inline float
ta_dc_filter_process(
    ta_dc_filter *filter,
    float input
)
{
    float output;

    output =
        input
        - filter->previous_input
        + filter->coefficient *
          filter->previous_output;

    filter->previous_input = input;
    filter->previous_output = output;

    return output;
}

/* ============================================================
 * FIRST ORDER HIGH PASS
 * ============================================================ */

typedef struct
{
    float x1;
    float y1;

    float alpha;

} ta_highpass;

/* ------------------------------------------------------------ */

static void
ta_highpass_init(
    ta_highpass *hp,
    float cutoff,
    float sample_rate
)
{
    float rc;
    float dt;

    rc =
        1.0f /
        (2.0f * (float)M_PI * cutoff);

    dt = 1.0f / sample_rate;

    hp->alpha =
        rc / (rc + dt);

    hp->x1 = 0.0f;
    hp->y1 = 0.0f;
}

/* ------------------------------------------------------------ */

static inline float
ta_highpass_process(
    ta_highpass *hp,
    float input
)
{
    float output;

    output =
        hp->alpha *
        (hp->y1 + input - hp->x1);

    hp->x1 = input;
    hp->y1 = output;

    return output;
}

/* ============================================================
 * RMS METER
 * ============================================================ */

typedef struct
{
    float squared_sum;

    uint32_t samples;

    float rms;
    float peak;

} ta_meter;

/* ------------------------------------------------------------ */

static void
ta_meter_reset(ta_meter *meter)
{
    meter->squared_sum = 0.0f;
    meter->samples = 0;
    meter->rms = 0.0f;
    meter->peak = 0.0f;
}

/* ------------------------------------------------------------ */

static inline void
ta_meter_process(
    ta_meter *meter,
    float sample
)
{
    float absolute;

    absolute = fabsf(sample);

    if (absolute > meter->peak)
        meter->peak = absolute;

    meter->squared_sum +=
        sample * sample;

    meter->samples++;

    if (meter->samples >= 480)
    {
        meter->rms =
            sqrtf(
                meter->squared_sum /
                (float)meter->samples
            );

        meter->squared_sum = 0.0f;
        meter->samples = 0;
    }
}

/* ============================================================
 * NOISE GATE
 * ============================================================ */

typedef struct
{
    float threshold;

    float attack;
    float release;

    float envelope;

    bool open;

} ta_noise_gate;

/* ------------------------------------------------------------ */

static void
ta_noise_gate_init(
    ta_noise_gate *gate,
    float threshold_db,
    float attack_ms,
    float release_ms,
    float sample_rate
)
{
    gate->threshold =
        powf(
            10.0f,
            threshold_db / 20.0f
        );

    gate->attack =
        expf(
            -1.0f /
            (
                attack_ms *
                0.001f *
                sample_rate
            )
        );

    gate->release =
        expf(
            -1.0f /
            (
                release_ms *
                0.001f *
                sample_rate
            )
        );

    gate->envelope = 0.0f;
    gate->open = false;
}

/* ------------------------------------------------------------ */

static inline float
ta_noise_gate_process(
    ta_noise_gate *gate,
    float input
)
{
    float absolute;

    absolute = fabsf(input);

    if (absolute > gate->envelope)
    {
        gate->envelope =
            gate->attack *
            gate->envelope
            +
            (1.0f - gate->attack) *
            absolute;
    }
    else
    {
        gate->envelope =
            gate->release *
            gate->envelope
            +
            (1.0f - gate->release) *
            absolute;
    }

    if (gate->envelope >= gate->threshold)
        gate->open = true;
    else
        gate->open = false;

    if (!gate->open)
        return input * 0.03f;

    return input;
}

/* ============================================================
 * AUTOMATIC GAIN CONTROL
 * ============================================================ */

typedef struct
{
    float target;

    float maximum_gain;
    float minimum_gain;

    float attack;
    float release;

    float gain;

    float envelope;

} ta_agc;

/* ------------------------------------------------------------ */

static void
ta_agc_init(
    ta_agc *agc,
    float target_db,
    float max_gain_db,
    float min_gain_db,
    float attack_ms,
    float release_ms,
    float sample_rate
)
{
    agc->target =
        powf(
            10.0f,
            target_db / 20.0f
        );

    agc->maximum_gain =
        powf(
            10.0f,
            max_gain_db / 20.0f
        );

    agc->minimum_gain =
        powf(
            10.0f,
            min_gain_db / 20.0f
        );

    agc->attack =
        expf(
            -1.0f /
            (
                attack_ms *
                0.001f *
                sample_rate
            )
        );

    agc->release =
        expf(
            -1.0f /
            (
                release_ms *
                0.001f *
                sample_rate
            )
        );

    agc->gain = 1.0f;
    agc->envelope = 0.0f;
}

/* ------------------------------------------------------------ */

static inline float
ta_agc_process(
    ta_agc *agc,
    float input
)
{
    float absolute;
    float desired_gain;

    absolute = fabsf(input);

    if (absolute > agc->envelope)
    {
        agc->envelope =
            agc->attack *
            agc->envelope
            +
            (1.0f - agc->attack) *
            absolute;
    }
    else
    {
        agc->envelope =
            agc->release *
            agc->envelope
            +
            (1.0f - agc->release) *
            absolute;
    }

    if (agc->envelope > TA_EPSILON)
    {
        desired_gain =
            agc->target /
            agc->envelope;
    }
    else
    {
        desired_gain =
            agc->maximum_gain;
    }

    if (desired_gain >
        agc->maximum_gain)
    {
        desired_gain =
            agc->maximum_gain;
    }

    if (desired_gain <
        agc->minimum_gain)
    {
        desired_gain =
            agc->minimum_gain;
    }

    /*
     * Smooth gain changes.
     */

    if (desired_gain < agc->gain)
    {
        agc->gain =
            agc->attack *
            agc->gain
            +
            (1.0f - agc->attack) *
            desired_gain;
    }
    else
    {
        agc->gain =
            agc->release *
            agc->gain
            +
            (1.0f - agc->release) *
            desired_gain;
    }

    return input * agc->gain;
}

/* ============================================================
 * VOICE ACTIVITY DETECTOR
 * ============================================================ */

typedef struct
{
    float threshold;

    float attack;
    float release;

    float envelope;

    bool active;

} ta_vad;

/* ------------------------------------------------------------ */

static void
ta_vad_init(
    ta_vad *vad,
    float threshold_db,
    float attack_ms,
    float release_ms,
    float sample_rate
)
{
    vad->threshold =
        powf(
            10.0f,
            threshold_db / 20.0f
        );

    vad->attack =
        expf(
            -1.0f /
            (
                attack_ms *
                0.001f *
                sample_rate
            )
        );

    vad->release =
        expf(
            -1.0f /
            (
                release_ms *
                0.001f *
                sample_rate
            )
        );

    vad->envelope = 0.0f;
    vad->active = false;
}

/* ------------------------------------------------------------ */

static inline bool
ta_vad_process(
    ta_vad *vad,
    float input
)
{
    float absolute;

    absolute = fabsf(input);

    if (absolute > vad->envelope)
    {
        vad->envelope =
            vad->attack *
            vad->envelope
            +
            (1.0f - vad->attack) *
            absolute;
    }
    else
    {
        vad->envelope =
            vad->release *
            vad->envelope
            +
            (1.0f - vad->release) *
            absolute;
    }

    vad->active =
        vad->envelope >
        vad->threshold;

    return vad->active;
}

/* ============================================================
 * SOFT LIMITER
 * ============================================================ */

static inline float
ta_soft_limit(float input)
{
    /*
     * Smooth saturation.
     *
     * This avoids a hard digital clip.
     */

    return tanhf(input);
}

/* ============================================================
 * AUDIO STREAM
 * ============================================================ */

typedef struct
{
    bool active;

    uint32_t id;

    ta_ring_buffer input;

    float gain;

    float pan;

    bool muted;

    bool vad_active;

    ta_dc_filter dc_left;
    ta_dc_filter dc_right;

    ta_highpass hp_left;
    ta_highpass hp_right;

    ta_noise_gate gate_left;
    ta_noise_gate gate_right;

    ta_agc agc_left;
    ta_agc agc_right;

    ta_vad vad_left;
    ta_vad vad_right;

    ta_meter meter_left;
    ta_meter meter_right;

} ta_audio_stream;

/* ============================================================
 * AUDIO ENGINE
 * ============================================================ */

typedef struct
{
    uint32_t sample_rate;

    uint32_t channels;

    uint32_t block_size;

    ta_audio_stream streams[
        TA_MAX_PARTICIPANTS
    ];

    ta_ring_buffer output;

    float master_gain;

    bool master_mute;

    ta_meter output_meter;

    atomic_bool running;

    atomic_uint_fast64_t
        processed_frames;

    atomic_uint_fast64_t
        audio_blocks;

} ta_audio_engine;

/* ============================================================
 * STREAM INITIALIZATION
 * ============================================================ */

static bool
ta_stream_init(
    ta_audio_stream *stream,
    uint32_t id
)
{
    memset(
        stream,
        0,
        sizeof(*stream)
    );

    stream->id = id;

    stream->active = true;

    stream->gain = 1.0f;

    stream->pan = 0.0f;

    stream->muted = false;

    if (!ta_ring_init(
            &stream->input,
            TA_MAX_BUFFER_FRAMES * 2))
    {
        return false;
    }

    ta_dc_filter_init(
        &stream->dc_left,
        0.995f
    );

    ta_dc_filter_init(
        &stream->dc_right,
        0.995f
    );

    ta_highpass_init(
        &stream->hp_left,
        80.0f,
        TA_SAMPLE_RATE
    );

    ta_highpass_init(
        &stream->hp_right,
        80.0f,
        TA_SAMPLE_RATE
    );

    ta_noise_gate_init(
        &stream->gate_left,
        -55.0f,
        2.0f,
        100.0f,
        TA_SAMPLE_RATE
    );

    ta_noise_gate_init(
        &stream->gate_right,
        -55.0f,
        2.0f,
        100.0f,
        TA_SAMPLE_RATE
    );

    ta_agc_init(
        &stream->agc_left,
        -18.0f,
        18.0f,
        -12.0f,
        10.0f,
        200.0f,
        TA_SAMPLE_RATE
    );

    ta_agc_init(
        &stream->agc_right,
        -18.0f,
        18.0f,
        -12.0f,
        10.0f,
        200.0f,
        TA_SAMPLE_RATE
    );

    ta_vad_init(
        &stream->vad_left,
        -45.0f,
        5.0f,
        100.0f,
        TA_SAMPLE_RATE
    );

    ta_vad_init(
        &stream->vad_right,
        -45.0f,
        5.0f,
        100.0f,
        TA_SAMPLE_RATE
    );

    ta_meter_reset(
        &stream->meter_left
    );

    ta_meter_reset(
        &stream->meter_right
    );

    return true;
}

/* ============================================================
 * STREAM DESTROY
 * ============================================================ */

static void
ta_stream_destroy(
    ta_audio_stream *stream
)
{
    ta_ring_destroy(
        &stream->input
    );

    stream->active = false;
}

/* ============================================================
 * ENGINE INITIALIZATION
 * ============================================================ */

static bool
ta_engine_init(
    ta_audio_engine *engine,
    uint32_t sample_rate,
    uint32_t channels,
    uint32_t block_size
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->sample_rate =
        sample_rate;

    engine->channels =
        channels;

    engine->block_size =
        block_size;

    engine->master_gain =
        1.0f;

    engine->master_mute =
        false;

    if (!ta_ring_init(
            &engine->output,
            TA_MAX_BUFFER_FRAMES * 2))
    {
        return false;
    }

    for (uint32_t i = 0;
         i < TA_MAX_PARTICIPANTS;
         ++i)
    {
        if (!ta_stream_init(
                &engine->streams[i],
                i))
        {
            for (uint32_t j = 0; j < i; ++j)
                ta_stream_destroy(
                    &engine->streams[j]
                );

            ta_ring_destroy(
                &engine->output
            );

            return false;
        }

        /*
         * Initially inactive.
         */

        engine->streams[i].active = false;
    }

    atomic_init(
        &engine->running,
        false
    );

    atomic_init(
        &engine->processed_frames,
        0
    );

    atomic_init(
        &engine->audio_blocks,
        0
    );

    ta_meter_reset(
        &engine->output_meter
    );

    return true;
}

/* ============================================================
 * ENGINE SHUTDOWN
 * ============================================================ */

static void
ta_engine_destroy(
    ta_audio_engine *engine
)
{
    for (uint32_t i = 0;
         i < TA_MAX_PARTICIPANTS;
         ++i)
    {
        ta_stream_destroy(
            &engine->streams[i]
        );
    }

    ta_ring_destroy(
        &engine->output
    );
}

/* ============================================================
 * ADD PARTICIPANT
 * ============================================================ */

static int
ta_engine_add_participant(
    ta_audio_engine *engine
)
{
    for (int i = 0;
         i < TA_MAX_PARTICIPANTS;
         ++i)
    {
        if (!engine->streams[i].active)
        {
            engine->streams[i].active = true;

            engine->streams[i].gain = 1.0f;
            engine->streams[i].pan = 0.0f;
            engine->streams[i].muted = false;

            return i;
        }
    }

    return -1;
}

/* ============================================================
 * REMOVE PARTICIPANT
 * ============================================================ */

static void
ta_engine_remove_participant(
    ta_audio_engine *engine,
    uint32_t id
)
{
    if (id >= TA_MAX_PARTICIPANTS)
        return;

    engine->streams[id].active = false;
}

/* ============================================================
 * SET PARTICIPANT GAIN
 * ============================================================ */

static void
ta_engine_set_gain(
    ta_audio_engine *engine,
    uint32_t id,
    float gain
)
{
    if (id >= TA_MAX_PARTICIPANTS)
        return;

    if (gain < 0.0f)
        gain = 0.0f;

    if (gain > 4.0f)
        gain = 4.0f;

    engine->streams[id].gain =
        gain;
}

/* ============================================================
 * SET PARTICIPANT PAN
 * ============================================================ */

static void
ta_engine_set_pan(
    ta_audio_engine *engine,
    uint32_t id,
    float pan
)
{
    if (id >= TA_MAX_PARTICIPANTS)
        return;

    if (pan < -1.0f)
        pan = -1.0f;

    if (pan > 1.0f)
        pan = 1.0f;

    engine->streams[id].pan =
        pan;
}

/* ============================================================
 * MUTE PARTICIPANT
 * ============================================================ */

static void
ta_engine_set_mute(
    ta_audio_engine *engine,
    uint32_t id,
    bool muted
)
{
    if (id >= TA_MAX_PARTICIPANTS)
        return;

    engine->streams[id].muted =
        muted;
}

/* ============================================================
 * WRITE PARTICIPANT AUDIO
 *
 * Stereo interleaved:
 *
 * L R L R L R ...
 * ============================================================ */

static size_t
ta_engine_push_audio(
    ta_audio_engine *engine,
    uint32_t id,
    const float *samples,
    size_t frames
)
{
    if (id >= TA_MAX_PARTICIPANTS)
        return 0;

    if (!engine->streams[id].active)
        return 0;

    return ta_ring_write(
        &engine->streams[id].input,
        samples,
        frames * 2
    );
}

/* ============================================================
 * PROCESS ONE STREAM
 * ============================================================ */

static void
ta_process_stream(
    ta_audio_stream *stream,
    float *mix_left,
    float *mix_right,
    size_t frames
)
{
    /*
     * Stereo interleaved temporary input.
     *
     * This is stack allocated and bounded.
     */

    float input[
        TA_AUDIO_BLOCK * 2
    ];

    size_t samples_read;

    samples_read =
        ta_ring_read(
            &stream->input,
            input,
            frames * 2
        );

    /*
     * Missing samples become silence.
     */

    for (size_t i = samples_read;
         i < frames * 2;
         ++i)
    {
        input[i] = 0.0f;
    }

    /*
     * Equal-power-ish pan coefficients.
     */

    float pan = stream->pan;

    float left_pan =
        cosf(
            (pan + 1.0f) *
            0.25f *
            (float)M_PI
        );

    float right_pan =
        sinf(
            (pan + 1.0f) *
            0.25f *
            (float)M_PI
        );

    for (size_t frame = 0;
         frame < frames;
         ++frame)
    {
        float left;
        float right;

        float processed_left;
        float processed_right;

        left =
            input[frame * 2];

        right =
            input[frame * 2 + 1];

        /*
         * DC removal.
         */

        left =
            ta_dc_filter_process(
                &stream->dc_left,
                left
            );

        right =
            ta_dc_filter_process(
                &stream->dc_right,
                right
            );

        /*
         * Remove low-frequency rumble.
         */

        left =
            ta_highpass_process(
                &stream->hp_left,
                left
            );

        right =
            ta_highpass_process(
                &stream->hp_right,
                right
            );

        /*
         * Noise gate.
         */

        left =
            ta_noise_gate_process(
                &stream->gate_left,
                left
            );

        right =
            ta_noise_gate_process(
                &stream->gate_right,
                right
            );

        /*
         * Automatic gain control.
         */

        left =
            ta_agc_process(
                &stream->agc_left,
                left
            );

        right =
            ta_agc_process(
                &stream->agc_right,
                right
            );

        /*
         * VAD.
         */

        stream->vad_left =
            stream->vad_left;

        stream->vad_active =
            ta_vad_process(
                &stream->vad_left,
                left
            )
            ||
            ta_vad_process(
                &stream->vad_right,
                right
            );

        /*
         * Individual participant gain.
         */

        left *= stream->gain;
        right *= stream->gain;

        /*
         * Pan.
         */

        processed_left =
            left * left_pan;

        processed_right =
            right * right_pan;

        /*
         * Mute.
         */

        if (stream->muted)
        {
            processed_left = 0.0f;
            processed_right = 0.0f;
        }

        /*
         * Metering.
         */

        ta_meter_process(
            &stream->meter_left,
            processed_left
        );

        ta_meter_process(
            &stream->meter_right,
            processed_right
        );

        /*
         * Accumulate into master mix.
         */

        mix_left[frame] +=
            processed_left;

        mix_right[frame] +=
            processed_right;
    }
}

/* ============================================================
 * MASTER MIX
 * ============================================================ */

static void
ta_engine_process(
    ta_audio_engine *engine,
    float *output,
    size_t frames
)
{
    /*
     * Bounded temporary mix buffers.
     */

    float mix_left[
        TA_AUDIO_BLOCK
    ];

    float mix_right[
        TA_AUDIO_BLOCK
    ];

    if (frames > TA_AUDIO_BLOCK)
        frames = TA_AUDIO_BLOCK;

    memset(
        mix_left,
        0,
        sizeof(mix_left)
    );

    memset(
        mix_right,
        0,
        sizeof(mix_right)
    );

    /*
     * Mix all active participants.
     */

    for (uint32_t i = 0;
         i < TA_MAX_PARTICIPANTS;
         ++i)
    {
        ta_audio_stream *stream =
            &engine->streams[i];

        if (!stream->active)
            continue;

        ta_process_stream(
            stream,
            mix_left,
            mix_right,
            frames
        );
    }

    /*
     * Master stage.
     */

    for (size_t i = 0;
         i < frames;
         ++i)
    {
        float left =
            mix_left[i] *
            engine->master_gain;

        float right =
            mix_right[i] *
            engine->master_gain;

        if (engine->master_mute)
        {
            left = 0.0f;
            right = 0.0f;
        }

        /*
         * Master soft limiter.
         */

        left =
            ta_soft_limit(left);

        right =
            ta_soft_limit(right);

        output[i * 2] =
            left;

        output[i * 2 + 1] =
            right;

        ta_meter_process(
            &engine->output_meter,
            left
        );

        ta_meter_process(
            &engine->output_meter,
            right
        );
    }

    atomic_fetch_add(
        &engine->processed_frames,
        frames
    );

    atomic_fetch_add(
        &engine->audio_blocks,
        1
    );
}

/* ============================================================
 * TEST TONE GENERATOR
 * ============================================================ */

static void
ta_generate_sine(
    float *buffer,
    size_t frames,
    float frequency,
    float amplitude,
    float *phase
)
{
    float phase_increment =
        2.0f *
        (float)M_PI *
        frequency /
        (float)TA_SAMPLE_RATE;

    for (size_t i = 0;
         i < frames;
         ++i)
    {
        float sample =
            sinf(*phase) *
            amplitude;

        buffer[i * 2] =
            sample;

        buffer[i * 2 + 1] =
            sample;

        *phase +=
            phase_increment;

        if (*phase >
            2.0f * (float)M_PI)
        {
            *phase -=
                2.0f * (float)M_PI;
        }
    }
}

/* ============================================================
 * WHITE NOISE GENERATOR
 * ============================================================ */

static float
ta_random_float(void)
{
    return
        ((float)rand() /
         (float)RAND_MAX)
        * 2.0f
        - 1.0f;
}

/* ------------------------------------------------------------ */

static void
ta_generate_noise(
    float *buffer,
    size_t frames,
    float amplitude
)
{
    for (size_t i = 0;
         i < frames;
         ++i)
    {
        float sample =
            ta_random_float() *
            amplitude;

        buffer[i * 2] =
            sample;

        buffer[i * 2 + 1] =
            sample;
    }
}

/* ============================================================
 * WAV WRITER
 * ============================================================ */

typedef struct
{
    FILE *file;

    uint32_t data_size;

} ta_wav_writer;

/* ------------------------------------------------------------ */

static void
ta_write_u16(
    FILE *file,
    uint16_t value
)
{
    fwrite(
        &value,
        sizeof(value),
        1,
        file
    );
}

/* ------------------------------------------------------------ */

static void
ta_write_u32(
    FILE *file,
    uint32_t value
)
{
    fwrite(
        &value,
        sizeof(value),
        1,
        file
    );
}

/* ------------------------------------------------------------ */

static bool
ta_wav_open(
    ta_wav_writer *writer,
    const char *filename
)
{
    writer->file =
        fopen(
            filename,
            "wb"
        );

    if (!writer->file)
        return false;

    writer->data_size = 0;

    /*
     * RIFF header placeholder.
     */

    fwrite("RIFF", 1, 4, writer->file);

    ta_write_u32(
        writer->file,
        0
    );

    fwrite("WAVE", 1, 4, writer->file);

    /*
     * fmt
     */

    fwrite("fmt ", 1, 4, writer->file);

    ta_write_u32(
        writer->file,
        16
    );

    /*
     * PCM
     */

    ta_write_u16(
        writer->file,
        1
    );

    ta_write_u16(
        writer->file,
        TA_CHANNELS
    );

    ta_write_u32(
        writer->file,
        TA_SAMPLE_RATE
    );

    uint32_t byte_rate =
        TA_SAMPLE_RATE *
        TA_CHANNELS *
        sizeof(int16_t);

    ta_write_u32(
        writer->file,
        byte_rate
    );

    uint16_t block_align =
        TA_CHANNELS *
        sizeof(int16_t);

    ta_write_u16(
        writer->file,
        block_align
    );

    ta_write_u16(
        writer->file,
        16
    );

    /*
     * data
     */

    fwrite(
        "data",
        1,
        4,
        writer->file
    );

    ta_write_u32(
        writer->file,
        0
    );

    return true;
}

/* ------------------------------------------------------------ */

static void
ta_wav_write(
    ta_wav_writer *writer,
    const float *samples,
    size_t frames
)
{
    for (size_t i = 0;
         i < frames * 2;
         ++i)
    {
        float sample =
            samples[i];

        if (sample > 1.0f)
            sample = 1.0f;

        if (sample < -1.0f)
            sample = -1.0f;

        int16_t pcm =
            (int16_t)
            (
                sample *
                32767.0f
            );

        fwrite(
            &pcm,
            sizeof(pcm),
            1,
            writer->file
        );

        writer->data_size +=
            sizeof(pcm);
    }
}

/* ------------------------------------------------------------ */

static void
ta_wav_close(
    ta_wav_writer *writer
)
{
    if (!writer->file)
        return;

    /*
     * Patch RIFF size.
     */

    uint32_t riff_size =
        36 +
        writer->data_size;

    fseek(
        writer->file,
        4,
        SEEK_SET
    );

    ta_write_u32(
        writer->file,
        riff_size
    );

    /*
     * Patch data size.
     */

    fseek(
        writer->file,
        40,
        SEEK_SET
    );

    ta_write_u32(
        writer->file,
        writer->data_size
    );

    fclose(
        writer->file
    );

    writer->file = NULL;
}

/* ============================================================
 * ENGINE DIAGNOSTICS
 * ============================================================ */

static void
ta_engine_print_stats(
    ta_audio_engine *engine
)
{
    uint64_t frames =
        atomic_load(
            &engine->processed_frames
        );

    uint64_t blocks =
        atomic_load(
            &engine->audio_blocks
        );

    printf(
        "\n"
        "==============================\n"
        " TEAMS AUDIO CORE STATISTICS\n"
        "==============================\n"
    );

    printf(
        "Sample rate:      %u Hz\n",
        engine->sample_rate
    );

    printf(
        "Channels:         %u\n",
        engine->channels
    );

    printf(
        "Block size:       %u frames\n",
        engine->block_size
    );

    printf(
        "Processed frames: %llu\n",
        (unsigned long long)frames
    );

    printf(
        "Audio blocks:     %llu\n",
        (unsigned long long)blocks
    );

    printf(
        "Output RMS:       %.4f\n",
        engine->output_meter.rms
    );

    printf(
        "Output peak:      %.4f\n",
        engine->output_meter.peak
    );

    printf(
        "\nParticipants:\n"
    );

    for (uint32_t i = 0;
         i < TA_MAX_PARTICIPANTS;
         ++i)
    {
        ta_audio_stream *s =
            &engine->streams[i];

        if (!s->active)
            continue;

        printf(
            "  [%02u] gain=%+.2f "
            "pan=%+.2f "
            "VAD=%s "
            "RMS=%.4f "
            "peak=%.4f\n",

            i,

            s->gain,

            s->pan,

            s->vad_active
                ? "VOICE"
                : "SILENCE",

            s->meter_left.rms,

            s->meter_left.peak
        );
    }

    printf(
        "==============================\n"
    );
}

/* ============================================================
 * TEST PROGRAM
 * ============================================================ */

int
main(void)
{
    ta_audio_engine engine;

    if (!ta_engine_init(
            &engine,
            TA_SAMPLE_RATE,
            TA_CHANNELS,
            TA_AUDIO_BLOCK))
    {
        fprintf(
            stderr,
            "Failed to initialize audio engine\n"
        );

        return 1;
    }

    /*
     * Add two simulated Teams participants.
     */

    int alice =
        ta_engine_add_participant(
            &engine
        );

    int bob =
        ta_engine_add_participant(
            &engine
        );

    if (alice < 0 ||
        bob < 0)
    {
        fprintf(
            stderr,
            "Could not create participants\n"
        );

        ta_engine_destroy(
            &engine
        );

        return 1;
    }

    /*
     * Alice:
     *
     * normal speech-level signal
     */

    ta_engine_set_gain(
        &engine,
        alice,
        1.0f
    );

    /*
     * Bob:
     *
     * slightly quieter.
     */

    ta_engine_set_gain(
        &engine,
        bob,
        0.75f
    );

    ta_engine_set_pan(
        &engine,
        alice,
        -0.15f
    );

    ta_engine_set_pan(
        &engine,
        bob,
        0.15f
    );

    /*
     * Output WAV.
     */

    ta_wav_writer wav;

    if (!ta_wav_open(
            &wav,
            "teams_audio_test.wav"))
    {
        fprintf(
            stderr,
            "Could not open WAV file\n"
        );

        ta_engine_destroy(
            &engine
        );

        return 1;
    }

    float alice_buffer[
        TA_AUDIO_BLOCK * 2
    ];

    float bob_buffer[
        TA_AUDIO_BLOCK * 2
    ];

    float output[
        TA_AUDIO_BLOCK * 2
    ];

    float alice_phase = 0.0f;
    float bob_phase = 0.0f;

    /*
     * Simulate 10 seconds of audio.
     */

    const size_t blocks =
        (
            TA_SAMPLE_RATE *
            10
        ) /
        TA_AUDIO_BLOCK;

    for (size_t block = 0;
         block < blocks;
         ++block)
    {
        /*
         * Simulated voices.
         */

        ta_generate_sine(
            alice_buffer,
            TA_AUDIO_BLOCK,
            180.0f,
            0.25f,
            &alice_phase
        );

        ta_generate_sine(
            bob_buffer,
            TA_AUDIO_BLOCK,
            240.0f,
            0.15f,
            &bob_phase
        );

        /*
         * Add some simulated background noise.
         */

        for (size_t i = 0;
             i < TA_AUDIO_BLOCK * 2;
             ++i)
        {
            alice_buffer[i] +=
                ta_random_float() *
                0.003f;

            bob_buffer[i] +=
                ta_random_float() *
                0.004f;
        }

        /*
         * Feed participant audio.
         */

        ta_engine_push_audio(
            &engine,
            alice,
            alice_buffer,
            TA_AUDIO_BLOCK
        );

        ta_engine_push_audio(
            &engine,
            bob,
            bob_buffer,
            TA_AUDIO_BLOCK
        );

        /*
         * Process mixer.
         */

        ta_engine_process(
            &engine,
            output,
            TA_AUDIO_BLOCK
        );

        /*
         * Save processed audio.
         */

        ta_wav_write(
            &wav,
            output,
            TA_AUDIO_BLOCK
        );
    }

    ta_wav_close(
        &wav
    );

    ta_engine_print_stats(
        &engine
    );

    ta_engine_destroy(
        &engine
    );

    printf(
        "\nGenerated teams_audio_test.wav\n"
    );

    return 0;
}
Compile it

On Linux/macOS:

gcc -std=c11 \
    -O3 \
    -ffast-math \
    -Wall \
    -Wextra \
    -pedantic \
    teams_audio_core.c \
    -o teams_audio \
    -lm \
    -pthread

Then:

./teams_audio

It will generate:

teams_audio_test.wav











teams_video_core.c
/*
 * ============================================================
 * Teams Video Core
 * ============================================================
 *
 * Native C video-frame processing engine.
 *
 * C11
 *
 * Designed as a high-performance processing layer for a
 * Teams-style real-time communications client.
 *
 * Pipeline:
 *
 *     Capture
 *       |
 *       v
 *     Frame
 *       |
 *       +--> Format conversion
 *       |
 *       +--> Crop
 *       |
 *       +--> Scale
 *       |
 *       +--> Luminance analysis
 *       |
 *       +--> Frame difference
 *       |
 *       +--> Temporal smoothing
 *       |
 *       +--> Adaptive frame-rate control
 *       |
 *       v
 *     Encoder
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* ============================================================
 * CONFIGURATION
 * ============================================================ */

#define TV_MAX_WIDTH              3840
#define TV_MAX_HEIGHT             2160

#define TV_MAX_PIXELS             \
    (TV_MAX_WIDTH * TV_MAX_HEIGHT)

#define TV_MAX_QUEUE              16

#define TV_DEFAULT_FPS            30

#define TV_MIN_FPS                5
#define TV_MAX_FPS                60

#define TV_FRAME_DIFFERENCE_THRESHOLD 0.025f

#define TV_LUMA_MIN               16.0f
#define TV_LUMA_MAX               235.0f


/* ============================================================
 * PIXEL TYPES
 * ============================================================ */

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

} tv_rgb24;


typedef struct
{
    uint8_t y;
    uint8_t u;
    uint8_t v;

} tv_yuv_pixel;


/* ============================================================
 * VIDEO FRAME
 * ============================================================ */

typedef enum
{
    TV_FORMAT_RGB24 = 0,
    TV_FORMAT_RGBA32,
    TV_FORMAT_GRAY8,
    TV_FORMAT_YUV420P

} tv_pixel_format;


/*
 * A video frame.
 *
 * For simplicity the first implementation supports packed
 * formats directly and has explicit Y/U/V planes for YUV420.
 */

typedef struct
{
    uint32_t width;
    uint32_t height;

    uint32_t stride;

    tv_pixel_format format;

    uint8_t *data;

    uint8_t *plane_y;
    uint8_t *plane_u;
    uint8_t *plane_v;

    uint32_t stride_y;
    uint32_t stride_u;
    uint32_t stride_v;

    uint64_t timestamp;

    uint64_t frame_number;

    bool key_frame;

} tv_frame;


/* ============================================================
 * FRAME ALLOCATION
 * ============================================================ */

static size_t
tv_frame_size(
    uint32_t width,
    uint32_t height,
    tv_pixel_format format
)
{
    switch (format)
    {
        case TV_FORMAT_RGB24:
            return
                (size_t)width *
                height *
                3;

        case TV_FORMAT_RGBA32:
            return
                (size_t)width *
                height *
                4;

        case TV_FORMAT_GRAY8:
            return
                (size_t)width *
                height;

        case TV_FORMAT_YUV420P:
            return
                (size_t)width *
                height
                +
                (size_t)(width / 2) *
                (height / 2) *
                2;

        default:
            return 0;
    }
}


/* ============================================================
 * FRAME CREATE
 * ============================================================ */

static tv_frame *
tv_frame_create(
    uint32_t width,
    uint32_t height,
    tv_pixel_format format
)
{
    tv_frame *frame;

    frame =
        calloc(
            1,
            sizeof(*frame)
        );

    if (!frame)
        return NULL;

    frame->width = width;
    frame->height = height;
    frame->format = format;

    frame->data =
        malloc(
            tv_frame_size(
                width,
                height,
                format
            )
        );

    if (!frame->data)
    {
        free(frame);
        return NULL;
    }

    switch (format)
    {
        case TV_FORMAT_RGB24:

            frame->stride =
                width * 3;

            break;

        case TV_FORMAT_RGBA32:

            frame->stride =
                width * 4;

            break;

        case TV_FORMAT_GRAY8:

            frame->stride =
                width;

            break;

        case TV_FORMAT_YUV420P:

            frame->plane_y =
                frame->data;

            frame->plane_u =
                frame->plane_y +
                width * height;

            frame->plane_v =
                frame->plane_u +
                (width / 2) *
                (height / 2);

            frame->stride_y =
                width;

            frame->stride_u =
                width / 2;

            frame->stride_v =
                width / 2;

            break;
    }

    return frame;
}


/* ============================================================
 * FRAME DESTROY
 * ============================================================ */

static void
tv_frame_destroy(
    tv_frame *frame
)
{
    if (!frame)
        return;

    free(frame->data);

    free(frame);
}


/* ============================================================
 * FRAME CLEAR
 * ============================================================ */

static void
tv_frame_clear(
    tv_frame *frame,
    uint8_t value
)
{
    if (!frame || !frame->data)
        return;

    memset(
        frame->data,
        value,
        tv_frame_size(
            frame->width,
            frame->height,
            frame->format
        )
    );
}


/* ============================================================
 * RGB → LUMINANCE
 * ============================================================ */

static inline uint8_t
tv_rgb_luma(
    uint8_t r,
    uint8_t g,
    uint8_t b
)
{
    /*
     * BT.601-ish integer approximation.
     */

    return
        (uint8_t)
        (
            (
                77 * r +
                150 * g +
                29 * b
            ) >> 8
        );
}


/* ============================================================
 * RGB → YUV
 * ============================================================ */

static inline void
tv_rgb_to_yuv(
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t *y,
    uint8_t *u,
    uint8_t *v
)
{
    int yi;
    int ui;
    int vi;

    yi =
        (
            77 * r +
            150 * g +
            29 * b
        ) >> 8;

    ui =
        (
            -43 * r -
            85 * g +
            128 * b
        ) >> 8;

    vi =
        (
            128 * r -
            107 * g -
            21 * b
        ) >> 8;

    yi += 16;
    ui += 128;
    vi += 128;

    if (yi < 0) yi = 0;
    if (yi > 255) yi = 255;

    if (ui < 0) ui = 0;
    if (ui > 255) ui = 255;

    if (vi < 0) vi = 0;
    if (vi > 255) vi = 255;

    *y = (uint8_t)yi;
    *u = (uint8_t)ui;
    *v = (uint8_t)vi;
}


/* ============================================================
 * RGB24 → GRAY
 * ============================================================ */

static void
tv_rgb_to_gray(
    const tv_frame *src,
    tv_frame *dst
)
{
    if (!src || !dst)
        return;

    if (src->format !=
        TV_FORMAT_RGB24)
        return;

    if (dst->format !=
        TV_FORMAT_GRAY8)
        return;

    for (uint32_t y = 0;
         y < src->height;
         ++y)
    {
        const uint8_t *input =
            src->data +
            y * src->stride;

        uint8_t *output =
            dst->data +
            y * dst->stride;

        for (uint32_t x = 0;
             x < src->width;
             ++x)
        {
            uint8_t r =
                input[x * 3];

            uint8_t g =
                input[x * 3 + 1];

            uint8_t b =
                input[x * 3 + 2];

            output[x] =
                tv_rgb_luma(
                    r,
                    g,
                    b
                );
        }
    }
}


/* ============================================================
 * RGB24 → YUV420P
 * ============================================================ */

static void
tv_rgb_to_yuv420(
    const tv_frame *src,
    tv_frame *dst
)
{
    if (!src || !dst)
        return;

    if (src->format !=
        TV_FORMAT_RGB24)
        return;

    if (dst->format !=
        TV_FORMAT_YUV420P)
        return;

    /*
     * Y plane.
     */

    for (uint32_t y = 0;
         y < src->height;
         ++y)
    {
        const uint8_t *input =
            src->data +
            y * src->stride;

        uint8_t *output =
            dst->plane_y +
            y * dst->stride_y;

        for (uint32_t x = 0;
             x < src->width;
             ++x)
        {
            uint8_t r =
                input[x * 3];

            uint8_t g =
                input[x * 3 + 1];

            uint8_t b =
                input[x * 3 + 2];

            uint8_t yy;
            uint8_t uu;
            uint8_t vv;

            tv_rgb_to_yuv(
                r,
                g,
                b,
                &yy,
                &uu,
                &vv
            );

            output[x] = yy;
        }
    }

    /*
     * Subsample U/V.
     */

    for (uint32_t y = 0;
         y < src->height;
         y += 2)
    {
        for (uint32_t x = 0;
             x < src->width;
             x += 2)
        {
            uint32_t r_sum = 0;
            uint32_t g_sum = 0;
            uint32_t b_sum = 0;

            uint32_t count = 0;

            for (uint32_t dy = 0;
                 dy < 2;
                 ++dy)
            {
                for (uint32_t dx = 0;
                     dx < 2;
                     ++dx)
                {
                    uint32_t px =
                        x + dx;

                    uint32_t py =
                        y + dy;

                    if (px >= src->width ||
                        py >= src->height)
                        continue;

                    const uint8_t *p =
                        src->data +
                        py * src->stride +
                        px * 3;

                    r_sum += p[0];
                    g_sum += p[1];
                    b_sum += p[2];

                    count++;
                }
            }

            uint8_t r =
                (uint8_t)
                (r_sum / count);

            uint8_t g =
                (uint8_t)
                (g_sum / count);

            uint8_t b =
                (uint8_t)
                (b_sum / count);

            uint8_t yy;
            uint8_t uu;
            uint8_t vv;

            tv_rgb_to_yuv(
                r,
                g,
                b,
                &yy,
                &uu,
                &vv
            );

            uint32_t ux =
                x / 2;

            uint32_t uy =
                y / 2;

            dst->plane_u[
                uy * dst->stride_u +
                ux
            ] = uu;

            dst->plane_v[
                uy * dst->stride_v +
                ux
            ] = vv;
        }
    }
}


/* ============================================================
 * BILINEAR RGB SCALER
 * ============================================================ */

static void
tv_scale_rgb(
    const tv_frame *src,
    tv_frame *dst
)
{
    if (!src || !dst)
        return;

    if (src->format !=
        TV_FORMAT_RGB24)
        return;

    if (dst->format !=
        TV_FORMAT_RGB24)
        return;

    float x_ratio =
        (float)src->width /
        (float)dst->width;

    float y_ratio =
        (float)src->height /
        (float)dst->height;

    for (uint32_t y = 0;
         y < dst->height;
         ++y)
    {
        float source_y =
            y * y_ratio;

        uint32_t y0 =
            (uint32_t)source_y;

        uint32_t y1 =
            y0 + 1;

        if (y1 >= src->height)
            y1 = src->height - 1;

        float fy =
            source_y -
            (float)y0;

        for (uint32_t x = 0;
             x < dst->width;
             ++x)
        {
            float source_x =
                x * x_ratio;

            uint32_t x0 =
                (uint32_t)source_x;

            uint32_t x1 =
                x0 + 1;

            if (x1 >= src->width)
                x1 = src->width - 1;

            float fx =
                source_x -
                (float)x0;

            const uint8_t *p00 =
                src->data +
                y0 * src->stride +
                x0 * 3;

            const uint8_t *p10 =
                src->data +
                y0 * src->stride +
                x1 * 3;

            const uint8_t *p01 =
                src->data +
                y1 * src->stride +
                x0 * 3;

            const uint8_t *p11 =
                src->data +
                y1 * src->stride +
                x1 * 3;

            uint8_t *out =
                dst->data +
                y * dst->stride +
                x * 3;

            for (int c = 0;
                 c < 3;
                 ++c)
            {
                float top =
                    p00[c] *
                    (1.0f - fx)
                    +
                    p10[c] *
                    fx;

                float bottom =
                    p01[c] *
                    (1.0f - fx)
                    +
                    p11[c] *
                    fx;

                float value =
                    top *
                    (1.0f - fy)
                    +
                    bottom *
                    fy;

                if (value < 0.0f)
                    value = 0.0f;

                if (value > 255.0f)
                    value = 255.0f;

                out[c] =
                    (uint8_t)value;
            }
        }
    }
}


/* ============================================================
 * CROP
 * ============================================================ */

static void
tv_crop_rgb(
    const tv_frame *src,
    tv_frame *dst,
    uint32_t crop_x,
    uint32_t crop_y
)
{
    if (!src || !dst)
        return;

    if (src->format !=
        TV_FORMAT_RGB24)
        return;

    if (dst->format !=
        TV_FORMAT_RGB24)
        return;

    if (crop_x + dst->width >
        src->width)
        return;

    if (crop_y + dst->height >
        src->height)
        return;

    for (uint32_t y = 0;
         y < dst->height;
         ++y)
    {
        const uint8_t *source =
            src->data +
            (crop_y + y) *
            src->stride +
            crop_x * 3;

        uint8_t *target =
            dst->data +
            y * dst->stride;

        memcpy(
            target,
            source,
            dst->width * 3
        );
    }
}


/* ============================================================
 * HORIZONTAL FLIP
 * ============================================================ */

static void
tv_flip_horizontal(
    tv_frame *frame
)
{
    if (!frame)
        return;

    if (frame->format !=
        TV_FORMAT_RGB24)
        return;

    for (uint32_t y = 0;
         y < frame->height;
         ++y)
    {
        uint8_t *row =
            frame->data +
            y * frame->stride;

        for (uint32_t x = 0;
             x < frame->width / 2;
             ++x)
        {
            uint32_t opposite =
                frame->width -
                1 -
                x;

            for (int c = 0;
                 c < 3;
                 ++c)
            {
                uint8_t tmp =
                    row[x * 3 + c];

                row[x * 3 + c] =
                    row[opposite * 3 + c];

                row[opposite * 3 + c] =
                    tmp;
            }
        }
    }
}


/* ============================================================
 * ROTATE 180°
 * ============================================================ */

static void
tv_rotate_180(
    tv_frame *frame
)
{
    if (!frame)
        return;

    if (frame->format !=
        TV_FORMAT_RGB24)
        return;

    uint32_t pixels =
        frame->width *
        frame->height;

    for (uint32_t i = 0;
         i < pixels / 2;
         ++i)
    {
        uint32_t opposite =
            pixels -
            1 -
            i;

        for (int c = 0;
             c < 3;
             ++c)
        {
            uint8_t tmp =
                frame->data[
                    i * 3 + c
                ];

            frame->data[
                i * 3 + c
            ] =
                frame->data[
                    opposite * 3 + c
                ];

            frame->data[
                opposite * 3 + c
            ] = tmp;
        }
    }
}


/* ============================================================
 * FRAME LUMINANCE
 * ============================================================ */

static float
tv_average_luminance(
    const tv_frame *frame
)
{
    if (!frame)
        return 0.0f;

    double sum = 0.0;

    uint64_t count = 0;

    if (frame->format ==
        TV_FORMAT_GRAY8)
    {
        for (uint32_t y = 0;
             y < frame->height;
             ++y)
        {
            const uint8_t *row =
                frame->data +
                y * frame->stride;

            for (uint32_t x = 0;
                 x < frame->width;
                 ++x)
            {
                sum += row[x];
                count++;
            }
        }
    }
    else if (
        frame->format ==
        TV_FORMAT_RGB24
    )
    {
        for (uint32_t y = 0;
             y < frame->height;
             ++y)
        {
            const uint8_t *row =
                frame->data +
                y * frame->stride;

            for (uint32_t x = 0;
                 x < frame->width;
                 ++x)
            {
                uint8_t luma =
                    tv_rgb_luma(
                        row[x * 3],
                        row[x * 3 + 1],
                        row[x * 3 + 2]
                    );

                sum += luma;
                count++;
            }
        }
    }

    if (count == 0)
        return 0.0f;

    return
        (float)
        (
            sum /
            (double)count
        );
}


/* ============================================================
 * FRAME DIFFERENCE
 * ============================================================ */

static float
tv_frame_difference(
    const tv_frame *a,
    const tv_frame *b
)
{
    if (!a || !b)
        return 1.0f;

    if (a->width != b->width ||
        a->height != b->height)
        return 1.0f;

    if (a->format !=
        TV_FORMAT_RGB24 ||
        b->format !=
        TV_FORMAT_RGB24)
        return 1.0f;

    /*
     * Sample rather than compare every pixel.
     *
     * This is useful for determining whether a frame has
     * meaningfully changed before spending expensive processing
     * time on it.
     */

    const uint32_t step = 8;

    uint64_t difference = 0;
    uint64_t samples = 0;

    for (uint32_t y = 0;
         y < a->height;
         y += step)
    {
        const uint8_t *row_a =
            a->data +
            y * a->stride;

        const uint8_t *row_b =
            b->data +
            y * b->stride;

        for (uint32_t x = 0;
             x < a->width;
             x += step)
        {
            uint32_t index =
                x * 3;

            int dr =
                abs(
                    (int)row_a[index]
                    -
                    (int)row_b[index]
                );

            int dg =
                abs(
                    (int)row_a[index + 1]
                    -
                    (int)row_b[index + 1]
                );

            int db =
                abs(
                    (int)row_a[index + 2]
                    -
                    (int)row_b[index + 2]
                );

            difference +=
                dr + dg + db;

            samples++;
        }
    }

    if (samples == 0)
        return 0.0f;

    return
        (float)difference /
        (
            (float)samples *
            3.0f *
            255.0f
        );
}


/* ============================================================
 * FRAME MOTION ESTIMATE
 * ============================================================ */

typedef struct
{
    float average_difference;

    float average_luminance;

    bool scene_changed;

    bool mostly_static;

} tv_frame_analysis;


/* ------------------------------------------------------------ */

static tv_frame_analysis
tv_analyse_frame(
    const tv_frame *current,
    const tv_frame *previous
)
{
    tv_frame_analysis analysis;

    memset(
        &analysis,
        0,
        sizeof(analysis)
    );

    analysis.average_luminance =
        tv_average_luminance(
            current
        );

    if (!previous)
    {
        analysis.average_difference =
            1.0f;

        analysis.scene_changed =
            true;

        analysis.mostly_static =
            false;

        return analysis;
    }

    analysis.average_difference =
        tv_frame_difference(
            current,
            previous
        );

    analysis.scene_changed =
        analysis.average_difference >
        0.20f;

    analysis.mostly_static =
        analysis.average_difference <
        TV_FRAME_DIFFERENCE_THRESHOLD;

    return analysis;
}


/* ============================================================
 * TEMPORAL SMOOTHER
 * ============================================================ */

typedef struct
{
    float strength;

    bool initialised;

} tv_temporal_filter;


/* ------------------------------------------------------------ */

static void
tv_temporal_filter_init(
    tv_temporal_filter *filter,
    float strength
)
{
    filter->strength =
        strength;

    filter->initialised =
        false;
}


/* ------------------------------------------------------------ */

static void
tv_temporal_filter_apply(
    tv_temporal_filter *filter,
    tv_frame *current,
    const tv_frame *previous
)
{
    if (!filter ||
        !current ||
        !previous)
        return;

    if (current->format !=
        TV_FORMAT_RGB24)
        return;

    if (previous->format !=
        TV_FORMAT_RGB24)
        return;

    if (current->width !=
        previous->width ||
        current->height !=
        previous->height)
        return;

    float current_weight =
        1.0f -
        filter->strength;

    float previous_weight =
        filter->strength;

    for (uint32_t y = 0;
         y < current->height;
         ++y)
    {
        uint8_t *current_row =
            current->data +
            y * current->stride;

        const uint8_t *previous_row =
            previous->data +
            y * previous->stride;

        for (uint32_t x = 0;
             x < current->width * 3;
             ++x)
        {
            float value =
                current_row[x] *
                current_weight
                +
                previous_row[x] *
                previous_weight;

            if (value < 0.0f)
                value = 0.0f;

            if (value > 255.0f)
                value = 255.0f;

            current_row[x] =
                (uint8_t)value;
        }
    }

    filter->initialised = true;
}


/* ============================================================
 * FRAME RATE CONTROLLER
 * ============================================================ */

typedef struct
{
    float target_fps;

    float current_fps;

    float minimum_fps;

    float maximum_fps;

    float motion;

    float cpu_load;

} tv_frame_rate_controller;


/* ------------------------------------------------------------ */

static void
tv_fps_controller_init(
    tv_frame_rate_controller *controller,
    float target
)
{
    controller->target_fps =
        target;

    controller->current_fps =
        target;

    controller->minimum_fps =
        TV_MIN_FPS;

    controller->maximum_fps =
        TV_MAX_FPS;

    controller->motion = 1.0f;

    controller->cpu_load = 0.0f;
}


/* ------------------------------------------------------------ */

static void
tv_fps_controller_update(
    tv_frame_rate_controller *controller,
    float frame_difference,
    float cpu_load
)
{
    controller->motion =
        frame_difference;

    controller->cpu_load =
        cpu_load;

    /*
     * Heavy CPU pressure.
     */

    if (cpu_load > 0.90f)
    {
        controller->current_fps *=
            0.90f;
    }

    /*
     * Moderate CPU pressure.
     */

    else if (cpu_load > 0.75f)
    {
        controller->current_fps *=
            0.97f;
    }

    /*
     * Lots of motion.
     */

    if (frame_difference > 0.20f)
    {
        controller->current_fps +=
            1.5f;
    }

    /*
     * Almost static image.
     */

    else if (
        frame_difference <
        0.02f
    )
    {
        controller->current_fps -=
            1.0f;
    }

    /*
     * Move gently back toward desired FPS.
     */

    controller->current_fps +=
        (
            controller->target_fps -
            controller->current_fps
        )
        *
        0.02f;

    if (controller->current_fps <
        controller->minimum_fps)
    {
        controller->current_fps =
            controller->minimum_fps;
    }

    if (controller->current_fps >
        controller->maximum_fps)
    {
        controller->current_fps =
            controller->maximum_fps;
    }
}


/* ============================================================
 * VIDEO QUALITY
 * ============================================================ */

typedef enum
{
    TV_QUALITY_LOW = 0,
    TV_QUALITY_MEDIUM,
    TV_QUALITY_HIGH,
    TV_QUALITY_ULTRA

} tv_quality;


typedef struct
{
    tv_quality quality;

    uint32_t width;
    uint32_t height;

    uint32_t bitrate;

    float fps;

} tv_video_profile;


/* ------------------------------------------------------------ */

static tv_video_profile
tv_profile_for_quality(
    tv_quality quality
)
{
    tv_video_profile profile;

    memset(
        &profile,
        0,
        sizeof(profile)
    );

    profile.quality =
        quality;

    switch (quality)
    {
        case TV_QUALITY_LOW:

            profile.width = 640;
            profile.height = 360;
            profile.bitrate = 500000;
            profile.fps = 15.0f;

            break;

        case TV_QUALITY_MEDIUM:

            profile.width = 1280;
            profile.height = 720;
            profile.bitrate = 1500000;
            profile.fps = 30.0f;

            break;

        case TV_QUALITY_HIGH:

            profile.width = 1920;
            profile.height = 1080;
            profile.bitrate = 3500000;
            profile.fps = 30.0f;

            break;

        case TV_QUALITY_ULTRA:

            profile.width = 3840;
            profile.height = 2160;
            profile.bitrate = 12000000;
            profile.fps = 60.0f;

            break;
    }

    return profile;
}


/* ============================================================
 * VIDEO ADAPTATION
 * ============================================================ */

typedef struct
{
    float network_bandwidth;

    float packet_loss;

    float cpu_load;

    float gpu_load;

    float available_memory;

} tv_system_state;


/* ------------------------------------------------------------ */

static tv_quality
tv_select_quality(
    const tv_system_state *state
)
{
    if (!state)
        return TV_QUALITY_MEDIUM;

    /*
     * Severe network degradation.
     */

    if (state->network_bandwidth < 800000.0f ||
        state->packet_loss > 0.15f)
    {
        return TV_QUALITY_LOW;
    }

    /*
     * CPU/GPU pressure.
     */

    if (state->cpu_load > 0.90f ||
        state->gpu_load > 0.95f)
    {
        return TV_QUALITY_MEDIUM;
    }

    /*
     * Good conditions.
     */

    if (state->network_bandwidth >
            8000000.0f &&
        state->cpu_load <
            0.60f &&
        state->gpu_load <
            0.70f)
    {
        return TV_QUALITY_HIGH;
    }

    return TV_QUALITY_MEDIUM;
}


/* ============================================================
 * VIDEO PROCESSING CONTEXT
 * ============================================================ */

typedef struct
{
    tv_video_profile profile;

    tv_frame *previous_frame;

    tv_frame *working_frame;

    tv_frame *scaled_frame;

    tv_frame *gray_frame;

    tv_temporal_filter temporal_filter;

    tv_frame_rate_controller fps_controller;

    tv_system_state system;

    uint64_t processed_frames;

    uint64_t dropped_frames;

    uint64_t static_frames;

    uint64_t scene_changes;

} tv_video_context;


/* ============================================================
 * CONTEXT INITIALIZATION
 * ============================================================ */

static bool
tv_context_init(
    tv_video_context *context,
    tv_quality quality
)
{
    memset(
        context,
        0,
        sizeof(*context)
    );

    context->profile =
        tv_profile_for_quality(
            quality
        );

    context->working_frame =
        tv_frame_create(
            context->profile.width,
            context->profile.height,
            TV_FORMAT_RGB24
        );

    context->scaled_frame =
        tv_frame_create(
            context->profile.width,
            context->profile.height,
            TV_FORMAT_RGB24
        );

    context->gray_frame =
        tv_frame_create(
            context->profile.width,
            context->profile.height,
            TV_FORMAT_GRAY8
        );

    if (!context->working_frame ||
        !context->scaled_frame ||
        !context->gray_frame)
    {
        return false;
    }

    tv_temporal_filter_init(
        &context->temporal_filter,
        0.08f
    );

    tv_fps_controller_init(
        &context->fps_controller,
        context->profile.fps
    );

    context->system.network_bandwidth =
        10000000.0f;

    context->system.packet_loss =
        0.0f;

    context->system.cpu_load =
        0.30f;

    context->system.gpu_load =
        0.30f;

    return true;
}


/* ============================================================
 * CONTEXT DESTROY
 * ============================================================ */

static void
tv_context_destroy(
    tv_video_context *context
)
{
    if (!context)
        return;

    tv_frame_destroy(
        context->previous_frame
    );

    tv_frame_destroy(
        context->working_frame
    );

    tv_frame_destroy(
        context->scaled_frame
    );

    tv_frame_destroy(
        context->gray_frame
    );
}


/* ============================================================
 * COPY FRAME
 * ============================================================ */

static void
tv_frame_copy(
    tv_frame *dst,
    const tv_frame *src
)
{
    if (!dst || !src)
        return;

    if (dst->format !=
        src->format)
        return;

    if (dst->width !=
        src->width ||
        dst->height !=
        src->height)
        return;

    size_t size =
        tv_frame_size(
            src->width,
            src->height,
            src->format
        );

    memcpy(
        dst->data,
        src->data,
        size
    );

    dst->timestamp =
        src->timestamp;

    dst->frame_number =
        src->frame_number;

    dst->key_frame =
        src->key_frame;
}


/* ============================================================
 * MAIN VIDEO PIPELINE
 * ============================================================ */

static bool
tv_process_frame(
    tv_video_context *context,
    const tv_frame *input,
    tv_frame **output
)
{
    if (!context ||
        !input ||
        !output)
        return false;

    /*
     * Input must be RGB24 for this prototype.
     */

    if (input->format !=
        TV_FORMAT_RGB24)
        return false;

    /*
     * Scale input into target profile.
     */

    tv_scale_rgb(
        input,
        context->scaled_frame
    );

    /*
     * Analyse motion.
     */

    tv_frame_analysis analysis =
        tv_analyse_frame(
            context->scaled_frame,
            context->previous_frame
        );

    /*
     * Update frame-rate controller.
     */

    tv_fps_controller_update(
        &context->fps_controller,
        analysis.average_difference,
        context->system.cpu_load
    );

    /*
     * Static frame.
     */

    if (analysis.mostly_static)
    {
        context->static_frames++;
    }

    /*
     * Scene transition.
     */

    if (analysis.scene_changed)
    {
        context->scene_changes++;

        context->scaled_frame->key_frame =
            true;
    }

    /*
     * Temporal smoothing.
     *
     * Only use this when the scene is reasonably stable.
     */

    if (!analysis.scene_changed &&
        !analysis.mostly_static &&
        context->previous_frame)
    {
        tv_frame_copy(
            context->working_frame,
            context->scaled_frame
        );

        tv_temporal_filter_apply(
            &context->temporal_filter,
            context->working_frame,
            context->previous_frame
        );
    }
    else
    {
        tv_frame_copy(
            context->working_frame,
            context->scaled_frame
        );
    }

    /*
     * Save previous frame.
     */

    if (!context->previous_frame)
    {
        context->previous_frame =
            tv_frame_create(
                context->profile.width,
                context->profile.height,
                TV_FORMAT_RGB24
            );

        if (!context->previous_frame)
            return false;
    }

    tv_frame_copy(
        context->previous_frame,
        context->scaled_frame
    );

    context->processed_frames++;

    *output =
        context->working_frame;

    return true;
}


/* ============================================================
 * TEST PATTERN
 * ============================================================ */

static void
tv_generate_test_pattern(
    tv_frame *frame,
    uint64_t frame_number
)
{
    if (!frame)
        return;

    if (frame->format !=
        TV_FORMAT_RGB24)
        return;

    for (uint32_t y = 0;
         y < frame->height;
         ++y)
    {
        uint8_t *row =
            frame->data +
            y * frame->stride;

        for (uint32_t x = 0;
             x < frame->width;
             ++x)
        {
            uint8_t r =
                (uint8_t)
                (
                    (x + frame_number)
                    %
                    256
                );

            uint8_t g =
                (uint8_t)
                (
                    (y + frame_number)
                    %
                    256
                );

            uint8_t b =
                (uint8_t)
                (
                    (
                        x +
                        y +
                        frame_number
                    )
                    %
                    256
                );

            row[x * 3] =
                r;

            row[x * 3 + 1] =
                g;

            row[x * 3 + 2] =
                b;
        }
    }
}


/* ============================================================
 * RAW RGB FRAME WRITER
 * ============================================================ */

static bool
tv_write_ppm(
    const char *filename,
    const tv_frame *frame
)
{
    if (!filename ||
        !frame)
        return false;

    if (frame->format !=
        TV_FORMAT_RGB24)
        return false;

    FILE *file =
        fopen(
            filename,
            "wb"
        );

    if (!file)
        return false;

    fprintf(
        file,
        "P6\n%u %u\n255\n",
        frame->width,
        frame->height
    );

    for (uint32_t y = 0;
         y < frame->height;
         ++y)
    {
        fwrite(
            frame->data +
            y * frame->stride,
            1,
            frame->width * 3,
            file
        );
    }

    fclose(file);

    return true;
}


/* ============================================================
 * STATISTICS
 * ============================================================ */

static void
tv_print_statistics(
    const tv_video_context *context
)
{
    if (!context)
        return;

    printf(
        "\n"
        "====================================\n"
        "       TEAMS VIDEO CORE\n"
        "====================================\n"
    );

    printf(
        "Resolution:       %ux%u\n",
        context->profile.width,
        context->profile.height
    );

    printf(
        "Target FPS:       %.1f\n",
        context->profile.fps
    );

    printf(
        "Current FPS:      %.1f\n",
        context->fps_controller.current_fps
    );

    printf(
        "Bitrate:          %u bps\n",
        context->profile.bitrate
    );

    printf(
        "Processed frames: %llu\n",
        (unsigned long long)
        context->processed_frames
    );

    printf(
        "Dropped frames:   %llu\n",
        (unsigned long long)
        context->dropped_frames
    );

    printf(
        "Static frames:    %llu\n",
        (unsigned long long)
        context->static_frames
    );

    printf(
        "Scene changes:    %llu\n",
        (unsigned long long)
        context->scene_changes
    );

    printf(
        "CPU load:         %.2f\n",
        context->system.cpu_load
    );

    printf(
        "GPU load:         %.2f\n",
        context->system.gpu_load
    );

    printf(
        "Network:          %.2f Mbps\n",
        context->system.network_bandwidth
        /
        1000000.0f
    );

    printf(
        "Packet loss:      %.2f%%\n",
        context->system.packet_loss *
        100.0f
    );

    printf(
        "====================================\n"
    );
}


/* ============================================================
 * BENCHMARK TIMER
 * ============================================================ */

static double
tv_time_seconds(void)
{
    struct timespec ts;

    clock_gettime(
        CLOCK_MONOTONIC,
        &ts
    );

    return
        (double)ts.tv_sec +
        (double)ts.tv_nsec /
        1000000000.0;
}


/* ============================================================
 * BENCHMARK
 * ============================================================ */

static void
tv_benchmark(
    tv_video_context *context,
    tv_frame *input,
    uint32_t frames
)
{
    double start =
        tv_time_seconds();

    for (uint32_t i = 0;
         i < frames;
         ++i)
    {
        input->frame_number = i;

        tv_generate_test_pattern(
            input,
            i
        );

        tv_frame *output = NULL;

        tv_process_frame(
            context,
            input,
            &output
        );
    }

    double end =
        tv_time_seconds();

    double elapsed =
        end - start;

    double fps =
        (double)frames /
        elapsed;

    printf(
        "\n"
        "Benchmark\n"
        "---------\n"
    );

    printf(
        "Frames: %.0f\n",
        (double)frames
    );

    printf(
        "Time: %.3f seconds\n",
        elapsed
    );

    printf(
        "Processing FPS: %.1f\n",
        fps
    );
}


/* ============================================================
 * MAIN
 * ============================================================ */

int
main(void)
{
    printf(
        "Starting Teams Video Core...\n"
    );

    /*
     * Start with 720p.
     */

    tv_video_context context;

    if (!tv_context_init(
            &context,
            TV_QUALITY_MEDIUM))
    {
        fprintf(
            stderr,
            "Could not initialize video context\n"
        );

        return 1;
    }

    /*
     * Simulated 1080p camera.
     */

    tv_frame *camera =
        tv_frame_create(
            1920,
            1080,
            TV_FORMAT_RGB24
        );

    if (!camera)
    {
        fprintf(
            stderr,
            "Could not allocate camera frame\n"
        );

        tv_context_destroy(
            &context
        );

        return 1;
    }

    /*
     * Generate/process frames.
     */

    for (uint32_t frame = 0;
         frame < 120;
         ++frame)
    {
        camera->frame_number =
            frame;

        camera->timestamp =
            frame *
            33333;

        tv_generate_test_pattern(
            camera,
            frame
        );

        tv_frame *processed = NULL;

        if (!tv_process_frame(
                &context,
                camera,
                &processed))
        {
            fprintf(
                stderr,
                "Frame processing failed\n"
            );

            break;
        }

        /*
         * Save one frame as proof of processing.
         */

        if (frame == 0)
        {
            tv_write_ppm(
                "teams_video_output.ppm",
                processed
            );
        }
    }

    /*
     * Benchmark.
     */

    tv_benchmark(
        &context,
        camera,
        100
    );

    /*
     * Test RGB → YUV420 conversion.
     */

    tv_frame *yuv =
        tv_frame_create(
            1280,
            720,
            TV_FORMAT_YUV420P
        );

    tv_frame *rgb =
        tv_frame_create(
            1280,
            720,
            TV_FORMAT_RGB24
        );

    if (yuv && rgb)
    {
        tv_generate_test_pattern(
            rgb,
            1
        );

        tv_rgb_to_yuv420(
            rgb,
            yuv
        );

        printf(
            "\nYUV420 conversion completed.\n"
        );
    }

    /*
     * Print final statistics.
     */

    tv_print_statistics(
        &context
    );

    /*
     * Cleanup.
     */

    tv_frame_destroy(
        yuv
    );

    tv_frame_destroy(
        rgb
    );

    tv_frame_destroy(
        camera
    );

    tv_context_destroy(
        &context
    );

    printf(
        "\nVideo engine shutdown complete.\n"
    );

    return 0;
}
Compile
gcc -std=c11 \
    -O3 \
    -march=native \
    -Wall \
    -Wextra \
    -pedantic \
    teams_video_core.c \
    -o teams_video \
    -lm

Run:

./teams_video

It produces a processed frame:

teams_video_output.ppm

and gives you a processing benchmark.

What this gives us

The interesting part isn't merely converting pixels. It's establishing a native video pipeline that can make decisions based on what is happening to the video.

For example:

                    CAMERA
                       │
                       ▼
                1920 × 1080
                       │
                       ▼
               ┌──────────────┐
               │ Frame Analyse│
               └───────┬──────┘
                       │
             ┌─────────┴─────────┐
             │                   │
        Lots of motion       Static scene
             │                   │
             ▼                   ▼
        Higher FPS           Lower FPS
             │                   │
             └─────────┬─────────┘
                       ▼
                   Scaling
                       │
                       ▼
                Temporal Filter
                       │
                       ▼
                 RGB → YUV420
                       │
                       ▼
                  VIDEO CODEC
                       │
                       ▼
                    NETWORK
The next major upgrade

The really interesting version would add a hardware-aware video backend:

                 Teams Video Core
                        │
          ┌─────────────┼─────────────┐
          │             │             │
          ▼             ▼             ▼
        CPU           GPU         NPU/AI
          │             │             │
       SIMD C      Metal/D3D/Vulkan   AI
          │             │             │
          └─────────────┼─────────────┘
                        ▼
                  Video Encoder
                        │
              ┌─────────┼─────────┐
              ▼         ▼         ▼
             H.264     H.265     AV1

That would let the C engine dynamically decide something like:

if (gpu_load < 0.70f &&
    network_bandwidth > 5000000.0f)
{
    use_hardware_encoder = true;
    target_fps = 30.0f;
    target_resolution = 1920 * 1080;
}
else if (network_bandwidth < 1000000.0f)
{
    target_fps = 15.0f;
    target_resolution = 1280 * 720;
}
else
{
    target_fps = 24.0f;
}






teams_network_core.c
/*
 * teams_network_core.c
 *
 * Native C adaptive networking engine for a Teams-like communications client.
 *
 * C11
 *
 * Features:
 *
 *   - Packet abstraction
 *   - Packet sequence numbers
 *   - Packet timestamps
 *   - Packet loss detection
 *   - Duplicate detection
 *   - Out-of-order detection
 *   - RTT estimation
 *   - Jitter estimation
 *   - Packet-loss estimation
 *   - Receive-rate estimation
 *   - Send-rate estimation
 *   - Sliding network statistics
 *   - EWMA smoothing
 *   - Congestion state machine
 *   - Bandwidth estimation
 *   - Adaptive bitrate
 *   - Adaptive video FPS
 *   - Adaptive resolution
 *   - Adaptive audio/video allocation
 *   - Network-quality scoring
 *   - Jitter buffer
 *   - Packet reordering
 *   - Late-packet handling
 *   - Packet-loss concealment hooks
 *   - Keepalive scheduling
 *   - Network health events
 *   - Connection state
 *   - Network profiles
 *   - Test packet generator
 *   - Network impairment simulator
 *   - Diagnostics
 *   - Benchmark
 *
 * This is a networking-engine prototype rather than a production
 * RTP/QUIC/WebRTC implementation. Production deployment should use
 * mature protocol implementations for actual transport, encryption,
 * NAT traversal, codecs, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ================================================================
 * CONSTANTS
 * ================================================================ */

#define TN_MAX_PACKET_SIZE          1500
#define TN_MAX_JITTER_PACKETS       256
#define TN_HISTORY_SIZE             256
#define TN_MAX_STREAMS              16

#define TN_MIN_BITRATE_KBPS         16
#define TN_MAX_BITRATE_KBPS         100000

#define TN_MIN_VIDEO_FPS            5.0
#define TN_MAX_VIDEO_FPS            60.0

#define TN_MIN_RTT_MS               1.0
#define TN_MAX_RTT_MS               5000.0

#define TN_MIN_JITTER_MS            0.0
#define TN_MAX_JITTER_MS            1000.0

#define TN_EWMA_ALPHA               0.125

#define TN_KEEPALIVE_MS             5000
#define TN_STATS_INTERVAL_MS        1000

#define TN_PACKET_HEADER_SIZE       32

/* ================================================================
 * UTILITY
 * ================================================================ */

static double tn_clamp(double x, double lo, double hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static int tn_clamp_int(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static double tn_abs(double x)
{
    return x < 0.0 ? -x : x;
}

/*
 * Monotonic-ish millisecond timer.
 *
 * For a production implementation use platform-specific monotonic
 * clocks such as QueryPerformanceCounter on Windows or clock_gettime
 * CLOCK_MONOTONIC on Linux.
 */
static uint64_t tn_now_ms(void)
{
    struct timespec ts;

#if defined(_WIN32)
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ================================================================
 * PACKET
 * ================================================================ */

typedef enum
{
    TN_PACKET_AUDIO = 0,
    TN_PACKET_VIDEO,
    TN_PACKET_DATA,
    TN_PACKET_CONTROL,
    TN_PACKET_KEEPALIVE
} TNPacketType;

typedef struct
{
    uint32_t stream_id;

    uint32_t sequence;

    uint64_t timestamp_ms;

    uint64_t send_time_ms;

    uint16_t payload_size;

    TNPacketType type;

    uint8_t payload[TN_MAX_PACKET_SIZE];

} TNPacket;

/* ================================================================
 * PACKET HISTORY
 * ================================================================ */

typedef struct
{
    uint32_t sequence;

    uint64_t arrival_ms;

    uint16_t size;

    bool valid;

} TNPacketHistoryEntry;

typedef struct
{
    TNPacketHistoryEntry entries[TN_HISTORY_SIZE];

    size_t write_index;

    uint64_t received;

    uint64_t lost;

    uint64_t duplicate;

    uint64_t reordered;

} TNPacketHistory;

/* ================================================================
 * RTT ESTIMATOR
 * ================================================================ */

typedef struct
{
    double srtt_ms;

    double rttvar_ms;

    double rto_ms;

    double min_rtt_ms;

    double max_rtt_ms;

    bool initialized;

} TNRttEstimator;

static void tn_rtt_init(TNRttEstimator *r)
{
    memset(r, 0, sizeof(*r));

    r->min_rtt_ms = TN_MAX_RTT_MS;
}

static void tn_rtt_update(TNRttEstimator *r, double sample)
{
    sample = tn_clamp(sample,
                      TN_MIN_RTT_MS,
                      TN_MAX_RTT_MS);

    if (!r->initialized)
    {
        r->srtt_ms = sample;
        r->rttvar_ms = sample / 2.0;
        r->initialized = true;
    }
    else
    {
        double error = tn_abs(r->srtt_ms - sample);

        r->rttvar_ms =
            (1.0 - 0.25) * r->rttvar_ms +
            0.25 * error;

        r->srtt_ms =
            (1.0 - 0.125) * r->srtt_ms +
            0.125 * sample;
    }

    r->rto_ms =
        tn_clamp(r->srtt_ms + 4.0 * r->rttvar_ms,
                 100.0,
                 5000.0);

    if (sample < r->min_rtt_ms)
        r->min_rtt_ms = sample;

    if (sample > r->max_rtt_ms)
        r->max_rtt_ms = sample;
}

/* ================================================================
 * JITTER ESTIMATOR
 * ================================================================ */

typedef struct
{
    double jitter_ms;

    double previous_transit;

    bool initialized;

} TNJitterEstimator;

static void tn_jitter_init(TNJitterEstimator *j)
{
    memset(j, 0, sizeof(*j));
}

static void tn_jitter_update(
    TNJitterEstimator *j,
    double send_time_ms,
    double arrival_time_ms)
{
    double transit =
        arrival_time_ms - send_time_ms;

    if (!j->initialized)
    {
        j->previous_transit = transit;
        j->initialized = true;
        return;
    }

    double delta =
        tn_abs(transit - j->previous_transit);

    /*
     * RTP-style exponentially smoothed jitter estimate.
     */
    j->jitter_ms +=
        (delta - j->jitter_ms) / 16.0;

    j->previous_transit = transit;

    j->jitter_ms =
        tn_clamp(j->jitter_ms,
                 TN_MIN_JITTER_MS,
                 TN_MAX_JITTER_MS);
}

/* ================================================================
 * BANDWIDTH ESTIMATOR
 * ================================================================ */

typedef struct
{
    double instantaneous_kbps;

    double smoothed_kbps;

    double peak_kbps;

    double minimum_kbps;

    uint64_t bytes;

    uint64_t last_update_ms;

    bool initialized;

} TNBandwidthEstimator;

static void tn_bandwidth_init(TNBandwidthEstimator *b)
{
    memset(b, 0, sizeof(*b));

    b->minimum_kbps =
        TN_MAX_BITRATE_KBPS;
}

static void tn_bandwidth_update(
    TNBandwidthEstimator *b,
    uint64_t bytes,
    uint64_t now_ms)
{
    b->bytes += bytes;

    if (!b->initialized)
    {
        b->last_update_ms = now_ms;
        b->initialized = true;
        return;
    }

    uint64_t elapsed =
        now_ms - b->last_update_ms;

    if (elapsed < 100)
        return;

    double seconds =
        (double)elapsed / 1000.0;

    double kbps =
        ((double)b->bytes * 8.0) /
        seconds / 1000.0;

    b->instantaneous_kbps = kbps;

    if (b->smoothed_kbps <= 0.0)
    {
        b->smoothed_kbps = kbps;
    }
    else
    {
        b->smoothed_kbps =
            0.20 * kbps +
            0.80 * b->smoothed_kbps;
    }

    if (kbps > b->peak_kbps)
        b->peak_kbps = kbps;

    if (kbps < b->minimum_kbps)
        b->minimum_kbps = kbps;

    b->bytes = 0;
    b->last_update_ms = now_ms;
}

/* ================================================================
 * LOSS ESTIMATOR
 * ================================================================ */

typedef struct
{
    uint64_t received;

    uint64_t lost;

    uint64_t duplicate;

    uint64_t reordered;

    double loss_ratio;

    double smoothed_loss_ratio;

} TNLossEstimator;

static void tn_loss_init(TNLossEstimator *l)
{
    memset(l, 0, sizeof(*l));
}

static void tn_loss_update(TNLossEstimator *l)
{
    uint64_t total =
        l->received + l->lost;

    if (total == 0)
    {
        l->loss_ratio = 0.0;
        return;
    }

    l->loss_ratio =
        (double)l->lost /
        (double)total;

    l->smoothed_loss_ratio =
        0.10 * l->loss_ratio +
        0.90 * l->smoothed_loss_ratio;
}

/* ================================================================
 * NETWORK QUALITY
 * ================================================================ */

typedef enum
{
    TN_QUALITY_UNKNOWN = 0,
    TN_QUALITY_EXCELLENT,
    TN_QUALITY_GOOD,
    TN_QUALITY_FAIR,
    TN_QUALITY_POOR,
    TN_QUALITY_CRITICAL
} TNNetworkQuality;

static const char *tn_quality_name(
    TNNetworkQuality q)
{
    switch (q)
    {
        case TN_QUALITY_EXCELLENT:
            return "EXCELLENT";

        case TN_QUALITY_GOOD:
            return "GOOD";

        case TN_QUALITY_FAIR:
            return "FAIR";

        case TN_QUALITY_POOR:
            return "POOR";

        case TN_QUALITY_CRITICAL:
            return "CRITICAL";

        default:
            return "UNKNOWN";
    }
}

/* ================================================================
 * CONGESTION STATE
 * ================================================================ */

typedef enum
{
    TN_CONGESTION_STARTUP = 0,
    TN_CONGESTION_STABLE,
    TN_CONGESTION_WARNING,
    TN_CONGESTION_CONGESTED,
    TN_CONGESTION_RECOVERY
} TNCongestionState;

static const char *tn_congestion_name(
    TNCongestionState s)
{
    switch (s)
    {
        case TN_CONGESTION_STARTUP:
            return "STARTUP";

        case TN_CONGESTION_STABLE:
            return "STABLE";

        case TN_CONGESTION_WARNING:
            return "WARNING";

        case TN_CONGESTION_CONGESTED:
            return "CONGESTED";

        case TN_CONGESTION_RECOVERY:
            return "RECOVERY";

        default:
            return "UNKNOWN";
    }
}

/* ================================================================
 * VIDEO ADAPTATION
 * ================================================================ */

typedef struct
{
    int width;

    int height;

    double fps;

    int bitrate_kbps;

    int keyframe_interval;

} TNVideoProfile;

static const TNVideoProfile TN_VIDEO_PROFILES[] =
{
    { 320,  180, 10.0, 150,  60 },
    { 640,  360, 15.0, 400,  60 },
    { 640,  360, 30.0, 700,  60 },
    { 1280, 720, 30.0, 1500, 60 },
    { 1280, 720, 60.0, 3000, 60 },
    { 1920,1080, 30.0, 4500, 60 },
    { 1920,1080, 60.0, 7000, 60 }
};

#define TN_VIDEO_PROFILE_COUNT \
    (sizeof(TN_VIDEO_PROFILES) / sizeof(TN_VIDEO_PROFILES[0]))

/* ================================================================
 * AUDIO PROFILE
 * ================================================================ */

typedef struct
{
    int bitrate_kbps;

    int sample_rate;

    int channels;

} TNAudioProfile;

static const TNAudioProfile TN_AUDIO_PROFILES[] =
{
    { 16,  16000, 1 },
    { 24,  24000, 1 },
    { 32,  32000, 1 },
    { 48,  48000, 1 },
    { 64,  48000, 2 },
    { 96,  48000, 2 }
};

#define TN_AUDIO_PROFILE_COUNT \
    (sizeof(TN_AUDIO_PROFILES) / sizeof(TN_AUDIO_PROFILES[0]))

/* ================================================================
 * ADAPTATION DECISION
 * ================================================================ */

typedef struct
{
    int target_bitrate_kbps;

    int video_bitrate_kbps;

    int audio_bitrate_kbps;

    int video_width;

    int video_height;

    double video_fps;

    int keyframe_interval;

    bool reduce_video;

    bool preserve_audio;

} TNAdaptationDecision;

/* ================================================================
 * JITTER BUFFER
 * ================================================================ */

typedef struct
{
    TNPacket packets[TN_MAX_JITTER_PACKETS];

    bool occupied[TN_MAX_JITTER_PACKETS];

    size_t count;

    uint32_t expected_sequence;

    bool initialized;

    double target_delay_ms;

    double minimum_delay_ms;

    double maximum_delay_ms;

    uint64_t packets_inserted;

    uint64_t packets_released;

    uint64_t packets_dropped;

    uint64_t packets_late;

} TNJitterBuffer;

static void tn_jitter_buffer_init(TNJitterBuffer *jb)
{
    memset(jb, 0, sizeof(*jb));

    jb->target_delay_ms = 60.0;
    jb->minimum_delay_ms = 20.0;
    jb->maximum_delay_ms = 250.0;
}

static size_t tn_jitter_slot(uint32_t sequence)
{
    return sequence % TN_MAX_JITTER_PACKETS;
}

static bool tn_sequence_before(
    uint32_t a,
    uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static bool tn_sequence_after(
    uint32_t a,
    uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

/*
 * Insert a packet into the jitter buffer.
 */
static bool tn_jitter_insert(
    TNJitterBuffer *jb,
    const TNPacket *packet)
{
    if (!jb->initialized)
    {
        jb->expected_sequence =
            packet->sequence;

        jb->initialized = true;
    }

    if (tn_sequence_before(
            packet->sequence,
            jb->expected_sequence))
    {
        jb->packets_late++;
        return false;
    }

    size_t slot =
        tn_jitter_slot(packet->sequence);

    if (jb->occupied[slot])
    {
        /*
         * Collision / duplicate / buffer pressure.
         */
        if (jb->packets[slot].sequence ==
            packet->sequence)
        {
            jb->packets_dropped++;
            return false;
        }

        jb->packets_dropped++;
        return false;
    }

    jb->packets[slot] = *packet;
    jb->occupied[slot] = true;
    jb->count++;
    jb->packets_inserted++;

    return true;
}

/*
 * Find the expected packet.
 */
static TNPacket *tn_jitter_peek(
    TNJitterBuffer *jb)
{
    if (!jb->initialized)
        return NULL;

    size_t slot =
        tn_jitter_slot(jb->expected_sequence);

    if (!jb->occupied[slot])
        return NULL;

    if (jb->packets[slot].sequence !=
        jb->expected_sequence)
        return NULL;

    return &jb->packets[slot];
}

/*
 * Release next packet.
 */
static bool tn_jitter_pop(
    TNJitterBuffer *jb,
    TNPacket *out)
{
    TNPacket *packet =
        tn_jitter_peek(jb);

    if (!packet)
        return false;

    *out = *packet;

    size_t slot =
        tn_jitter_slot(jb->expected_sequence);

    jb->occupied[slot] = false;

    if (jb->count > 0)
        jb->count--;

    jb->expected_sequence++;

    jb->packets_released++;

    return true;
}

/*
 * Adapt jitter-buffer delay.
 */
static void tn_jitter_adapt(
    TNJitterBuffer *jb,
    double jitter_ms)
{
    double desired =
        jitter_ms * 3.0 + 20.0;

    desired =
        tn_clamp(desired,
                 jb->minimum_delay_ms,
                 jb->maximum_delay_ms);

    /*
     * Slowly move toward target rather than constantly
     * resizing the buffer.
     */
    jb->target_delay_ms =
        0.10 * desired +
        0.90 * jb->target_delay_ms;
}

/* ================================================================
 * STREAM STATE
 * ================================================================ */

typedef struct
{
    uint32_t stream_id;

    TNPacketType type;

    uint32_t last_sequence;

    bool sequence_initialized;

    uint64_t packets_received;

    uint64_t packets_sent;

    uint64_t bytes_received;

    uint64_t bytes_sent;

    uint64_t packets_lost;

    uint64_t packets_duplicate;

    uint64_t packets_reordered;

    TNRttEstimator rtt;

    TNJitterEstimator jitter;

    TNLossEstimator loss;

    TNBandwidthEstimator receive_bandwidth;

    TNBandwidthEstimator send_bandwidth;

    TNJitterBuffer jitter_buffer;

} TNNetworkStream;

/* ================================================================
 * NETWORK CONFIGURATION
 * ================================================================ */

typedef struct
{
    int minimum_video_bitrate;

    int maximum_video_bitrate;

    int minimum_audio_bitrate;

    int maximum_audio_bitrate;

    double maximum_acceptable_loss;

    double maximum_acceptable_jitter;

    double maximum_acceptable_rtt;

    bool adaptive_video;

    bool adaptive_audio;

    bool allow_resolution_scaling;

    bool allow_fps_scaling;

} TNNetworkConfig;

static TNNetworkConfig tn_default_config(void)
{
    TNNetworkConfig c;

    c.minimum_video_bitrate = 150;
    c.maximum_video_bitrate = 7000;

    c.minimum_audio_bitrate = 16;
    c.maximum_audio_bitrate = 96;

    c.maximum_acceptable_loss = 0.05;
    c.maximum_acceptable_jitter = 50.0;
    c.maximum_acceptable_rtt = 300.0;

    c.adaptive_video = true;
    c.adaptive_audio = true;

    c.allow_resolution_scaling = true;
    c.allow_fps_scaling = true;

    return c;
}

/* ================================================================
 * NETWORK ENGINE
 * ================================================================ */

typedef struct
{
    TNNetworkConfig config;

    TNNetworkStream streams[TN_MAX_STREAMS];

    size_t stream_count;

    TNCongestionState congestion_state;

    TNNetworkQuality quality;

    double estimated_bandwidth_kbps;

    double target_bitrate_kbps;

    double available_bitrate_kbps;

    double network_score;

    uint64_t last_adaptation_ms;

    uint64_t last_keepalive_ms;

    uint64_t total_packets_sent;

    uint64_t total_packets_received;

    uint64_t total_bytes_sent;

    uint64_t total_bytes_received;

    uint64_t total_packets_lost;

    uint64_t total_packets_dropped;

} TNNetworkEngine;

/* ================================================================
 * ENGINE INIT
 * ================================================================ */

static void tn_network_init(
    TNNetworkEngine *engine)
{
    memset(engine, 0, sizeof(*engine));

    engine->config =
        tn_default_config();

    engine->congestion_state =
        TN_CONGESTION_STARTUP;

    engine->quality =
        TN_QUALITY_UNKNOWN;

    engine->estimated_bandwidth_kbps =
        1000.0;

    engine->target_bitrate_kbps =
        750.0;

    engine->available_bitrate_kbps =
        750.0;

    engine->last_adaptation_ms =
        tn_now_ms();

    engine->last_keepalive_ms =
        tn_now_ms();
}

/* ================================================================
 * STREAM MANAGEMENT
 * ================================================================ */

static TNNetworkStream *tn_find_stream(
    TNNetworkEngine *engine,
    uint32_t stream_id)
{
    for (size_t i = 0;
         i < engine->stream_count;
         ++i)
    {
        if (engine->streams[i].stream_id ==
            stream_id)
        {
            return &engine->streams[i];
        }
    }

    return NULL;
}

static TNNetworkStream *tn_add_stream(
    TNNetworkEngine *engine,
    uint32_t stream_id,
    TNPacketType type)
{
    if (engine->stream_count >=
        TN_MAX_STREAMS)
    {
        return NULL;
    }

    TNNetworkStream *s =
        &engine->streams[engine->stream_count++];

    memset(s, 0, sizeof(*s));

    s->stream_id = stream_id;
    s->type = type;

    tn_rtt_init(&s->rtt);
    tn_jitter_init(&s->jitter);
    tn_loss_init(&s->loss);
    tn_bandwidth_init(&s->receive_bandwidth);
    tn_bandwidth_init(&s->send_bandwidth);
    tn_jitter_buffer_init(&s->jitter_buffer);

    return s;
}

/* ================================================================
 * SEQUENCE ANALYSIS
 * ================================================================ */

static void tn_process_sequence(
    TNNetworkStream *stream,
    uint32_t sequence)
{
    if (!stream->sequence_initialized)
    {
        stream->last_sequence =
            sequence;

        stream->sequence_initialized =
            true;

        return;
    }

    if (sequence == stream->last_sequence)
    {
        stream->packets_duplicate++;
        stream->loss.duplicate++;
        return;
    }

    if (tn_sequence_after(
            sequence,
            stream->last_sequence))
    {
        uint32_t gap =
            sequence -
            stream->last_sequence;

        if (gap > 1)
        {
            /*
             * Missing packets.
             */
            uint32_t missing =
                gap - 1;

            stream->packets_lost +=
                missing;

            stream->loss.lost +=
                missing;
        }

        stream->last_sequence =
            sequence;
    }
    else
    {
        stream->packets_reordered++;
        stream->loss.reordered++;
    }
}

/* ================================================================
 * RECEIVE PACKET
 * ================================================================ */

static bool tn_receive_packet(
    TNNetworkEngine *engine,
    const TNPacket *packet,
    uint64_t arrival_ms)
{
    TNNetworkStream *stream =
        tn_find_stream(
            engine,
            packet->stream_id);

    if (!stream)
    {
        stream =
            tn_add_stream(
                engine,
                packet->stream_id,
                packet->type);
    }

    if (!stream)
        return false;

    tn_process_sequence(
        stream,
        packet->sequence);

    stream->packets_received++;

    stream->bytes_received +=
        packet->payload_size;

    stream->loss.received++;

    tn_jitter_update(
        &stream->jitter,
        (double)packet->send_time_ms,
        (double)arrival_ms);

    /*
     * The packet's send timestamp allows an RTT-like
     * one-way transit measurement in this simulation.
     *
     * Real bidirectional systems should derive RTT from
     * explicit timestamp/acknowledgement exchange.
     */
    double transit =
        (double)arrival_ms -
        (double)packet->send_time_ms;

    if (transit > 0.0)
        tn_rtt_update(
            &stream->rtt,
            transit * 2.0);

    tn_bandwidth_update(
        &stream->receive_bandwidth,
        packet->payload_size,
        arrival_ms);

    tn_loss_update(
        &stream->loss);

    tn_jitter_adapt(
        &stream->jitter_buffer,
        stream->jitter.jitter_ms);

    tn_jitter_insert(
        &stream->jitter_buffer,
        packet);

    engine->total_packets_received++;

    engine->total_bytes_received +=
        packet->payload_size;

    engine->total_packets_lost =
        stream->packets_lost;

    return true;
}

/* ================================================================
 * SEND PACKET
 * ================================================================ */

static bool tn_send_packet(
    TNNetworkEngine *engine,
    uint32_t stream_id,
    TNPacketType type,
    const uint8_t *data,
    size_t size,
    uint32_t sequence,
    uint64_t now_ms)
{
    if (size > TN_MAX_PACKET_SIZE)
        return false;

    TNNetworkStream *stream =
        tn_find_stream(
            engine,
            stream_id);

    if (!stream)
    {
        stream =
            tn_add_stream(
                engine,
                stream_id,
                type);
    }

    if (!stream)
        return false;

    TNPacket packet;

    memset(&packet, 0, sizeof(packet));

    packet.stream_id = stream_id;
    packet.sequence = sequence;
    packet.timestamp_ms = now_ms;
    packet.send_time_ms = now_ms;
    packet.payload_size = (uint16_t)size;
    packet.type = type;

    if (data && size > 0)
    {
        memcpy(packet.payload,
               data,
               size);
    }

    stream->packets_sent++;

    stream->bytes_sent += size;

    tn_bandwidth_update(
        &stream->send_bandwidth,
        size,
        now_ms);

    engine->total_packets_sent++;
    engine->total_bytes_sent += size;

    /*
     * Transport transmission would occur here.
     *
     * This prototype intentionally separates the network
     * adaptation engine from the actual socket/QUIC/RTP/etc.
     * transport.
     */

    return true;
}

/* ================================================================
 * QUALITY SCORE
 * ================================================================ */

static double tn_calculate_stream_score(
    const TNNetworkStream *s)
{
    double rtt_score =
        100.0 -
        tn_clamp(
            (s->rtt.srtt_ms / 500.0) * 100.0,
            0.0,
            100.0);

    double jitter_score =
        100.0 -
        tn_clamp(
            (s->jitter.jitter_ms / 100.0) * 100.0,
            0.0,
            100.0);

    double loss_score =
        100.0 -
        tn_clamp(
            s->loss.smoothed_loss_ratio *
            1000.0,
            0.0,
            100.0);

    /*
     * Weight loss heavily because interactive media
     * is highly sensitive to packet loss.
     */
    return
        0.35 * rtt_score +
        0.25 * jitter_score +
        0.40 * loss_score;
}

/* ================================================================
 * QUALITY CLASSIFICATION
 * ================================================================ */

static TNNetworkQuality tn_classify_quality(
    double score)
{
    if (score >= 90.0)
        return TN_QUALITY_EXCELLENT;

    if (score >= 75.0)
        return TN_QUALITY_GOOD;

    if (score >= 55.0)
        return TN_QUALITY_FAIR;

    if (score >= 30.0)
        return TN_QUALITY_POOR;

    return TN_QUALITY_CRITICAL;
}

/* ================================================================
 * NETWORK ANALYSIS
 * ================================================================ */

static void tn_update_network_metrics(
    TNNetworkEngine *engine)
{
    if (engine->stream_count == 0)
    {
        engine->network_score = 100.0;
        engine->quality =
            TN_QUALITY_EXCELLENT;
        return;
    }

    double total_score = 0.0;

    double bandwidth = 0.0;

    size_t active = 0;

    for (size_t i = 0;
         i < engine->stream_count;
         ++i)
    {
        TNNetworkStream *s =
            &engine->streams[i];

        if (s->packets_received == 0)
            continue;

        double score =
            tn_calculate_stream_score(s);

        total_score += score;

        if (s->receive_bandwidth.smoothed_kbps >
            bandwidth)
        {
            bandwidth =
                s->receive_bandwidth.smoothed_kbps;
        }

        active++;
    }

    if (active > 0)
    {
        engine->network_score =
            total_score /
            (double)active;
    }

    if (bandwidth > 0.0)
    {
        engine->estimated_bandwidth_kbps =
            0.80 *
            engine->estimated_bandwidth_kbps +
            0.20 *
            bandwidth;
    }

    engine->quality =
        tn_classify_quality(
            engine->network_score);
}

/* ================================================================
 * CONGESTION DETECTION
 * ================================================================ */

static void tn_update_congestion(
    TNNetworkEngine *engine)
{
    double loss = 0.0;
    double jitter = 0.0;
    double rtt = 0.0;

    size_t active = 0;

    for (size_t i = 0;
         i < engine->stream_count;
         ++i)
    {
        TNNetworkStream *s =
            &engine->streams[i];

        if (s->packets_received == 0)
            continue;

        loss +=
            s->loss.smoothed_loss_ratio;

        jitter +=
            s->jitter.jitter_ms;

        if (s->rtt.srtt_ms > rtt)
            rtt = s->rtt.srtt_ms;

        active++;
    }

    if (active == 0)
        return;

    loss /= active;
    jitter /= active;

    if (loss > 0.10 ||
        jitter > 100.0 ||
        rtt > 500.0)
    {
        engine->congestion_state =
            TN_CONGESTION_CONGESTED;
    }
    else if (loss > 0.05 ||
             jitter > 50.0 ||
             rtt > 300.0)
    {
        engine->congestion_state =
            TN_CONGESTION_WARNING;
    }
    else
    {
        if (engine->congestion_state ==
            TN_CONGESTION_CONGESTED)
        {
            engine->congestion_state =
                TN_CONGESTION_RECOVERY;
        }
        else
        {
            engine->congestion_state =
                TN_CONGESTION_STABLE;
        }
    }
}

/* ================================================================
 * BITRATE CONTROL
 * ================================================================ */

static double tn_calculate_target_bitrate(
    TNNetworkEngine *engine)
{
    double bandwidth =
        engine->estimated_bandwidth_kbps;

    if (bandwidth <= 0.0)
        bandwidth = 500.0;

    /*
     * Don't consume all estimated capacity.
     */
    double target =
        bandwidth * 0.75;

    switch (engine->congestion_state)
    {
        case TN_CONGESTION_STARTUP:
            target *= 0.50;
            break;

        case TN_CONGESTION_WARNING:
            target *= 0.70;
            break;

        case TN_CONGESTION_CONGESTED:
            target *= 0.45;
            break;

        case TN_CONGESTION_RECOVERY:
            target *= 0.60;
            break;

        default:
            break;
    }

    /*
     * Additional safety based on packet loss.
     */
    if (engine->quality ==
        TN_QUALITY_POOR)
    {
        target *= 0.75;
    }

    if (engine->quality ==
        TN_QUALITY_CRITICAL)
    {
        target *= 0.45;
    }

    target =
        tn_clamp(
            target,
            TN_MIN_BITRATE_KBPS,
            TN_MAX_BITRATE_KBPS);

    return target;
}

/* ================================================================
 * ADAPTIVE MEDIA DECISION
 * ================================================================ */

static TNAdaptationDecision
tn_make_adaptation_decision(
    TNNetworkEngine *engine)
{
    TNAdaptationDecision d;

    memset(&d, 0, sizeof(d));

    d.target_bitrate_kbps =
        (int)engine->target_bitrate_kbps;

    /*
     * Reserve bandwidth for audio.
     *
     * Voice should generally survive a video degradation event.
     */
    int audio_bitrate = 48;

    if (engine->quality ==
        TN_QUALITY_POOR)
    {
        audio_bitrate = 32;
    }

    if (engine->quality ==
        TN_QUALITY_CRITICAL)
    {
        audio_bitrate = 24;
    }

    audio_bitrate =
        tn_clamp_int(
            audio_bitrate,
            engine->config.minimum_audio_bitrate,
            engine->config.maximum_audio_bitrate);

    d.audio_bitrate_kbps =
        audio_bitrate;

    int video_budget =
        d.target_bitrate_kbps -
        audio_bitrate;

    if (video_budget < 0)
        video_budget = 0;

    /*
     * Find the highest video profile that fits.
     */
    const TNVideoProfile *selected =
        &TN_VIDEO_PROFILES[0];

    for (size_t i = 0;
         i < TN_VIDEO_PROFILE_COUNT;
         ++i)
    {
        const TNVideoProfile *p =
            &TN_VIDEO_PROFILES[i];

        if (p->bitrate_kbps <= video_budget)
        {
            selected = p;
        }
    }

    d.video_bitrate_kbps =
        selected->bitrate_kbps;

    d.video_width =
        selected->width;

    d.video_height =
        selected->height;

    d.video_fps =
        selected->fps;

    d.keyframe_interval =
        selected->keyframe_interval;

    d.preserve_audio = true;

    d.reduce_video =
        selected->bitrate_kbps <
        1500;

    if (!engine->config.allow_fps_scaling)
    {
        d.video_fps = 30.0;
    }

    if (!engine->config.allow_resolution_scaling)
    {
        d.video_width = 1280;
        d.video_height = 720;
    }

    return d;
}

/* ================================================================
 * ADAPTATION STEP
 * ================================================================ */

static TNAdaptationDecision
tn_network_adapt(
    TNNetworkEngine *engine,
    uint64_t now_ms)
{
    tn_update_network_metrics(
        engine);

    tn_update_congestion(
        engine);

    double desired =
        tn_calculate_target_bitrate(
            engine);

    /*
     * Add hysteresis to bitrate changes.
     *
     * Drop quickly.
     * Increase slowly.
     */
    if (desired <
        engine->target_bitrate_kbps)
    {
        engine->target_bitrate_kbps =
            0.65 *
            engine->target_bitrate_kbps +
            0.35 *
            desired;
    }
    else
    {
        engine->target_bitrate_kbps =
            0.90 *
            engine->target_bitrate_kbps +
            0.10 *
            desired;
    }

    engine->target_bitrate_kbps =
        tn_clamp(
            engine->target_bitrate_kbps,
            TN_MIN_BITRATE_KBPS,
            TN_MAX_BITRATE_KBPS);

    engine->available_bitrate_kbps =
        engine->target_bitrate_kbps;

    engine->last_adaptation_ms =
        now_ms;

    return tn_make_adaptation_decision(
        engine);
}

/* ================================================================
 * PACKET LOSS CONCEALMENT
 * ================================================================ */

typedef enum
{
    TN_PLC_NONE = 0,
    TN_PLC_REPEAT_LAST,
    TN_PLC_INTERPOLATE,
    TN_PLC_COMFORT_NOISE,
    TN_PLC_KEYFRAME_REQUEST

} TNPLCMode;

static TNPLCMode tn_select_plc(
    TNNetworkStream *stream)
{
    if (stream->type ==
        TN_PACKET_AUDIO)
    {
        if (stream->loss.smoothed_loss_ratio < 0.03)
            return TN_PLC_REPEAT_LAST;

        if (stream->loss.smoothed_loss_ratio < 0.10)
            return TN_PLC_INTERPOLATE;

        return TN_PLC_COMFORT_NOISE;
    }

    if (stream->type ==
        TN_PACKET_VIDEO)
    {
        if (stream->loss.smoothed_loss_ratio < 0.03)
            return TN_PLC_REPEAT_LAST;

        return TN_PLC_KEYFRAME_REQUEST;
    }

    return TN_PLC_NONE;
}

/* ================================================================
 * KEEPALIVE
 * ================================================================ */

static bool tn_should_keepalive(
    TNNetworkEngine *engine,
    uint64_t now_ms)
{
    return
        now_ms -
        engine->last_keepalive_ms >=
        TN_KEEPALIVE_MS;
}

static void tn_keepalive_sent(
    TNNetworkEngine *engine,
    uint64_t now_ms)
{
    engine->last_keepalive_ms =
        now_ms;
}

/* ================================================================
 * NETWORK EVENT
 * ================================================================ */

typedef enum
{
    TN_EVENT_NONE = 0,

    TN_EVENT_NETWORK_GOOD,

    TN_EVENT_NETWORK_DEGRADED,

    TN_EVENT_NETWORK_CRITICAL,

    TN_EVENT_CONGESTION,

    TN_EVENT_RECOVERY,

    TN_EVENT_PACKET_LOSS,

    TN_EVENT_HIGH_LATENCY,

    TN_EVENT_HIGH_JITTER

} TNNetworkEvent;

static TNNetworkEvent
tn_detect_event(
    TNNetworkEngine *engine)
{
    if (engine->quality ==
        TN_QUALITY_CRITICAL)
    {
        return TN_EVENT_NETWORK_CRITICAL;
    }

    if (engine->quality ==
        TN_QUALITY_POOR)
    {
        return TN_EVENT_NETWORK_DEGRADED;
    }

    if (engine->congestion_state ==
        TN_CONGESTION_CONGESTED)
    {
        return TN_EVENT_CONGESTION;
    }

    for (size_t i = 0;
         i < engine->stream_count;
         ++i)
    {
        TNNetworkStream *s =
            &engine->streams[i];

        if (s->loss.smoothed_loss_ratio >
            0.05)
        {
            return TN_EVENT_PACKET_LOSS;
        }

        if (s->rtt.srtt_ms > 300.0)
        {
            return TN_EVENT_HIGH_LATENCY;
        }

        if (s->jitter.jitter_ms > 50.0)
        {
            return TN_EVENT_HIGH_JITTER;
        }
    }

    if (engine->quality ==
        TN_QUALITY_GOOD ||
        engine->quality ==
        TN_QUALITY_EXCELLENT)
    {
        return TN_EVENT_NETWORK_GOOD;
    }

    return TN_EVENT_NONE;
}

/* ================================================================
 * NETWORK DIAGNOSTICS
 * ================================================================ */

static void tn_print_stream_stats(
    const TNNetworkStream *s)
{
    printf("\nSTREAM %u\n",
           s->stream_id);

    printf("  Type:              %d\n",
           s->type);

    printf("  RX packets:        %llu\n",
           (unsigned long long)
           s->packets_received);

    printf("  TX packets:        %llu\n",
           (unsigned long long)
           s->packets_sent);

    printf("  RX bytes:          %llu\n",
           (unsigned long long)
           s->bytes_received);

    printf("  TX bytes:          %llu\n",
           (unsigned long long)
           s->bytes_sent);

    printf("  Lost packets:      %llu\n",
           (unsigned long long)
           s->packets_lost);

    printf("  Duplicate:         %llu\n",
           (unsigned long long)
           s->packets_duplicate);

    printf("  Reordered:         %llu\n",
           (unsigned long long)
           s->packets_reordered);

    printf("  RTT:               %.2f ms\n",
           s->rtt.srtt_ms);

    printf("  RTT variance:      %.2f ms\n",
           s->rtt.rttvar_ms);

    printf("  RTO:               %.2f ms\n",
           s->rtt.rto_ms);

    printf("  Jitter:            %.2f ms\n",
           s->jitter.jitter_ms);

    printf("  Loss ratio:        %.2f %%\n",
           s->loss.loss_ratio * 100.0);

    printf("  Smoothed loss:     %.2f %%\n",
           s->loss.smoothed_loss_ratio * 100.0);

    printf("  RX bandwidth:      %.2f kbps\n",
           s->receive_bandwidth.smoothed_kbps);

    printf("  TX bandwidth:      %.2f kbps\n",
           s->send_bandwidth.smoothed_kbps);

    printf("  Jitter buffer:     %zu packets\n",
           s->jitter_buffer.count);

    printf("  Target delay:      %.2f ms\n",
           s->jitter_buffer.target_delay_ms);

    printf("  PLC mode:          %d\n",
           tn_select_plc(
               (TNNetworkStream *)s));
}

/* ================================================================
 * ENGINE DIAGNOSTICS
 * ================================================================ */

static void tn_print_engine_stats(
    const TNNetworkEngine *engine)
{
    printf("\n");
    printf("====================================================\n");
    printf(" TEAMS NATIVE NETWORK ENGINE\n");
    printf("====================================================\n");

    printf("Streams:             %zu\n",
           engine->stream_count);

    printf("Network score:       %.2f / 100\n",
           engine->network_score);

    printf("Network quality:     %s\n",
           tn_quality_name(
               engine->quality));

    printf("Congestion:          %s\n",
           tn_congestion_name(
               engine->congestion_state));

    printf("Estimated bandwidth: %.2f kbps\n",
           engine->estimated_bandwidth_kbps);

    printf("Target bitrate:      %.2f kbps\n",
           engine->target_bitrate_kbps);

    printf("Available bitrate:   %.2f kbps\n",
           engine->available_bitrate_kbps);

    printf("Packets TX:          %llu\n",
           (unsigned long long)
           engine->total_packets_sent);

    printf("Packets RX:          %llu\n",
           (unsigned long long)
           engine->total_packets_received);

    printf("Bytes TX:            %llu\n",
           (unsigned long long)
           engine->total_bytes_sent);

    printf("Bytes RX:            %llu\n",
           (unsigned long long)
           engine->total_bytes_received);

    printf("Packets lost:        %llu\n",
           (unsigned long long)
           engine->total_packets_lost);

    printf("====================================================\n");

    for (size_t i = 0;
         i < engine->stream_count;
         ++i)
    {
        tn_print_stream_stats(
            &engine->streams[i]);
    }
}

/* ================================================================
 * TEST NETWORK
 * ================================================================ */

typedef struct
{
    double packet_loss_probability;

    double duplicate_probability;

    double reorder_probability;

    double base_latency_ms;

    double jitter_ms;

    double bandwidth_kbps;

} TNNetworkSimulator;

static double tn_random_unit(void)
{
    return
        (double)rand() /
        (double)RAND_MAX;
}

static bool tn_simulate_loss(
    const TNNetworkSimulator *n)
{
    return
        tn_random_unit() <
        n->packet_loss_probability;
}

static uint64_t tn_simulate_delay(
    const TNNetworkSimulator *n)
{
    double random =
        (tn_random_unit() * 2.0) - 1.0;

    double delay =
        n->base_latency_ms +
        random * n->jitter_ms;

    if (delay < 0.0)
        delay = 0.0;

    return (uint64_t)delay;
}

/* ================================================================
 * TEST PACKET GENERATION
 * ================================================================ */

static TNPacket tn_make_test_packet(
    uint32_t stream_id,
    TNPacketType type,
    uint32_t sequence,
    size_t size,
    uint64_t now_ms)
{
    TNPacket p;

    memset(&p, 0, sizeof(p));

    p.stream_id = stream_id;
    p.sequence = sequence;

    p.timestamp_ms = now_ms;
    p.send_time_ms = now_ms;

    p.payload_size =
        (uint16_t)tn_clamp_int(
            (int)size,
            1,
            TN_MAX_PACKET_SIZE);

    p.type = type;

    for (size_t i = 0;
         i < p.payload_size;
         ++i)
    {
        p.payload[i] =
            (uint8_t)(i + sequence);
    }

    return p;
}

/* ================================================================
 * SIMULATION
 * ================================================================ */

static void tn_run_network_simulation(void)
{
    TNNetworkEngine engine;

    tn_network_init(&engine);

    TNNetworkSimulator network;

    network.packet_loss_probability = 0.015;
    network.duplicate_probability = 0.002;
    network.reorder_probability = 0.010;
    network.base_latency_ms = 35.0;
    network.jitter_ms = 10.0;
    network.bandwidth_kbps = 5000.0;

    uint64_t simulation_start =
        tn_now_ms();

    uint32_t video_sequence = 0;
    uint32_t audio_sequence = 0;

    /*
     * Simulate approximately 30 seconds.
     */
    for (int tick = 0;
         tick < 3000;
         ++tick)
    {
        uint64_t send_time =
            simulation_start +
            (uint64_t)tick * 10ULL;

        /*
         * Video packet.
         */
        TNPacket video =
            tn_make_test_packet(
                1,
                TN_PACKET_VIDEO,
                video_sequence++,
                1000,
                send_time);

        if (!tn_simulate_loss(&network))
        {
            uint64_t delay =
                tn_simulate_delay(&network);

            uint64_t arrival =
                send_time + delay;

            tn_receive_packet(
                &engine,
                &video,
                arrival);
        }

        /*
         * Audio packets at a lower payload size.
         */
        if ((tick % 20) == 0)
        {
            TNPacket audio =
                tn_make_test_packet(
                    2,
                    TN_PACKET_AUDIO,
                    audio_sequence++,
                    160,
                    send_time);

            if (!tn_simulate_loss(&network))
            {
                uint64_t delay =
                    tn_simulate_delay(&network);

                tn_receive_packet(
                    &engine,
                    &audio,
                    send_time + delay);
            }
        }

        /*
         * Every second, run adaptation.
         */
        if ((tick % 100) == 0)
        {
            uint64_t now =
                send_time;

            TNAdaptationDecision d =
                tn_network_adapt(
                    &engine,
                    now);

            printf(
                "[%5.1fs] "
                "score=%6.2f "
                "quality=%-9s "
                "congestion=%-10s "
                "bw=%7.1f kbps "
                "target=%7.1f kbps "
                "video=%dx%d@%.0f\n",

                (double)tick / 100.0,

                engine.network_score,

                tn_quality_name(
                    engine.quality),

                tn_congestion_name(
                    engine.congestion_state),

                engine.estimated_bandwidth_kbps,

                engine.target_bitrate_kbps,

                d.video_width,

                d.video_height,

                d.video_fps
            );

            TNNetworkEvent event =
                tn_detect_event(&engine);

            if (event != TN_EVENT_NONE)
            {
                printf(
                    "           EVENT: %d\n",
                    event);
            }
        }
    }

    tn_print_engine_stats(
        &engine);
}

/* ================================================================
 * CONGESTION STRESS TEST
 * ================================================================ */

static void tn_run_congestion_test(void)
{
    TNNetworkEngine engine;

    tn_network_init(&engine);

    printf("\n");
    printf("====================================================\n");
    printf(" CONGESTION STRESS TEST\n");
    printf("====================================================\n");

    uint64_t base =
        tn_now_ms();

    uint32_t sequence = 0;

    /*
     * Start healthy.
     */
    for (int i = 0; i < 500; ++i)
    {
        TNPacket p =
            tn_make_test_packet(
                10,
                TN_PACKET_VIDEO,
                sequence++,
                1200,
                base + i * 20);

        tn_receive_packet(
            &engine,
            &p,
            p.send_time_ms + 30);
    }

    TNAdaptationDecision d =
        tn_network_adapt(
            &engine,
            base + 10000);

    printf(
        "Healthy network: "
        "%s / %.1f kbps / %dx%d @ %.0f FPS\n",
        tn_quality_name(engine.quality),
        engine.target_bitrate_kbps,
        d.video_width,
        d.video_height,
        d.video_fps);

    /*
     * Introduce severe latency and packet loss.
     */
    for (int i = 0; i < 300; ++i)
    {
        if ((i % 5) == 0)
            continue;

        TNPacket p =
            tn_make_test_packet(
                10,
                TN_PACKET_VIDEO,
                sequence++,
                600,
                base + 11000 + i * 30);

        tn_receive_packet(
            &engine,
            &p,
            p.send_time_ms + 250);
    }

    d =
        tn_network_adapt(
            &engine,
            base + 30000);

    printf(
        "Congested network: "
        "%s / %.1f kbps / %dx%d @ %.0f FPS\n",
        tn_quality_name(engine.quality),
        engine.target_bitrate_kbps,
        d.video_width,
        d.video_height,
        d.video_fps);

    tn_print_engine_stats(
        &engine);
}

/* ================================================================
 * JITTER BUFFER TEST
 * ================================================================ */

static void tn_run_jitter_test(void)
{
    TNJitterBuffer jb;

    tn_jitter_buffer_init(&jb);

    printf("\n");
    printf("====================================================\n");
    printf(" JITTER BUFFER TEST\n");
    printf("====================================================\n");

    uint64_t now =
        tn_now_ms();

    /*
     * Deliberately insert packets out of order.
     */
    int order[] =
    {
        0, 1, 3, 2, 5, 4, 6, 7
    };

    for (size_t i = 0;
         i < sizeof(order) / sizeof(order[0]);
         ++i)
    {
        TNPacket p =
            tn_make_test_packet(
                100,
                TN_PACKET_AUDIO,
                (uint32_t)order[i],
                160,
                now);

        tn_jitter_insert(
            &jb,
            &p);
    }

    printf(
        "Inserted:    %llu\n",
        (unsigned long long)
        jb.packets_inserted);

    printf(
        "Buffered:    %zu\n",
        jb.count);

    TNPacket out;

    while (tn_jitter_pop(
        &jb,
        &out))
    {
        printf(
            "Released sequence %u\n",
            out.sequence);
    }

    printf(
        "Released:    %llu\n",
        (unsigned long long)
        jb.packets_released);

    printf(
        "Dropped:     %llu\n",
        (unsigned long long)
        jb.packets_dropped);

    printf(
        "Late:        %llu\n",
        (unsigned long long)
        jb.packets_late);
}

/* ================================================================
 * ADAPTIVE PROFILE TEST
 * ================================================================ */

static void tn_print_adaptation(
    TNNetworkEngine *engine,
    double bandwidth,
    double loss,
    double rtt,
    double jitter)
{
    engine->estimated_bandwidth_kbps =
        bandwidth;

    TNNetworkStream *s =
        tn_find_stream(
            engine,
            500);

    if (!s)
    {
        s =
            tn_add_stream(
                engine,
                500,
                TN_PACKET_VIDEO);
    }

    s->rtt.srtt_ms = rtt;

    s->jitter.jitter_ms = jitter;

    s->loss.smoothed_loss_ratio =
        loss;

    engine->network_score =
        tn_calculate_stream_score(s);

    engine->quality =
        tn_classify_quality(
            engine->network_score);

    tn_update_congestion(
        engine);

    TNAdaptationDecision d =
        tn_make_adaptation_decision(
            engine);

    printf(
        "BW=%5.0f kbps "
        "loss=%5.1f%% "
        "RTT=%5.0f ms "
        "jitter=%5.0f ms "
        "=> %-9s "
        "%4dx%-4d "
        "%2.0f FPS "
        "%4d kbps\n",

        bandwidth,
        loss * 100.0,
        rtt,
        jitter,

        tn_quality_name(
            engine->quality),

        d.video_width,
        d.video_height,
        d.video_fps,
        d.target_bitrate_kbps);
}

static void tn_run_adaptation_matrix(void)
{
    TNNetworkEngine engine;

    tn_network_init(&engine);

    printf("\n");
    printf("====================================================\n");
    printf(" ADAPTIVE QUALITY MATRIX\n");
    printf("====================================================\n");

    tn_print_adaptation(
        &engine,
        10000,
        0.001,
        25,
        5);

    tn_print_adaptation(
        &engine,
        5000,
        0.005,
        50,
        10);

    tn_print_adaptation(
        &engine,
        2500,
        0.020,
        100,
        25);

    tn_print_adaptation(
        &engine,
        1000,
        0.050,
        200,
        60);

    tn_print_adaptation(
        &engine,
        400,
        0.150,
        600,
        150);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void)
{
    srand(12345);

    printf("\n");
    printf("====================================================\n");
    printf(" TEAMS NATIVE C NETWORK ENGINE\n");
    printf("====================================================\n");

    tn_run_adaptation_matrix();

    tn_run_jitter_test();

    tn_run_congestion_test();

    tn_run_network_simulation();

    printf("\n");
    printf("Network engine tests complete.\n");

    return 0;
}
Compile

Linux:

gcc -std=c11 -O3 -march=native \
    -Wall -Wextra -pedantic \
    teams_network_core.c \
    -o teams_network \
    -lm

Then:

./teams_network





/*
 * teams_meeting_performance.c
 *
 * Native C Meeting Performance Manager
 *
 * C11
 *
 * Designed as the performance/control layer above:
 *
 *      Audio Core        (#1)
 *      Video Core        (#2)
 *      Network Core      (#3)
 *
 * Responsibilities:
 *
 *  - CPU monitoring
 *  - memory monitoring
 *  - GPU-load abstraction
 *  - thermal-state abstraction
 *  - frame timing
 *  - dropped-frame tracking
 *  - audio deadline tracking
 *  - audio underrun/overrun tracking
 *  - render latency
 *  - capture latency
 *  - encoding latency
 *  - network latency integration
 *  - participant workload
 *  - rolling performance statistics
 *  - performance scoring
 *  - automatic quality adaptation
 *  - degradation detection
 *  - recovery detection
 *  - meeting mode
 *  - background workload management
 *  - performance events
 *  - diagnostics
 *  - simulation / stress testing
 *
 * This is a platform-neutral core.
 *
 * A production implementation would connect the monitoring
 * functions to Windows APIs such as:
 *
 *   - QueryPerformanceCounter
 *   - GetProcessTimes
 *   - GetProcessMemoryInfo
 *   - DXGI / D3D performance counters
 *   - Windows power/thermal APIs
 *   - WASAPI
 *   - Media Foundation
 *
 * rather than relying on the simulation functions below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ================================================================
 * CONSTANTS
 * ================================================================ */

#define MP_MAX_PARTICIPANTS          64
#define MP_HISTORY_SIZE              256
#define MP_EVENT_HISTORY_SIZE        128

#define MP_AUDIO_FRAME_MS            10.0
#define MP_VIDEO_TARGET_FPS          30.0

#define MP_WARNING_CPU               75.0
#define MP_CRITICAL_CPU             90.0

#define MP_WARNING_MEMORY            75.0
#define MP_CRITICAL_MEMORY           90.0

#define MP_WARNING_GPU               80.0
#define MP_CRITICAL_GPU              95.0

#define MP_WARNING_FRAME_DROP        3.0
#define MP_CRITICAL_FRAME_DROP      10.0

#define MP_WARNING_AUDIO_DEADLINE    1.0
#define MP_CRITICAL_AUDIO_DEADLINE   5.0

#define MP_WARNING_RENDER_LATENCY   100.0
#define MP_CRITICAL_RENDER_LATENCY  250.0

#define MP_ADAPTATION_INTERVAL_MS   1000

/* ================================================================
 * UTILITY
 * ================================================================ */

static double mp_clamp(
    double x,
    double lo,
    double hi)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}

static uint64_t mp_now_ms(void)
{
    struct timespec ts;

#if defined(_WIN32)

    timespec_get(&ts, TIME_UTC);

#else

    clock_gettime(
        CLOCK_MONOTONIC,
        &ts);

#endif

    return
        (uint64_t)ts.tv_sec * 1000ULL +
        (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ================================================================
 * PERFORMANCE STATE
 * ================================================================ */

typedef enum
{
    MP_STATE_STARTING = 0,

    MP_STATE_EXCELLENT,

    MP_STATE_GOOD,

    MP_STATE_DEGRADED,

    MP_STATE_POOR,

    MP_STATE_CRITICAL

} MPPerformanceState;

static const char *
mp_state_name(
    MPPerformanceState state)
{
    switch (state)
    {
        case MP_STATE_STARTING:
            return "STARTING";

        case MP_STATE_EXCELLENT:
            return "EXCELLENT";

        case MP_STATE_GOOD:
            return "GOOD";

        case MP_STATE_DEGRADED:
            return "DEGRADED";

        case MP_STATE_POOR:
            return "POOR";

        case MP_STATE_CRITICAL:
            return "CRITICAL";

        default:
            return "UNKNOWN";
    }
}

/* ================================================================
 * MEETING MODE
 * ================================================================ */

typedef enum
{
    MP_MODE_IDLE = 0,

    MP_MODE_JOINING,

    MP_MODE_AUDIO_ONLY,

    MP_MODE_AUDIO_VIDEO,

    MP_MODE_SCREEN_SHARE,

    MP_MODE_PRESENTATION,

    MP_MODE_RECORDING,

    MP_MODE_LOW_POWER,

    MP_MODE_DEGRADED

} MPMeetingMode;

static const char *
mp_mode_name(
    MPMeetingMode mode)
{
    switch (mode)
    {
        case MP_MODE_IDLE:
            return "IDLE";

        case MP_MODE_JOINING:
            return "JOINING";

        case MP_MODE_AUDIO_ONLY:
            return "AUDIO_ONLY";

        case MP_MODE_AUDIO_VIDEO:
            return "AUDIO_VIDEO";

        case MP_MODE_SCREEN_SHARE:
            return "SCREEN_SHARE";

        case MP_MODE_PRESENTATION:
            return "PRESENTATION";

        case MP_MODE_RECORDING:
            return "RECORDING";

        case MP_MODE_LOW_POWER:
            return "LOW_POWER";

        case MP_MODE_DEGRADED:
            return "DEGRADED";

        default:
            return "UNKNOWN";
    }
}

/* ================================================================
 * THERMAL STATE
 * ================================================================ */

typedef enum
{
    MP_THERMAL_NORMAL = 0,

    MP_THERMAL_WARM,

    MP_THERMAL_HOT,

    MP_THERMAL_CRITICAL

} MPThermalState;

/* ================================================================
 * PARTICIPANT
 * ================================================================ */

typedef struct
{
    uint32_t participant_id;

    bool active;

    bool video_enabled;

    bool audio_enabled;

    bool screen_sharing;

    double decode_time_ms;

    double render_time_ms;

    uint64_t frames_received;

    uint64_t frames_rendered;

    uint64_t frames_dropped;

    uint64_t audio_packets;

    uint64_t audio_underruns;

    uint64_t audio_overruns;

    double average_video_fps;

    double average_audio_latency_ms;

} MPParticipant;

/* ================================================================
 * FRAME METRICS
 * ================================================================ */

typedef struct
{
    uint64_t captured;

    uint64_t encoded;

    uint64_t transmitted;

    uint64_t decoded;

    uint64_t rendered;

    uint64_t dropped;

    double capture_latency_ms;

    double encode_latency_ms;

    double decode_latency_ms;

    double render_latency_ms;

    double end_to_end_latency_ms;

} MPFrameMetrics;

/* ================================================================
 * AUDIO METRICS
 * ================================================================ */

typedef struct
{
    uint64_t processed_frames;

    uint64_t underruns;

    uint64_t overruns;

    uint64_t deadline_misses;

    double processing_time_ms;

    double buffer_depth_ms;

    double latency_ms;

    double deadline_ms;

} MPAudioMetrics;

/* ================================================================
 * CPU METRICS
 * ================================================================ */

typedef struct
{
    double process_cpu_percent;

    double total_cpu_percent;

    double peak_cpu_percent;

    double average_cpu_percent;

    uint64_t samples;

} MPCPUMetrics;

/* ================================================================
 * MEMORY METRICS
 * ================================================================ */

typedef struct
{
    uint64_t process_bytes;

    uint64_t working_set_bytes;

    uint64_t peak_bytes;

    double system_memory_percent;

    double process_memory_percent;

} MPMemoryMetrics;

/* ================================================================
 * GPU METRICS
 * ================================================================ */

typedef struct
{
    double gpu_percent;

    double video_engine_percent;

    double encode_engine_percent;

    double decode_engine_percent;

    uint64_t memory_bytes;

} MPGpuMetrics;

/* ================================================================
 * THERMAL METRICS
 * ================================================================ */

typedef struct
{
    double temperature_c;

    MPThermalState state;

    bool throttling;

} MPThermalMetrics;

/* ================================================================
 * NETWORK METRICS
 * ================================================================ */

typedef struct
{
    double bandwidth_kbps;

    double rtt_ms;

    double jitter_ms;

    double packet_loss_percent;

    double network_score;

} MPNetworkMetrics;

/* ================================================================
 * RENDER PIPELINE
 * ================================================================ */

typedef struct
{
    double capture_ms;

    double processing_ms;

    double encode_ms;

    double network_ms;

    double decode_ms;

    double render_ms;

    double total_ms;

} MPPipelineTiming;

/* ================================================================
 * ROLLING STATISTICS
 * ================================================================ */

typedef struct
{
    double values[MP_HISTORY_SIZE];

    size_t count;

    size_t write_index;

} MPRollingStats;

static void mp_stats_init(
    MPRollingStats *s)
{
    memset(s, 0, sizeof(*s));
}

static void mp_stats_push(
    MPRollingStats *s,
    double value)
{
    s->values[s->write_index] =
        value;

    s->write_index =
        (s->write_index + 1) %
        MP_HISTORY_SIZE;

    if (s->count <
        MP_HISTORY_SIZE)
    {
        s->count++;
    }
}

static double mp_stats_average(
    const MPRollingStats *s)
{
    if (s->count == 0)
        return 0.0;

    double total = 0.0;

    for (size_t i = 0;
         i < s->count;
         ++i)
    {
        total += s->values[i];
    }

    return
        total /
        (double)s->count;
}

static double mp_stats_max(
    const MPRollingStats *s)
{
    if (s->count == 0)
        return 0.0;

    double max = s->values[0];

    for (size_t i = 1;
         i < s->count;
         ++i)
    {
        if (s->values[i] > max)
            max = s->values[i];
    }

    return max;
}

/* ================================================================
 * PERFORMANCE EVENT
 * ================================================================ */

typedef enum
{
    MP_EVENT_NONE = 0,

    MP_EVENT_CPU_HIGH,

    MP_EVENT_CPU_CRITICAL,

    MP_EVENT_MEMORY_HIGH,

    MP_EVENT_MEMORY_CRITICAL,

    MP_EVENT_GPU_HIGH,

    MP_EVENT_GPU_CRITICAL,

    MP_EVENT_FRAME_DROPS,

    MP_EVENT_AUDIO_UNDERRUN,

    MP_EVENT_AUDIO_OVERRUN,

    MP_EVENT_AUDIO_DEADLINE,

    MP_EVENT_HIGH_LATENCY,

    MP_EVENT_THERMAL,

    MP_EVENT_THERMAL_CRITICAL,

    MP_EVENT_NETWORK_DEGRADED,

    MP_EVENT_PERFORMANCE_DEGRADED,

    MP_EVENT_RECOVERY,

    MP_EVENT_ENTER_LOW_POWER,

    MP_EVENT_EXIT_LOW_POWER

} MPPerformanceEvent;

typedef struct
{
    MPPerformanceEvent type;

    uint64_t timestamp_ms;

    double value;

    char description[128];

} MPEventRecord;

/* ================================================================
 * EVENT LOG
 * ================================================================ */

typedef struct
{
    MPEventRecord events[
        MP_EVENT_HISTORY_SIZE];

    size_t count;

    size_t write_index;

} MPEventLog;

static void mp_event_log_init(
    MPEventLog *log)
{
    memset(log, 0, sizeof(*log));
}

static void mp_log_event(
    MPEventLog *log,
    MPPerformanceEvent event,
    double value,
    const char *description)
{
    MPEventRecord *record =
        &log->events[log->write_index];

    memset(record, 0, sizeof(*record));

    record->type = event;

    record->timestamp_ms =
        mp_now_ms();

    record->value = value;

    if (description)
    {
        snprintf(
            record->description,
            sizeof(record->description),
            "%s",
            description);
    }

    log->write_index =
        (log->write_index + 1) %
        MP_EVENT_HISTORY_SIZE;

    if (log->count <
        MP_EVENT_HISTORY_SIZE)
    {
        log->count++;
    }
}

/* ================================================================
 * QUALITY DECISION
 * ================================================================ */

typedef struct
{
    bool reduce_video_resolution;

    bool reduce_video_fps;

    bool reduce_video_bitrate;

    bool reduce_participant_count;

    bool disable_background_blur;

    bool disable_video_effects;

    bool reduce_screen_share_quality;

    bool prioritize_audio;

    bool reduce_render_rate;

    bool enter_low_power;

    int target_video_width;

    int target_video_height;

    double target_video_fps;

    int target_video_bitrate_kbps;

} MPQualityDecision;

/* ================================================================
 * PERFORMANCE ENGINE
 * ================================================================ */

typedef struct
{
    MPMeetingMode mode;

    MPPerformanceState state;

    MPThermalMetrics thermal;

    MPCPUMetrics cpu;

    MPMemoryMetrics memory;

    MPGpuMetrics gpu;

    MPNetworkMetrics network;

    MPFrameMetrics frames;

    MPAudioMetrics audio;

    MPPipelineTiming pipeline;

    MPParticipant participants[
        MP_MAX_PARTICIPANTS];

    size_t participant_count;

    MPRollingStats cpu_history;

    MPRollingStats gpu_history;

    MPRollingStats latency_history;

    MPRollingStats frame_drop_history;

    MPRollingStats audio_processing_history;

    MPEventLog event_log;

    double performance_score;

    double previous_score;

    uint64_t last_adaptation_ms;

    uint64_t total_runtime_ms;

    uint64_t adaptation_count;

    uint64_t recovery_count;

} MPPerformanceEngine;

/* ================================================================
 * INIT
 * ================================================================ */

static void mp_engine_init(
    MPPerformanceEngine *engine)
{
    memset(engine, 0, sizeof(*engine));

    engine->mode =
        MP_MODE_IDLE;

    engine->state =
        MP_STATE_STARTING;

    mp_stats_init(
        &engine->cpu_history);

    mp_stats_init(
        &engine->gpu_history);

    mp_stats_init(
        &engine->latency_history);

    mp_stats_init(
        &engine->frame_drop_history);

    mp_stats_init(
        &engine->audio_processing_history);

    mp_event_log_init(
        &engine->event_log);

    engine->last_adaptation_ms =
        mp_now_ms();
}

/* ================================================================
 * PARTICIPANT MANAGEMENT
 * ================================================================ */

static MPParticipant *
mp_add_participant(
    MPPerformanceEngine *engine,
    uint32_t id)
{
    if (engine->participant_count >=
        MP_MAX_PARTICIPANTS)
    {
        return NULL;
    }

    MPParticipant *p =
        &engine->participants[
            engine->participant_count++];

    memset(p, 0, sizeof(*p));

    p->participant_id = id;

    p->active = true;

    p->video_enabled = true;

    p->audio_enabled = true;

    return p;
}

static MPParticipant *
mp_find_participant(
    MPPerformanceEngine *engine,
    uint32_t id)
{
    for (size_t i = 0;
         i < engine->participant_count;
         ++i)
    {
        if (engine->participants[i]
                .participant_id == id)
        {
            return
                &engine->participants[i];
        }
    }

    return NULL;
}

static void mp_remove_participant(
    MPPerformanceEngine *engine,
    uint32_t id)
{
    MPParticipant *p =
        mp_find_participant(
            engine,
            id);

    if (!p)
        return;

    p->active = false;
}

/* ================================================================
 * MEETING MODE
 * ================================================================ */

static void mp_set_mode(
    MPPerformanceEngine *engine,
    MPMeetingMode mode)
{
    if (engine->mode == mode)
        return;

    engine->mode = mode;

    printf(
        "[MEETING] mode -> %s\n",
        mp_mode_name(mode));
}

/* ================================================================
 * CPU UPDATE
 * ================================================================ */

static void mp_update_cpu(
    MPPerformanceEngine *engine,
    double process_cpu,
    double total_cpu)
{
    engine->cpu.process_cpu_percent =
        mp_clamp(
            process_cpu,
            0.0,
            100.0);

    engine->cpu.total_cpu_percent =
        mp_clamp(
            total_cpu,
            0.0,
            100.0);

    if (process_cpu >
        engine->cpu.peak_cpu_percent)
    {
        engine->cpu.peak_cpu_percent =
            process_cpu;
    }

    engine->cpu.samples++;

    engine->cpu.average_cpu_percent =
        ((engine->cpu.average_cpu_percent *
          (double)(engine->cpu.samples - 1)) +
         process_cpu) /
        (double)engine->cpu.samples;

    mp_stats_push(
        &engine->cpu_history,
        process_cpu);
}

/* ================================================================
 * MEMORY UPDATE
 * ================================================================ */

static void mp_update_memory(
    MPPerformanceEngine *engine,
    uint64_t working_set,
    double system_percent,
    double process_percent)
{
    engine->memory.working_set_bytes =
        working_set;

    if (working_set >
        engine->memory.peak_bytes)
    {
        engine->memory.peak_bytes =
            working_set;
    }

    engine->memory.system_memory_percent =
        mp_clamp(
            system_percent,
            0.0,
            100.0);

    engine->memory.process_memory_percent =
        mp_clamp(
            process_percent,
            0.0,
            100.0);
}

/* ================================================================
 * GPU UPDATE
 * ================================================================ */

static void mp_update_gpu(
    MPPerformanceEngine *engine,
    double gpu,
    double video,
    double encode,
    double decode)
{
    engine->gpu.gpu_percent =
        mp_clamp(
            gpu,
            0.0,
            100.0);

    engine->gpu.video_engine_percent =
        mp_clamp(
            video,
            0.0,
            100.0);

    engine->gpu.encode_engine_percent =
        mp_clamp(
            encode,
            0.0,
            100.0);

    engine->gpu.decode_engine_percent =
        mp_clamp(
            decode,
            0.0,
            100.0);

    mp_stats_push(
        &engine->gpu_history,
        gpu);
}

/* ================================================================
 * THERMAL UPDATE
 * ================================================================ */

static void mp_update_thermal(
    MPPerformanceEngine *engine,
    double temperature,
    MPThermalState state,
    bool throttling)
{
    engine->thermal.temperature_c =
        temperature;

    engine->thermal.state =
        state;

    engine->thermal.throttling =
        throttling;
}

/* ================================================================
 * NETWORK UPDATE
 * ================================================================ */

static void mp_update_network(
    MPPerformanceEngine *engine,
    double bandwidth,
    double rtt,
    double jitter,
    double loss,
    double score)
{
    engine->network.bandwidth_kbps =
        bandwidth;

    engine->network.rtt_ms =
        rtt;

    engine->network.jitter_ms =
        jitter;

    engine->network.packet_loss_percent =
        loss;

    engine->network.network_score =
        score;
}

/* ================================================================
 * FRAME CAPTURE
 * ================================================================ */

static void mp_frame_captured(
    MPPerformanceEngine *engine,
    double capture_latency)
{
    engine->frames.captured++;

    engine->frames.capture_latency_ms =
        capture_latency;
}

/* ================================================================
 * FRAME ENCODED
 * ================================================================ */

static void mp_frame_encoded(
    MPPerformanceEngine *engine,
    double encode_latency)
{
    engine->frames.encoded++;

    engine->frames.encode_latency_ms =
        encode_latency;
}

/* ================================================================
 * FRAME DECODED
 * ================================================================ */

static void mp_frame_decoded(
    MPPerformanceEngine *engine,
    double decode_latency)
{
    engine->frames.decoded++;

    engine->frames.decode_latency_ms =
        decode_latency;
}

/* ================================================================
 * FRAME RENDERED
 * ================================================================ */

static void mp_frame_rendered(
    MPPerformanceEngine *engine,
    double render_latency)
{
    engine->frames.rendered++;

    engine->frames.render_latency_ms =
        render_latency;

    engine->pipeline.render_ms =
        render_latency;

    engine->pipeline.total_ms =
        engine->pipeline.capture_ms +
        engine->pipeline.processing_ms +
        engine->pipeline.encode_ms +
        engine->pipeline.network_ms +
        engine->pipeline.decode_ms +
        engine->pipeline.render_ms;

    engine->frames.end_to_end_latency_ms =
        engine->pipeline.total_ms;

    mp_stats_push(
        &engine->latency_history,
        engine->pipeline.total_ms);
}

/* ================================================================
 * FRAME DROPPED
 * ================================================================ */

static void mp_frame_dropped(
    MPPerformanceEngine *engine)
{
    engine->frames.dropped++;
}

/* ================================================================
 * FRAME DROP RATE
 * ================================================================ */

static double mp_frame_drop_rate(
    const MPPerformanceEngine *engine)
{
    uint64_t total =
        engine->frames.rendered +
        engine->frames.dropped;

    if (total == 0)
        return 0.0;

    return
        ((double)engine->frames.dropped /
         (double)total) *
        100.0;
}

/* ================================================================
 * AUDIO PROCESSING
 * ================================================================ */

static void mp_audio_processed(
    MPPerformanceEngine *engine,
    double processing_ms,
    double deadline_ms)
{
    engine->audio.processed_frames++;

    engine->audio.processing_time_ms =
        processing_ms;

    engine->audio.deadline_ms =
        deadline_ms;

    mp_stats_push(
        &engine->audio_processing_history,
        processing_ms);

    if (processing_ms >
        deadline_ms)
    {
        engine->audio.deadline_misses++;

        mp_log_event(
            &engine->event_log,
            MP_EVENT_AUDIO_DEADLINE,
            processing_ms,
            "Audio processing exceeded deadline");
    }
}

/* ================================================================
 * AUDIO UNDERRUN
 * ================================================================ */

static void mp_audio_underrun(
    MPPerformanceEngine *engine)
{
    engine->audio.underruns++;

    mp_log_event(
        &engine->event_log,
        MP_EVENT_AUDIO_UNDERRUN,
        (double)engine->audio.underruns,
        "Audio output underrun");
}

/* ================================================================
 * AUDIO OVERRUN
 * ================================================================ */

static void mp_audio_overrun(
    MPPerformanceEngine *engine)
{
    engine->audio.overruns++;

    mp_log_event(
        &engine->event_log,
        MP_EVENT_AUDIO_OVERRUN,
        (double)engine->audio.overruns,
        "Audio input overrun");
}

/* ================================================================
 * PERFORMANCE SCORE
 * ================================================================ */

static double mp_calculate_score(
    const MPPerformanceEngine *e)
{
    /*
     * CPU
     */
    double cpu_score =
        100.0 -
        e->cpu.process_cpu_percent;

    /*
     * GPU
     */
    double gpu_score =
        100.0 -
        e->gpu.gpu_percent;

    /*
     * Memory
     */
    double memory_score =
        100.0 -
        e->memory.system_memory_percent;

    /*
     * Frame drops.
     */
    double drop_rate =
        mp_frame_drop_rate(e);

    double frame_score =
        100.0 -
        mp_clamp(
            drop_rate * 10.0,
            0.0,
            100.0);

    /*
     * Latency.
     */
    double latency =
        e->frames.end_to_end_latency_ms;

    double latency_score =
        100.0 -
        mp_clamp(
            latency / 5.0,
            0.0,
            100.0);

    /*
     * Audio deadline misses.
     */
    double audio_score = 100.0;

    if (e->audio.processed_frames > 0)
    {
        double miss_rate =
            ((double)e->audio.deadline_misses /
             (double)e->audio.processed_frames)
            * 100.0;

        audio_score =
            100.0 -
            mp_clamp(
                miss_rate * 20.0,
                0.0,
                100.0);
    }

    /*
     * Network.
     */
    double network_score =
        mp_clamp(
            e->network.network_score,
            0.0,
            100.0);

    /*
     * Thermal.
     */
    double thermal_score = 100.0;

    switch (e->thermal.state)
    {
        case MP_THERMAL_WARM:
            thermal_score = 85.0;
            break;

        case MP_THERMAL_HOT:
            thermal_score = 55.0;
            break;

        case MP_THERMAL_CRITICAL:
            thermal_score = 20.0;
            break;

        default:
            break;
    }

    /*
     * Audio gets a large weight because audio quality
     * is more important than visual quality in a meeting.
     */
    double score =
        0.15 * cpu_score +
        0.10 * gpu_score +
        0.10 * memory_score +
        0.15 * frame_score +
        0.10 * latency_score +
        0.15 * audio_score +
        0.20 * network_score +
        0.05 * thermal_score;

    return
        mp_clamp(score, 0.0, 100.0);
}

/* ================================================================
 * PERFORMANCE STATE
 * ================================================================ */

static MPPerformanceState
mp_classify_score(double score)
{
    if (score >= 90.0)
        return MP_STATE_EXCELLENT;

    if (score >= 75.0)
        return MP_STATE_GOOD;

    if (score >= 55.0)
        return MP_STATE_DEGRADED;

    if (score >= 30.0)
        return MP_STATE_POOR;

    return MP_STATE_CRITICAL;
}

/* ================================================================
 * PERFORMANCE ANALYSIS
 * ================================================================ */

static void mp_analyse(
    MPPerformanceEngine *engine)
{
    engine->previous_score =
        engine->performance_score;

    engine->performance_score =
        mp_calculate_score(engine);

    MPPerformanceState previous =
        engine->state;

    engine->state =
        mp_classify_score(
            engine->performance_score);

    if (previous != engine->state)
    {
        if (engine->state ==
            MP_STATE_DEGRADED)
        {
            mp_log_event(
                &engine->event_log,
                MP_EVENT_PERFORMANCE_DEGRADED,
                engine->performance_score,
                "Meeting performance degraded");
        }

        if (engine->state ==
            MP_STATE_EXCELLENT ||
            engine->state ==
            MP_STATE_GOOD)
        {
            if (previous ==
                MP_STATE_POOR ||
                previous ==
                MP_STATE_CRITICAL ||
                previous ==
                MP_STATE_DEGRADED)
            {
                engine->recovery_count++;

                mp_log_event(
                    &engine->event_log,
                    MP_EVENT_RECOVERY,
                    engine->performance_score,
                    "Meeting performance recovered");
            }
        }
    }
}

/* ================================================================
 * BOTTLENECK DETECTION
 * ================================================================ */

typedef enum
{
    MP_BOTTLENECK_NONE = 0,

    MP_BOTTLENECK_CPU,

    MP_BOTTLENECK_GPU,

    MP_BOTTLENECK_MEMORY,

    MP_BOTTLENECK_NETWORK,

    MP_BOTTLENECK_AUDIO,

    MP_BOTTLENECK_RENDER,

    MP_BOTTLENECK_THERMAL

} MPBottleneck;

static MPBottleneck mp_find_bottleneck(
    const MPPerformanceEngine *e)
{
    if (e->thermal.state ==
        MP_THERMAL_CRITICAL)
    {
        return MP_BOTTLENECK_THERMAL;
    }

    if (e->cpu.process_cpu_percent >
        MP_CRITICAL_CPU)
    {
        return MP_BOTTLENECK_CPU;
    }

    if (e->gpu.gpu_percent >
        MP_CRITICAL_GPU)
    {
        return MP_BOTTLENECK_GPU;
    }

    if (e->memory.system_memory_percent >
        MP_CRITICAL_MEMORY)
    {
        return MP_BOTTLENECK_MEMORY;
    }

    if (e->network.network_score < 40.0)
    {
        return MP_BOTTLENECK_NETWORK;
    }

    if (e->audio.deadline_misses > 0)
    {
        return MP_BOTTLENECK_AUDIO;
    }

    if (e->frames.render_latency_ms >
        MP_CRITICAL_RENDER_LATENCY)
    {
        return MP_BOTTLENECK_RENDER;
    }

    return MP_BOTTLENECK_NONE;
}

/* ================================================================
 * QUALITY DECISION
 * ================================================================ */

static MPQualityDecision
mp_make_quality_decision(
    MPPerformanceEngine *e)
{
    MPQualityDecision d;

    memset(&d, 0, sizeof(d));

    /*
     * Start with a strong HD baseline.
     */
    d.target_video_width = 1280;
    d.target_video_height = 720;
    d.target_video_fps = 30.0;
    d.target_video_bitrate_kbps = 1500;

    d.prioritize_audio = true;

    MPBottleneck bottleneck =
        mp_find_bottleneck(e);

    /*
     * CPU pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_CPU)
    {
        d.reduce_video_fps = true;
        d.disable_video_effects = true;
        d.disable_background_blur = true;

        d.target_video_fps = 15.0;
    }

    /*
     * GPU pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_GPU)
    {
        d.disable_video_effects = true;
        d.disable_background_blur = true;
        d.reduce_video_resolution = true;

        d.target_video_width = 640;
        d.target_video_height = 360;
    }

    /*
     * Memory pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_MEMORY)
    {
        d.reduce_participant_count = true;
        d.disable_background_blur = true;
        d.disable_video_effects = true;
    }

    /*
     * Network pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_NETWORK)
    {
        d.reduce_video_bitrate = true;
        d.reduce_video_resolution = true;
        d.reduce_video_fps = true;

        d.target_video_width = 640;
        d.target_video_height = 360;

        d.target_video_fps = 15.0;

        d.target_video_bitrate_kbps = 400;
    }

    /*
     * Audio pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_AUDIO)
    {
        d.prioritize_audio = true;

        d.reduce_video_fps = true;
        d.target_video_fps = 15.0;
    }

    /*
     * Rendering pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_RENDER)
    {
        d.reduce_render_rate = true;
        d.reduce_video_resolution = true;

        d.target_video_width = 640;
        d.target_video_height = 360;
    }

    /*
     * Thermal pressure.
     */
    if (bottleneck ==
        MP_BOTTLENECK_THERMAL)
    {
        d.reduce_video_fps = true;
        d.reduce_video_resolution = true;
        d.disable_video_effects = true;
        d.disable_background_blur = true;

        d.target_video_width = 640;
        d.target_video_height = 360;

        d.target_video_fps = 15.0;

        d.enter_low_power = true;
    }

    /*
     * Globally poor performance.
     */
    if (e->state ==
        MP_STATE_CRITICAL)
    {
        d.reduce_video_fps = true;
        d.reduce_video_resolution = true;
        d.reduce_video_bitrate = true;

        d.target_video_width = 320;
        d.target_video_height = 180;

        d.target_video_fps = 10.0;

        d.target_video_bitrate_kbps = 150;

        d.disable_background_blur = true;
        d.disable_video_effects = true;

        d.prioritize_audio = true;
    }

    return d;
}

/* ================================================================
 * APPLY QUALITY DECISION
 * ================================================================ */

static void mp_apply_quality_decision(
    MPPerformanceEngine *engine,
    const MPQualityDecision *d)
{
    printf("\n");
    printf("[ADAPTATION]\n");

    printf(
        "  Video:             %dx%d\n",
        d->target_video_width,
        d->target_video_height);

    printf(
        "  FPS:               %.1f\n",
        d->target_video_fps);

    printf(
        "  Bitrate:           %d kbps\n",
        d->target_video_bitrate_kbps);

    printf(
        "  Reduce resolution: %s\n",
        d->reduce_video_resolution
            ? "YES" : "NO");

    printf(
        "  Reduce FPS:        %s\n",
        d->reduce_video_fps
            ? "YES" : "NO");

    printf(
        "  Disable effects:   %s\n",
        d->disable_video_effects
            ? "YES" : "NO");

    printf(
        "  Disable blur:      %s\n",
        d->disable_background_blur
            ? "YES" : "NO");

    printf(
        "  Prioritize audio:  %s\n",
        d->prioritize_audio
            ? "YES" : "NO");

    printf(
        "  Low power:         %s\n",
        d->enter_low_power
            ? "YES" : "NO");

    if (d->enter_low_power)
    {
        if (engine->mode !=
            MP_MODE_LOW_POWER)
        {
            mp_set_mode(
                engine,
                MP_MODE_LOW_POWER);

            mp_log_event(
                &engine->event_log,
                MP_EVENT_ENTER_LOW_POWER,
                engine->performance_score,
                "Entering low-power meeting mode");
        }
    }
}

/* ================================================================
 * ADAPTATION LOOP
 * ================================================================ */

static void mp_adapt(
    MPPerformanceEngine *engine,
    uint64_t now_ms)
{
    if (now_ms -
        engine->last_adaptation_ms <
        MP_ADAPTATION_INTERVAL_MS)
    {
        return;
    }

    mp_analyse(engine);

    MPQualityDecision decision =
        mp_make_quality_decision(engine);

    mp_apply_quality_decision(
        engine,
        &decision);

    engine->last_adaptation_ms =
        now_ms;

    engine->adaptation_count++;
}

/* ================================================================
 * DIAGNOSTIC REPORT
 * ================================================================ */

static void mp_print_report(
    const MPPerformanceEngine *e)
{
    printf("\n");
    printf("====================================================\n");
    printf(" MEETING PERFORMANCE MANAGER\n");
    printf("====================================================\n");

    printf(
        "Meeting mode:             %s\n",
        mp_mode_name(e->mode));

    printf(
        "Performance state:       %s\n",
        mp_state_name(e->state));

    printf(
        "Performance score:       %.2f / 100\n",
        e->performance_score);

    printf("\nSYSTEM\n");

    printf(
        "CPU:                     %.1f %%\n",
        e->cpu.process_cpu_percent);

    printf(
        "CPU average:             %.1f %%\n",
        e->cpu.average_cpu_percent);

    printf(
        "CPU peak:                %.1f %%\n",
        e->cpu.peak_cpu_percent);

    printf(
        "GPU:                     %.1f %%\n",
        e->gpu.gpu_percent);

    printf(
        "Video engine:            %.1f %%\n",
        e->gpu.video_engine_percent);

    printf(
        "Encode engine:           %.1f %%\n",
        e->gpu.encode_engine_percent);

    printf(
        "Decode engine:           %.1f %%\n",
        e->gpu.decode_engine_percent);

    printf(
        "System memory:           %.1f %%\n",
        e->memory.system_memory_percent);

    printf(
        "Working set:             %.2f MB\n",
        (double)e->memory.working_set_bytes /
        (1024.0 * 1024.0));

    printf(
        "Temperature:             %.1f C\n",
        e->thermal.temperature_c);

    printf(
        "Thermal state:           %d\n",
        e->thermal.state);

    printf(
        "Thermal throttling:      %s\n",
        e->thermal.throttling
            ? "YES" : "NO");

    printf("\nNETWORK\n");

    printf(
        "Bandwidth:               %.1f kbps\n",
        e->network.bandwidth_kbps);

    printf(
        "RTT:                     %.1f ms\n",
        e->network.rtt_ms);

    printf(
        "Jitter:                  %.1f ms\n",
        e->network.jitter_ms);

    printf(
        "Packet loss:             %.2f %%\n",
        e->network.packet_loss_percent);

    printf(
        "Network score:           %.1f\n",
        e->network.network_score);

    printf("\nVIDEO\n");

    printf(
        "Captured:                %llu\n",
        (unsigned long long)
        e->frames.captured);

    printf(
        "Encoded:                 %llu\n",
        (unsigned long long)
        e->frames.encoded);

    printf(
        "Decoded:                 %llu\n",
        (unsigned long long)
        e->frames.decoded);

    printf(
        "Rendered:                %llu\n",
        (unsigned long long)
        e->frames.rendered);

    printf(
        "Dropped:                 %llu\n",
        (unsigned long long)
        e->frames.dropped);

    printf(
        "Drop rate:               %.2f %%\n",
        mp_frame_drop_rate(e));

    printf(
        "End-to-end latency:      %.2f ms\n",
        e->frames.end_to_end_latency_ms);

    printf("\nAUDIO\n");

    printf(
        "Processed frames:       %llu\n",
        (unsigned long long)
        e->audio.processed_frames);

    printf(
        "Underruns:               %llu\n",
        (unsigned long long)
        e->audio.underruns);

    printf(
        "Overruns:                %llu\n",
        (unsigned long long)
        e->audio.overruns);

    printf(
        "Deadline misses:        %llu\n",
        (unsigned long long)
        e->audio.deadline_misses);

    printf(
        "Processing time:         %.2f ms\n",
        e->audio.processing_time_ms);

    printf(
        "Deadline:                %.2f ms\n",
        e->audio.deadline_ms);

    printf("\nPARTICIPANTS\n");

    printf(
        "Participant slots:       %zu\n",
        e->participant_count);

    for (size_t i = 0;
         i < e->participant_count;
         ++i)
    {
        const MPParticipant *p =
            &e->participants[i];

        printf(
            "  [%u] active=%s "
            "video=%s "
            "frames=%llu "
            "dropped=%llu "
            "decode=%.2fms "
            "render=%.2fms\n",

            p->participant_id,

            p->active ? "yes" : "no",

            p->video_enabled
                ? "yes" : "no",

            (unsigned long long)
            p->frames_received,

            (unsigned long long)
            p->frames_dropped,

            p->decode_time_ms,

            p->render_time_ms);
    }

    printf("\nENGINE\n");

    printf(
        "Adaptation cycles:       %llu\n",
        (unsigned long long)
        e->adaptation_count);

    printf(
        "Recoveries:              %llu\n",
        (unsigned long long)
        e->recovery_count);

    printf(
        "Average CPU:             %.2f %%\n",
        mp_stats_average(
            &e->cpu_history));

    printf(
        "Average GPU:             %.2f %%\n",
        mp_stats_average(
            &e->gpu_history));

    printf(
        "Average latency:         %.2f ms\n",
        mp_stats_average(
            &e->latency_history));

    printf(
        "Peak latency:            %.2f ms\n",
        mp_stats_max(
            &e->latency_history));

    printf("====================================================\n");
}

/* ================================================================
 * EVENT REPORT
 * ================================================================ */

static void mp_print_events(
    const MPEventLog *log)
{
    printf("\n");
    printf("====================================================\n");
    printf(" PERFORMANCE EVENTS\n");
    printf("====================================================\n");

    size_t start =
        log->count < MP_EVENT_HISTORY_SIZE
            ? 0
            : log->write_index;

    for (size_t i = 0;
         i < log->count;
         ++i)
    {
        size_t index =
            (start + i) %
            MP_EVENT_HISTORY_SIZE;

        const MPEventRecord *event =
            &log->events[index];

        printf(
            "[%llu] event=%d value=%.2f %s\n",

            (unsigned long long)
            event->timestamp_ms,

            event->type,

            event->value,

            event->description);
    }
}

/* ================================================================
 * BOTTLENECK REPORT
 * ================================================================ */

static const char *
mp_bottleneck_name(
    MPBottleneck b)
{
    switch (b)
    {
        case MP_BOTTLENECK_CPU:
            return "CPU";

        case MP_BOTTLENECK_GPU:
            return "GPU";

        case MP_BOTTLENECK_MEMORY:
            return "MEMORY";

        case MP_BOTTLENECK_NETWORK:
            return "NETWORK";

        case MP_BOTTLENECK_AUDIO:
            return "AUDIO";

        case MP_BOTTLENECK_RENDER:
            return "RENDER";

        case MP_BOTTLENECK_THERMAL:
            return "THERMAL";

        default:
            return "NONE";
    }
}

/* ================================================================
 * SIMULATION
 * ================================================================ */

static void mp_run_simulation(void)
{
    MPPerformanceEngine engine;

    mp_engine_init(&engine);

    mp_set_mode(
        &engine,
        MP_MODE_AUDIO_VIDEO);

    /*
     * Add meeting participants.
     */
    for (uint32_t i = 1;
         i <= 12;
         ++i)
    {
        mp_add_participant(
            &engine,
            i);
    }

    printf("\n");
    printf("====================================================\n");
    printf(" PERFORMANCE SIMULATION\n");
    printf("====================================================\n");

    uint64_t start =
        mp_now_ms();

    /*
     * 60 simulation seconds.
     */
    for (int second = 0;
         second < 60;
         ++second)
    {
        double cpu;
        double gpu;
        double memory;
        double network;
        double rtt;
        double jitter;
        double loss;
        double temperature;

        /*
         * Different workload phases.
         */

        if (second < 10)
        {
            /*
             * Excellent machine/network.
             */
            cpu = 35.0;
            gpu = 40.0;
            memory = 45.0;

            network = 98.0;
            rtt = 25.0;
            jitter = 4.0;
            loss = 0.2;

            temperature = 55.0;
        }
        else if (second < 20)
        {
            /*
             * CPU-heavy meeting.
             */
            cpu = 82.0;
            gpu = 70.0;
            memory = 60.0;

            network = 95.0;
            rtt = 30.0;
            jitter = 6.0;
            loss = 0.5;

            temperature = 70.0;
        }
        else if (second < 30)
        {
            /*
             * GPU saturation.
             */
            cpu = 75.0;
            gpu = 94.0;
            memory = 70.0;

            network = 92.0;
            rtt = 35.0;
            jitter = 8.0;
            loss = 0.8;

            temperature = 78.0;
        }
        else if (second < 40)
        {
            /*
             * Network collapse.
             */
            cpu = 65.0;
            gpu = 75.0;
            memory = 70.0;

            network = 35.0;
            rtt = 350.0;
            jitter = 90.0;
            loss = 8.0;

            temperature = 75.0;
        }
        else if (second < 50)
        {
            /*
             * Thermal pressure.
             */
            cpu = 88.0;
            gpu = 90.0;
            memory = 78.0;

            network = 75.0;
            rtt = 80.0;
            jitter = 20.0;
            loss = 2.0;

            temperature = 96.0;
        }
        else
        {
            /*
             * Recovery.
             */
            cpu = 42.0;
            gpu = 45.0;
            memory = 50.0;

            network = 96.0;
            rtt = 30.0;
            jitter = 5.0;
            loss = 0.3;

            temperature = 58.0;
        }

        MPThermalState thermal_state;

        if (temperature >= 90.0)
            thermal_state =
                MP_THERMAL_CRITICAL;
        else if (temperature >= 75.0)
            thermal_state =
                MP_THERMAL_HOT;
        else if (temperature >= 65.0)
            thermal_state =
                MP_THERMAL_WARM;
        else
            thermal_state =
                MP_THERMAL_NORMAL;

        bool throttling =
            temperature >= 90.0;

        mp_update_cpu(
            &engine,
            cpu,
            cpu + 5.0);

        mp_update_gpu(
            &engine,
            gpu,
            gpu * 0.80,
            gpu * 0.60,
            gpu * 0.70);

        mp_update_memory(
            &engine,
            (uint64_t)
            (memory * 10000000.0),
            memory,
            memory * 0.55);

        mp_update_thermal(
            &engine,
            temperature,
            thermal_state,
            throttling);

        mp_update_network(
            &engine,
            5000.0 * (network / 100.0),
            rtt,
            jitter,
            loss,
            network);

        /*
         * Simulated video pipeline.
         */
        engine.pipeline.capture_ms =
            5.0 + cpu * 0.03;

        engine.pipeline.processing_ms =
            4.0 + cpu * 0.04;

        engine.pipeline.encode_ms =
            8.0 + gpu * 0.05;

        engine.pipeline.network_ms =
            rtt;

        engine.pipeline.decode_ms =
            5.0 + gpu * 0.04;

        engine.pipeline.render_ms =
            8.0 + gpu * 0.08;

        double total_latency =
            engine.pipeline.capture_ms +
            engine.pipeline.processing_ms +
            engine.pipeline.encode_ms +
            engine.pipeline.network_ms +
            engine.pipeline.decode_ms +
            engine.pipeline.render_ms;

        /*
         * Generate frames.
         */
        int frames_this_second = 30;

        for (int frame = 0;
             frame < frames_this_second;
             ++frame)
        {
            mp_frame_captured(
                &engine,
                engine.pipeline.capture_ms);

            mp_frame_encoded(
                &engine,
                engine.pipeline.encode_ms);

            mp_frame_decoded(
                &engine,
                engine.pipeline.decode_ms);

            /*
             * Drop frames under severe pressure.
             */
            double drop_probability = 0.0;

            if (cpu > 85.0)
                drop_probability += 0.08;

            if (gpu > 90.0)
                drop_probability += 0.12;

            if (network < 50.0)
                drop_probability += 0.10;

            if (temperature > 90.0)
                drop_probability += 0.15;

            double random =
                (double)rand() /
                (double)RAND_MAX;

            if (random < drop_probability)
            {
                mp_frame_dropped(
                    &engine);
            }
            else
            {
                mp_frame_rendered(
                    &engine,
                    engine.pipeline.render_ms);
            }
        }

        /*
         * Simulated audio processing.
         */
        for (int audio_frame = 0;
             audio_frame < 100;
             ++audio_frame)
        {
            double processing =
                1.5 +
                cpu * 0.015;

            mp_audio_processed(
                &engine,
                processing,
                MP_AUDIO_FRAME_MS);

            if (processing >
                MP_AUDIO_FRAME_MS)
            {
                if ((rand() % 5) == 0)
                    mp_audio_underrun(
                        &engine);
            }
        }

        /*
         * Participant metrics.
         */
        for (size_t i = 0;
             i < engine.participant_count;
             ++i)
        {
            MPParticipant *p =
                &engine.participants[i];

            if (!p->active)
                continue;

            p->frames_received +=
                frames_this_second;

            p->frames_rendered +=
                (uint64_t)
                ((double)frames_this_second *
                 (1.0 -
                  mp_frame_drop_rate(
                      &engine) / 100.0));

            p->frames_dropped +=
                (uint64_t)
                ((double)frames_this_second *
                 mp_frame_drop_rate(
                     &engine) / 100.0);

            p->decode_time_ms =
                engine.pipeline.decode_ms;

            p->render_time_ms =
                engine.pipeline.render_ms;
        }

        /*
         * Analyze.
         */
        mp_adapt(
            &engine,
            start +
            (uint64_t)second *
            1000ULL);

        printf(
            "[%02d s] "
            "score=%6.1f "
            "state=%-10s "
            "CPU=%5.1f "
            "GPU=%5.1f "
            "net=%5.1f "
            "lat=%6.1fms "
            "drop=%5.1f%% "
            "thermal=%5.1fC "
            "bottleneck=%-8s\n",

            second,

            engine.performance_score,

            mp_state_name(
                engine.state),

            cpu,

            gpu,

            network,

            total_latency,

            mp_frame_drop_rate(
                &engine),

            temperature,

            mp_bottleneck_name(
                mp_find_bottleneck(
                    &engine)));
    }

    mp_print_report(
        &engine);

    mp_print_events(
        &engine);
}

/* ================================================================
 * STRESS TEST
 * ================================================================ */

static void mp_run_stress_test(void)
{
    MPPerformanceEngine engine;

    mp_engine_init(&engine);

    mp_set_mode(
        &engine,
        MP_MODE_SCREEN_SHARE);

    /*
     * 64 participants.
     */
    for (uint32_t i = 0;
         i < MP_MAX_PARTICIPANTS;
         ++i)
    {
        mp_add_participant(
            &engine,
            i + 1);
    }

    /*
     * Worst-case system conditions.
     */
    mp_update_cpu(
        &engine,
        97.0,
        99.0);

    mp_update_gpu(
        &engine,
        99.0,
        98.0,
        99.0,
        97.0);

    mp_update_memory(
        &engine,
        15ULL * 1024ULL * 1024ULL * 1024ULL,
        96.0,
        92.0);

    mp_update_thermal(
        &engine,
        101.0,
        MP_THERMAL_CRITICAL,
        true);

    mp_update_network(
        &engine,
        300.0,
        600.0,
        150.0,
        15.0,
        20.0);

    engine.pipeline.capture_ms = 30.0;
    engine.pipeline.processing_ms = 50.0;
    engine.pipeline.encode_ms = 80.0;
    engine.pipeline.network_ms = 600.0;
    engine.pipeline.decode_ms = 70.0;
    engine.pipeline.render_ms = 100.0;

    for (int i = 0;
         i < 300;
         ++i)
    {
        mp_frame_dropped(
            &engine);
    }

    for (int i = 0;
         i < 1000;
         ++i)
    {
        mp_audio_processed(
            &engine,
            12.0,
            10.0);
    }

    mp_analyse(
        &engine);

    MPQualityDecision decision =
        mp_make_quality_decision(
            &engine);

    printf("\n");
    printf("====================================================\n");
    printf(" STRESS TEST\n");
    printf("====================================================\n");

    printf(
        "Score:                 %.2f\n",
        engine.performance_score);

    printf(
        "State:                 %s\n",
        mp_state_name(engine.state));

    printf(
        "Bottleneck:            %s\n",
        mp_bottleneck_name(
            mp_find_bottleneck(
                &engine)));

    printf(
        "Emergency resolution:  %dx%d\n",
        decision.target_video_width,
        decision.target_video_height);

    printf(
        "Emergency FPS:         %.1f\n",
        decision.target_video_fps);

    printf(
        "Emergency bitrate:     %d kbps\n",
        decision.target_video_bitrate_kbps);

    printf(
        "Audio priority:        %s\n",
        decision.prioritize_audio
            ? "YES"
            : "NO");

    printf(
        "Low power:             %s\n",
        decision.enter_low_power
            ? "YES"
            : "NO");
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void)
{
    srand(123456);

    printf("\n");
    printf("====================================================\n");
    printf(" TEAMS NATIVE C MEETING PERFORMANCE CORE\n");
    printf("====================================================\n");

    mp_run_simulation();

    mp_run_stress_test();

    printf("\n");
    printf("Meeting performance tests complete.\n");

    return 0;
}
Compile
gcc -std=c11 -O3 -march=native \
    -Wall -Wextra -pedantic \
    teams_meeting_performance.c \
    -o teams_performance \
    -lm

Then:

./teams_performance











Below is a substantial prototype: teams_search_core.c.

/*
 * teams_search_core.c
 *
 * Native C11 local message/search engine for a Teams-style client.
 *
 * Features:
 *   - Message/document storage
 *   - Tokenisation
 *   - Text normalisation
 *   - Stop-word filtering
 *   - Inverted index
 *   - Posting lists
 *   - Term dictionary
 *   - Prefix search
 *   - Bounded fuzzy matching
 *   - BM25-style relevance scoring
 *   - Recency boosting
 *   - Sender/channel filtering
 *   - Date filtering
 *   - Incremental indexing
 *   - Message update/delete
 *   - Query parsing
 *   - Result sorting
 *   - Search statistics
 *   - Memory diagnostics
 *   - Test harness
 *
 * Build:
 *
 *   gcc -std=c11 -O3 -march=native \
 *       -Wall -Wextra -pedantic \
 *       teams_search_core.c -o teams_search -lm
 *
 * This is a standalone search/indexing core, not Microsoft's proprietary
 * Teams search implementation or protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

/* ============================================================
 * Configuration
 * ============================================================ */

#define TS_MAX_TEXT               8192
#define TS_MAX_TOKEN              128
#define TS_MAX_TOKENS             512
#define TS_MAX_RESULTS             100
#define TS_MAX_QUERY_TOKENS         64
#define TS_HASH_BUCKETS           4096
#define TS_INITIAL_MESSAGES        1024
#define TS_INITIAL_POSTINGS        16
#define TS_INITIAL_TERMS           1024
#define TS_INITIAL_RESULTS         128
#define TS_MAX_FUZZY_DISTANCE        2

#define TS_BM25_K1               1.20
#define TS_BM25_B                0.75

#define TS_RECENCY_HALF_LIFE_DAYS 30.0

/* ============================================================
 * Utility
 * ============================================================ */

static double ts_clamp_double(double x, double lo, double hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static uint64_t ts_hash_string(const char *s)
{
    uint64_t hash = 1469598103934665603ULL;

    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static char *ts_strdup(const char *s)
{
    if (!s)
        return NULL;

    size_t n = strlen(s);

    char *p = malloc(n + 1);

    if (!p)
        return NULL;

    memcpy(p, s, n + 1);

    return p;
}

static int ts_compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    if (x < y) return -1;
    if (x > y) return 1;

    return 0;
}

/* ============================================================
 * Stop words
 * ============================================================ */

static const char *TS_STOP_WORDS[] = {
    "a",
    "an",
    "and",
    "are",
    "as",
    "at",
    "be",
    "been",
    "but",
    "by",
    "can",
    "could",
    "did",
    "do",
    "does",
    "for",
    "from",
    "had",
    "has",
    "have",
    "he",
    "her",
    "here",
    "him",
    "his",
    "how",
    "i",
    "if",
    "in",
    "into",
    "is",
    "it",
    "its",
    "me",
    "my",
    "no",
    "not",
    "of",
    "on",
    "or",
    "our",
    "ours",
    "she",
    "should",
    "so",
    "than",
    "that",
    "the",
    "their",
    "them",
    "there",
    "these",
    "they",
    "this",
    "those",
    "to",
    "was",
    "we",
    "were",
    "what",
    "when",
    "where",
    "which",
    "who",
    "will",
    "with",
    "would",
    "you",
    "your"
};

#define TS_STOP_WORD_COUNT \
    (sizeof(TS_STOP_WORDS) / sizeof(TS_STOP_WORDS[0]))

static bool ts_is_stop_word(const char *word)
{
    for (size_t i = 0; i < TS_STOP_WORD_COUNT; ++i) {
        if (strcmp(word, TS_STOP_WORDS[i]) == 0)
            return true;
    }

    return false;
}

/* ============================================================
 * Message model
 * ============================================================ */

typedef uint64_t TSMessageID;

typedef struct {
    TSMessageID id;

    char *sender;
    char *channel;
    char *conversation;

    char *text;

    time_t timestamp;

    bool deleted;
    bool indexed;

    size_t token_count;
} TSMessage;

/* ============================================================
 * Token
 * ============================================================ */

typedef struct {
    char text[TS_MAX_TOKEN];

    uint32_t position;
} TSToken;

/* ============================================================
 * Token vector
 * ============================================================ */

typedef struct {
    TSToken *items;

    size_t count;
    size_t capacity;
} TSTokenVector;

static bool ts_token_vector_init(TSTokenVector *v)
{
    v->count = 0;
    v->capacity = 64;

    v->items = calloc(v->capacity, sizeof(TSToken));

    return v->items != NULL;
}

static void ts_token_vector_destroy(TSTokenVector *v)
{
    free(v->items);

    v->items = NULL;
    v->count = 0;
    v->capacity = 0;
}

static bool ts_token_vector_push(
    TSTokenVector *v,
    const char *text,
    uint32_t position)
{
    if (v->count >= v->capacity) {

        size_t new_capacity = v->capacity * 2;

        TSToken *new_items =
            realloc(v->items,
                    new_capacity * sizeof(TSToken));

        if (!new_items)
            return false;

        v->items = new_items;
        v->capacity = new_capacity;
    }

    strncpy(
        v->items[v->count].text,
        text,
        TS_MAX_TOKEN - 1
    );

    v->items[v->count].text[TS_MAX_TOKEN - 1] = '\0';

    v->items[v->count].position = position;

    v->count++;

    return true;
}

/* ============================================================
 * Normalisation
 * ============================================================ */

static void ts_normalise_word(
    const char *src,
    char *dst,
    size_t capacity)
{
    size_t out = 0;

    for (size_t i = 0;
         src[i] != '\0' && out + 1 < capacity;
         ++i)
    {
        unsigned char c = (unsigned char)src[i];

        if (isalnum(c)) {
            dst[out++] = (char)tolower(c);
        }
    }

    dst[out] = '\0';
}

/* ============================================================
 * Tokeniser
 * ============================================================ */

static bool ts_tokenise(
    const char *text,
    TSTokenVector *tokens)
{
    char word[TS_MAX_TOKEN];

    size_t word_len = 0;

    uint32_t position = 0;

    for (size_t i = 0;; ++i) {

        unsigned char c = (unsigned char)text[i];

        bool boundary =
            c == '\0' ||
            !isalnum(c);

        if (!boundary) {

            if (word_len < TS_MAX_TOKEN - 1) {
                word[word_len++] =
                    (char)tolower(c);
            }

            continue;
        }

        if (word_len > 0) {

            word[word_len] = '\0';

            if (!ts_is_stop_word(word)) {

                if (!ts_token_vector_push(
                        tokens,
                        word,
                        position))
                {
                    return false;
                }
            }

            position++;

            word_len = 0;
        }

        if (c == '\0')
            break;
    }

    return true;
}

/* ============================================================
 * Posting
 * ============================================================ */

typedef struct {
    TSMessageID message_id;

    uint32_t frequency;

    uint32_t first_position;
} TSPosting;

typedef struct {
    TSPosting *items;

    size_t count;
    size_t capacity;
} TSPostingList;

static bool ts_posting_list_init(TSPostingList *list)
{
    list->count = 0;
    list->capacity = TS_INITIAL_POSTINGS;

    list->items =
        calloc(
            list->capacity,
            sizeof(TSPosting)
        );

    return list->items != NULL;
}

static void ts_posting_list_destroy(
    TSPostingList *list)
{
    free(list->items);

    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static TSPosting *ts_posting_find(
    TSPostingList *list,
    TSMessageID id)
{
    for (size_t i = 0; i < list->count; ++i) {

        if (list->items[i].message_id == id)
            return &list->items[i];
    }

    return NULL;
}

static bool ts_posting_add(
    TSPostingList *list,
    TSMessageID id,
    uint32_t position)
{
    TSPosting *existing =
        ts_posting_find(list, id);

    if (existing) {

        existing->frequency++;

        return true;
    }

    if (list->count >= list->capacity) {

        size_t new_capacity =
            list->capacity * 2;

        TSPosting *new_items =
            realloc(
                list->items,
                new_capacity *
                sizeof(TSPosting)
            );

        if (!new_items)
            return false;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    TSPosting *posting =
        &list->items[list->count++];

    posting->message_id = id;
    posting->frequency = 1;
    posting->first_position = position;

    return true;
}

static void ts_posting_remove(
    TSPostingList *list,
    TSMessageID id)
{
    for (size_t i = 0; i < list->count; ++i) {

        if (list->items[i].message_id == id) {

            if (i + 1 < list->count) {

                memmove(
                    &list->items[i],
                    &list->items[i + 1],
                    (list->count - i - 1) *
                    sizeof(TSPosting)
                );
            }

            list->count--;

            return;
        }
    }
}

/* ============================================================
 * Term
 * ============================================================ */

typedef struct TSTerm {
    char *term;

    TSPostingList postings;

    uint64_t hash;

    struct TSTerm *next;
} TSTerm;

/* ============================================================
 * Term dictionary
 * ============================================================ */

typedef struct {
    TSTerm **buckets;

    size_t bucket_count;
    size_t term_count;
} TSTermDictionary;

static bool ts_dictionary_init(
    TSTermDictionary *dict)
{
    dict->bucket_count = TS_HASH_BUCKETS;
    dict->term_count = 0;

    dict->buckets =
        calloc(
            dict->bucket_count,
            sizeof(TSTerm *)
        );

    return dict->buckets != NULL;
}

static void ts_dictionary_destroy(
    TSTermDictionary *dict)
{
    if (!dict || !dict->buckets)
        return;

    for (size_t i = 0;
         i < dict->bucket_count;
         ++i)
    {
        TSTerm *term = dict->buckets[i];

        while (term) {

            TSTerm *next = term->next;

            free(term->term);

            ts_posting_list_destroy(
                &term->postings
            );

            free(term);

            term = next;
        }
    }

    free(dict->buckets);

    dict->buckets = NULL;
    dict->bucket_count = 0;
    dict->term_count = 0;
}

static TSTerm *ts_dictionary_find(
    TSTermDictionary *dict,
    const char *word)
{
    uint64_t hash =
        ts_hash_string(word);

    size_t bucket =
        hash % dict->bucket_count;

    TSTerm *term =
        dict->buckets[bucket];

    while (term) {

        if (term->hash == hash &&
            strcmp(term->term, word) == 0)
        {
            return term;
        }

        term = term->next;
    }

    return NULL;
}

static TSTerm *ts_dictionary_get_or_create(
    TSTermDictionary *dict,
    const char *word)
{
    TSTerm *existing =
        ts_dictionary_find(dict, word);

    if (existing)
        return existing;

    TSTerm *term =
        calloc(1, sizeof(TSTerm));

    if (!term)
        return NULL;

    term->term = ts_strdup(word);

    if (!term->term) {
        free(term);
        return NULL;
    }

    if (!ts_posting_list_init(
            &term->postings))
    {
        free(term->term);
        free(term);
        return NULL;
    }

    term->hash =
        ts_hash_string(word);

    size_t bucket =
        term->hash % dict->bucket_count;

    term->next =
        dict->buckets[bucket];

    dict->buckets[bucket] = term;

    dict->term_count++;

    return term;
}

/* ============================================================
 * Message store
 * ============================================================ */

typedef struct {
    TSMessage **items;

    size_t count;
    size_t capacity;
} TSMessageStore;

static bool ts_message_store_init(
    TSMessageStore *store)
{
    store->count = 0;
    store->capacity = TS_INITIAL_MESSAGES;

    store->items =
        calloc(
            store->capacity,
            sizeof(TSMessage *)
        );

    return store->items != NULL;
}

static void ts_message_destroy(
    TSMessage *message)
{
    if (!message)
        return;

    free(message->sender);
    free(message->channel);
    free(message->conversation);
    free(message->text);

    free(message);
}

static void ts_message_store_destroy(
    TSMessageStore *store)
{
    for (size_t i = 0;
         i < store->count;
         ++i)
    {
        ts_message_destroy(
            store->items[i]
        );
    }

    free(store->items);

    store->items = NULL;
    store->count = 0;
    store->capacity = 0;
}

static TSMessage *ts_message_find(
    TSMessageStore *store,
    TSMessageID id)
{
    for (size_t i = 0;
         i < store->count;
         ++i)
    {
        if (store->items[i]->id == id)
            return store->items[i];
    }

    return NULL;
}

static bool ts_message_store_add(
    TSMessageStore *store,
    TSMessage *message)
{
    if (store->count >= store->capacity) {

        size_t new_capacity =
            store->capacity * 2;

        TSMessage **new_items =
            realloc(
                store->items,
                new_capacity *
                sizeof(TSMessage *)
            );

        if (!new_items)
            return false;

        store->items = new_items;
        store->capacity = new_capacity;
    }

    store->items[store->count++] = message;

    return true;
}

/* ============================================================
 * Search filters
 * ============================================================ */

typedef struct {
    const char *sender;
    const char *channel;
    const char *conversation;

    time_t after;
    time_t before;

    bool include_deleted;
} TSSearchFilter;

/* ============================================================
 * Search result
 * ============================================================ */

typedef struct {
    TSMessageID message_id;

    double score;

    uint32_t matched_terms;

    bool fuzzy_match;
    bool prefix_match;
} TSSearchResult;

/* ============================================================
 * Search result vector
 * ============================================================ */

typedef struct {
    TSSearchResult *items;

    size_t count;
    size_t capacity;
} TSResultVector;

static bool ts_result_vector_init(
    TSResultVector *results)
{
    results->count = 0;
    results->capacity = TS_INITIAL_RESULTS;

    results->items =
        calloc(
            results->capacity,
            sizeof(TSSearchResult)
        );

    return results->items != NULL;
}

static void ts_result_vector_destroy(
    TSResultVector *results)
{
    free(results->items);

    results->items = NULL;
    results->count = 0;
    results->capacity = 0;
}

static bool ts_result_vector_push(
    TSResultVector *results,
    const TSSearchResult *result)
{
    if (results->count >= results->capacity) {

        size_t new_capacity =
            results->capacity * 2;

        TSSearchResult *new_items =
            realloc(
                results->items,
                new_capacity *
                sizeof(TSSearchResult)
            );

        if (!new_items)
            return false;

        results->items = new_items;
        results->capacity = new_capacity;
    }

    results->items[
        results->count++
    ] = *result;

    return true;
}

/* ============================================================
 * Levenshtein distance
 * ============================================================ */

static int ts_levenshtein(
    const char *a,
    const char *b,
    int max_distance)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);

    if ((int)abs((int)na - (int)nb) >
        max_distance)
    {
        return max_distance + 1;
    }

    if (na > 128 || nb > 128)
        return max_distance + 1;

    int prev[129];
    int curr[129];

    for (size_t j = 0; j <= nb; ++j)
        prev[j] = (int)j;

    for (size_t i = 1; i <= na; ++i) {

        curr[0] = (int)i;

        int row_min = curr[0];

        for (size_t j = 1; j <= nb; ++j) {

            int insertion =
                curr[j - 1] + 1;

            int deletion =
                prev[j] + 1;

            int substitution =
                prev[j - 1] +
                (a[i - 1] != b[j - 1]);

            int value = insertion;

            if (deletion < value)
                value = deletion;

            if (substitution < value)
                value = substitution;

            curr[j] = value;

            if (value < row_min)
                row_min = value;
        }

        if (row_min > max_distance)
            return max_distance + 1;

        memcpy(
            prev,
            curr,
            (nb + 1) * sizeof(int)
        );
    }

    return prev[nb];
}

/* ============================================================
 * Prefix matching
 * ============================================================ */

static bool ts_has_prefix(
    const char *term,
    const char *prefix)
{
    size_t a = strlen(term);
    size_t b = strlen(prefix);

    if (b > a)
        return false;

    return strncmp(term, prefix, b) == 0;
}

/* ============================================================
 * Query
 * ============================================================ */

typedef struct {
    TSTokenVector tokens;

    bool prefix_mode;
    bool fuzzy_mode;

    TSSearchFilter filter;
} TSQuery;

static bool ts_query_init(
    TSQuery *query,
    const char *text)
{
    memset(query, 0, sizeof(*query));

    if (!ts_token_vector_init(
            &query->tokens))
    {
        return false;
    }

    if (!ts_tokenise(
            text,
            &query->tokens))
    {
        ts_token_vector_destroy(
            &query->tokens
        );

        return false;
    }

    return true;
}

static void ts_query_destroy(
    TSQuery *query)
{
    ts_token_vector_destroy(
        &query->tokens
    );
}

/* ============================================================
 * Engine
 * ============================================================ */

typedef struct {
    TSMessageStore messages;

    TSTermDictionary dictionary;

    uint64_t next_message_id;

    size_t total_tokens;

    double average_document_length;

    uint64_t searches;
    uint64_t indexed_messages;
    uint64_t deleted_messages;

    uint64_t comparisons;
} TSSearchEngine;

/* ============================================================
 * Engine lifecycle
 * ============================================================ */

static bool ts_search_engine_init(
    TSSearchEngine *engine)
{
    memset(engine, 0, sizeof(*engine));

    if (!ts_message_store_init(
            &engine->messages))
    {
        return false;
    }

    if (!ts_dictionary_init(
            &engine->dictionary))
    {
        ts_message_store_destroy(
            &engine->messages
        );

        return false;
    }

    engine->next_message_id = 1;

    return true;
}

static void ts_search_engine_destroy(
    TSSearchEngine *engine)
{
    ts_dictionary_destroy(
        &engine->dictionary
    );

    ts_message_store_destroy(
        &engine->messages
    );

    memset(engine, 0, sizeof(*engine));
}

/* ============================================================
 * Message creation
 * ============================================================ */

static TSMessage *ts_message_create(
    TSMessageID id,
    const char *sender,
    const char *channel,
    const char *conversation,
    const char *text,
    time_t timestamp)
{
    TSMessage *message =
        calloc(1, sizeof(TSMessage));

    if (!message)
        return NULL;

    message->id = id;

    message->sender =
        ts_strdup(sender ? sender : "");

    message->channel =
        ts_strdup(channel ? channel : "");

    message->conversation =
        ts_strdup(
            conversation ?
            conversation : ""
        );

    message->text =
        ts_strdup(text ? text : "");

    message->timestamp = timestamp;

    if (!message->sender ||
        !message->channel ||
        !message->conversation ||
        !message->text)
    {
        ts_message_destroy(message);
        return NULL;
    }

    return message;
}

/* ============================================================
 * Index insertion
 * ============================================================ */

static bool ts_index_message(
    TSSearchEngine *engine,
    TSMessage *message)
{
    if (message->indexed)
        return true;

    TSTokenVector tokens;

    if (!ts_token_vector_init(&tokens))
        return false;

    if (!ts_tokenise(
            message->text,
            &tokens))
    {
        ts_token_vector_destroy(&tokens);
        return false;
    }

    message->token_count =
        tokens.count;

    for (size_t i = 0;
         i < tokens.count;
         ++i)
    {
        TSToken *token =
            &tokens.items[i];

        TSTerm *term =
            ts_dictionary_get_or_create(
                &engine->dictionary,
                token->text
            );

        if (!term) {
            ts_token_vector_destroy(&tokens);
            return false;
        }

        if (!ts_posting_add(
                &term->postings,
                message->id,
                token->position))
        {
            ts_token_vector_destroy(&tokens);
            return false;
        }
    }

    engine->total_tokens +=
        tokens.count;

    engine->indexed_messages++;

    message->indexed = true;

    if (engine->indexed_messages > 0) {

        engine->average_document_length =
            (double)engine->total_tokens /
            (double)engine->indexed_messages;
    }

    ts_token_vector_destroy(&tokens);

    return true;
}

/* ============================================================
 * Index removal
 * ============================================================ */

static void ts_unindex_message(
    TSSearchEngine *engine,
    TSMessage *message)
{
    if (!message->indexed)
        return;

    TSTokenVector tokens;

    if (!ts_token_vector_init(&tokens))
        return;

    if (ts_tokenise(
            message->text,
            &tokens))
    {
        for (size_t i = 0;
             i < tokens.count;
             ++i)
        {
            TSTerm *term =
                ts_dictionary_find(
                    &engine->dictionary,
                    tokens.items[i].text
                );

            if (!term)
                continue;

            TSPosting *posting =
                ts_posting_find(
                    &term->postings,
                    message->id
                );

            if (posting) {

                if (engine->total_tokens >=
                    posting->frequency)
                {
                    engine->total_tokens -=
                        posting->frequency;
                }
            }

            ts_posting_remove(
                &term->postings,
                message->id
            );
        }
    }

    message->indexed = false;

    if (engine->indexed_messages > 0)
        engine->indexed_messages--;

    if (engine->indexed_messages > 0) {

        engine->average_document_length =
            (double)engine->total_tokens /
            (double)engine->indexed_messages;

    } else {

        engine->average_document_length = 0.0;
    }

    ts_token_vector_destroy(&tokens);
}

/* ============================================================
 * Add message
 * ============================================================ */

static TSMessageID ts_search_add_message(
    TSSearchEngine *engine,
    const char *sender,
    const char *channel,
    const char *conversation,
    const char *text,
    time_t timestamp)
{
    TSMessageID id =
        engine->next_message_id++;

    TSMessage *message =
        ts_message_create(
            id,
            sender,
            channel,
            conversation,
            text,
            timestamp
        );

    if (!message)
        return 0;

    if (!ts_message_store_add(
            &engine->messages,
            message))
    {
        ts_message_destroy(message);
        return 0;
    }

    if (!ts_index_message(
            engine,
            message))
    {
        return 0;
    }

    return id;
}

/* ============================================================
 * Update message
 * ============================================================ */

static bool ts_search_update_message(
    TSSearchEngine *engine,
    TSMessageID id,
    const char *new_text)
{
    TSMessage *message =
        ts_message_find(
            &engine->messages,
            id
        );

    if (!message)
        return false;

    ts_unindex_message(
        engine,
        message
    );

    char *new_value =
        ts_strdup(new_text);

    if (!new_value)
        return false;

    free(message->text);

    message->text = new_value;

    return ts_index_message(
        engine,
        message
    );
}

/* ============================================================
 * Delete message
 * ============================================================ */

static bool ts_search_delete_message(
    TSSearchEngine *engine,
    TSMessageID id)
{
    TSMessage *message =
        ts_message_find(
            &engine->messages,
            id
        );

    if (!message)
        return false;

    if (message->deleted)
        return true;

    ts_unindex_message(
        engine,
        message
    );

    message->deleted = true;

    engine->deleted_messages++;

    return true;
}

/* ============================================================
 * Filters
 * ============================================================ */

static bool ts_filter_matches(
    const TSMessage *message,
    const TSSearchFilter *filter)
{
    if (message->deleted &&
        !filter->include_deleted)
    {
        return false;
    }

    if (filter->sender &&
        filter->sender[0] != '\0')
    {
        if (strcmp(
                message->sender,
                filter->sender) != 0)
        {
            return false;
        }
    }

    if (filter->channel &&
        filter->channel[0] != '\0')
    {
        if (strcmp(
                message->channel,
                filter->channel) != 0)
        {
            return false;
        }
    }

    if (filter->conversation &&
        filter->conversation[0] != '\0')
    {
        if (strcmp(
                message->conversation,
                filter->conversation) != 0)
        {
            return false;
        }
    }

    if (filter->after != 0 &&
        message->timestamp < filter->after)
    {
        return false;
    }

    if (filter->before != 0 &&
        message->timestamp > filter->before)
    {
        return false;
    }

    return true;
}

/* ============================================================
 * Document frequency
 * ============================================================ */

static size_t ts_document_frequency(
    const TSTerm *term)
{
    size_t count = 0;

    for (size_t i = 0;
         i < term->postings.count;
         ++i)
    {
        count++;
    }

    return count;
}

/* ============================================================
 * BM25
 * ============================================================ */

static double ts_bm25_score(
    const TSSearchEngine *engine,
    const TSMessage *message,
    const TSTerm *term,
    const TSPosting *posting)
{
    size_t N =
        engine->indexed_messages;

    size_t df =
        ts_document_frequency(term);

    if (N == 0 || df == 0)
        return 0.0;

    double idf =
        log(
            1.0 +
            (
                ((double)N - (double)df + 0.5) /
                ((double)df + 0.5)
            )
        );

    double dl =
        (double)message->token_count;

    double avgdl =
        engine->average_document_length;

    if (avgdl <= 0.0)
        avgdl = 1.0;

    double tf =
        (double)posting->frequency;

    double denominator =
        tf +
        TS_BM25_K1 *
        (
            1.0 -
            TS_BM25_B +
            TS_BM25_B *
            (dl / avgdl)
        );

    return idf *
           (
               (tf * (TS_BM25_K1 + 1.0)) /
               denominator
           );
}

/* ============================================================
 * Recency
 * ============================================================ */

static double ts_recency_score(
    time_t timestamp,
    time_t now)
{
    if (timestamp <= 0)
        return 0.0;

    double age =
        difftime(now, timestamp);

    if (age < 0)
        age = 0;

    double days =
        age / 86400.0;

    return pow(
        0.5,
        days / TS_RECENCY_HALF_LIFE_DAYS
    );
}

/* ============================================================
 * Result lookup
 * ============================================================ */

static TSSearchResult *ts_result_find(
    TSResultVector *results,
    TSMessageID id)
{
    for (size_t i = 0;
         i < results->count;
         ++i)
    {
        if (results->items[i].message_id == id)
            return &results->items[i];
    }

    return NULL;
}

/* ============================================================
 * Match a query token against a term
 * ============================================================ */

typedef struct {
    bool matched;

    bool fuzzy;
    bool prefix;

    double multiplier;
} TSTermMatch;

static TSTermMatch ts_match_term(
    const char *query,
    const char *term,
    bool prefix_mode,
    bool fuzzy_mode)
{
    TSTermMatch result = {0};

    if (strcmp(query, term) == 0) {

        result.matched = true;
        result.multiplier = 1.0;

        return result;
    }

    if (prefix_mode &&
        ts_has_prefix(term, query))
    {
        result.matched = true;
        result.prefix = true;

        result.multiplier = 0.72;

        return result;
    }

    if (fuzzy_mode) {

        int distance =
            ts_levenshtein(
                query,
                term,
                TS_MAX_FUZZY_DISTANCE
            );

        if (distance <=
            TS_MAX_FUZZY_DISTANCE)
        {
            result.matched = true;
            result.fuzzy = true;

            result.multiplier =
                distance == 1 ?
                0.60 :
                0.35;
        }
    }

    return result;
}

/* ============================================================
 * Search
 * ============================================================ */

static size_t ts_search(
    TSSearchEngine *engine,
    const char *query_text,
    const TSQuery *query_options,
    TSSearchResult *output,
    size_t output_capacity)
{
    if (!engine ||
        !query_text ||
        !output ||
        output_capacity == 0)
    {
        return 0;
    }

    engine->searches++;

    TSQuery local_query;

    if (!ts_query_init(
            &local_query,
            query_text))
    {
        return 0;
    }

    local_query.prefix_mode =
        query_options ?
        query_options->prefix_mode :
        false;

    local_query.fuzzy_mode =
        query_options ?
        query_options->fuzzy_mode :
        false;

    if (query_options)
        local_query.filter =
            query_options->filter;

    TSResultVector results;

    if (!ts_result_vector_init(
            &results))
    {
        ts_query_destroy(&local_query);
        return 0;
    }

    time_t now = time(NULL);

    for (size_t q = 0;
         q < local_query.tokens.count;
         ++q)
    {
        const char *query_token =
            local_query.tokens.items[q].text;

        for (size_t b = 0;
             b < engine->dictionary.bucket_count;
             ++b)
        {
            TSTerm *term =
                engine->dictionary.buckets[b];

            while (term) {

                engine->comparisons++;

                TSTermMatch match =
                    ts_match_term(
                        query_token,
                        term->term,
                        local_query.prefix_mode,
                        local_query.fuzzy_mode
                    );

                if (!match.matched) {
                    term = term->next;
                    continue;
                }

                for (size_t p = 0;
                     p < term->postings.count;
                     ++p)
                {
                    TSPosting *posting =
                        &term->postings.items[p];

                    TSMessage *message =
                        ts_message_find(
                            &engine->messages,
                            posting->message_id
                        );

                    if (!message)
                        continue;

                    if (!ts_filter_matches(
                            message,
                            &local_query.filter))
                    {
                        continue;
                    }

                    double score =
                        ts_bm25_score(
                            engine,
                            message,
                            term,
                            posting
                        );

                    score *=
                        match.multiplier;

                    /*
                     * Exact phrase-ish bonus:
                     * repeated matching terms accumulate.
                     */
                    if (match.fuzzy)
                        score *= 0.75;

                    if (match.prefix)
                        score *= 0.85;

                    TSSearchResult *existing =
                        ts_result_find(
                            &results,
                            message->id
                        );

                    if (!existing) {

                        TSSearchResult result;

                        memset(
                            &result,
                            0,
                            sizeof(result)
                        );

                        result.message_id =
                            message->id;

                        result.score = score;

                        result.matched_terms = 1;

                        result.fuzzy_match =
                            match.fuzzy;

                        result.prefix_match =
                            match.prefix;

                        ts_result_vector_push(
                            &results,
                            &result
                        );

                    } else {

                        existing->score += score;

                        existing->matched_terms++;

                        existing->fuzzy_match |=
                            match.fuzzy;

                        existing->prefix_match |=
                            match.prefix;
                    }
                }

                term = term->next;
            }
        }
    }

    /*
     * Recency adjustment.
     */
    for (size_t i = 0;
         i < results.count;
         ++i)
    {
        TSSearchResult *result =
            &results.items[i];

        TSMessage *message =
            ts_message_find(
                &engine->messages,
                result->message_id
            );

        if (!message)
            continue;

        double recency =
            ts_recency_score(
                message->timestamp,
                now
            );

        /*
         * Recency contributes gently rather
         * than dominating textual relevance.
         */
        result->score *=
            1.0 + 0.15 * recency;

        /*
         * Multi-term matches are important.
         */
        if (local_query.tokens.count > 1) {

            double coverage =
                (double)result->matched_terms /
                (double)local_query.tokens.count;

            result->score *=
                0.5 + coverage;
        }
    }

    /*
     * Sort descending by score.
     */
    for (size_t i = 0;
         i < results.count;
         ++i)
    {
        for (size_t j = i + 1;
             j < results.count;
             ++j)
        {
            if (results.items[j].score >
                results.items[i].score)
            {
                TSSearchResult tmp =
                    results.items[i];

                results.items[i] =
                    results.items[j];

                results.items[j] =
                    tmp;
            }
        }
    }

    size_t count =
        results.count < output_capacity ?
        results.count :
        output_capacity;

    memcpy(
        output,
        results.items,
        count * sizeof(TSSearchResult)
    );

    ts_result_vector_destroy(
        &results
    );

    ts_query_destroy(
        &local_query
    );

    return count;
}

/* ============================================================
 * Search helpers
 * ============================================================ */

static void ts_print_message(
    const TSSearchEngine *engine,
    TSMessageID id)
{
    TSMessage *message =
        ts_message_find(
            (TSMessageStore *)&engine->messages,
            id
        );

    if (!message)
        return;

    printf(
        "[%llu] %s / #%s\n"
        "    %s\n",
        (unsigned long long)message->id,
        message->sender,
        message->channel,
        message->text
    );
}

/* ============================================================
 * Search report
 * ============================================================ */

static void ts_print_results(
    TSSearchEngine *engine,
    TSSearchResult *results,
    size_t count)
{
    printf("\n");
    printf("SEARCH RESULTS: %zu\n", count);
    printf("--------------------------------------------\n");

    for (size_t i = 0;
         i < count;
         ++i)
    {
        TSSearchResult *r =
            &results[i];

        TSMessage *message =
            ts_message_find(
                &engine->messages,
                r->message_id
            );

        if (!message)
            continue;

        printf(
            "%2zu. score=%8.4f "
            "matches=%u "
            "%s%s\n",
            i + 1,
            r->score,
            r->matched_terms,
            r->fuzzy_match ? "FUZZY " : "",
            r->prefix_match ? "PREFIX" : ""
        );

        printf(
            "    %s | #%s\n",
            message->sender,
            message->channel
        );

        printf(
            "    %s\n",
            message->text
        );
    }
}

/* ============================================================
 * Diagnostics
 * ============================================================ */

static void ts_search_diagnostics(
    const TSSearchEngine *engine)
{
    printf("\n");
    printf("============================================\n");
    printf(" SEARCH ENGINE DIAGNOSTICS\n");
    printf("============================================\n");

    printf(
        "Messages:              %zu\n",
        engine->messages.count
    );

    printf(
        "Indexed messages:      %llu\n",
        (unsigned long long)
        engine->indexed_messages
    );

    printf(
        "Deleted messages:      %llu\n",
        (unsigned long long)
        engine->deleted_messages
    );

    printf(
        "Unique terms:          %zu\n",
        engine->dictionary.term_count
    );

    printf(
        "Total tokens:          %llu\n",
        (unsigned long long)
        engine->total_tokens
    );

    printf(
        "Average document len:  %.2f\n",
        engine->average_document_length
    );

    printf(
        "Searches:              %llu\n",
        (unsigned long long)
        engine->searches
    );

    printf(
        "Term comparisons:      %llu\n",
        (unsigned long long)
        engine->comparisons
    );

    printf("============================================\n");
}

/* ============================================================
 * Memory estimation
 * ============================================================ */

static size_t ts_estimate_memory(
    const TSSearchEngine *engine)
{
    size_t bytes = 0;

    bytes +=
        engine->dictionary.bucket_count *
        sizeof(TSTerm *);

    bytes +=
        engine->messages.capacity *
        sizeof(TSMessage *);

    for (size_t i = 0;
         i < engine->messages.count;
         ++i)
    {
        TSMessage *m =
            engine->messages.items[i];

        bytes += sizeof(TSMessage);

        if (m->sender)
            bytes += strlen(m->sender) + 1;

        if (m->channel)
            bytes += strlen(m->channel) + 1;

        if (m->conversation)
            bytes +=
                strlen(m->conversation) + 1;

        if (m->text)
            bytes += strlen(m->text) + 1;
    }

    for (size_t b = 0;
         b < engine->dictionary.bucket_count;
         ++b)
    {
        TSTerm *term =
            engine->dictionary.buckets[b];

        while (term) {

            bytes += sizeof(TSTerm);

            bytes +=
                strlen(term->term) + 1;

            bytes +=
                term->postings.capacity *
                sizeof(TSPosting);

            term = term->next;
        }
    }

    return bytes;
}

/* ============================================================
 * Query convenience
 * ============================================================ */

static void ts_execute_query(
    TSSearchEngine *engine,
    const char *text,
    bool prefix,
    bool fuzzy,
    const char *sender,
    const char *channel)
{
    TSQuery options;

    memset(&options, 0, sizeof(options));

    options.prefix_mode = prefix;
    options.fuzzy_mode = fuzzy;

    options.filter.sender = sender;
    options.filter.channel = channel;

    TSSearchResult results[TS_MAX_RESULTS];

    size_t count =
        ts_search(
            engine,
            text,
            &options,
            results,
            TS_MAX_RESULTS
        );

    printf("\n");
    printf(
        "QUERY: \"%s\" "
        "prefix=%s fuzzy=%s\n",
        text,
        prefix ? "yes" : "no",
        fuzzy ? "yes" : "no"
    );

    if (sender)
        printf("Sender filter: %s\n", sender);

    if (channel)
        printf("Channel filter: %s\n", channel);

    ts_print_results(
        engine,
        results,
        count
    );
}

/* ============================================================
 * Benchmark
 * ============================================================ */

static void ts_benchmark(
    TSSearchEngine *engine,
    const char *query,
    size_t iterations)
{
    printf("\n");
    printf("BENCHMARK: %s\n", query);
    printf(
        "Iterations: %zu\n",
        iterations
    );

    clock_t start = clock();

    size_t total_results = 0;

    TSSearchResult results[50];

    for (size_t i = 0;
         i < iterations;
         ++i)
    {
        total_results +=
            ts_search(
                engine,
                query,
                NULL,
                results,
                50
            );
    }

    clock_t end = clock();

    double elapsed =
        (double)(end - start) /
        (double)CLOCKS_PER_SEC;

    double searches_per_second =
        elapsed > 0.0 ?
        (double)iterations / elapsed :
        0.0;

    printf(
        "Elapsed: %.4f seconds\n",
        elapsed
    );

    printf(
        "Searches/sec: %.2f\n",
        searches_per_second
    );

    printf(
        "Average results: %.2f\n",
        (double)total_results /
        (double)iterations
    );
}

/* ============================================================
 * Test data
 * ============================================================ */

static void ts_insert_test_data(
    TSSearchEngine *engine)
{
    time_t now = time(NULL);

    ts_search_add_message(
        engine,
        "alice",
        "engineering",
        "project-alpha",
        "We need to optimise the database query before Friday.",
        now - 3600
    );

    ts_search_add_message(
        engine,
        "bob",
        "engineering",
        "project-alpha",
        "The database index is causing a performance regression.",
        now - 7200
    );

    ts_search_add_message(
        engine,
        "charlie",
        "engineering",
        "project-beta",
        "I have pushed the new search indexing implementation.",
        now - 10000
    );

    ts_search_add_message(
        engine,
        "diana",
        "product",
        "roadmap",
        "The search experience should feel instantaneous for users.",
        now - 15000
    );

    ts_search_add_message(
        engine,
        "alice",
        "product",
        "roadmap",
        "We should add fuzzy search and better ranking.",
        now - 20000
    );

    ts_search_add_message(
        engine,
        "edward",
        "sales",
        "customer-a",
        "The customer wants searchable meeting history.",
        now - 40000
    );

    ts_search_add_message(
        engine,
        "frank",
        "engineering",
        "project-gamma",
        "Video performance depends on efficient local caching.",
        now - 50000
    );

    ts_search_add_message(
        engine,
        "grace",
        "engineering",
        "project-alpha",
        "The cache should persist messages locally.",
        now - 70000
    );

    ts_search_add_message(
        engine,
        "henry",
        "management",
        "weekly",
        "We need diagnostics for CPU memory network and search performance.",
        now - 90000
    );

    ts_search_add_message(
        engine,
        "isabelle",
        "engineering",
        "project-beta",
        "Prefix searching makes finding conversations much faster.",
        now - 100000
    );
}

/* ============================================================
 * Stress dataset
 * ============================================================ */

static void ts_generate_stress_data(
    TSSearchEngine *engine,
    size_t count)
{
    const char *senders[] = {
        "alice",
        "bob",
        "charlie",
        "diana",
        "edward",
        "frank",
        "grace",
        "henry"
    };

    const char *channels[] = {
        "engineering",
        "product",
        "sales",
        "management",
        "support",
        "research"
    };

    const char *phrases[] = {
        "database optimisation and search indexing",
        "network performance and latency diagnostics",
        "video encoding pipeline optimisation",
        "audio processing and packet recovery",
        "meeting performance and resource management",
        "local cache persistence and synchronisation",
        "customer messaging and conversation history",
        "distributed systems reliability engineering"
    };

    size_t sender_count =
        sizeof(senders) /
        sizeof(senders[0]);

    size_t channel_count =
        sizeof(channels) /
        sizeof(channels[0]);

    size_t phrase_count =
        sizeof(phrases) /
        sizeof(phrases[0]);

    time_t now = time(NULL);

    for (size_t i = 0;
         i < count;
         ++i)
    {
        char text[1024];

        snprintf(
            text,
            sizeof(text),
            "%s message %zu meeting "
            "project architecture "
            "performance",
            phrases[i % phrase_count],
            i
        );

        ts_search_add_message(
            engine,
            senders[i % sender_count],
            channels[i % channel_count],
            "stress-project",
            text,
            now - (time_t)(i % 100000)
        );
    }
}

/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    printf(
        "Teams-style Native Search Engine\n"
    );

    printf(
        "============================================\n"
    );

    TSSearchEngine engine;

    if (!ts_search_engine_init(
            &engine))
    {
        fprintf(
            stderr,
            "Failed to initialise search engine.\n"
        );

        return EXIT_FAILURE;
    }

    /*
     * Initial test corpus.
     */
    ts_insert_test_data(&engine);

    ts_search_diagnostics(&engine);

    /*
     * Exact relevance search.
     */
    ts_execute_query(
        &engine,
        "database search",
        false,
        false,
        NULL,
        NULL
    );

    /*
     * Prefix search.
     */
    ts_execute_query(
        &engine,
        "optim",
        true,
        false,
        NULL,
        NULL
    );

    /*
     * Fuzzy search.
     */
    ts_execute_query(
        &engine,
        "databse",
        false,
        true,
        NULL,
        NULL
    );

    /*
     * Sender filter.
     */
    ts_execute_query(
        &engine,
        "search",
        false,
        false,
        "alice",
        NULL
    );

    /*
     * Channel filter.
     */
    ts_execute_query(
        &engine,
        "performance",
        false,
        false,
        NULL,
        "engineering"
    );

    /*
     * Update a message.
     */
    printf(
        "\nUpdating message #1...\n"
    );

    ts_search_update_message(
        &engine,
        1,
        "We need to optimise the database "
        "search engine and indexing system."
    );

    ts_execute_query(
        &engine,
        "indexing system",
        false,
        false,
        NULL,
        NULL
    );

    /*
     * Delete a message.
     */
    printf(
        "\nDeleting message #2...\n"
    );

    ts_search_delete_message(
        &engine,
        2
    );

    ts_execute_query(
        &engine,
        "database performance",
        false,
        false,
        NULL,
        NULL
    );

    /*
     * Memory diagnostics.
     */
    size_t memory =
        ts_estimate_memory(&engine);

    printf(
        "\nEstimated memory usage: %zu bytes "
        "(%.2f MB)\n",
        memory,
        (double)memory /
        (1024.0 * 1024.0)
    );

    /*
     * Add a larger synthetic dataset.
     */
    printf(
        "\nGenerating stress corpus...\n"
    );

    ts_generate_stress_data(
        &engine,
        5000
    );

    ts_search_diagnostics(&engine);

    /*
     * Benchmark.
     */
    ts_benchmark(
        &engine,
        "network performance",
        100
    );

    ts_benchmark(
        &engine,
        "database search",
        100
    );

    ts_benchmark(
        &engine,
        "video optimisation",
        100
    );

    /*
     * Final search.
     */
    ts_execute_query(
        &engine,
        "performance",
        false,
        false,
        NULL,
        NULL
    );

    ts_search_engine_destroy(
        &engine
    );

    printf(
        "\nSearch engine shutdown cleanly.\n"
    );

    return EXIT_SUCCESS;
}
What #5 now gives the client

The architecture is roughly:

                    TEAMS-LIKE CLIENT
                           │
                           ▼
                ┌─────────────────────┐
                │   Search Controller │
                └──────────┬──────────┘
                           │
                           ▼
              ┌─────────────────────────┐
              │   C SEARCH ENGINE #5    │
              ├─────────────────────────┤
              │ Query Parser             │
              │ Tokeniser                │
              │ Normaliser               │
              │ Stop-word filtering      │
              │                           │
              │ Inverted Index            │
              │ ├── Term Dictionary      │
              │ ├── Posting Lists        │
              │ └── Message IDs          │
              │                           │
              │ Ranking                   │
              │ ├── BM25                 │
              │ ├── Term Frequency       │
              │ ├── Document Frequency   │
              │ └── Recency              │
              │                           │
              │ Retrieval                │
              │ ├── Exact                │
              │ ├── Prefix               │
              │ └── Fuzzy                │
              │                           │
              │ Filters                  │
              │ ├── Sender               │
              │ ├── Channel              │
              │ ├── Conversation         │
              │ └── Date                 │
              └──────────┬──────────────┘
                         │
              ┌──────────▼───────────┐
              │   LOCAL MESSAGE DB   │
              │                      │
              │ Messages             │
              │ Metadata             │
              │ Timestamps           │
              │ Conversations        │
              └──────────────────────┘

The important thing is that search doesn't need to scan every message.

Instead of:

message 1 → inspect
message 2 → inspect
message 3 → inspect
...
message 5,000,000 → inspect

it can do:

"database"
      ↓
TERM DICTIONARY
      ↓
POSTING LIST
      ↓
[17, 44, 91, 102, 884, 1402, ...]
      ↓
BM25 ranking
      ↓
top results









/*
 * teams_storage_core.c
 *
 * Native C11 local cache + persistence engine for a
 * Teams-style collaboration client.
 *
 * Features:
 *
 *   - Local object/message cache
 *   - Key/value storage
 *   - Binary persistence
 *   - Write-ahead journal
 *   - Crash recovery
 *   - Checksums
 *   - Versioning
 *   - Dirty-object tracking
 *   - TTL / expiration
 *   - LRU cache eviction
 *   - Namespace support
 *   - Offline writes
 *   - Tombstones
 *   - Compaction
 *   - Statistics
 *   - Storage diagnostics
 *   - Atomic file replacement
 *   - Transaction-like batch writes
 *   - Sync-state metadata
 *
 * Build:
 *
 *   gcc -std=c11 -O3 -march=native \
 *       -Wall -Wextra -pedantic \
 *       teams_storage_core.c -o teams_storage -lm
 *
 * This is a standalone local storage prototype.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_if_needed(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_if_needed(p) mkdir((p), 0755)
#endif


/* ============================================================
 * Configuration
 * ============================================================ */

#define TS_STORAGE_MAGIC          0x54534348U
#define TS_STORAGE_VERSION        1U

#define TS_DEFAULT_BUCKETS       4096
#define TS_INITIAL_ENTRIES       1024

#define TS_MAX_KEY               512
#define TS_MAX_NAMESPACE         128

#define TS_DEFAULT_CACHE_LIMIT   (64ULL * 1024ULL * 1024ULL)

#define TS_WAL_FILE              "storage.wal"
#define TS_DATA_FILE             "storage.dat"
#define TS_TEMP_FILE             "storage.tmp"

#define TS_MAX_VALUE_SIZE        (16ULL * 1024ULL * 1024ULL)


/* ============================================================
 * Utility
 * ============================================================ */

static uint64_t storage_hash(
    const void *data,
    size_t length)
{
    const unsigned char *p =
        (const unsigned char *)data;

    uint64_t hash =
        1469598103934665603ULL;

    for (size_t i = 0; i < length; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static uint64_t storage_hash_string(
    const char *s)
{
    return storage_hash(
        s,
        strlen(s)
    );
}

static char *storage_strdup(
    const char *s)
{
    if (!s)
        return NULL;

    size_t n = strlen(s);

    char *p =
        malloc(n + 1);

    if (!p)
        return NULL;

    memcpy(
        p,
        s,
        n + 1
    );

    return p;
}

static uint64_t storage_now_ms(void)
{
    struct timespec ts;

    if (timespec_get(
            &ts,
            TIME_UTC) != TIME_UTC)
    {
        return 0;
    }

    return
        (uint64_t)ts.tv_sec * 1000ULL +
        (uint64_t)ts.tv_nsec / 1000000ULL;
}


/* ============================================================
 * Storage value
 * ============================================================ */

typedef struct {
    unsigned char *data;
    size_t size;
} StorageValue;


static void storage_value_destroy(
    StorageValue *value)
{
    if (!value)
        return;

    free(value->data);

    value->data = NULL;
    value->size = 0;
}


static bool storage_value_copy(
    StorageValue *dst,
    const void *data,
    size_t size)
{
    dst->data = NULL;
    dst->size = 0;

    if (size == 0)
        return true;

    dst->data =
        malloc(size);

    if (!dst->data)
        return false;

    memcpy(
        dst->data,
        data,
        size
    );

    dst->size = size;

    return true;
}


/* ============================================================
 * Entry
 * ============================================================ */

typedef struct StorageEntry {

    char *namespace_name;

    char *key;

    unsigned char *value;

    size_t value_size;

    uint64_t hash;

    uint64_t version;

    uint64_t created_at;

    uint64_t updated_at;

    uint64_t expires_at;

    uint64_t last_access;

    bool dirty;

    bool tombstone;

    bool persistent;

    struct StorageEntry *next;

} StorageEntry;


/* ============================================================
 * Entry helpers
 * ============================================================ */

static void storage_entry_destroy(
    StorageEntry *entry)
{
    if (!entry)
        return;

    free(entry->namespace_name);
    free(entry->key);
    free(entry->value);

    free(entry);
}


static StorageEntry *storage_entry_create(
    const char *namespace_name,
    const char *key,
    const void *value,
    size_t value_size)
{
    StorageEntry *entry =
        calloc(
            1,
            sizeof(StorageEntry)
        );

    if (!entry)
        return NULL;

    entry->namespace_name =
        storage_strdup(
            namespace_name ?
            namespace_name :
            "default"
        );

    entry->key =
        storage_strdup(key);

    if (!entry->namespace_name ||
        !entry->key)
    {
        storage_entry_destroy(entry);
        return NULL;
    }

    if (!storage_value_copy(
            &(StorageValue){
                .data = NULL,
                .size = 0
            },
            value,
            value_size))
    {
        /*
         * The compound literal above cannot be used
         * as the destination because its allocated
         * pointer would be inaccessible here.
         *
         * Value allocation is therefore performed
         * explicitly below.
         */
    }

    if (value_size > 0) {

        entry->value =
            malloc(value_size);

        if (!entry->value) {
            storage_entry_destroy(entry);
            return NULL;
        }

        memcpy(
            entry->value,
            value,
            value_size
        );
    }

    entry->value_size =
        value_size;

    entry->hash =
        storage_hash_string(key);

    uint64_t now =
        storage_now_ms();

    entry->created_at = now;
    entry->updated_at = now;
    entry->last_access = now;

    entry->version = 1;

    return entry;
}


/* ============================================================
 * Hash table
 * ============================================================ */

typedef struct {

    StorageEntry **buckets;

    size_t bucket_count;

    size_t count;

} StorageTable;


static bool storage_table_init(
    StorageTable *table,
    size_t bucket_count)
{
    table->bucket_count =
        bucket_count;

    table->count = 0;

    table->buckets =
        calloc(
            bucket_count,
            sizeof(StorageEntry *)
        );

    return table->buckets != NULL;
}


static void storage_table_destroy(
    StorageTable *table)
{
    if (!table ||
        !table->buckets)
        return;

    for (size_t i = 0;
         i < table->bucket_count;
         ++i)
    {
        StorageEntry *entry =
            table->buckets[i];

        while (entry) {

            StorageEntry *next =
                entry->next;

            storage_entry_destroy(
                entry
            );

            entry = next;
        }
    }

    free(table->buckets);

    table->buckets = NULL;
    table->bucket_count = 0;
    table->count = 0;
}


/* ============================================================
 * Composite key
 * ============================================================ */

static uint64_t storage_composite_hash(
    const char *namespace_name,
    const char *key)
{
    uint64_t h =
        storage_hash_string(
            namespace_name
        );

    h ^= storage_hash_string(key);

    h *= 1099511628211ULL;

    return h;
}


static bool storage_key_equal(
    const StorageEntry *entry,
    const char *namespace_name,
    const char *key)
{
    return
        strcmp(
            entry->namespace_name,
            namespace_name
        ) == 0 &&
        strcmp(
            entry->key,
            key
        ) == 0;
}


static StorageEntry *storage_table_find(
    StorageTable *table,
    const char *namespace_name,
    const char *key)
{
    uint64_t hash =
        storage_composite_hash(
            namespace_name,
            key
        );

    size_t bucket =
        hash % table->bucket_count;

    StorageEntry *entry =
        table->buckets[bucket];

    while (entry) {

        if (entry->hash == hash &&
            storage_key_equal(
                entry,
                namespace_name,
                key
            ))
        {
            return entry;
        }

        entry = entry->next;
    }

    return NULL;
}


static bool storage_table_insert(
    StorageTable *table,
    StorageEntry *entry)
{
    size_t bucket =
        entry->hash %
        table->bucket_count;

    entry->next =
        table->buckets[bucket];

    table->buckets[bucket] =
        entry;

    table->count++;

    return true;
}


/* ============================================================
 * WAL
 * ============================================================ */

typedef enum {

    STORAGE_WAL_SET = 1,
    STORAGE_WAL_DELETE = 2,
    STORAGE_WAL_CLEAR = 3

} StorageWALType;


typedef struct {

    uint32_t magic;

    uint32_t version;

    uint32_t operation;

    uint32_t reserved;

    uint64_t namespace_size;

    uint64_t key_size;

    uint64_t value_size;

    uint64_t entry_version;

    uint64_t timestamp;

    uint64_t checksum;

} StorageWALHeader;


/* ============================================================
 * Main storage engine
 * ============================================================ */

typedef struct {

    StorageTable table;

    char directory[1024];

    char data_path[1200];

    char wal_path[1200];

    char temp_path[1200];

    uint64_t cache_limit;

    uint64_t memory_used;

    uint64_t next_version;

    uint64_t writes;

    uint64_t reads;

    uint64_t hits;

    uint64_t misses;

    uint64_t deletes;

    uint64_t evictions;

    uint64_t recoveries;

    uint64_t compactions;

    bool opened;

} StorageEngine;


/* ============================================================
 * Engine initialisation
 * ============================================================ */

static bool storage_engine_init(
    StorageEngine *engine,
    const char *directory)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    if (!storage_table_init(
            &engine->table,
            TS_DEFAULT_BUCKETS))
    {
        return false;
    }

    strncpy(
        engine->directory,
        directory,
        sizeof(engine->directory) - 1
    );

    engine->directory[
        sizeof(engine->directory) - 1
    ] = '\0';

    snprintf(
        engine->data_path,
        sizeof(engine->data_path),
        "%s/%s",
        engine->directory,
        TS_DATA_FILE
    );

    snprintf(
        engine->wal_path,
        sizeof(engine->wal_path),
        "%s/%s",
        engine->directory,
        TS_WAL_FILE
    );

    snprintf(
        engine->temp_path,
        sizeof(engine->temp_path),
        "%s/%s",
        engine->directory,
        TS_TEMP_FILE
    );

    engine->cache_limit =
        TS_DEFAULT_CACHE_LIMIT;

    engine->next_version = 1;

    return true;
}


/* ============================================================
 * Memory accounting
 * ============================================================ */

static size_t storage_entry_memory(
    const StorageEntry *entry)
{
    if (!entry)
        return 0;

    return
        sizeof(StorageEntry) +
        strlen(entry->namespace_name) + 1 +
        strlen(entry->key) + 1 +
        entry->value_size;
}


static void storage_recalculate_memory(
    StorageEngine *engine)
{
    engine->memory_used = 0;

    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry *entry =
            engine->table.buckets[i];

        while (entry) {

            engine->memory_used +=
                storage_entry_memory(
                    entry
                );

            entry = entry->next;
        }
    }
}


/* ============================================================
 * WAL append
 * ============================================================ */

static bool storage_wal_append(
    StorageEngine *engine,
    StorageWALType operation,
    const char *namespace_name,
    const char *key,
    const void *value,
    size_t value_size,
    uint64_t version)
{
    FILE *file =
        fopen(
            engine->wal_path,
            "ab"
        );

    if (!file)
        return false;

    StorageWALHeader header;

    memset(
        &header,
        0,
        sizeof(header)
    );

    header.magic =
        TS_STORAGE_MAGIC;

    header.version =
        TS_STORAGE_VERSION;

    header.operation =
        (uint32_t)operation;

    header.namespace_size =
        strlen(namespace_name);

    header.key_size =
        strlen(key);

    header.value_size =
        value_size;

    header.entry_version =
        version;

    header.timestamp =
        storage_now_ms();

    uint64_t checksum =
        storage_hash(
            namespace_name,
            header.namespace_size
        );

    checksum ^=
        storage_hash(
            key,
            header.key_size
        );

    if (value && value_size > 0)
        checksum ^=
            storage_hash(
                value,
                value_size
            );

    header.checksum =
        checksum;

    bool success = true;

    if (fwrite(
            &header,
            sizeof(header),
            1,
            file) != 1)
    {
        success = false;
    }

    if (success &&
        header.namespace_size > 0)
    {
        if (fwrite(
                namespace_name,
                1,
                header.namespace_size,
                file) !=
            header.namespace_size)
        {
            success = false;
        }
    }

    if (success &&
        header.key_size > 0)
    {
        if (fwrite(
                key,
                1,
                header.key_size,
                file) !=
            header.key_size)
        {
            success = false;
        }
    }

    if (success &&
        value &&
        value_size > 0)
    {
        if (fwrite(
                value,
                1,
                value_size,
                file) !=
            value_size)
        {
            success = false;
        }
    }

    fflush(file);

    fclose(file);

    return success;
}


/* ============================================================
 * Set
 * ============================================================ */

static bool storage_set(
    StorageEngine *engine,
    const char *namespace_name,
    const char *key,
    const void *value,
    size_t value_size,
    uint64_t ttl_ms)
{
    if (!engine ||
        !key ||
        !namespace_name)
        return false;

    if (value_size >
        TS_MAX_VALUE_SIZE)
    {
        return false;
    }

    StorageEntry *entry =
        storage_table_find(
            &engine->table,
            namespace_name,
            key
        );

    uint64_t version =
        engine->next_version++;

    if (!entry) {

        entry =
            storage_entry_create(
                namespace_name,
                key,
                value,
                value_size
            );

        if (!entry)
            return false;

        entry->hash =
            storage_composite_hash(
                namespace_name,
                key
            );

        entry->version =
            version;

        if (ttl_ms > 0)
            entry->expires_at =
                storage_now_ms() + ttl_ms;

        entry->dirty = true;
        entry->persistent = true;

        if (!storage_wal_append(
                engine,
                STORAGE_WAL_SET,
                namespace_name,
                key,
                value,
                value_size,
                version))
        {
            storage_entry_destroy(
                entry
            );

            return false;
        }

        storage_table_insert(
            &engine->table,
            entry
        );

    } else {

        unsigned char *new_value = NULL;

        if (value_size > 0) {

            new_value =
                malloc(value_size);

            if (!new_value)
                return false;

            memcpy(
                new_value,
                value,
                value_size
            );
        }

        if (!storage_wal_append(
                engine,
                STORAGE_WAL_SET,
                namespace_name,
                key,
                value,
                value_size,
                version))
        {
            free(new_value);
            return false;
        }

        free(entry->value);

        entry->value =
            new_value;

        entry->value_size =
            value_size;

        entry->version =
            version;

        entry->updated_at =
            storage_now_ms();

        entry->last_access =
            entry->updated_at;

        entry->expires_at =
            ttl_ms > 0 ?
            entry->updated_at + ttl_ms :
            0;

        entry->dirty = true;
        entry->tombstone = false;
    }

    engine->writes++;

    storage_recalculate_memory(
        engine
    );

    return true;
}


/* ============================================================
 * Get
 * ============================================================ */

static bool storage_get(
    StorageEngine *engine,
    const char *namespace_name,
    const char *key,
    StorageValue *output)
{
    if (!engine ||
        !namespace_name ||
        !key ||
        !output)
    {
        return false;
    }

    output->data = NULL;
    output->size = 0;

    engine->reads++;

    StorageEntry *entry =
        storage_table_find(
            &engine->table,
            namespace_name,
            key
        );

    if (!entry ||
        entry->tombstone)
    {
        engine->misses++;
        return false;
    }

    uint64_t now =
        storage_now_ms();

    if (entry->expires_at > 0 &&
        now >= entry->expires_at)
    {
        engine->misses++;

        return false;
    }

    if (!storage_value_copy(
            output,
            entry->value,
            entry->value_size))
    {
        engine->misses++;
        return false;
    }

    entry->last_access =
        now;

    engine->hits++;

    return true;
}


/* ============================================================
 * Delete
 * ============================================================ */

static bool storage_delete(
    StorageEngine *engine,
    const char *namespace_name,
    const char *key)
{
    StorageEntry *entry =
        storage_table_find(
            &engine->table,
            namespace_name,
            key
        );

    if (!entry)
        return false;

    if (!storage_wal_append(
            engine,
            STORAGE_WAL_DELETE,
            namespace_name,
            key,
            NULL,
            0,
            engine->next_version++))
    {
        return false;
    }

    entry->tombstone = true;
    entry->dirty = true;
    entry->updated_at =
        storage_now_ms();

    engine->deletes++;

    return true;
}


/* ============================================================
 * TTL cleanup
 * ============================================================ */

static size_t storage_expire_entries(
    StorageEngine *engine)
{
    uint64_t now =
        storage_now_ms();

    size_t expired = 0;

    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry *entry =
            engine->table.buckets[i];

        while (entry) {

            if (!entry->tombstone &&
                entry->expires_at > 0 &&
                now >= entry->expires_at)
            {
                entry->tombstone = true;
                entry->dirty = true;

                expired++;
            }

            entry = entry->next;
        }
    }

    return expired;
}


/* ============================================================
 * LRU eviction
 * ============================================================ */

static StorageEntry *storage_find_lru(
    StorageEngine *engine)
{
    StorageEntry *oldest = NULL;

    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry *entry =
            engine->table.buckets[i];

        while (entry) {

            if (!entry->tombstone) {

                if (!oldest ||
                    entry->last_access <
                    oldest->last_access)
                {
                    oldest = entry;
                }
            }

            entry = entry->next;
        }
    }

    return oldest;
}


static size_t storage_evict(
    StorageEngine *engine)
{
    size_t count = 0;

    storage_recalculate_memory(
        engine
    );

    while (
        engine->memory_used >
        engine->cache_limit)
    {
        StorageEntry *entry =
            storage_find_lru(
                engine
            );

        if (!entry)
            break;

        entry->tombstone = true;

        entry->dirty = true;

        engine->evictions++;

        count++;

        storage_recalculate_memory(
            engine
        );
    }

    return count;
}


/* ============================================================
 * Data file format
 * ============================================================ */

typedef struct {

    uint32_t magic;

    uint32_t version;

    uint64_t namespace_size;

    uint64_t key_size;

    uint64_t value_size;

    uint64_t entry_version;

    uint64_t created_at;

    uint64_t updated_at;

    uint64_t expires_at;

    uint64_t checksum;

    uint8_t tombstone;

    uint8_t reserved[7];

} StorageRecord;


/* ============================================================
 * Write record
 * ============================================================ */

static bool storage_write_record(
    FILE *file,
    const StorageEntry *entry)
{
    StorageRecord record;

    memset(
        &record,
        0,
        sizeof(record)
    );

    record.magic =
        TS_STORAGE_MAGIC;

    record.version =
        TS_STORAGE_VERSION;

    record.namespace_size =
        strlen(entry->namespace_name);

    record.key_size =
        strlen(entry->key);

    record.value_size =
        entry->value_size;

    record.entry_version =
        entry->version;

    record.created_at =
        entry->created_at;

    record.updated_at =
        entry->updated_at;

    record.expires_at =
        entry->expires_at;

    record.tombstone =
        entry->tombstone ? 1 : 0;

    uint64_t checksum =
        storage_hash(
            entry->namespace_name,
            record.namespace_size
        );

    checksum ^=
        storage_hash(
            entry->key,
            record.key_size
        );

    if (entry->value &&
        entry->value_size > 0)
    {
        checksum ^=
            storage_hash(
                entry->value,
                entry->value_size
            );
    }

    record.checksum =
        checksum;

    if (fwrite(
            &record,
            sizeof(record),
            1,
            file) != 1)
    {
        return false;
    }

    if (record.namespace_size > 0) {

        if (fwrite(
                entry->namespace_name,
                1,
                record.namespace_size,
                file) !=
            record.namespace_size)
        {
            return false;
        }
    }

    if (record.key_size > 0) {

        if (fwrite(
                entry->key,
                1,
                record.key_size,
                file) !=
            record.key_size)
        {
            return false;
        }
    }

    if (record.value_size > 0) {

        if (fwrite(
                entry->value,
                1,
                record.value_size,
                file) !=
            record.value_size)
        {
            return false;
        }
    }

    return true;
}


/* ============================================================
 * Persistent snapshot
 * ============================================================ */

static bool storage_save_snapshot(
    StorageEngine *engine)
{
    FILE *file =
        fopen(
            engine->temp_path,
            "wb"
        );

    if (!file)
        return false;

    bool success = true;

    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry *entry =
            engine->table.buckets[i];

        while (entry) {

            if (!storage_write_record(
                    file,
                    entry))
            {
                success = false;
                break;
            }

            entry = entry->next;
        }

        if (!success)
            break;
    }

    fflush(file);

    fclose(file);

    if (!success) {
        remove(
            engine->temp_path
        );

        return false;
    }

    /*
     * Atomic-ish replacement.
     *
     * On production systems this should be strengthened
     * with fsync + directory fsync and platform-specific
     * atomic replacement APIs.
     */
    remove(
        engine->data_path
    );

    if (rename(
            engine->temp_path,
            engine->data_path) != 0)
    {
        return false;
    }

    return true;
}


/* ============================================================
 * Load one record
 * ============================================================ */

static bool storage_load_record(
    StorageEngine *engine,
    FILE *file)
{
    StorageRecord record;

    size_t n =
        fread(
            &record,
            1,
            sizeof(record),
            file
        );

    if (n == 0)
        return false;

    if (n != sizeof(record))
        return false;

    if (record.magic !=
        TS_STORAGE_MAGIC)
    {
        return false;
    }

    if (record.version !=
        TS_STORAGE_VERSION)
    {
        return false;
    }

    if (record.namespace_size >
        TS_MAX_NAMESPACE)
    {
        return false;
    }

    if (record.key_size >
        TS_MAX_KEY)
    {
        return false;
    }

    if (record.value_size >
        TS_MAX_VALUE_SIZE)
    {
        return false;
    }

    char *namespace_name =
        calloc(
            1,
            record.namespace_size + 1
        );

    char *key =
        calloc(
            1,
            record.key_size + 1
        );

    unsigned char *value =
        NULL;

    if (record.value_size > 0) {

        value =
            malloc(
                record.value_size
            );
    }

    if (!namespace_name ||
        !key ||
        (record.value_size > 0 &&
         !value))
    {
        free(namespace_name);
        free(key);
        free(value);

        return false;
    }

    bool success = true;

    if (record.namespace_size > 0) {

        if (fread(
                namespace_name,
                1,
                record.namespace_size,
                file) !=
            record.namespace_size)
        {
            success = false;
        }
    }

    if (success &&
        record.key_size > 0)
    {
        if (fread(
                key,
                1,
                record.key_size,
                file) !=
            record.key_size)
        {
            success = false;
        }
    }

    if (success &&
        record.value_size > 0)
    {
        if (fread(
                value,
                1,
                record.value_size,
                file) !=
            record.value_size)
        {
            success = false;
        }
    }

    if (!success) {

        free(namespace_name);
        free(key);
        free(value);

        return false;
    }

    uint64_t checksum =
        storage_hash(
            namespace_name,
            record.namespace_size
        );

    checksum ^=
        storage_hash(
            key,
            record.key_size
        );

    if (value &&
        record.value_size > 0)
    {
        checksum ^=
            storage_hash(
                value,
                record.value_size
            );
    }

    if (checksum !=
        record.checksum)
    {
        free(namespace_name);
        free(key);
        free(value);

        return false;
    }

    StorageEntry *existing =
        storage_table_find(
            &engine->table,
            namespace_name,
            key
        );

    if (existing) {

        if (record.entry_version <
            existing->version)
        {
            free(namespace_name);
            free(key);
            free(value);

            return true;
        }

        free(existing->value);

        existing->value = value;

        existing->value_size =
            record.value_size;

        existing->version =
            record.entry_version;

        existing->created_at =
            record.created_at;

        existing->updated_at =
            record.updated_at;

        existing->expires_at =
            record.expires_at;

        existing->tombstone =
            record.tombstone != 0;

        existing->persistent = true;

    } else {

        StorageEntry *entry =
            calloc(
                1,
                sizeof(StorageEntry)
            );

        if (!entry) {

            free(namespace_name);
            free(key);
            free(value);

            return false;
        }

        entry->namespace_name =
            namespace_name;

        entry->key =
            key;

        entry->value =
            value;

        entry->value_size =
            record.value_size;

        entry->version =
            record.entry_version;

        entry->created_at =
            record.created_at;

        entry->updated_at =
            record.updated_at;

        entry->last_access =
            record.updated_at;

        entry->expires_at =
            record.expires_at;

        entry->tombstone =
            record.tombstone != 0;

        entry->persistent = true;

        entry->hash =
            storage_composite_hash(
                namespace_name,
                key
            );

        storage_table_insert(
            &engine->table,
            entry
        );
    }

    if (record.entry_version >=
        engine->next_version)
    {
        engine->next_version =
            record.entry_version + 1;
    }

    return true;
}


/* ============================================================
 * Load snapshot
 * ============================================================ */

static bool storage_load_snapshot(
    StorageEngine *engine)
{
    FILE *file =
        fopen(
            engine->data_path,
            "rb"
        );

    if (!file) {

        if (errno == ENOENT)
            return true;

        return false;
    }

    while (!feof(file)) {

        long position =
            ftell(file);

        if (position < 0)
            break;

        if (!storage_load_record(
                engine,
                file))
        {
            /*
             * A truncated final record can happen after
             * an interrupted write. Stop rather than
             * corrupting previously loaded state.
             */
            break;
        }
    }

    fclose(file);

    storage_recalculate_memory(
        engine
    );

    return true;
}


/* ============================================================
 * WAL recovery
 * ============================================================ */

static bool storage_apply_wal_record(
    StorageEngine *engine,
    const StorageWALHeader *header,
    const char *namespace_name,
    const char *key,
    const unsigned char *value)
{
    StorageEntry *entry =
        storage_table_find(
            &engine->table,
            namespace_name,
            key
        );

    if (entry &&
        header->entry_version <
        entry->version)
    {
        return true;
    }

    if (header->operation ==
        STORAGE_WAL_SET)
    {
        if (!entry) {

            entry =
                storage_entry_create(
                    namespace_name,
                    key,
                    value,
                    header->value_size
                );

            if (!entry)
                return false;

            entry->hash =
                storage_composite_hash(
                    namespace_name,
                    key
                );

            entry->version =
                header->entry_version;

            entry->updated_at =
                header->timestamp;

            entry->persistent = true;

            storage_table_insert(
                &engine->table,
                entry
            );

        } else {

            unsigned char *new_value =
                NULL;

            if (header->value_size > 0) {

                new_value =
                    malloc(
                        header->value_size
                    );

                if (!new_value)
                    return false;

                memcpy(
                    new_value,
                    value,
                    header->value_size
                );
            }

            free(entry->value);

            entry->value =
                new_value;

            entry->value_size =
                header->value_size;

            entry->version =
                header->entry_version;

            entry->updated_at =
                header->timestamp;

            entry->tombstone = false;
        }
    }
    else if (header->operation ==
             STORAGE_WAL_DELETE)
    {
        if (entry) {

            entry->tombstone = true;

            entry->version =
                header->entry_version;

            entry->updated_at =
                header->timestamp;
        }
    }

    if (header->entry_version >=
        engine->next_version)
    {
        engine->next_version =
            header->entry_version + 1;
    }

    return true;
}


static bool storage_recover_wal(
    StorageEngine *engine)
{
    FILE *file =
        fopen(
            engine->wal_path,
            "rb"
        );

    if (!file) {

        if (errno == ENOENT)
            return true;

        return false;
    }

    bool recovered_any =
        false;

    while (1) {

        StorageWALHeader header;

        size_t n =
            fread(
                &header,
                1,
                sizeof(header),
                file
            );

        if (n == 0)
            break;

        if (n != sizeof(header))
            break;

        if (header.magic !=
            TS_STORAGE_MAGIC)
        {
            break;
        }

        if (header.version !=
            TS_STORAGE_VERSION)
        {
            break;
        }

        if (header.namespace_size >
            TS_MAX_NAMESPACE ||
            header.key_size >
            TS_MAX_KEY ||
            header.value_size >
            TS_MAX_VALUE_SIZE)
        {
            break;
        }

        char *namespace_name =
            calloc(
                1,
                header.namespace_size + 1
            );

        char *key =
            calloc(
                1,
                header.key_size + 1
            );

        unsigned char *value =
            NULL;

        if (header.value_size > 0) {

            value =
                malloc(
                    header.value_size
                );
        }

        if (!namespace_name ||
            !key ||
            (header.value_size > 0 &&
             !value))
        {
            free(namespace_name);
            free(key);
            free(value);

            break;
        }

        bool success = true;

        if (header.namespace_size > 0) {

            if (fread(
                    namespace_name,
                    1,
                    header.namespace_size,
                    file) !=
                header.namespace_size)
            {
                success = false;
            }
        }

        if (success &&
            header.key_size > 0)
        {
            if (fread(
                    key,
                    1,
                    header.key_size,
                    file) !=
                header.key_size)
            {
                success = false;
            }
        }

        if (success &&
            header.value_size > 0)
        {
            if (fread(
                    value,
                    1,
                    header.value_size,
                    file) !=
                header.value_size)
            {
                success = false;
            }
        }

        if (!success) {

            free(namespace_name);
            free(key);
            free(value);

            break;
        }

        uint64_t checksum =
            storage_hash(
                namespace_name,
                header.namespace_size
            );

        checksum ^=
            storage_hash(
                key,
                header.key_size
            );

        if (value &&
            header.value_size > 0)
        {
            checksum ^=
                storage_hash(
                    value,
                    header.value_size
                );
        }

        if (checksum !=
            header.checksum)
        {
            free(namespace_name);
            free(key);
            free(value);

            break;
        }

        if (!storage_apply_wal_record(
                engine,
                &header,
                namespace_name,
                key,
                value))
        {
            free(namespace_name);
            free(key);
            free(value);

            break;
        }

        recovered_any = true;

        free(namespace_name);
        free(key);
        free(value);
    }

    fclose(file);

    if (recovered_any)
        engine->recoveries++;

    storage_recalculate_memory(
        engine
    );

    return true;
}


/* ============================================================
 * Open
 * ============================================================ */

static bool storage_open(
    StorageEngine *engine)
{
    /*
     * Create directory.
     */
    struct stat st;

    if (stat(
            engine->directory,
            &st) != 0)
    {
        if (mkdir_if_needed(
                engine->directory) != 0 &&
            errno != EEXIST)
        {
            return false;
        }
    }

    if (!storage_load_snapshot(
            engine))
    {
        return false;
    }

    if (!storage_recover_wal(
            engine))
    {
        return false;
    }

    engine->opened = true;

    return true;
}


/* ============================================================
 * Clear WAL
 * ============================================================ */

static bool storage_clear_wal(
    StorageEngine *engine)
{
    FILE *file =
        fopen(
            engine->wal_path,
            "wb"
        );

    if (!file)
        return false;

    fflush(file);

    fclose(file);

    return true;
}


/* ============================================================
 * Compact
 * ============================================================ */

static bool storage_compact(
    StorageEngine *engine)
{
    /*
     * First write the latest state to a new snapshot.
     */
    if (!storage_save_snapshot(
            engine))
    {
        return false;
    }

    /*
     * Once the snapshot exists, the WAL can be cleared.
     */
    if (!storage_clear_wal(
            engine))
    {
        return false;
    }

    /*
     * Remove tombstones from the hash table.
     */
    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry **cursor =
            &engine->table.buckets[i];

        while (*cursor) {

            StorageEntry *entry =
                *cursor;

            if (entry->tombstone) {

                *cursor =
                    entry->next;

                storage_entry_destroy(
                    entry
                );

                engine->table.count--;

            } else {

                entry->dirty = false;

                cursor =
                    &entry->next;
            }
        }
    }

    engine->compactions++;

    storage_recalculate_memory(
        engine
    );

    return true;
}


/* ============================================================
 * Flush
 * ============================================================ */

static bool storage_flush(
    StorageEngine *engine)
{
    if (!storage_save_snapshot(
            engine))
    {
        return false;
    }

    if (!storage_clear_wal(
            engine))
    {
        return false;
    }

    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry *entry =
            engine->table.buckets[i];

        while (entry) {

            entry->dirty = false;

            entry = entry->next;
        }
    }

    return true;
}


/* ============================================================
 * Namespace enumeration
 * ============================================================ */

static size_t storage_count_namespace(
    StorageEngine *engine,
    const char *namespace_name)
{
    size_t count = 0;

    for (size_t i = 0;
         i < engine->table.bucket_count;
         ++i)
    {
        StorageEntry *entry =
            engine->table.buckets[i];

        while (entry) {

            if (!entry->tombstone &&
                strcmp(
                    entry->namespace_name,
                    namespace_name
                ) == 0)
            {
                count++;
            }

            entry = entry->next;
        }
    }

    return count;
}


/* ============================================================
 * Statistics
 * ============================================================ */

static void storage_print_stats(
    StorageEngine *engine)
{
    printf("\n");
    printf(
        "============================================\n"
    );

    printf(
        " LOCAL STORAGE ENGINE\n"
    );

    printf(
        "============================================\n"
    );

    printf(
        "Entries:              %zu\n",
        engine->table.count
    );

    printf(
        "Memory used:          %llu bytes\n",
        (unsigned long long)
        engine->memory_used
    );

    printf(
        "Cache limit:          %llu bytes\n",
        (unsigned long long)
        engine->cache_limit
    );

    printf(
        "Writes:               %llu\n",
        (unsigned long long)
        engine->writes
    );

    printf(
        "Reads:                %llu\n",
        (unsigned long long)
        engine->reads
    );

    printf(
        "Cache hits:           %llu\n",
        (unsigned long long)
        engine->hits
    );

    printf(
        "Cache misses:         %llu\n",
        (unsigned long long)
        engine->misses
    );

    printf(
        "Deletes:              %llu\n",
        (unsigned long long)
        engine->deletes
    );

    printf(
        "Evictions:            %llu\n",
        (unsigned long long)
        engine->evictions
    );

    printf(
        "Recoveries:           %llu\n",
        (unsigned long long)
        engine->recoveries
    );

    printf(
        "Compactions:          %llu\n",
        (unsigned long long)
        engine->compactions
    );

    if (engine->reads > 0) {

        printf(
            "Hit ratio:            %.2f%%\n",
            100.0 *
            (double)engine->hits /
            (double)engine->reads
        );
    }

    printf(
        "============================================\n"
    );
}


/* ============================================================
 * Dump entry
 * ============================================================ */

static void storage_dump_entry(
    StorageEngine *engine,
    const char *namespace_name,
    const char *key)
{
    StorageEntry *entry =
        storage_table_find(
            &engine->table,
            namespace_name,
            key
        );

    if (!entry) {

        printf(
            "Entry not found: %s/%s\n",
            namespace_name,
            key
        );

        return;
    }

    printf("\n");
    printf(
        "Entry: %s/%s\n",
        namespace_name,
        key
    );

    printf(
        "Version:      %llu\n",
        (unsigned long long)
        entry->version
    );

    printf(
        "Size:         %zu\n",
        entry->value_size
    );

    printf(
        "Dirty:        %s\n",
        entry->dirty ? "yes" : "no"
    );

    printf(
        "Tombstone:    %s\n",
        entry->tombstone ? "yes" : "no"
    );

    printf(
        "Persistent:   %s\n",
        entry->persistent ? "yes" : "no"
    );

    printf(
        "Expires:      %llu\n",
        (unsigned long long)
        entry->expires_at
    );

    if (entry->value &&
        entry->value_size > 0)
    {
        /*
         * Only print values that appear to be text.
         */
        bool printable = true;

        for (size_t i = 0;
             i < entry->value_size;
             ++i)
        {
            unsigned char c =
                entry->value[i];

            if (c < 9 ||
                (c > 13 && c < 32))
            {
                printable = false;
                break;
            }
        }

        if (printable) {

            printf(
                "Value:        %.*s\n",
                (int)entry->value_size,
                (const char *)entry->value
            );
        }
    }
}


/* ============================================================
 * Demo
 * ============================================================ */

static void storage_demo(
    StorageEngine *engine)
{
    const char *message1 =
        "{\"sender\":\"alice\","
        "\"channel\":\"engineering\","
        "\"text\":\"hello world\"}";

    const char *message2 =
        "{\"sender\":\"bob\","
        "\"channel\":\"product\","
        "\"text\":\"meeting tomorrow\"}";

    const char *settings =
        "{\"theme\":\"dark\","
        "\"animations\":true,"
        "\"density\":\"comfortable\"}";

    printf(
        "\nWriting local messages...\n"
    );

    storage_set(
        engine,
        "messages",
        "message/1",
        message1,
        strlen(message1),
        0
    );

    storage_set(
        engine,
        "messages",
        "message/2",
        message2,
        strlen(message2),
        0
    );

    printf(
        "Writing settings...\n"
    );

    storage_set(
        engine,
        "settings",
        "ui/preferences",
        settings,
        strlen(settings),
        0
    );

    printf(
        "Writing temporary presence state...\n"
    );

    const char *presence =
        "{\"state\":\"available\"}";

    storage_set(
        engine,
        "presence",
        "alice",
        presence,
        strlen(presence),
        10000
    );

    /*
     * Read.
     */
    StorageValue value;

    if (storage_get(
            engine,
            "messages",
            "message/1",
            &value))
    {
        printf(
            "\nREAD message/1:\n%s\n",
            (char *)value.data
        );

        storage_value_destroy(
            &value
        );
    }

    /*
     * Update.
     */
    const char *updated =
        "{\"sender\":\"alice\","
        "\"channel\":\"engineering\","
        "\"text\":\"hello from the updated client\"}";

    storage_set(
        engine,
        "messages",
        "message/1",
        updated,
        strlen(updated),
        0
    );

    /*
     * Delete.
     */
    storage_delete(
        engine,
        "messages",
        "message/2"
    );

    /*
     * Namespace statistics.
     */
    printf(
        "\nMessages namespace: %zu entries\n",
        storage_count_namespace(
            engine,
            "messages"
        )
    );

    printf(
        "Settings namespace: %zu entries\n",
        storage_count_namespace(
            engine,
            "settings"
        )
    );

    /*
     * Dump.
     */
    storage_dump_entry(
        engine,
        "messages",
        "message/1"
    );

    /*
     * TTL cleanup.
     */
    size_t expired =
        storage_expire_entries(
            engine
        );

    printf(
        "\nExpired entries: %zu\n",
        expired
    );

    /*
     * Force cache policy.
     */
    storage_evict(
        engine
    );

    storage_print_stats(
        engine
    );
}


/* ============================================================
 * Crash simulation
 * ============================================================ */

static void storage_crash_recovery_demo(
    const char *directory)
{
    printf(
        "\n============================================\n"
    );

    printf(
        " CRASH RECOVERY TEST\n"
    );

    printf(
        "============================================\n"
    );

    StorageEngine engine;

    if (!storage_engine_init(
            &engine,
            directory))
    {
        return;
    }

    if (!storage_open(
            &engine))
    {
        storage_table_destroy(
            &engine.table
        );

        return;
    }

    const char *value =
        "{\"offline\":true,"
        "\"queued\":\"message\"}";

    /*
     * This writes to the WAL immediately.
     */
    storage_set(
        &engine,
        "offline",
        "queued/1",
        value,
        strlen(value),
        0
    );

    storage_print_stats(
        &engine
    );

    /*
     * Intentionally do NOT flush.
     *
     * The WAL should allow the next engine instance
     * to reconstruct the update.
     */
    storage_table_destroy(
        &engine.table
    );

    printf(
        "\nSimulated abrupt process termination.\n"
    );

    StorageEngine recovered;

    if (!storage_engine_init(
            &recovered,
            directory))
    {
        return;
    }

    if (!storage_open(
            &recovered))
    {
        storage_table_destroy(
            &recovered.table
        );

        return;
    }

    StorageValue result;

    if (storage_get(
            &recovered,
            "offline",
            "queued/1",
            &result))
    {
        printf(
            "RECOVERED:\n%s\n",
            (char *)result.data
        );

        storage_value_destroy(
            &result
        );
    }
    else {

        printf(
            "Recovery failed.\n"
        );
    }

    storage_print_stats(
        &recovered
    );

    storage_flush(
        &recovered
    );

    storage_table_destroy(
        &recovered.table
    );
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    const char *directory =
        "./teams_storage";

    printf(
        "Teams-style Local Storage Engine\n"
    );

    printf(
        "Directory: %s\n",
        directory
    );

    StorageEngine engine;

    if (!storage_engine_init(
            &engine,
            directory))
    {
        fprintf(
            stderr,
            "Could not initialise storage engine.\n"
        );

        return EXIT_FAILURE;
    }

    if (!storage_open(
            &engine))
    {
        fprintf(
            stderr,
            "Could not open storage engine.\n"
        );

        storage_table_destroy(
            &engine.table
        );

        return EXIT_FAILURE;
    }

    /*
     * Main demonstration.
     */
    storage_demo(
        &engine
    );

    /*
     * Save everything.
     */
    printf(
        "\nFlushing storage...\n"
    );

    if (!storage_flush(
            &engine))
    {
        fprintf(
            stderr,
            "Storage flush failed.\n"
        );
    }

    storage_print_stats(
        &engine
    );

    /*
     * Compact.
     */
    printf(
        "\nCompacting storage...\n"
    );

    if (storage_compact(
            &engine))
    {
        printf(
            "Compaction completed.\n"
        );
    }
    else {

        printf(
            "Compaction failed.\n"
        );
    }

    storage_print_stats(
        &engine
    );

    /*
     * Demonstrate crash recovery.
     */
    storage_table_destroy(
        &engine.table
    );

    storage_crash_recovery_demo(
        directory
    );

    printf(
        "\nStorage engine shutdown.\n"
    );

    return EXIT_SUCCESS;
}









/*
 * teams_screen_capture.c
 *
 * Native C11 screen-sharing capture engine.
 *
 * Core responsibilities:
 *
 *   - Desktop/window capture abstraction
 *   - Frame buffers
 *   - RGB/BGRA conversion
 *   - Dirty-region detection
 *   - Tile-based change detection
 *   - Frame differencing
 *   - Cursor composition
 *   - Adaptive capture FPS
 *   - Adaptive resolution
 *   - Static-screen detection
 *   - Scrolling/change detection
 *   - Frame pacing
 *   - Capture statistics
 *   - Quality profiles
 *   - Encoder handoff abstraction
 *
 * Build:
 *
 *   gcc -std=c11 -O3 -march=native \
 *       -Wall -Wextra -pedantic \
 *       teams_screen_capture.c -o teams_screen_capture -lm
 *
 * This file deliberately does NOT implement Microsoft's
 * proprietary screen-sharing protocol.
 *
 * The CaptureBackend interface can later be connected to:
 *
 *   Windows:
 *       Desktop Duplication API / Windows Graphics Capture
 *
 *   macOS:
 *       ScreenCaptureKit
 *
 *   Linux:
 *       PipeWire / Wayland / X11
 *
 * The processing engine remains platform independent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>


/* ============================================================
 * Configuration
 * ============================================================ */

#define SC_MAX_WIDTH             7680
#define SC_MAX_HEIGHT            4320

#define SC_DEFAULT_TILE          32

#define SC_MAX_DIRTY_REGIONS     2048

#define SC_DEFAULT_FPS           30.0
#define SC_MIN_FPS               1.0
#define SC_MAX_FPS               60.0

#define SC_STATIC_THRESHOLD      0.0015
#define SC_LOW_CHANGE_THRESHOLD  0.01
#define SC_HIGH_CHANGE_THRESHOLD 0.15

#define SC_CURSOR_MAX_SIZE       256


/* ============================================================
 * Time
 * ============================================================ */

static uint64_t screen_now_ns(void)
{
    struct timespec ts;

    if (timespec_get(
            &ts,
            TIME_UTC) != TIME_UTC)
    {
        return 0;
    }

    return
        (uint64_t)ts.tv_sec * 1000000000ULL +
        (uint64_t)ts.tv_nsec;
}


/* ============================================================
 * Clamp
 * ============================================================ */

static int screen_clamp_int(
    int value,
    int minimum,
    int maximum)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}


static double screen_clamp_double(
    double value,
    double minimum,
    double maximum)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}


/* ============================================================
 * Pixel formats
 * ============================================================ */

typedef enum {

    SC_FORMAT_BGRA32 = 0,
    SC_FORMAT_RGBA32,
    SC_FORMAT_RGB24,
    SC_FORMAT_GRAY8

} ScreenPixelFormat;


/* ============================================================
 * Frame
 * ============================================================ */

typedef struct {

    int width;

    int height;

    int stride;

    ScreenPixelFormat format;

    uint8_t *data;

    size_t size;

    uint64_t timestamp_ns;

    uint64_t sequence;

} ScreenFrame;


/* ============================================================
 * Frame allocation
 * ============================================================ */

static bool screen_frame_init(
    ScreenFrame *frame,
    int width,
    int height,
    ScreenPixelFormat format)
{
    if (!frame ||
        width <= 0 ||
        height <= 0)
    {
        return false;
    }

    memset(
        frame,
        0,
        sizeof(*frame)
    );

    int bytes_per_pixel;

    switch (format) {

        case SC_FORMAT_BGRA32:
        case SC_FORMAT_RGBA32:
            bytes_per_pixel = 4;
            break;

        case SC_FORMAT_RGB24:
            bytes_per_pixel = 3;
            break;

        case SC_FORMAT_GRAY8:
            bytes_per_pixel = 1;
            break;

        default:
            return false;
    }

    frame->width = width;
    frame->height = height;

    frame->stride =
        width * bytes_per_pixel;

    frame->format =
        format;

    frame->size =
        (size_t)frame->stride *
        (size_t)height;

    frame->data =
        malloc(frame->size);

    if (!frame->data) {

        memset(
            frame,
            0,
            sizeof(*frame)
        );

        return false;
    }

    memset(
        frame->data,
        0,
        frame->size
    );

    return true;
}


static void screen_frame_destroy(
    ScreenFrame *frame)
{
    if (!frame)
        return;

    free(frame->data);

    memset(
        frame,
        0,
        sizeof(*frame)
    );
}


static bool screen_frame_resize(
    ScreenFrame *frame,
    int width,
    int height)
{
    if (!frame)
        return false;

    ScreenFrame replacement;

    if (!screen_frame_init(
            &replacement,
            width,
            height,
            frame->format))
    {
        return false;
    }

    screen_frame_destroy(
        frame
    );

    *frame =
        replacement;

    return true;
}


/* ============================================================
 * Frame copy
 * ============================================================ */

static bool screen_frame_copy(
    ScreenFrame *destination,
    const ScreenFrame *source)
{
    if (!destination ||
        !source)
    {
        return false;
    }

    if (destination->width !=
        source->width ||
        destination->height !=
        source->height ||
        destination->format !=
        source->format)
    {
        return false;
    }

    memcpy(
        destination->data,
        source->data,
        source->size
    );

    destination->timestamp_ns =
        source->timestamp_ns;

    destination->sequence =
        source->sequence;

    return true;
}


/* ============================================================
 * BGRA -> RGB
 * ============================================================ */

static void screen_bgra_to_rgb(
    const ScreenFrame *source,
    ScreenFrame *destination)
{
    if (!source ||
        !destination)
        return;

    if (source->format !=
        SC_FORMAT_BGRA32 ||
        destination->format !=
        SC_FORMAT_RGB24)
    {
        return;
    }

    for (int y = 0;
         y < source->height;
         ++y)
    {
        const uint8_t *src =
            source->data +
            (size_t)y *
            source->stride;

        uint8_t *dst =
            destination->data +
            (size_t)y *
            destination->stride;

        for (int x = 0;
             x < source->width;
             ++x)
        {
            dst[x * 3 + 0] =
                src[x * 4 + 2];

            dst[x * 3 + 1] =
                src[x * 4 + 1];

            dst[x * 3 + 2] =
                src[x * 4 + 0];
        }
    }
}


/* ============================================================
 * RGB -> grayscale
 * ============================================================ */

static void screen_rgb_to_gray(
    const ScreenFrame *source,
    ScreenFrame *destination)
{
    if (!source ||
        !destination)
        return;

    if (source->format !=
        SC_FORMAT_RGB24 ||
        destination->format !=
        SC_FORMAT_GRAY8)
    {
        return;
    }

    for (int y = 0;
         y < source->height;
         ++y)
    {
        const uint8_t *src =
            source->data +
            (size_t)y *
            source->stride;

        uint8_t *dst =
            destination->data +
            (size_t)y *
            destination->stride;

        for (int x = 0;
             x < source->width;
             ++x)
        {
            int r =
                src[x * 3 + 0];

            int g =
                src[x * 3 + 1];

            int b =
                src[x * 3 + 2];

            int luminance =
                (77 * r +
                 150 * g +
                 29 * b) >> 8;

            dst[x] =
                (uint8_t)luminance;
        }
    }
}


/* ============================================================
 * RGB -> BGRA
 * ============================================================ */

static void screen_rgb_to_bgra(
    const ScreenFrame *source,
    ScreenFrame *destination)
{
    if (!source ||
        !destination)
        return;

    if (source->format !=
        SC_FORMAT_RGB24 ||
        destination->format !=
        SC_FORMAT_BGRA32)
    {
        return;
    }

    for (int y = 0;
         y < source->height;
         ++y)
    {
        const uint8_t *src =
            source->data +
            (size_t)y *
            source->stride;

        uint8_t *dst =
            destination->data +
            (size_t)y *
            destination->stride;

        for (int x = 0;
             x < source->width;
             ++x)
        {
            dst[x * 4 + 0] =
                src[x * 3 + 2];

            dst[x * 4 + 1] =
                src[x * 3 + 1];

            dst[x * 4 + 2] =
                src[x * 3 + 0];

            dst[x * 4 + 3] =
                255;
        }
    }
}


/* ============================================================
 * Dirty rectangle
 * ============================================================ */

typedef struct {

    int x;

    int y;

    int width;

    int height;

} ScreenRect;


/* ============================================================
 * Dirty region collection
 * ============================================================ */

typedef struct {

    ScreenRect regions[
        SC_MAX_DIRTY_REGIONS
    ];

    size_t count;

    int min_x;

    int min_y;

    int max_x;

    int max_y;

} DirtyRegionSet;


static void dirty_regions_reset(
    DirtyRegionSet *set)
{
    if (!set)
        return;

    memset(
        set,
        0,
        sizeof(*set)
    );

    set->min_x = INT32_MAX;
    set->min_y = INT32_MAX;
}


static void dirty_regions_add(
    DirtyRegionSet *set,
    int x,
    int y,
    int width,
    int height)
{
    if (!set ||
        width <= 0 ||
        height <= 0)
        return;

    if (set->count <
        SC_MAX_DIRTY_REGIONS)
    {
        set->regions[
            set->count++
        ] = (ScreenRect){
            x,
            y,
            width,
            height
        };
    }

    if (x < set->min_x)
        set->min_x = x;

    if (y < set->min_y)
        set->min_y = y;

    if (x + width > set->max_x)
        set->max_x = x + width;

    if (y + height > set->max_y)
        set->max_y = y + height;
}


/* ============================================================
 * Tile
 * ============================================================ */

typedef struct {

    int x;

    int y;

    int width;

    int height;

    double difference;

} ScreenTile;


/* ============================================================
 * Tile comparison
 * ============================================================ */

static double screen_tile_difference(
    const ScreenFrame *current,
    const ScreenFrame *previous,
    int x0,
    int y0,
    int width,
    int height)
{
    if (!current ||
        !previous)
        return 1.0;

    if (current->format !=
        SC_FORMAT_RGB24 ||
        previous->format !=
        SC_FORMAT_RGB24)
    {
        return 1.0;
    }

    uint64_t difference = 0;
    uint64_t pixels = 0;

    for (int y = y0;
         y < y0 + height;
         ++y)
    {
        const uint8_t *a =
            current->data +
            (size_t)y *
            current->stride +
            (size_t)x0 * 3;

        const uint8_t *b =
            previous->data +
            (size_t)y *
            previous->stride +
            (size_t)x0 * 3;

        for (int x = 0;
             x < width;
             ++x)
        {
            int dr =
                abs(
                    (int)a[x * 3 + 0] -
                    (int)b[x * 3 + 0]
                );

            int dg =
                abs(
                    (int)a[x * 3 + 1] -
                    (int)b[x * 3 + 1]
                );

            int db =
                abs(
                    (int)a[x * 3 + 2] -
                    (int)b[x * 3 + 2]
                );

            difference +=
                (uint64_t)
                (dr + dg + db);

            pixels++;
        }
    }

    if (pixels == 0)
        return 0.0;

    return
        (double)difference /
        ((double)pixels * 765.0);
}


/* ============================================================
 * Full-frame difference
 * ============================================================ */

static double screen_frame_difference(
    const ScreenFrame *current,
    const ScreenFrame *previous)
{
    if (!current ||
        !previous)
        return 1.0;

    if (current->width !=
        previous->width ||
        current->height !=
        previous->height ||
        current->format !=
        previous->format)
    {
        return 1.0;
    }

    return screen_tile_difference(
        current,
        previous,
        0,
        0,
        current->width,
        current->height
    );
}


/* ============================================================
 * Dirty-region detection
 * ============================================================ */

static double screen_detect_dirty_regions(
    const ScreenFrame *current,
    const ScreenFrame *previous,
    int tile_size,
    DirtyRegionSet *regions)
{
    if (!current ||
        !previous ||
        !regions)
        return 1.0;

    dirty_regions_reset(
        regions
    );

    double total_difference = 0.0;

    size_t tiles = 0;

    for (int y = 0;
         y < current->height;
         y += tile_size)
    {
        for (int x = 0;
             x < current->width;
             x += tile_size)
        {
            int width =
                screen_clamp_int(
                    tile_size,
                    1,
                    current->width - x
                );

            int height =
                screen_clamp_int(
                    tile_size,
                    1,
                    current->height - y
                );

            double difference =
                screen_tile_difference(
                    current,
                    previous,
                    x,
                    y,
                    width,
                    height
                );

            total_difference +=
                difference;

            tiles++;

            if (difference >
                SC_LOW_CHANGE_THRESHOLD)
            {
                dirty_regions_add(
                    regions,
                    x,
                    y,
                    width,
                    height
                );
            }
        }
    }

    if (tiles == 0)
        return 0.0;

    return
        total_difference /
        (double)tiles;
}


/* ============================================================
 * Cursor
 * ============================================================ */

typedef struct {

    bool visible;

    int x;

    int y;

    int width;

    int height;

    uint8_t *rgba;

} ScreenCursor;


/* ============================================================
 * Cursor init
 * ============================================================ */

static bool screen_cursor_init(
    ScreenCursor *cursor,
    int width,
    int height)
{
    if (!cursor ||
        width <= 0 ||
        height <= 0 ||
        width > SC_CURSOR_MAX_SIZE ||
        height > SC_CURSOR_MAX_SIZE)
    {
        return false;
    }

    memset(
        cursor,
        0,
        sizeof(*cursor)
    );

    cursor->rgba =
        calloc(
            (size_t)width *
            (size_t)height *
            4,
            1
        );

    if (!cursor->rgba)
        return false;

    cursor->width = width;
    cursor->height = height;

    return true;
}


static void screen_cursor_destroy(
    ScreenCursor *cursor)
{
    if (!cursor)
        return;

    free(cursor->rgba);

    memset(
        cursor,
        0,
        sizeof(*cursor)
    );
}


/* ============================================================
 * Cursor composition
 * ============================================================ */

static void screen_composite_cursor(
    ScreenFrame *frame,
    const ScreenCursor *cursor)
{
    if (!frame ||
        !cursor ||
        !cursor->visible ||
        !cursor->rgba)
    {
        return;
    }

    if (frame->format !=
        SC_FORMAT_RGB24)
    {
        return;
    }

    for (int cy = 0;
         cy < cursor->height;
         ++cy)
    {
        int fy =
            cursor->y + cy;

        if (fy < 0 ||
            fy >= frame->height)
            continue;

        for (int cx = 0;
             cx < cursor->width;
             ++cx)
        {
            int fx =
                cursor->x + cx;

            if (fx < 0 ||
                fx >= frame->width)
                continue;

            const uint8_t *src =
                cursor->rgba +
                ((size_t)cy *
                 cursor->width +
                 cx) * 4;

            uint8_t alpha =
                src[3];

            if (alpha == 0)
                continue;

            uint8_t *dst =
                frame->data +
                (size_t)fy *
                frame->stride +
                (size_t)fx * 3;

            double a =
                (double)alpha / 255.0;

            dst[0] =
                (uint8_t)
                (src[0] * a +
                 dst[0] * (1.0 - a));

            dst[1] =
                (uint8_t)
                (src[1] * a +
                 dst[1] * (1.0 - a));

            dst[2] =
                (uint8_t)
                (src[2] * a +
                 dst[2] * (1.0 - a));
        }
    }
}


/* ============================================================
 * Capture backend
 * ============================================================ */

typedef struct CaptureBackend CaptureBackend;


/*
 * Platform-specific backend interface.
 *
 * A Windows implementation could use:
 *
 *     Desktop Duplication API
 *
 * A macOS implementation:
 *
 *     ScreenCaptureKit
 *
 * A Linux implementation:
 *
 *     PipeWire / Wayland / X11
 */
typedef struct {

    bool (*open)(
        CaptureBackend *backend,
        int display,
        int width,
        int height
    );

    bool (*capture)(
        CaptureBackend *backend,
        ScreenFrame *frame
    );

    void (*close)(
        CaptureBackend *backend
    );

} CaptureBackendVTable;


struct CaptureBackend {

    const CaptureBackendVTable *vtable;

    void *platform_data;

    int display;

    int width;

    int height;

    bool opened;
};


/* ============================================================
 * Synthetic backend
 *
 * Useful for testing the entire engine without an OS API.
 * ============================================================ */

typedef struct {

    uint64_t frame;

    double phase;

} SyntheticCaptureState;


static bool synthetic_open(
    CaptureBackend *backend,
    int display,
    int width,
    int height)
{
    (void)display;

    SyntheticCaptureState *state =
        calloc(
            1,
            sizeof(SyntheticCaptureState)
        );

    if (!state)
        return false;

    backend->platform_data =
        state;

    backend->display =
        display;

    backend->width =
        width;

    backend->height =
        height;

    backend->opened =
        true;

    return true;
}


static bool synthetic_capture(
    CaptureBackend *backend,
    ScreenFrame *frame)
{
    if (!backend ||
        !frame ||
        !backend->platform_data)
    {
        return false;
    }

    SyntheticCaptureState *state =
        backend->platform_data;

    state->frame++;

    /*
     * Mostly-static desktop with moving window.
     */
    for (int y = 0;
         y < frame->height;
         ++y)
    {
        uint8_t *row =
            frame->data +
            (size_t)y *
            frame->stride;

        for (int x = 0;
             x < frame->width;
             ++x)
        {
            uint8_t base =
                (uint8_t)
                ((x + y) & 0xFF);

            row[x * 3 + 0] =
                base;

            row[x * 3 + 1] =
                (uint8_t)
                ((base + 30) & 0xFF);

            row[x * 3 + 2] =
                (uint8_t)
                ((base + 60) & 0xFF);
        }
    }

    /*
     * Simulated moving application window.
     */
    int window_width =
        frame->width / 3;

    int window_height =
        frame->height / 3;

    int period =
        frame->width -
        window_width;

    if (period < 1)
        period = 1;

    int window_x =
        (int)(
            state->frame %
            (uint64_t)period
        );

    int window_y =
        frame->height / 3;

    for (int y = 0;
         y < window_height;
         ++y)
    {
        int fy =
            window_y + y;

        if (fy >= frame->height)
            continue;

        uint8_t *row =
            frame->data +
            (size_t)fy *
            frame->stride;

        for (int x = 0;
             x < window_width;
             ++x)
        {
            int fx =
                window_x + x;

            if (fx >= frame->width)
                continue;

            row[fx * 3 + 0] = 240;
            row[fx * 3 + 1] = 240;
            row[fx * 3 + 2] = 240;
        }
    }

    frame->timestamp_ns =
        screen_now_ns();

    frame->sequence =
        state->frame;

    return true;
}


static void synthetic_close(
    CaptureBackend *backend)
{
    if (!backend)
        return;

    free(
        backend->platform_data
    );

    backend->platform_data =
        NULL;

    backend->opened =
        false;
}


static const CaptureBackendVTable
synthetic_backend_vtable = {

    synthetic_open,
    synthetic_capture,
    synthetic_close

};


/* ============================================================
 * Capture quality
 * ============================================================ */

typedef enum {

    SC_QUALITY_LOW = 0,
    SC_QUALITY_MEDIUM,
    SC_QUALITY_HIGH,
    SC_QUALITY_ULTRA

} ScreenQuality;


typedef struct {

    ScreenQuality quality;

    int width;

    int height;

    double fps;

    int target_bitrate_kbps;

    bool cursor;

    bool dirty_region_optimization;

} ScreenCaptureProfile;


/* ============================================================
 * Default profiles
 * ============================================================ */

static ScreenCaptureProfile
screen_profile_for_quality(
    ScreenQuality quality,
    int source_width,
    int source_height)
{
    ScreenCaptureProfile profile;

    memset(
        &profile,
        0,
        sizeof(profile)
    );

    profile.quality =
        quality;

    switch (quality) {

        case SC_QUALITY_LOW:

            profile.width =
                source_width / 2;

            profile.height =
                source_height / 2;

            profile.fps = 10.0;

            profile.target_bitrate_kbps =
                1200;

            profile.cursor = true;

            profile.dirty_region_optimization =
                true;

            break;


        case SC_QUALITY_MEDIUM:

            profile.width =
                source_width > 2560 ?
                2560 :
                source_width;

            profile.height =
                source_height > 1440 ?
                1440 :
                source_height;

            profile.fps = 15.0;

            profile.target_bitrate_kbps =
                2500;

            profile.cursor = true;

            profile.dirty_region_optimization =
                true;

            break;


        case SC_QUALITY_HIGH:

            profile.width =
                source_width > 3840 ?
                3840 :
                source_width;

            profile.height =
                source_height > 2160 ?
                2160 :
                source_height;

            profile.fps = 30.0;

            profile.target_bitrate_kbps =
                5000;

            profile.cursor = true;

            profile.dirty_region_optimization =
                true;

            break;


        case SC_QUALITY_ULTRA:

            profile.width =
                source_width;

            profile.height =
                source_height;

            profile.fps = 60.0;

            profile.target_bitrate_kbps =
                10000;

            profile.cursor = true;

            profile.dirty_region_optimization =
                false;

            break;
    }

    return profile;
}


/* ============================================================
 * Statistics
 * ============================================================ */

typedef struct {

    uint64_t captured_frames;

    uint64_t delivered_frames;

    uint64_t skipped_frames;

    uint64_t static_frames;

    uint64_t changed_frames;

    uint64_t dirty_tiles;

    uint64_t total_tiles;

    uint64_t capture_failures;

    uint64_t dropped_frames;

    uint64_t cursor_updates;

    uint64_t processing_ns;

    double average_difference;

    double maximum_difference;

    double current_fps;

} ScreenCaptureStats;


/* ============================================================
 * Capture engine
 * ============================================================ */

typedef struct {

    CaptureBackend backend;

    ScreenFrame current;

    ScreenFrame previous;

    ScreenFrame rgb_frame;

    ScreenFrame gray_frame;

    ScreenCursor cursor;

    DirtyRegionSet dirty_regions;

    ScreenCaptureProfile profile;

    ScreenCaptureStats stats;

    int source_width;

    int source_height;

    int tile_size;

    double target_fps;

    double actual_fps;

    double change_rate;

    double average_processing_ms;

    uint64_t last_capture_ns;

    uint64_t last_frame_ns;

    uint64_t sequence;

    bool have_previous;

    bool running;

} ScreenCaptureEngine;


/* ============================================================
 * Engine initialisation
 * ============================================================ */

static bool screen_engine_init(
    ScreenCaptureEngine *engine,
    int width,
    int height)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->source_width =
        width;

    engine->source_height =
        height;

    engine->tile_size =
        SC_DEFAULT_TILE;

    engine->profile =
        screen_profile_for_quality(
            SC_QUALITY_HIGH,
            width,
            height
        );

    engine->target_fps =
        engine->profile.fps;

    if (!screen_frame_init(
            &engine->current,
            width,
            height,
            SC_FORMAT_RGB24))
    {
        return false;
    }

    if (!screen_frame_init(
            &engine->previous,
            width,
            height,
            SC_FORMAT_RGB24))
    {
        screen_frame_destroy(
            &engine->current
        );

        return false;
    }

    if (!screen_frame_init(
            &engine->rgb_frame,
            width,
            height,
            SC_FORMAT_RGB24))
    {
        screen_frame_destroy(
            &engine->current
        );

        screen_frame_destroy(
            &engine->previous
        );

        return false;
    }

    if (!screen_frame_init(
            &engine->gray_frame,
            width,
            height,
            SC_FORMAT_GRAY8))
    {
        screen_frame_destroy(
            &engine->current
        );

        screen_frame_destroy(
            &engine->previous
        );

        screen_frame_destroy(
            &engine->rgb_frame
        );

        return false;
    }

    if (!screen_cursor_init(
            &engine->cursor,
            32,
            32))
    {
        screen_frame_destroy(
            &engine->current
        );

        screen_frame_destroy(
            &engine->previous
        );

        screen_frame_destroy(
            &engine->rgb_frame
        );

        screen_frame_destroy(
            &engine->gray_frame
        );

        return false;
    }

    return true;
}


/* ============================================================
 * Backend opening
 * ============================================================ */

static bool screen_engine_open(
    ScreenCaptureEngine *engine)
{
    engine->backend.vtable =
        &synthetic_backend_vtable;

    if (!engine->backend.vtable->open(
            &engine->backend,
            0,
            engine->source_width,
            engine->source_height))
    {
        return false;
    }

    engine->running = true;

    return true;
}


/* ============================================================
 * Set profile
 * ============================================================ */

static void screen_engine_set_quality(
    ScreenCaptureEngine *engine,
    ScreenQuality quality)
{
    engine->profile =
        screen_profile_for_quality(
            quality,
            engine->source_width,
            engine->source_height
        );

    engine->target_fps =
        engine->profile.fps;
}


/* ============================================================
 * Frame pacing
 * ============================================================ */

static bool screen_should_capture(
    ScreenCaptureEngine *engine,
    uint64_t now_ns)
{
    if (!engine->last_capture_ns)
        return true;

    double interval_ns =
        1000000000.0 /
        engine->target_fps;

    return
        (double)
        (now_ns -
         engine->last_capture_ns)
        >= interval_ns;
}


/* ============================================================
 * Adaptive FPS
 * ============================================================ */

static void screen_adapt_fps(
    ScreenCaptureEngine *engine)
{
    double change =
        engine->change_rate;

    /*
     * Completely static desktop.
     *
     * We still occasionally capture frames to make
     * sure the state remains valid.
     */
    if (change <
        SC_STATIC_THRESHOLD)
    {
        engine->target_fps =
            screen_clamp_double(
                engine->target_fps * 0.75,
                SC_MIN_FPS,
                SC_MAX_FPS
            );

        return;
    }

    /*
     * Light changes.
     */
    if (change <
        SC_LOW_CHANGE_THRESHOLD)
    {
        engine->target_fps =
            screen_clamp_double(
                engine->target_fps * 0.90,
                SC_MIN_FPS,
                SC_MAX_FPS
            );

        return;
    }

    /*
     * Significant motion.
     */
    if (change >
        SC_HIGH_CHANGE_THRESHOLD)
    {
        engine->target_fps =
            screen_clamp_double(
                engine->target_fps * 1.20,
                SC_MIN_FPS,
                SC_MAX_FPS
            );

        return;
    }

    /*
     * Gradually return toward profile target.
     */
    double target =
        engine->profile.fps;

    engine->target_fps +=
        (target -
         engine->target_fps)
        * 0.10;

    engine->target_fps =
        screen_clamp_double(
            engine->target_fps,
            SC_MIN_FPS,
            SC_MAX_FPS
        );
}


/* ============================================================
 * Capture
 * ============================================================ */

static bool screen_engine_capture(
    ScreenCaptureEngine *engine)
{
    if (!engine ||
        !engine->running)
    {
        return false;
    }

    uint64_t now =
        screen_now_ns();

    if (!screen_should_capture(
            engine,
            now))
    {
        engine->stats.skipped_frames++;
        return false;
    }

    uint64_t start =
        screen_now_ns();

    if (!engine->backend.vtable->capture(
            &engine->backend,
            &engine->current))
    {
        engine->stats.capture_failures++;
        return false;
    }

    engine->stats.captured_frames++;

    /*
     * Cursor is composed after native capture.
     */
    if (engine->profile.cursor &&
        engine->cursor.visible)
    {
        screen_composite_cursor(
            &engine->current,
            &engine->cursor
        );

        engine->stats.cursor_updates++;
    }

    /*
     * First frame always has to be delivered.
     */
    if (!engine->have_previous) {

        screen_frame_copy(
            &engine->previous,
            &engine->current
        );

        engine->have_previous =
            true;

        engine->change_rate =
            1.0;

        engine->stats.changed_frames++;

        engine->stats.delivered_frames++;

        engine->sequence++;

        engine->last_capture_ns =
            now;

        engine->last_frame_ns =
            now;

        return true;
    }

    /*
     * Dirty-region analysis.
     */
    double difference =
        screen_detect_dirty_regions(
            &engine->current,
            &engine->previous,
            engine->tile_size,
            &engine->dirty_regions
        );

    engine->change_rate =
        difference;

    engine->stats.average_difference =
        engine->stats.average_difference *
        0.95 +
        difference * 0.05;

    if (difference >
        engine->stats.maximum_difference)
    {
        engine->stats.maximum_difference =
            difference;
    }

    engine->stats.dirty_tiles +=
        engine->dirty_regions.count;

    engine->stats.total_tiles +=
        (uint64_t)
        (
            ((engine->source_width +
              engine->tile_size - 1) /
             engine->tile_size)
            *
            ((engine->source_height +
              engine->tile_size - 1) /
             engine->tile_size)
        );

    if (difference <
        SC_STATIC_THRESHOLD)
    {
        engine->stats.static_frames++;

        /*
         * Don't repeatedly transmit identical
         * desktop frames.
         */
        screen_adapt_fps(
            engine
        );

        screen_frame_copy(
            &engine->previous,
            &engine->current
        );

        engine->last_capture_ns =
            now;

        uint64_t end =
            screen_now_ns();

        engine->stats.processing_ns +=
            end - start;

        return false;
    }

    engine->stats.changed_frames++;

    /*
     * Current frame becomes previous frame.
     */
    screen_frame_copy(
        &engine->previous,
        &engine->current
    );

    engine->stats.delivered_frames++;

    engine->sequence++;

    engine->current.sequence =
        engine->sequence;

    engine->last_capture_ns =
        now;

    if (engine->last_frame_ns > 0) {

        uint64_t elapsed =
            now -
            engine->last_frame_ns;

        if (elapsed > 0) {

            double fps =
                1000000000.0 /
                (double)elapsed;

            engine->actual_fps =
                engine->actual_fps *
                0.90 +
                fps * 0.10;

            engine->stats.current_fps =
                engine->actual_fps;
        }
    }

    engine->last_frame_ns =
        now;

    screen_adapt_fps(
        engine
    );

    uint64_t end =
        screen_now_ns();

    engine->stats.processing_ns +=
        end - start;

    engine->average_processing_ms =
        (
            double)
            engine->stats.processing_ns /
            (
                double)
                engine->stats.captured_frames
            /
            1000000.0;

    return true;
}


/* ============================================================
 * Set cursor position
 * ============================================================ */

static void screen_engine_set_cursor(
    ScreenCaptureEngine *engine,
    int x,
    int y,
    bool visible)
{
    if (!engine)
        return;

    engine->cursor.x = x;
    engine->cursor.y = y;
    engine->cursor.visible = visible;
}


/* ============================================================
 * Render simple cursor
 * ============================================================ */

static void screen_cursor_make_arrow(
    ScreenCursor *cursor)
{
    if (!cursor ||
        !cursor->rgba)
        return;

    memset(
        cursor->rgba,
        0,
        (size_t)cursor->width *
        cursor->height *
        4
    );

    for (int y = 0;
         y < cursor->height;
         ++y)
    {
        int max_x =
            y / 2 + 1;

        if (max_x >
            cursor->width)
        {
            max_x =
                cursor->width;
        }

        for (int x = 0;
             x < max_x;
             ++x)
        {
            size_t index =
                (
                    (size_t)y *
                    cursor->width +
                    x
                ) * 4;

            cursor->rgba[index + 0] =
                255;

            cursor->rgba[index + 1] =
                255;

            cursor->rgba[index + 2] =
                255;

            cursor->rgba[index + 3] =
                220;
        }
    }
}


/* ============================================================
 * Statistics
 * ============================================================ */

static void screen_print_stats(
    const ScreenCaptureEngine *engine)
{
    const ScreenCaptureStats *s =
        &engine->stats;

    printf("\n");
    printf(
        "============================================\n"
    );

    printf(
        " SCREEN CAPTURE ENGINE\n"
    );

    printf(
        "============================================\n"
    );

    printf(
        "Captured frames:       %llu\n",
        (unsigned long long)
        s->captured_frames
    );

    printf(
        "Delivered frames:      %llu\n",
        (unsigned long long)
        s->delivered_frames
    );

    printf(
        "Static frames:         %llu\n",
        (unsigned long long)
        s->static_frames
    );

    printf(
        "Skipped frames:        %llu\n",
        (unsigned long long)
        s->skipped_frames
    );

    printf(
        "Capture failures:      %llu\n",
        (unsigned long long)
        s->capture_failures
    );

    printf(
        "Dirty tiles:            %llu\n",
        (unsigned long long)
        s->dirty_tiles
    );

    printf(
        "Total tiles:            %llu\n",
        (unsigned long long)
        s->total_tiles
    );

    printf(
        "Average difference:     %.6f\n",
        s->average_difference
    );

    printf(
        "Maximum difference:     %.6f\n",
        s->maximum_difference
    );

    printf(
        "Current FPS:            %.2f\n",
        s->current_fps
    );

    printf(
        "Target FPS:             %.2f\n",
        engine->target_fps
    );

    printf(
        "Processing time:        %.3f ms\n",
        engine->average_processing_ms
    );

    printf(
        "============================================\n"
    );
}


/* ============================================================
 * PPM output
 * ============================================================ */

static bool screen_write_ppm(
    const ScreenFrame *frame,
    const char *filename)
{
    if (!frame ||
        frame->format !=
        SC_FORMAT_RGB24)
    {
        return false;
    }

    FILE *file =
        fopen(
            filename,
            "wb"
        );

    if (!file)
        return false;

    fprintf(
        file,
        "P6\n%d %d\n255\n",
        frame->width,
        frame->height
    );

    for (int y = 0;
         y < frame->height;
         ++y)
    {
        const uint8_t *row =
            frame->data +
            (size_t)y *
            frame->stride;

        fwrite(
            row,
            1,
            (size_t)
            frame->width * 3,
            file
        );
    }

    fclose(file);

    return true;
}


/* ============================================================
 * Benchmark
 * ============================================================ */

static void screen_benchmark(
    ScreenCaptureEngine *engine,
    int frames)
{
    printf(
        "\nRunning %d-frame capture benchmark...\n",
        frames
    );

    uint64_t start =
        screen_now_ns();

    int delivered = 0;

    for (int i = 0;
         i < frames;
         ++i)
    {
        /*
         * Force capture by resetting pacing.
         */
        engine->last_capture_ns = 0;

        if (screen_engine_capture(
                engine))
        {
            delivered++;
        }
    }

    uint64_t end =
        screen_now_ns();

    double seconds =
        (double)(end - start) /
        1000000000.0;

    printf(
        "Elapsed: %.3f seconds\n",
        seconds
    );

    printf(
        "Frames delivered: %d\n",
        delivered
    );

    if (seconds > 0.0) {

        printf(
            "Processing FPS: %.2f\n",
            (double)frames /
            seconds
        );
    }
}


/* ============================================================
 * Quality adaptation based on network conditions
 * ============================================================ */

typedef struct {

    double packet_loss;

    double rtt_ms;

    double bandwidth_kbps;

    double cpu_usage;

    double gpu_usage;

} ScreenNetworkState;


static ScreenQuality screen_select_quality(
    const ScreenNetworkState *network)
{
    if (!network)
        return SC_QUALITY_MEDIUM;

    /*
     * Network-constrained.
     */
    if (network->packet_loss > 0.08 ||
        network->bandwidth_kbps < 1500)
    {
        return SC_QUALITY_LOW;
    }

    if (network->packet_loss > 0.03 ||
        network->bandwidth_kbps < 3000)
    {
        return SC_QUALITY_MEDIUM;
    }

    /*
     * Resource constrained.
     */
    if (network->cpu_usage > 90.0 ||
        network->gpu_usage > 90.0)
    {
        return SC_QUALITY_MEDIUM;
    }

    if (network->bandwidth_kbps >= 10000 &&
        network->cpu_usage < 70.0 &&
        network->gpu_usage < 70.0)
    {
        return SC_QUALITY_ULTRA;
    }

    return SC_QUALITY_HIGH;
}


/* ============================================================
 * Simulated network adaptation
 * ============================================================ */

static void screen_simulate_adaptation(
    ScreenCaptureEngine *engine)
{
    ScreenNetworkState state;

    state.packet_loss = 0.01;
    state.rtt_ms = 25.0;
    state.bandwidth_kbps = 12000;
    state.cpu_usage = 45.0;
    state.gpu_usage = 40.0;

    ScreenQuality quality =
        screen_select_quality(
            &state
        );

    screen_engine_set_quality(
        engine,
        quality
    );

    printf(
        "Good network -> quality %d, "
        "%.0f kbps, %.0f FPS\n",
        quality,
        (double)
        engine->profile.target_bitrate_kbps,
        engine->profile.fps
    );

    state.packet_loss = 0.09;
    state.bandwidth_kbps = 900;

    quality =
        screen_select_quality(
            &state
        );

    screen_engine_set_quality(
        engine,
        quality
    );

    printf(
        "Poor network -> quality %d, "
        "%.0f kbps, %.0f FPS\n",
        quality,
        (double)
        engine->profile.target_bitrate_kbps,
        engine->profile.fps
    );

    state.packet_loss = 0.01;
    state.bandwidth_kbps = 15000;
    state.cpu_usage = 96.0;
    state.gpu_usage = 95.0;

    quality =
        screen_select_quality(
            &state
        );

    screen_engine_set_quality(
        engine,
        quality
    );

    printf(
        "Resource pressure -> quality %d, "
        "%.0f kbps, %.0f FPS\n",
        quality,
        (double)
        engine->profile.target_bitrate_kbps,
        engine->profile.fps
    );
}


/* ============================================================
 * Engine shutdown
 * ============================================================ */

static void screen_engine_destroy(
    ScreenCaptureEngine *engine)
{
    if (!engine)
        return;

    if (engine->backend.opened &&
        engine->backend.vtable &&
        engine->backend.vtable->close)
    {
        engine->backend.vtable->close(
            &engine->backend
        );
    }

    screen_cursor_destroy(
        &engine->cursor
    );

    screen_frame_destroy(
        &engine->current
    );

    screen_frame_destroy(
        &engine->previous
    );

    screen_frame_destroy(
        &engine->rgb_frame
    );

    screen_frame_destroy(
        &engine->gray_frame
    );

    memset(
        engine,
        0,
        sizeof(*engine)
    );
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    /*
     * Simulate a 1920x1080 desktop.
     *
     * A production build would obtain these dimensions
     * from the platform capture backend.
     */
    const int width = 1920;
    const int height = 1080;

    printf(
        "Teams-style Screen Capture Engine\n"
    );

    printf(
        "Desktop: %dx%d\n",
        width,
        height
    );

    ScreenCaptureEngine engine;

    if (!screen_engine_init(
            &engine,
            width,
            height))
    {
        fprintf(
            stderr,
            "Could not initialise screen engine.\n"
        );

        return EXIT_FAILURE;
    }

    if (!screen_engine_open(
            &engine))
    {
        fprintf(
            stderr,
            "Could not open capture backend.\n"
        );

        screen_engine_destroy(
            &engine
        );

        return EXIT_FAILURE;
    }

    /*
     * Configure cursor.
     */
    screen_cursor_make_arrow(
        &engine.cursor
    );

    screen_engine_set_cursor(
        &engine,
        width / 2,
        height / 2,
        true
    );

    /*
     * Start at high quality.
     */
    screen_engine_set_quality(
        &engine,
        SC_QUALITY_HIGH
    );

    /*
     * Capture a series of frames.
     */
    printf(
        "\nCapturing desktop...\n"
    );

    for (int i = 0;
         i < 120;
         ++i)
    {
        /*
         * For demonstration, bypass real-time pacing.
         */
        engine.last_capture_ns = 0;

        bool delivered =
            screen_engine_capture(
                &engine
            );

        if (delivered) {

            if (i == 0 ||
                i == 30 ||
                i == 60 ||
                i == 90)
            {
                printf(
                    "Frame %d delivered: "
                    "difference %.5f, "
                    "dirty regions %zu, "
                    "target FPS %.2f\n",
                    i,
                    engine.change_rate,
                    engine.dirty_regions.count,
                    engine.target_fps
                );
            }
        }
    }

    /*
     * Save a visual test frame.
     */
    if (screen_write_ppm(
            &engine.current,
            "screen_capture_output.ppm"))
    {
        printf(
            "\nWrote screen_capture_output.ppm\n"
        );
    }

    /*
     * Test adaptation.
     */
    printf(
        "\nNetwork/resource adaptation:\n"
    );

    screen_simulate_adaptation(
        &engine
    );

    /*
     * Benchmark.
     */
    screen_benchmark(
        &engine,
        30
    );

    /*
     * Diagnostics.
     */
    screen_print_stats(
        &engine
    );

    screen_engine_destroy(
        &engine
    );

    printf(
        "\nScreen capture engine shutdown.\n"
    );

    return EXIT_SUCCESS;
}







/*
 * teams_secure_transport.c
 *
 * Native C11 secure transport core for a Teams-style
 * communications client.
 *
 * Responsibilities:
 *
 *   - Secure session lifecycle
 *   - Packet framing
 *   - Sequence numbers
 *   - Replay protection
 *   - Nonce management
 *   - Key epochs
 *   - Rekeying
 *   - Authentication tags
 *   - Secure memory handling
 *   - Packet validation
 *   - Transport state machine
 *   - Key expiration
 *   - Anti-replay window
 *   - Statistics
 *   - Cryptographic backend abstraction
 *
 * IMPORTANT:
 *
 * This is a transport/security ARCHITECTURE prototype.
 *
 * It deliberately does not implement AES, ChaCha20, SHA-256,
 * X25519, Ed25519, etc. from scratch.
 *
 * Production implementations should connect this interface
 * to a mature cryptographic library or OS security facility.
 *
 * Example production choices:
 *
 *   OpenSSL / BoringSSL
 *   libsodium
 *   Apple Security / CryptoKit bridge
 *   Windows CNG
 *
 * Build:
 *
 *   gcc -std=c11 -O3 -march=native \
 *       -Wall -Wextra -pedantic \
 *       teams_secure_transport.c \
 *       -o teams_secure_transport
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>


/* ============================================================
 * Configuration
 * ============================================================ */

#define ST_VERSION                  1

#define ST_MAX_PACKET_SIZE          65536
#define ST_MAX_PAYLOAD_SIZE         60000

#define ST_KEY_SIZE                 32
#define ST_NONCE_SIZE               12
#define ST_TAG_SIZE                 16

#define ST_SESSION_ID_SIZE          16

#define ST_REPLAY_WINDOW            64

#define ST_REKEY_PACKET_LIMIT       1000000ULL
#define ST_REKEY_TIME_SECONDS       3600ULL

#define ST_MAX_STREAMS               64

#define ST_MAGIC                    0x53544331u


/* ============================================================
 * Secure zero
 *
 * Prevent the compiler from optimising the wipe away.
 * ============================================================ */

static void secure_zero(
    void *memory,
    size_t size)
{
    volatile uint8_t *ptr =
        (volatile uint8_t *)memory;

    while (size--)
        *ptr++ = 0;
}


/* ============================================================
 * Time
 * ============================================================ */

static uint64_t secure_now_seconds(void)
{
    return (uint64_t)time(NULL);
}


/* ============================================================
 * Randomness abstraction
 * ============================================================ */

typedef bool (*SecureRandomFunction)(
    uint8_t *output,
    size_t length,
    void *user_data
);


/*
 * DEMONSTRATION ONLY.
 *
 * This is not suitable for cryptographic key generation.
 *
 * Replace with:
 *
 *   getrandom()
 *   BCryptGenRandom()
 *   SecRandomCopyBytes()
 *   a CSPRNG from libsodium/OpenSSL/etc.
 */
static bool demo_random(
    uint8_t *output,
    size_t length,
    void *user_data)
{
    (void)user_data;

    static uint64_t state =
        0x9E3779B97F4A7C15ULL;

    for (size_t i = 0;
         i < length;
         ++i)
    {
        state ^=
            state << 13;

        state ^=
            state >> 7;

        state ^=
            state << 17;

        output[i] =
            (uint8_t)
            (state >> 24);
    }

    return true;
}


/* ============================================================
 * Cryptographic backend
 * ============================================================ */

/*
 * The secure transport engine never directly implements a
 * cryptographic primitive.
 *
 * Instead, it asks the backend to perform:
 *
 *     AEAD encrypt
 *     AEAD decrypt
 *     key derivation
 *     random generation
 *
 * A production implementation would map these functions
 * onto a vetted crypto library.
 */

typedef struct CryptoBackend CryptoBackend;

typedef struct {

    bool (*aead_encrypt)(
        CryptoBackend *backend,
        const uint8_t *key,
        size_t key_length,
        const uint8_t *nonce,
        size_t nonce_length,
        const uint8_t *aad,
        size_t aad_length,
        const uint8_t *plaintext,
        size_t plaintext_length,
        uint8_t *ciphertext,
        uint8_t *tag,
        size_t tag_length
    );

    bool (*aead_decrypt)(
        CryptoBackend *backend,
        const uint8_t *key,
        size_t key_length,
        const uint8_t *nonce,
        size_t nonce_length,
        const uint8_t *aad,
        size_t aad_length,
        const uint8_t *ciphertext,
        size_t ciphertext_length,
        const uint8_t *tag,
        size_t tag_length,
        uint8_t *plaintext
    );

    bool (*derive_key)(
        CryptoBackend *backend,
        const uint8_t *input,
        size_t input_length,
        const uint8_t *context,
        size_t context_length,
        uint8_t *output,
        size_t output_length
    );

    SecureRandomFunction random_bytes;

} CryptoBackendVTable;


struct CryptoBackend {

    const CryptoBackendVTable *vtable;

    void *user_data;
};


/* ============================================================
 * DEMONSTRATION CRYPTO BACKEND
 *
 * NOT cryptographically secure.
 *
 * It exists only so the transport state machine can be
 * compiled and tested without external dependencies.
 * ============================================================ */

static uint8_t demo_mix(
    uint8_t x,
    uint8_t y,
    uint8_t z)
{
    x ^= y;

    x =
        (uint8_t)
        ((x << 3) |
         (x >> 5));

    x += z;

    x ^=
        (uint8_t)
        (x >> 3);

    return x;
}


static bool demo_aead_encrypt(
    CryptoBackend *backend,
    const uint8_t *key,
    size_t key_length,
    const uint8_t *nonce,
    size_t nonce_length,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *plaintext,
    size_t plaintext_length,
    uint8_t *ciphertext,
    uint8_t *tag,
    size_t tag_length)
{
    (void)backend;

    if (!key ||
        !nonce ||
        !plaintext ||
        !ciphertext ||
        !tag)
    {
        return false;
    }

    /*
     * This is deliberately just a test transformation.
     *
     * DO NOT use this as encryption.
     */
    for (size_t i = 0;
         i < plaintext_length;
         ++i)
    {
        uint8_t k =
            key[i % key_length];

        uint8_t n =
            nonce[i % nonce_length];

        ciphertext[i] =
            plaintext[i] ^
            demo_mix(k, n, (uint8_t)i);
    }

    memset(
        tag,
        0,
        tag_length
    );

    for (size_t i = 0;
         i < plaintext_length;
         ++i)
    {
        tag[i % tag_length] ^=
            ciphertext[i];
    }

    for (size_t i = 0;
         i < aad_length;
         ++i)
    {
        tag[i % tag_length] ^=
            aad[i];
    }

    return true;
}


static bool demo_aead_decrypt(
    CryptoBackend *backend,
    const uint8_t *key,
    size_t key_length,
    const uint8_t *nonce,
    size_t nonce_length,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *ciphertext,
    size_t ciphertext_length,
    const uint8_t *tag,
    size_t tag_length,
    uint8_t *plaintext)
{
    (void)backend;

    uint8_t calculated_tag[
        ST_TAG_SIZE
    ];

    memset(
        calculated_tag,
        0,
        sizeof(calculated_tag)
    );

    for (size_t i = 0;
         i < ciphertext_length;
         ++i)
    {
        calculated_tag[
            i % tag_length
        ] ^= ciphertext[i];
    }

    for (size_t i = 0;
         i < aad_length;
         ++i)
    {
        calculated_tag[
            i % tag_length
        ] ^= aad[i];
    }

    /*
     * Constant-time comparison.
     */
    uint8_t difference = 0;

    for (size_t i = 0;
         i < tag_length;
         ++i)
    {
        difference |=
            calculated_tag[i] ^
            tag[i];
    }

    if (difference != 0)
        return false;

    for (size_t i = 0;
         i < ciphertext_length;
         ++i)
    {
        uint8_t k =
            key[i % key_length];

        uint8_t n =
            nonce[i % nonce_length];

        plaintext[i] =
            ciphertext[i] ^
            demo_mix(k, n, (uint8_t)i);
    }

    secure_zero(
        calculated_tag,
        sizeof(calculated_tag)
    );

    return true;
}


static bool demo_derive_key(
    CryptoBackend *backend,
    const uint8_t *input,
    size_t input_length,
    const uint8_t *context,
    size_t context_length,
    uint8_t *output,
    size_t output_length)
{
    (void)backend;

    if (!input ||
        !context ||
        !output)
    {
        return false;
    }

    for (size_t i = 0;
         i < output_length;
         ++i)
    {
        uint8_t value =
            (uint8_t)i;

        for (size_t j = 0;
             j < input_length;
             ++j)
        {
            value =
                demo_mix(
                    value,
                    input[j],
                    (uint8_t)j
                );
        }

        for (size_t j = 0;
             j < context_length;
             ++j)
        {
            value =
                demo_mix(
                    value,
                    context[j],
                    (uint8_t)(j + i)
                );
        }

        output[i] =
            value;
    }

    return true;
}


static const CryptoBackendVTable
demo_crypto_vtable = {

    demo_aead_encrypt,
    demo_aead_decrypt,
    demo_derive_key,
    demo_random

};


/* ============================================================
 * Packet types
 * ============================================================ */

typedef enum {

    ST_PACKET_AUDIO = 1,
    ST_PACKET_VIDEO = 2,
    ST_PACKET_SCREEN = 3,
    ST_PACKET_CONTROL = 4,
    ST_PACKET_KEEPALIVE = 5,
    ST_PACKET_KEY_UPDATE = 6

} SecurePacketType;


/* ============================================================
 * Transport states
 * ============================================================ */

typedef enum {

    ST_STATE_CLOSED = 0,
    ST_STATE_CONNECTING,
    ST_STATE_ESTABLISHED,
    ST_STATE_REKEYING,
    ST_STATE_CLOSING,
    ST_STATE_FAILED

} SecureTransportState;


/* ============================================================
 * Packet header
 * ============================================================ */

typedef struct {

    uint32_t magic;

    uint8_t version;

    uint8_t packet_type;

    uint16_t header_length;

    uint32_t stream_id;

    uint64_t sequence;

    uint64_t timestamp;

    uint32_t payload_length;

    uint32_t key_epoch;

} SecurePacketHeader;


/* ============================================================
 * Packet
 * ============================================================ */

typedef struct {

    SecurePacketHeader header;

    uint8_t nonce[
        ST_NONCE_SIZE
    ];

    uint8_t *payload;

    uint8_t tag[
        ST_TAG_SIZE
    ];

} SecurePacket;


/* ============================================================
 * Replay protection
 * ============================================================ */

typedef struct {

    uint64_t highest_sequence;

    uint64_t bitmap;

    bool initialised;

} ReplayWindow;


/* ============================================================
 * Replay check
 * ============================================================ */

static bool replay_is_valid(
    ReplayWindow *window,
    uint64_t sequence)
{
    if (!window)
        return false;

    if (!window->initialised)
        return true;

    if (sequence >
        window->highest_sequence)
    {
        return true;
    }

    uint64_t difference =
        window->highest_sequence -
        sequence;

    if (difference >=
        ST_REPLAY_WINDOW)
    {
        return false;
    }

    uint64_t mask =
        1ULL << difference;

    if (window->bitmap & mask)
        return false;

    return true;
}


/* ============================================================
 * Replay commit
 * ============================================================ */

static void replay_commit(
    ReplayWindow *window,
    uint64_t sequence)
{
    if (!window)
        return;

    if (!window->initialised)
    {
        window->initialised =
            true;

        window->highest_sequence =
            sequence;

        window->bitmap =
            1ULL;

        return;
    }

    if (sequence >
        window->highest_sequence)
    {
        uint64_t shift =
            sequence -
            window->highest_sequence;

        if (shift >=
            ST_REPLAY_WINDOW)
        {
            window->bitmap =
                1ULL;
        }
        else
        {
            window->bitmap <<=
                shift;

            window->bitmap |=
                1ULL;
        }

        window->highest_sequence =
            sequence;

        return;
    }

    uint64_t difference =
        window->highest_sequence -
        sequence;

    if (difference <
        ST_REPLAY_WINDOW)
    {
        window->bitmap |=
            1ULL << difference;
    }
}


/* ============================================================
 * Stream
 * ============================================================ */

typedef struct {

    uint32_t id;

    uint64_t tx_sequence;

    ReplayWindow rx_replay;

    uint64_t packets_sent;

    uint64_t packets_received;

    uint64_t packets_rejected;

    uint64_t bytes_sent;

    uint64_t bytes_received;

} SecureStream;


/* ============================================================
 * Session keys
 * ============================================================ */

typedef struct {

    uint8_t tx_key[
        ST_KEY_SIZE
    ];

    uint8_t rx_key[
        ST_KEY_SIZE
    ];

    uint32_t epoch;

    uint64_t created_at;

    uint64_t packets_sent;

} SessionKeys;


/* ============================================================
 * Transport statistics
 * ============================================================ */

typedef struct {

    uint64_t packets_encrypted;

    uint64_t packets_decrypted;

    uint64_t packets_rejected;

    uint64_t replay_attacks;

    uint64_t authentication_failures;

    uint64_t malformed_packets;

    uint64_t bytes_encrypted;

    uint64_t bytes_decrypted;

    uint64_t key_rotations;

} SecureTransportStats;


/* ============================================================
 * Transport engine
 * ============================================================ */

typedef struct {

    SecureTransportState state;

    CryptoBackend crypto;

    uint8_t session_id[
        ST_SESSION_ID_SIZE
    ];

    SessionKeys keys;

    SecureStream streams[
        ST_MAX_STREAMS
    ];

    size_t stream_count;

    SecureTransportStats stats;

    uint64_t created_at;

    uint64_t last_activity;

    bool authenticated;

} SecureTransport;


/* ============================================================
 * Header serialisation
 * ============================================================ */

static void write_u16(
    uint8_t *buffer,
    uint16_t value)
{
    buffer[0] =
        (uint8_t)(value >> 8);

    buffer[1] =
        (uint8_t)value;
}


static void write_u32(
    uint8_t *buffer,
    uint32_t value)
{
    buffer[0] =
        (uint8_t)(value >> 24);

    buffer[1] =
        (uint8_t)(value >> 16);

    buffer[2] =
        (uint8_t)(value >> 8);

    buffer[3] =
        (uint8_t)value;
}


static void write_u64(
    uint8_t *buffer,
    uint64_t value)
{
    for (int i = 7;
         i >= 0;
         --i)
    {
        buffer[7 - i] =
            (uint8_t)
            (value >> (i * 8));
    }
}


static uint16_t read_u16(
    const uint8_t *buffer)
{
    return
        ((uint16_t)buffer[0] << 8) |
        buffer[1];
}


static uint32_t read_u32(
    const uint8_t *buffer)
{
    return
        ((uint32_t)buffer[0] << 24) |
        ((uint32_t)buffer[1] << 16) |
        ((uint32_t)buffer[2] << 8) |
        buffer[3];
}


static uint64_t read_u64(
    const uint8_t *buffer)
{
    uint64_t value = 0;

    for (int i = 0;
         i < 8;
         ++i)
    {
        value =
            (value << 8) |
            buffer[i];
    }

    return value;
}


/* ============================================================
 * Header encode
 * ============================================================ */

#define ST_SERIALIZED_HEADER_SIZE 40


static void packet_header_encode(
    const SecurePacketHeader *header,
    uint8_t *buffer)
{
    write_u32(
        buffer + 0,
        header->magic
    );

    buffer[4] =
        header->version;

    buffer[5] =
        header->packet_type;

    write_u16(
        buffer + 6,
        header->header_length
    );

    write_u32(
        buffer + 8,
        header->stream_id
    );

    write_u64(
        buffer + 12,
        header->sequence
    );

    write_u64(
        buffer + 20,
        header->timestamp
    );

    write_u32(
        buffer + 28,
        header->payload_length
    );

    write_u32(
        buffer + 32,
        header->key_epoch
    );

    /*
     * Reserved bytes.
     */
    memset(
        buffer + 36,
        0,
        4
    );
}


/* ============================================================
 * Header decode
 * ============================================================ */

static bool packet_header_decode(
    SecurePacketHeader *header,
    const uint8_t *buffer,
    size_t length)
{
    if (!header ||
        !buffer ||
        length <
        ST_SERIALIZED_HEADER_SIZE)
    {
        return false;
    }

    header->magic =
        read_u32(buffer + 0);

    header->version =
        buffer[4];

    header->packet_type =
        buffer[5];

    header->header_length =
        read_u16(buffer + 6);

    header->stream_id =
        read_u32(buffer + 8);

    header->sequence =
        read_u64(buffer + 12);

    header->timestamp =
        read_u64(buffer + 20);

    header->payload_length =
        read_u32(buffer + 28);

    header->key_epoch =
        read_u32(buffer + 32);

    return true;
}


/* ============================================================
 * Stream lookup
 * ============================================================ */

static SecureStream *
secure_find_stream(
    SecureTransport *transport,
    uint32_t stream_id)
{
    for (size_t i = 0;
         i < transport->stream_count;
         ++i)
    {
        if (transport->streams[i].id ==
            stream_id)
        {
            return
                &transport->streams[i];
        }
    }

    return NULL;
}


/* ============================================================
 * Stream creation
 * ============================================================ */

static SecureStream *
secure_add_stream(
    SecureTransport *transport,
    uint32_t stream_id)
{
    if (!transport)
        return NULL;

    SecureStream *existing =
        secure_find_stream(
            transport,
            stream_id
        );

    if (existing)
        return existing;

    if (transport->stream_count >=
        ST_MAX_STREAMS)
    {
        return NULL;
    }

    SecureStream *stream =
        &transport->streams[
            transport->stream_count++
        ];

    memset(
        stream,
        0,
        sizeof(*stream)
    );

    stream->id =
        stream_id;

    /*
     * Start sequence at a random-looking value in production.
     */
    stream->tx_sequence = 1;

    return stream;
}


/* ============================================================
 * Key initialisation
 * ============================================================ */

static bool secure_generate_session_keys(
    SecureTransport *transport)
{
    if (!transport ||
        !transport->crypto.vtable)
    {
        return false;
    }

    uint8_t master[
        ST_KEY_SIZE
    ];

    if (!transport->crypto.vtable->random_bytes(
            master,
            sizeof(master),
            transport->crypto.user_data))
    {
        return false;
    }

    uint8_t tx_context[] =
        "teams-style-tx";

    uint8_t rx_context[] =
        "teams-style-rx";

    if (!transport->crypto.vtable->derive_key(
            &transport->crypto,
            master,
            sizeof(master),
            tx_context,
            sizeof(tx_context) - 1,
            transport->keys.tx_key,
            sizeof(transport->keys.tx_key)))
    {
        secure_zero(
            master,
            sizeof(master)
        );

        return false;
    }

    if (!transport->crypto.vtable->derive_key(
            &transport->crypto,
            master,
            sizeof(master),
            rx_context,
            sizeof(rx_context) - 1,
            transport->keys.rx_key,
            sizeof(transport->keys.rx_key)))
    {
        secure_zero(
            master,
            sizeof(master)
        );

        return false;
    }

    transport->keys.epoch = 1;

    transport->keys.created_at =
        secure_now_seconds();

    transport->keys.packets_sent =
        0;

    secure_zero(
        master,
        sizeof(master)
    );

    return true;
}


/* ============================================================
 * Transport initialisation
 * ============================================================ */

static bool secure_transport_init(
    SecureTransport *transport,
    const CryptoBackend *crypto)
{
    if (!transport ||
        !crypto ||
        !crypto->vtable)
    {
        return false;
    }

    memset(
        transport,
        0,
        sizeof(*transport)
    );

    transport->crypto =
        *crypto;

    transport->state =
        ST_STATE_CLOSED;

    transport->created_at =
        secure_now_seconds();

    return true;
}


/* ============================================================
 * Session open
 * ============================================================ */

static bool secure_transport_open(
    SecureTransport *transport)
{
    if (!transport)
        return false;

    if (transport->state !=
        ST_STATE_CLOSED)
    {
        return false;
    }

    if (!transport->crypto.vtable->random_bytes(
            transport->session_id,
            sizeof(transport->session_id),
            transport->crypto.user_data))
    {
        transport->state =
            ST_STATE_FAILED;

        return false;
    }

    transport->state =
        ST_STATE_CONNECTING;

    if (!secure_generate_session_keys(
            transport))
    {
        transport->state =
            ST_STATE_FAILED;

        return false;
    }

    transport->authenticated =
        true;

    transport->state =
        ST_STATE_ESTABLISHED;

    transport->last_activity =
        secure_now_seconds();

    return true;
}


/* ============================================================
 * Nonce generation
 *
 * In a real AEAD implementation the nonce construction must
 * be designed around the exact cipher/protocol.
 * ============================================================ */

static void secure_make_nonce(
    const SecureTransport *transport,
    uint32_t stream_id,
    uint64_t sequence,
    uint8_t nonce[ST_NONCE_SIZE])
{
    memset(
        nonce,
        0,
        ST_NONCE_SIZE
    );

    /*
     * Session-derived prefix.
     */
    memcpy(
        nonce,
        transport->session_id,
        8
    );

    /*
     * Stream + sequence.
     */
    nonce[8] =
        (uint8_t)(stream_id >> 24);

    nonce[9] =
        (uint8_t)(stream_id >> 16);

    nonce[10] =
        (uint8_t)(stream_id >> 8);

    nonce[11] =
        (uint8_t)sequence;
}


/* ============================================================
 * Key rotation
 * ============================================================ */

static bool secure_rotate_keys(
    SecureTransport *transport)
{
    if (!transport)
        return false;

    uint8_t old_key[
        ST_KEY_SIZE
    ];

    memcpy(
        old_key,
        transport->keys.tx_key,
        sizeof(old_key)
    );

    uint8_t context[32];

    memset(
        context,
        0,
        sizeof(context)
    );

    memcpy(
        context,
        "rekey",
        5
    );

    write_u32(
        context + 8,
        transport->keys.epoch + 1
    );

    if (!transport->crypto.vtable->derive_key(
            &transport->crypto,
            old_key,
            sizeof(old_key),
            context,
            sizeof(context),
            transport->keys.tx_key,
            sizeof(transport->keys.tx_key)))
    {
        secure_zero(
            old_key,
            sizeof(old_key)
        );

        secure_zero(
            context,
            sizeof(context)
        );

        return false;
    }

    /*
     * Derive receive key from previous receive key.
     */
    memcpy(
        old_key,
        transport->keys.rx_key,
        sizeof(old_key)
    );

    if (!transport->crypto.vtable->derive_key(
            &transport->crypto,
            old_key,
            sizeof(old_key),
            context,
            sizeof(context),
            transport->keys.rx_key,
            sizeof(transport->keys.rx_key)))
    {
        secure_zero(
            old_key,
            sizeof(old_key)
        );

        secure_zero(
            context,
            sizeof(context)
        );

        return false;
    }

    transport->keys.epoch++;

    transport->keys.created_at =
        secure_now_seconds();

    transport->keys.packets_sent =
        0;

    transport->stats.key_rotations++;

    secure_zero(
        old_key,
        sizeof(old_key)
    );

    secure_zero(
        context,
        sizeof(context)
    );

    return true;
}


/* ============================================================
 * Rekey policy
 * ============================================================ */

static bool secure_should_rekey(
    const SecureTransport *transport)
{
    if (!transport)
        return false;

    uint64_t now =
        secure_now_seconds();

    if (transport->keys.packets_sent >=
        ST_REKEY_PACKET_LIMIT)
    {
        return true;
    }

    if (now >
        transport->keys.created_at &&
        now -
        transport->keys.created_at >=
        ST_REKEY_TIME_SECONDS)
    {
        return true;
    }

    return false;
}


/* ============================================================
 * Packet size
 * ============================================================ */

static size_t secure_packet_wire_size(
    const SecurePacket *packet)
{
    if (!packet)
        return 0;

    return
        ST_SERIALIZED_HEADER_SIZE +
        ST_NONCE_SIZE +
        packet->header.payload_length +
        ST_TAG_SIZE;
}


/* ============================================================
 * Packet allocation
 * ============================================================ */

static SecurePacket *
secure_packet_create(
    size_t payload_size)
{
    if (payload_size >
        ST_MAX_PAYLOAD_SIZE)
    {
        return NULL;
    }

    SecurePacket *packet =
        calloc(
            1,
            sizeof(*packet)
        );

    if (!packet)
        return NULL;

    if (payload_size > 0)
    {
        packet->payload =
            malloc(payload_size);

        if (!packet->payload)
        {
            free(packet);
            return NULL;
        }
    }

    packet->header.payload_length =
        (uint32_t)payload_size;

    return packet;
}


/* ============================================================
 * Packet destroy
 * ============================================================ */

static void secure_packet_destroy(
    SecurePacket *packet)
{
    if (!packet)
        return;

    if (packet->payload)
    {
        secure_zero(
            packet->payload,
            packet->header.payload_length
        );

        free(
            packet->payload
        );
    }

    secure_zero(
        packet->nonce,
        sizeof(packet->nonce)
    );

    secure_zero(
        packet->tag,
        sizeof(packet->tag)
    );

    secure_zero(
        &packet->header,
        sizeof(packet->header)
    );

    free(packet);
}


/* ============================================================
 * Packet encryption
 * ============================================================ */

static bool secure_encrypt_packet(
    SecureTransport *transport,
    uint32_t stream_id,
    SecurePacketType packet_type,
    const uint8_t *plaintext,
    size_t plaintext_length,
    SecurePacket **output)
{
    if (!transport ||
        !plaintext ||
        !output)
    {
        return false;
    }

    *output = NULL;

    if (transport->state !=
        ST_STATE_ESTABLISHED)
    {
        return false;
    }

    if (plaintext_length >
        ST_MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    SecureStream *stream =
        secure_find_stream(
            transport,
            stream_id
        );

    if (!stream)
    {
        stream =
            secure_add_stream(
                transport,
                stream_id
            );
    }

    if (!stream)
        return false;

    /*
     * Rekey before creating the packet.
     */
    if (secure_should_rekey(
            transport))
    {
        transport->state =
            ST_STATE_REKEYING;

        if (!secure_rotate_keys(
                transport))
        {
            transport->state =
                ST_STATE_FAILED;

            return false;
        }

        transport->state =
            ST_STATE_ESTABLISHED;
    }

    SecurePacket *packet =
        secure_packet_create(
            plaintext_length
        );

    if (!packet)
        return false;

    packet->header.magic =
        ST_MAGIC;

    packet->header.version =
        ST_VERSION;

    packet->header.packet_type =
        (uint8_t)packet_type;

    packet->header.header_length =
        ST_SERIALIZED_HEADER_SIZE;

    packet->header.stream_id =
        stream_id;

    packet->header.sequence =
        stream->tx_sequence++;

    packet->header.timestamp =
        secure_now_seconds();

    packet->header.key_epoch =
        transport->keys.epoch;

    secure_make_nonce(
        transport,
        stream_id,
        packet->header.sequence,
        packet->nonce
    );

    memcpy(
        packet->payload,
        plaintext,
        plaintext_length
    );

    uint8_t header[
        ST_SERIALIZED_HEADER_SIZE
    ];

    packet_header_encode(
        &packet->header,
        header
    );

    if (!transport->crypto.vtable->aead_encrypt(
            &transport->crypto,
            transport->keys.tx_key,
            ST_KEY_SIZE,
            packet->nonce,
            ST_NONCE_SIZE,
            header,
            sizeof(header),
            packet->payload,
            plaintext_length,
            packet->payload,
            packet->tag,
            ST_TAG_SIZE))
    {
        secure_packet_destroy(
            packet
        );

        return false;
    }

    stream->packets_sent++;

    stream->bytes_sent +=
        plaintext_length;

    transport->keys.packets_sent++;

    transport->stats.packets_encrypted++;

    transport->stats.bytes_encrypted +=
        plaintext_length;

    transport->last_activity =
        secure_now_seconds();

    *output =
        packet;

    return true;
}


/* ============================================================
 * Packet serialisation
 * ============================================================ */

static bool secure_packet_serialize(
    const SecurePacket *packet,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size)
{
    if (!packet ||
        !output ||
        !output_size)
    {
        return false;
    }

    size_t required =
        secure_packet_wire_size(
            packet
        );

    if (output_capacity <
        required)
    {
        return false;
    }

    uint8_t header[
        ST_SERIALIZED_HEADER_SIZE
    ];

    packet_header_encode(
        &packet->header,
        header
    );

    size_t offset = 0;

    memcpy(
        output + offset,
        header,
        sizeof(header)
    );

    offset +=
        sizeof(header);

    memcpy(
        output + offset,
        packet->nonce,
        ST_NONCE_SIZE
    );

    offset +=
        ST_NONCE_SIZE;

    if (packet->header.payload_length)
    {
        memcpy(
            output + offset,
            packet->payload,
            packet->header.payload_length
        );

        offset +=
            packet->header.payload_length;
    }

    memcpy(
        output + offset,
        packet->tag,
        ST_TAG_SIZE
    );

    offset +=
        ST_TAG_SIZE;

    *output_size =
        offset;

    return true;
}


/* ============================================================
 * Packet parsing
 * ============================================================ */

static SecurePacket *
secure_packet_parse(
    const uint8_t *input,
    size_t input_size)
{
    if (!input ||
        input_size <
        ST_SERIALIZED_HEADER_SIZE +
        ST_NONCE_SIZE +
        ST_TAG_SIZE)
    {
        return NULL;
    }

    SecurePacketHeader header;

    if (!packet_header_decode(
            &header,
            input,
            input_size))
    {
        return NULL;
    }

    if (header.magic !=
        ST_MAGIC)
    {
        return NULL;
    }

    if (header.version !=
        ST_VERSION)
    {
        return NULL;
    }

    if (header.header_length !=
        ST_SERIALIZED_HEADER_SIZE)
    {
        return NULL;
    }

    if (header.payload_length >
        ST_MAX_PAYLOAD_SIZE)
    {
        return NULL;
    }

    size_t required =
        ST_SERIALIZED_HEADER_SIZE +
        ST_NONCE_SIZE +
        header.payload_length +
        ST_TAG_SIZE;

    if (input_size <
        required)
    {
        return NULL;
    }

    SecurePacket *packet =
        secure_packet_create(
            header.payload_length
        );

    if (!packet)
        return NULL;

    packet->header =
        header;

    size_t offset =
        ST_SERIALIZED_HEADER_SIZE;

    memcpy(
        packet->nonce,
        input + offset,
        ST_NONCE_SIZE
    );

    offset +=
        ST_NONCE_SIZE;

    if (header.payload_length)
    {
        memcpy(
            packet->payload,
            input + offset,
            header.payload_length
        );

        offset +=
            header.payload_length;
    }

    memcpy(
        packet->tag,
        input + offset,
        ST_TAG_SIZE
    );

    return packet;
}


/* ============================================================
 * Packet validation
 * ============================================================ */

static bool secure_validate_packet(
    const SecurePacket *packet)
{
    if (!packet)
        return false;

    if (packet->header.magic !=
        ST_MAGIC)
        return false;

    if (packet->header.version !=
        ST_VERSION)
        return false;

    if (packet->header.payload_length >
        ST_MAX_PAYLOAD_SIZE)
        return false;

    if (packet->header.stream_id == 0)
        return false;

    if (packet->header.sequence == 0)
        return false;

    if (packet->header.key_epoch == 0)
        return false;

    return true;
}


/* ============================================================
 * Packet decryption
 * ============================================================ */

static bool secure_decrypt_packet(
    SecureTransport *transport,
    const SecurePacket *packet,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    size_t *plaintext_size)
{
    if (!transport ||
        !packet ||
        !plaintext ||
        !plaintext_size)
    {
        return false;
    }

    *plaintext_size = 0;

    if (transport->state !=
        ST_STATE_ESTABLISHED)
    {
        return false;
    }

    if (!secure_validate_packet(
            packet))
    {
        transport->stats.malformed_packets++;
        return false;
    }

    if (packet->header.payload_length >
        plaintext_capacity)
    {
        transport->stats.malformed_packets++;
        return false;
    }

    /*
     * For this prototype we require the current key epoch.
     *
     * A production protocol can retain a short-lived previous
     * epoch to allow packets already in flight during rekey.
     */
    if (packet->header.key_epoch !=
        transport->keys.epoch)
    {
        transport->stats.packets_rejected++;
        return false;
    }

    SecureStream *stream =
        secure_find_stream(
            transport,
            packet->header.stream_id
        );

    if (!stream)
    {
        stream =
            secure_add_stream(
                transport,
                packet->header.stream_id
            );
    }

    if (!stream)
        return false;

    /*
     * Check replay BEFORE expensive crypto.
     */
    if (!replay_is_valid(
            &stream->rx_replay,
            packet->header.sequence))
    {
        stream->packets_rejected++;

        transport->stats.replay_attacks++;

        return false;
    }

    uint8_t header[
        ST_SERIALIZED_HEADER_SIZE
    ];

    packet_header_encode(
        &packet->header,
        header
    );

    /*
     * Authenticate/decrypt.
     */
    if (!transport->crypto.vtable->aead_decrypt(
            &transport->crypto,
            transport->keys.rx_key,
            ST_KEY_SIZE,
            packet->nonce,
            ST_NONCE_SIZE,
            header,
            sizeof(header),
            packet->payload,
            packet->header.payload_length,
            packet->tag,
            ST_TAG_SIZE,
            plaintext))
    {
        stream->packets_rejected++;

        transport->stats.authentication_failures++;

        return false;
    }

    /*
     * Only commit replay state AFTER successful
     * authentication.
     */
    replay_commit(
        &stream->rx_replay,
        packet->header.sequence
    );

    stream->packets_received++;

    stream->bytes_received +=
        packet->header.payload_length;

    transport->stats.packets_decrypted++;

    transport->stats.bytes_decrypted +=
        packet->header.payload_length;

    transport->last_activity =
        secure_now_seconds();

    *plaintext_size =
        packet->header.payload_length;

    return true;
}


/* ============================================================
 * Constant-time memory comparison
 * ============================================================ */

static bool secure_constant_time_equal(
    const uint8_t *a,
    const uint8_t *b,
    size_t length)
{
    if (!a || !b)
        return false;

    uint8_t difference = 0;

    for (size_t i = 0;
         i < length;
         ++i)
    {
        difference |=
            a[i] ^ b[i];
    }

    return difference == 0;
}


/* ============================================================
 * Session ID printing
 * ============================================================ */

static void print_hex(
    const uint8_t *data,
    size_t length)
{
    for (size_t i = 0;
         i < length;
         ++i)
    {
        printf(
            "%02x",
            data[i]
        );
    }

    printf("\n");
}


/* ============================================================
 * Statistics
 * ============================================================ */

static void secure_print_stats(
    const SecureTransport *transport)
{
    const SecureTransportStats *s =
        &transport->stats;

    printf(
        "\n"
        "============================================\n"
        " SECURE TRANSPORT STATISTICS\n"
        "============================================\n"
    );

    printf(
        "Encrypted packets:       %llu\n",
        (unsigned long long)
        s->packets_encrypted
    );

    printf(
        "Decrypted packets:       %llu\n",
        (unsigned long long)
        s->packets_decrypted
    );

    printf(
        "Rejected packets:        %llu\n",
        (unsigned long long)
        s->packets_rejected
    );

    printf(
        "Replay attacks:          %llu\n",
        (unsigned long long)
        s->replay_attacks
    );

    printf(
        "Authentication failures: %llu\n",
        (unsigned long long)
        s->authentication_failures
    );

    printf(
        "Malformed packets:       %llu\n",
        (unsigned long long)
        s->malformed_packets
    );

    printf(
        "Bytes encrypted:         %llu\n",
        (unsigned long long)
        s->bytes_encrypted
    );

    printf(
        "Bytes decrypted:         %llu\n",
        (unsigned long long)
        s->bytes_decrypted
    );

    printf(
        "Key rotations:           %llu\n",
        (unsigned long long)
        s->key_rotations
    );

    printf(
        "Current key epoch:       %u\n",
        transport->keys.epoch
    );

    printf(
        "============================================\n"
    );
}


/* ============================================================
 * Stream statistics
 * ============================================================ */

static void secure_print_streams(
    const SecureTransport *transport)
{
    printf(
        "\nStreams: %zu\n",
        transport->stream_count
    );

    for (size_t i = 0;
         i < transport->stream_count;
         ++i)
    {
        const SecureStream *stream =
            &transport->streams[i];

        printf(
            "  stream=%u "
            "tx=%llu "
            "rx=%llu "
            "rejected=%llu "
            "tx_bytes=%llu "
            "rx_bytes=%llu\n",

            stream->id,

            (unsigned long long)
            stream->packets_sent,

            (unsigned long long)
            stream->packets_received,

            (unsigned long long)
            stream->packets_rejected,

            (unsigned long long)
            stream->bytes_sent,

            (unsigned long long)
            stream->bytes_received
        );
    }
}


/* ============================================================
 * Test: normal encryption flow
 * ============================================================ */

static bool test_basic_packet_flow(
    SecureTransport *transport)
{
    const char message[] =
        "hello from secure transport";

    SecurePacket *packet = NULL;

    if (!secure_encrypt_packet(
            transport,
            1,
            ST_PACKET_CONTROL,
            (const uint8_t *)message,
            strlen(message),
            &packet))
    {
        return false;
    }

    uint8_t wire[
        ST_MAX_PACKET_SIZE
    ];

    size_t wire_size = 0;

    if (!secure_packet_serialize(
            packet,
            wire,
            sizeof(wire),
            &wire_size))
    {
        secure_packet_destroy(
            packet
        );

        return false;
    }

    SecurePacket *received =
        secure_packet_parse(
            wire,
            wire_size
        );

    if (!received)
    {
        secure_packet_destroy(
            packet
        );

        return false;
    }

    uint8_t plaintext[
        ST_MAX_PAYLOAD_SIZE
    ];

    size_t plaintext_size = 0;

    /*
     * IMPORTANT:
     *
     * This demo uses the same transport object to
     * demonstrate the framing/replay machinery.
     *
     * A real connection would have separate directional
     * keys for sender and receiver.
     */
    memcpy(
        transport->keys.rx_key,
        transport->keys.tx_key,
        ST_KEY_SIZE
    );

    bool result =
        secure_decrypt_packet(
            transport,
            received,
            plaintext,
            sizeof(plaintext),
            &plaintext_size
        );

    if (result)
    {
        plaintext[
            plaintext_size
        ] = '\0';

        printf(
            "Decrypted: %s\n",
            plaintext
        );
    }

    secure_zero(
        plaintext,
        sizeof(plaintext)
    );

    secure_packet_destroy(
        packet
    );

    secure_packet_destroy(
        received
    );

    return result;
}


/* ============================================================
 * Test: replay attack
 * ============================================================ */

static bool test_replay_protection(
    SecureTransport *transport)
{
    const char message[] =
        "replay test";

    SecurePacket *packet = NULL;

    if (!secure_encrypt_packet(
            transport,
            2,
            ST_PACKET_AUDIO,
            (const uint8_t *)message,
            strlen(message),
            &packet))
    {
        return false;
    }

    /*
     * Again use the TX key for local loopback testing.
     */
    memcpy(
        transport->keys.rx_key,
        transport->keys.tx_key,
        ST_KEY_SIZE
    );

    uint8_t plaintext[
        ST_MAX_PAYLOAD_SIZE
    ];

    size_t plaintext_size;

    bool first =
        secure_decrypt_packet(
            transport,
            packet,
            plaintext,
            sizeof(plaintext),
            &plaintext_size
        );

    bool second =
        secure_decrypt_packet(
            transport,
            packet,
            plaintext,
            sizeof(plaintext),
            &plaintext_size
        );

    secure_zero(
        plaintext,
        sizeof(plaintext)
    );

    secure_packet_destroy(
        packet
    );

    printf(
        "First delivery: %s\n",
        first ? "accepted" : "rejected"
    );

    printf(
        "Replay delivery: %s\n",
        second ? "accepted" : "rejected"
    );

    return
        first &&
        !second;
}


/* ============================================================
 * Test: authentication failure
 * ============================================================ */

static bool test_tamper_detection(
    SecureTransport *transport)
{
    const char message[] =
        "tamper test";

    SecurePacket *packet = NULL;

    if (!secure_encrypt_packet(
            transport,
            3,
            ST_PACKET_VIDEO,
            (const uint8_t *)message,
            strlen(message),
            &packet))
    {
        return false;
    }

    memcpy(
        transport->keys.rx_key,
        transport->keys.tx_key,
        ST_KEY_SIZE
    );

    /*
     * Alter ciphertext.
     */
    if (packet->header.payload_length > 0)
    {
        packet->payload[0] ^= 0x80;
    }

    uint8_t plaintext[
        ST_MAX_PAYLOAD_SIZE
    ];

    size_t plaintext_size;

    bool result =
        secure_decrypt_packet(
            transport,
            packet,
            plaintext,
            sizeof(plaintext),
            &plaintext_size
        );

    secure_zero(
        plaintext,
        sizeof(plaintext)
    );

    secure_packet_destroy(
        packet
    );

    printf(
        "Tampered packet: %s\n",
        result ?
        "ACCEPTED (bad)" :
        "REJECTED (expected)"
    );

    return !result;
}


/* ============================================================
 * Test: malformed packet
 * ============================================================ */

static bool test_malformed_packet(
    SecureTransport *transport)
{
    uint8_t malformed[
        ST_SERIALIZED_HEADER_SIZE
    ];

    memset(
        malformed,
        0,
        sizeof(malformed)
    );

    SecurePacket *packet =
        secure_packet_parse(
            malformed,
            sizeof(malformed)
        );

    if (packet)
    {
        secure_packet_destroy(
            packet
        );

        return false;
    }

    transport->stats.malformed_packets++;

    return true;
}


/* ============================================================
 * Test: multiple media streams
 * ============================================================ */

static bool test_media_streams(
    SecureTransport *transport)
{
    const char audio[] =
        "audio-frame";

    const char video[] =
        "video-frame";

    const char screen[] =
        "screen-frame";

    SecurePacket *a = NULL;
    SecurePacket *v = NULL;
    SecurePacket *s = NULL;

    bool success = true;

    success &=
        secure_encrypt_packet(
            transport,
            100,
            ST_PACKET_AUDIO,
            (const uint8_t *)audio,
            strlen(audio),
            &a
        );

    success &=
        secure_encrypt_packet(
            transport,
            200,
            ST_PACKET_VIDEO,
            (const uint8_t *)video,
            strlen(video),
            &v
        );

    success &=
        secure_encrypt_packet(
            transport,
            300,
            ST_PACKET_SCREEN,
            (const uint8_t *)screen,
            strlen(screen),
            &s
        );

    printf(
        "Audio packet:  %s\n",
        a ? "created" : "failed"
    );

    printf(
        "Video packet:  %s\n",
        v ? "created" : "failed"
    );

    printf(
        "Screen packet: %s\n",
        s ? "created" : "failed"
    );

    secure_packet_destroy(a);
    secure_packet_destroy(v);
    secure_packet_destroy(s);

    return success;
}


/* ============================================================
 * Transport shutdown
 * ============================================================ */

static void secure_transport_destroy(
    SecureTransport *transport)
{
    if (!transport)
        return;

    secure_zero(
        transport->keys.tx_key,
        sizeof(transport->keys.tx_key)
    );

    secure_zero(
        transport->keys.rx_key,
        sizeof(transport->keys.rx_key)
    );

    secure_zero(
        transport->session_id,
        sizeof(transport->session_id)
    );

    secure_zero(
        transport,
        sizeof(*transport)
    );
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        " TEAMS-STYLE SECURE TRANSPORT CORE\n"
        "============================================\n"
    );

    /*
     * Create cryptographic backend.
     *
     * In production this should be replaced with a vetted
     * crypto implementation.
     */
    CryptoBackend crypto;

    memset(
        &crypto,
        0,
        sizeof(crypto)
    );

    crypto.vtable =
        &demo_crypto_vtable;

    SecureTransport transport;

    if (!secure_transport_init(
            &transport,
            &crypto))
    {
        fprintf(
            stderr,
            "Transport initialisation failed.\n"
        );

        return EXIT_FAILURE;
    }

    if (!secure_transport_open(
            &transport))
    {
        fprintf(
            stderr,
            "Secure session creation failed.\n"
        );

        secure_transport_destroy(
            &transport
        );

        return EXIT_FAILURE;
    }

    printf(
        "Transport state: ESTABLISHED\n"
    );

    printf(
        "Session ID: "
    );

    print_hex(
        transport.session_id,
        ST_SESSION_ID_SIZE
    );

    printf(
        "Key epoch: %u\n",
        transport.keys.epoch
    );

    /*
     * Basic encrypted transport.
     */
    printf(
        "\n[TEST] Basic encrypted packet\n"
    );

    if (!test_basic_packet_flow(
            &transport))
    {
        fprintf(
            stderr,
            "Basic packet test failed.\n"
        );
    }

    /*
     * Replay protection.
     */
    printf(
        "\n[TEST] Replay protection\n"
    );

    if (!test_replay_protection(
            &transport))
    {
        fprintf(
            stderr,
            "Replay protection test failed.\n"
        );
    }

    /*
     * Authentication / tamper detection.
     */
    printf(
        "\n[TEST] Tamper detection\n"
    );

    if (!test_tamper_detection(
            &transport))
    {
        fprintf(
            stderr,
            "Tamper detection test failed.\n"
        );
    }

    /*
     * Malformed packet.
     */
    printf(
        "\n[TEST] Malformed packet\n"
    );

    printf(
        "Malformed packet: %s\n",
        test_malformed_packet(
            &transport
        )
        ? "rejected"
        : "accepted"
    );

    /*
     * Multiple media streams.
     */
    printf(
        "\n[TEST] Media streams\n"
    );

    test_media_streams(
        &transport
    );

    /*
     * Diagnostics.
     */
    secure_print_streams(
        &transport
    );

    secure_print_stats(
        &transport
    );

    /*
     * Demonstrate constant-time comparison helper.
     */
    uint8_t a[4] =
        {1, 2, 3, 4};

    uint8_t b[4] =
        {1, 2, 3, 4};

    printf(
        "\nConstant-time comparison: %s\n",
        secure_constant_time_equal(
            a,
            b,
            sizeof(a)
        )
        ? "equal"
        : "different"
    );

    secure_transport_destroy(
        &transport
    );

    printf(
        "\nTransport securely shut down.\n"
    );

    return EXIT_SUCCESS;
}
What #8 now gives the rest of the system

The media engines can all use the same secure transport abstraction:

                    #1 AUDIO
                       │
                 audio frames
                       │
                       ▼
              ┌────────────────┐
              │                │
              │       #8       │
              │ SECURE LAYER   │
              │                │
              └───────┬────────┘
                      │
       ┌──────────────┼───────────────┐
       ▼              ▼               ▼
    stream 100     stream 200      stream 300
       │              │               │
      AUDIO          VIDEO          SCREEN
       │              │               │
       └──────────────┼───────────────┘
                      ▼
                    #3
                 NETWORK

So, for example:

secure_encrypt_packet(
    &transport,
    100,
    ST_PACKET_AUDIO,
    audio_frame,
    audio_size,
    &packet
);

and screen sharing can use:

secure_encrypt_packet(
    &transport,
    300,
    ST_PACKET_SCREEN,
    screen_frame,
    screen_size,
    &packet
);






/*
 * teams_notification_core.c
 *
 * Native C11 Notification & Event Engine
 *
 * Part of a Teams-style native communications client.
 *
 * Responsibilities:
 *
 *   - Application event bus
 *   - Typed events
 *   - Priority queue
 *   - Event subscriptions
 *   - Notification rules
 *   - Mentions
 *   - Incoming-call notifications
 *   - Participant events
 *   - Screen-share events
 *   - Network events
 *   - Message events
 *   - Badge counters
 *   - Per-conversation unread counts
 *   - Debounce
 *   - Notification throttling
 *   - Desktop notification abstraction
 *   - Sound abstraction
 *   - Haptic abstraction
 *   - Event history
 *   - Event acknowledgement
 *   - Event cancellation
 *   - Persistence hooks
 *   - Diagnostics
 *   - Stress testing
 *
 * Build:
 *
 *   gcc -std=c11 -O3 -march=native \
 *       -Wall -Wextra -pedantic \
 *       teams_notification_core.c \
 *       -o teams_notification \
 *
 * No external dependencies required.
 *
 * Production integrations can connect the output layer to:
 *
 *   Windows:
 *       Windows toast notifications / WinRT
 *
 *   macOS:
 *       UserNotifications
 *
 *   Linux:
 *       D-Bus / desktop notification service
 *
 *   iOS:
 *       UNUserNotificationCenter
 *
 *   Android:
 *       NotificationManager
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>


/* ============================================================
 * Configuration
 * ============================================================ */

#define NE_MAX_EVENTS              8192
#define NE_MAX_SUBSCRIBERS         256
#define NE_MAX_CONVERSATIONS       2048
#define NE_MAX_HISTORY             4096
#define NE_MAX_TEXT                512
#define NE_MAX_TITLE               160
#define NE_MAX_SENDER              128
#define NE_MAX_CHANNEL             128
#define NE_MAX_ATTACHMENT          256

#define NE_MAX_BADGE               9999

#define NE_DEFAULT_DEBOUNCE_MS     500
#define NE_NOTIFICATION_WINDOW_MS  10000
#define NE_MAX_NOTIFICATIONS_WINDOW 8

#define NE_EVENT_MAGIC             0x45564E54u


/* ============================================================
 * Time
 * ============================================================ */

static uint64_t ne_now_ms(void)
{
    struct timespec ts;

    if (timespec_get(
            &ts,
            TIME_UTC) != TIME_UTC)
    {
        return 0;
    }

    return
        (uint64_t)ts.tv_sec * 1000ULL +
        (uint64_t)ts.tv_nsec / 1000000ULL;
}


/* ============================================================
 * Utility
 * ============================================================ */

static int ne_clamp_int(
    int value,
    int minimum,
    int maximum)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}


static void ne_copy_string(
    char *destination,
    size_t capacity,
    const char *source)
{
    if (!destination ||
        capacity == 0)
        return;

    if (!source)
    {
        destination[0] = '\0';
        return;
    }

    strncpy(
        destination,
        source,
        capacity - 1
    );

    destination[
        capacity - 1
    ] = '\0';
}


/* ============================================================
 * Event types
 * ============================================================ */

typedef enum {

    NE_EVENT_NONE = 0,

    /* Messaging */
    NE_EVENT_MESSAGE_RECEIVED,
    NE_EVENT_MESSAGE_SENT,
    NE_EVENT_MESSAGE_EDITED,
    NE_EVENT_MESSAGE_DELETED,
    NE_EVENT_MESSAGE_MENTION,
    NE_EVENT_REACTION,

    /* Meetings */
    NE_EVENT_CALL_INCOMING,
    NE_EVENT_CALL_STARTED,
    NE_EVENT_CALL_ENDED,
    NE_EVENT_PARTICIPANT_JOINED,
    NE_EVENT_PARTICIPANT_LEFT,
    NE_EVENT_PARTICIPANT_MUTED,
    NE_EVENT_PARTICIPANT_UNMUTED,

    /* Screen sharing */
    NE_EVENT_SCREEN_SHARE_STARTED,
    NE_EVENT_SCREEN_SHARE_STOPPED,
    NE_EVENT_SCREEN_SHARE_CHANGED,

    /* Network */
    NE_EVENT_NETWORK_CONNECTED,
    NE_EVENT_NETWORK_DISCONNECTED,
    NE_EVENT_NETWORK_DEGRADED,
    NE_EVENT_NETWORK_RECOVERED,

    /* Recording */
    NE_EVENT_RECORDING_STARTED,
    NE_EVENT_RECORDING_STOPPED,

    /* Storage */
    NE_EVENT_SYNC_STARTED,
    NE_EVENT_SYNC_COMPLETED,
    NE_EVENT_SYNC_FAILED,

    /* System */
    NE_EVENT_APPLICATION_STARTED,
    NE_EVENT_APPLICATION_SUSPENDED,
    NE_EVENT_APPLICATION_RESUMED,

    NE_EVENT_COUNT

} NotificationEventType;


/* ============================================================
 * Priority
 * ============================================================ */

typedef enum {

    NE_PRIORITY_LOW = 0,
    NE_PRIORITY_NORMAL,
    NE_PRIORITY_HIGH,
    NE_PRIORITY_CRITICAL

} NotificationPriority;


/* ============================================================
 * Notification channel
 * ============================================================ */

typedef enum {

    NE_CHANNEL_NONE = 0,

    NE_CHANNEL_IN_APP,
    NE_CHANNEL_DESKTOP,
    NE_CHANNEL_SOUND,
    NE_CHANNEL_HAPTIC,
    NE_CHANNEL_BADGE

} NotificationChannel;


/* ============================================================
 * Event source
 * ============================================================ */

typedef enum {

    NE_SOURCE_UNKNOWN = 0,

    NE_SOURCE_AUDIO,
    NE_SOURCE_VIDEO,
    NE_SOURCE_NETWORK,
    NE_SOURCE_STORAGE,
    NE_SOURCE_SEARCH,
    NE_SOURCE_SCREEN,
    NE_SOURCE_MEETING,
    NE_SOURCE_SERVER,
    NE_SOURCE_USER,
    NE_SOURCE_SYSTEM

} NotificationEventSource;


/* ============================================================
 * Event
 * ============================================================ */

typedef struct {

    uint32_t magic;

    uint64_t id;

    NotificationEventType type;

    NotificationPriority priority;

    NotificationEventSource source;

    uint64_t timestamp_ms;

    uint32_t conversation_id;

    uint32_t participant_id;

    uint32_t channel_id;

    char sender[
        NE_MAX_SENDER
    ];

    char title[
        NE_MAX_TITLE
    ];

    char text[
        NE_MAX_TEXT
    ];

    char attachment[
        NE_MAX_ATTACHMENT
    ];

    bool requires_user_action;

    bool mention;

    bool persistent;

    bool silent;

    bool acknowledged;

    bool cancelled;

} NotificationEvent;


/* ============================================================
 * Queue
 * ============================================================ */

typedef struct {

    NotificationEvent events[
        NE_MAX_EVENTS
    ];

    size_t count;

} EventPriorityQueue;


/* ============================================================
 * Event handler
 * ============================================================ */

typedef void (*NotificationEventHandler)(
    const NotificationEvent *event,
    void *user_data
);


/* ============================================================
 * Subscription
 * ============================================================ */

typedef struct {

    uint64_t id;

    NotificationEventType type;

    NotificationPriority minimum_priority;

    NotificationEventSource source;

    bool filter_conversation;

    uint32_t conversation_id;

    NotificationEventHandler handler;

    void *user_data;

    bool active;

} EventSubscription;


/* ============================================================
 * Conversation state
 * ============================================================ */

typedef struct {

    uint32_t conversation_id;

    uint32_t unread_messages;

    uint32_t unread_mentions;

    uint32_t unread_reactions;

    uint32_t unread_calls;

    uint64_t last_message_event;

    uint64_t last_notification_ms;

    bool muted;

    bool archived;

} ConversationNotificationState;


/* ============================================================
 * Notification policy
 * ============================================================ */

typedef struct {

    bool enable_desktop;

    bool enable_sound;

    bool enable_haptic;

    bool enable_badges;

    bool notify_mentions;

    bool notify_direct_messages;

    bool notify_reactions;

    bool notify_participant_events;

    bool notify_screen_share;

    bool notify_network;

    bool notify_recording;

    bool suppress_when_in_meeting;

    bool suppress_when_window_focused;

    bool group_messages;

    uint32_t debounce_ms;

    uint32_t notification_window_ms;

    uint32_t maximum_notifications_window;

} NotificationPolicy;


/* ============================================================
 * Notification statistics
 * ============================================================ */

typedef struct {

    uint64_t events_published;

    uint64_t events_processed;

    uint64_t events_dropped;

    uint64_t events_cancelled;

    uint64_t notifications_generated;

    uint64_t desktop_notifications;

    uint64_t sound_notifications;

    uint64_t haptic_notifications;

    uint64_t badge_updates;

    uint64_t mentions;

    uint64_t messages;

    uint64_t calls;

    uint64_t participant_events;

    uint64_t network_events;

    uint64_t screen_events;

} NotificationStats;


/* ============================================================
 * Output adapters
 * ============================================================ */

typedef struct NotificationEngine
    NotificationEngine;


/* ============================================================
 * Desktop notification callback
 * ============================================================ */

typedef void (*DesktopNotificationFunction)(
    const char *title,
    const char *body,
    NotificationPriority priority,
    void *user_data
);


/* ============================================================
 * Sound callback
 * ============================================================ */

typedef void (*SoundNotificationFunction)(
    const char *sound_name,
    NotificationPriority priority,
    void *user_data
);


/* ============================================================
 * Haptic callback
 * ============================================================ */

typedef void (*HapticNotificationFunction)(
    NotificationPriority priority,
    void *user_data
);


/* ============================================================
 * Persistence callback
 * ============================================================ */

typedef void (*NotificationPersistenceFunction)(
    const NotificationEvent *event,
    void *user_data
);


/* ============================================================
 * Notification output
 * ============================================================ */

typedef struct {

    DesktopNotificationFunction
        desktop;

    SoundNotificationFunction
        sound;

    HapticNotificationFunction
        haptic;

    NotificationPersistenceFunction
        persist;

    void *user_data;

} NotificationOutput;


/* ============================================================
 * Event history
 * ============================================================ */

typedef struct {

    NotificationEvent events[
        NE_MAX_HISTORY
    ];

    size_t count;

    size_t next;

} EventHistory;


/* ============================================================
 * Rate limiter
 * ============================================================ */

typedef struct {

    uint64_t timestamps[
        NE_MAX_NOTIFICATIONS_WINDOW * 2
    ];

    size_t count;

} NotificationRateLimiter;


/* ============================================================
 * Engine
 * ============================================================ */

struct NotificationEngine {

    EventPriorityQueue queue;

    EventSubscription subscriptions[
        NE_MAX_SUBSCRIBERS
    ];

    size_t subscription_count;

    ConversationNotificationState conversations[
        NE_MAX_CONVERSATIONS
    ];

    size_t conversation_count;

    EventHistory history;

    NotificationRateLimiter rate_limiter;

    NotificationPolicy policy;

    NotificationOutput output;

    NotificationStats stats;

    uint64_t next_event_id;

    uint64_t next_subscription_id;

    uint32_t global_badge;

    bool application_focused;

    bool in_meeting;

    bool running;

};


/* ============================================================
 * Event name
 * ============================================================ */

static const char *
ne_event_name(
    NotificationEventType type)
{
    switch (type) {

        case NE_EVENT_MESSAGE_RECEIVED:
            return "message.received";

        case NE_EVENT_MESSAGE_SENT:
            return "message.sent";

        case NE_EVENT_MESSAGE_EDITED:
            return "message.edited";

        case NE_EVENT_MESSAGE_DELETED:
            return "message.deleted";

        case NE_EVENT_MESSAGE_MENTION:
            return "message.mention";

        case NE_EVENT_REACTION:
            return "message.reaction";

        case NE_EVENT_CALL_INCOMING:
            return "call.incoming";

        case NE_EVENT_CALL_STARTED:
            return "call.started";

        case NE_EVENT_CALL_ENDED:
            return "call.ended";

        case NE_EVENT_PARTICIPANT_JOINED:
            return "participant.joined";

        case NE_EVENT_PARTICIPANT_LEFT:
            return "participant.left";

        case NE_EVENT_PARTICIPANT_MUTED:
            return "participant.muted";

        case NE_EVENT_PARTICIPANT_UNMUTED:
            return "participant.unmuted";

        case NE_EVENT_SCREEN_SHARE_STARTED:
            return "screen.started";

        case NE_EVENT_SCREEN_SHARE_STOPPED:
            return "screen.stopped";

        case NE_EVENT_SCREEN_SHARE_CHANGED:
            return "screen.changed";

        case NE_EVENT_NETWORK_CONNECTED:
            return "network.connected";

        case NE_EVENT_NETWORK_DISCONNECTED:
            return "network.disconnected";

        case NE_EVENT_NETWORK_DEGRADED:
            return "network.degraded";

        case NE_EVENT_NETWORK_RECOVERED:
            return "network.recovered";

        case NE_EVENT_RECORDING_STARTED:
            return "recording.started";

        case NE_EVENT_RECORDING_STOPPED:
            return "recording.stopped";

        case NE_EVENT_SYNC_STARTED:
            return "sync.started";

        case NE_EVENT_SYNC_COMPLETED:
            return "sync.completed";

        case NE_EVENT_SYNC_FAILED:
            return "sync.failed";

        case NE_EVENT_APPLICATION_STARTED:
            return "application.started";

        case NE_EVENT_APPLICATION_SUSPENDED:
            return "application.suspended";

        case NE_EVENT_APPLICATION_RESUMED:
            return "application.resumed";

        default:
            return "unknown";
    }
}


/* ============================================================
 * Default policy
 * ============================================================ */

static NotificationPolicy
ne_default_policy(void)
{
    NotificationPolicy policy;

    memset(
        &policy,
        0,
        sizeof(policy)
    );

    policy.enable_desktop = true;
    policy.enable_sound = true;
    policy.enable_haptic = true;
    policy.enable_badges = true;

    policy.notify_mentions = true;
    policy.notify_direct_messages = true;
    policy.notify_reactions = true;

    policy.notify_participant_events = true;
    policy.notify_screen_share = true;
    policy.notify_network = true;
    policy.notify_recording = true;

    policy.suppress_when_in_meeting = false;
    policy.suppress_when_window_focused = false;

    policy.group_messages = true;

    policy.debounce_ms =
        NE_DEFAULT_DEBOUNCE_MS;

    policy.notification_window_ms =
        NE_NOTIFICATION_WINDOW_MS;

    policy.maximum_notifications_window =
        NE_MAX_NOTIFICATIONS_WINDOW;

    return policy;
}


/* ============================================================
 * Engine init
 * ============================================================ */

static bool ne_init(
    NotificationEngine *engine)
{
    if (!engine)
        return false;

    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->policy =
        ne_default_policy();

    engine->next_event_id =
        1;

    engine->next_subscription_id =
        1;

    engine->running =
        true;

    return true;
}


/* ============================================================
 * Queue priority comparison
 * ============================================================ */

static bool ne_higher_priority(
    const NotificationEvent *a,
    const NotificationEvent *b)
{
    if (a->priority !=
        b->priority)
    {
        return
            a->priority >
            b->priority;
    }

    return
        a->timestamp_ms <
        b->timestamp_ms;
}


/* ============================================================
 * Queue push
 * ============================================================ */

static bool ne_queue_push(
    EventPriorityQueue *queue,
    const NotificationEvent *event)
{
    if (!queue ||
        !event)
        return false;

    if (queue->count >=
        NE_MAX_EVENTS)
    {
        return false;
    }

    size_t index =
        queue->count++;

    queue->events[index] =
        *event;

    /*
     * Binary heap.
     */
    while (index > 0)
    {
        size_t parent =
            (index - 1) / 2;

        if (!ne_higher_priority(
                &queue->events[index],
                &queue->events[parent]))
        {
            break;
        }

        NotificationEvent temp =
            queue->events[parent];

        queue->events[parent] =
            queue->events[index];

        queue->events[index] =
            temp;

        index =
            parent;
    }

    return true;
}


/* ============================================================
 * Queue pop
 * ============================================================ */

static bool ne_queue_pop(
    EventPriorityQueue *queue,
    NotificationEvent *event)
{
    if (!queue ||
        !event ||
        queue->count == 0)
    {
        return false;
    }

    *event =
        queue->events[0];

    queue->count--;

    if (queue->count == 0)
        return true;

    queue->events[0] =
        queue->events[
            queue->count
        ];

    size_t index = 0;

    while (true)
    {
        size_t left =
            index * 2 + 1;

        size_t right =
            index * 2 + 2;

        size_t best =
            index;

        if (left < queue->count &&
            ne_higher_priority(
                &queue->events[left],
                &queue->events[best]))
        {
            best = left;
        }

        if (right < queue->count &&
            ne_higher_priority(
                &queue->events[right],
                &queue->events[best]))
        {
            best = right;
        }

        if (best == index)
            break;

        NotificationEvent temp =
            queue->events[index];

        queue->events[index] =
            queue->events[best];

        queue->events[best] =
            temp;

        index =
            best;
    }

    return true;
}


/* ============================================================
 * Conversation lookup
 * ============================================================ */

static ConversationNotificationState *
ne_find_conversation(
    NotificationEngine *engine,
    uint32_t conversation_id)
{
    for (size_t i = 0;
         i < engine->conversation_count;
         ++i)
    {
        if (engine->conversations[i]
                .conversation_id ==
            conversation_id)
        {
            return
                &engine->conversations[i];
        }
    }

    return NULL;
}


/* ============================================================
 * Conversation creation
 * ============================================================ */

static ConversationNotificationState *
ne_get_conversation(
    NotificationEngine *engine,
    uint32_t conversation_id)
{
    ConversationNotificationState *state =
        ne_find_conversation(
            engine,
            conversation_id
        );

    if (state)
        return state;

    if (engine->conversation_count >=
        NE_MAX_CONVERSATIONS)
    {
        return NULL;
    }

    state =
        &engine->conversations[
            engine->conversation_count++
        ];

    memset(
        state,
        0,
        sizeof(*state)
    );

    state->conversation_id =
        conversation_id;

    return state;
}


/* ============================================================
 * Subscription matching
 * ============================================================ */

static bool ne_subscription_matches(
    const EventSubscription *subscription,
    const NotificationEvent *event)
{
    if (!subscription ||
        !event ||
        !subscription->active)
    {
        return false;
    }

    if (subscription->type !=
        NE_EVENT_NONE &&
        subscription->type !=
        event->type)
    {
        return false;
    }

    if (event->priority <
        subscription->minimum_priority)
    {
        return false;
    }

    if (subscription->source !=
        NE_SOURCE_UNKNOWN &&
        subscription->source !=
        event->source)
    {
        return false;
    }

    if (subscription->filter_conversation &&
        subscription->conversation_id !=
        event->conversation_id)
    {
        return false;
    }

    return true;
}


/* ============================================================
 * Subscribe
 * ============================================================ */

static uint64_t ne_subscribe(
    NotificationEngine *engine,
    NotificationEventType type,
    NotificationPriority minimum_priority,
    NotificationEventSource source,
    NotificationEventHandler handler,
    void *user_data)
{
    if (!engine ||
        !handler)
    {
        return 0;
    }

    if (engine->subscription_count >=
        NE_MAX_SUBSCRIBERS)
    {
        return 0;
    }

    EventSubscription *subscription =
        &engine->subscriptions[
            engine->subscription_count++
        ];

    memset(
        subscription,
        0,
        sizeof(*subscription)
    );

    subscription->id =
        engine->next_subscription_id++;

    subscription->type =
        type;

    subscription->minimum_priority =
        minimum_priority;

    subscription->source =
        source;

    subscription->handler =
        handler;

    subscription->user_data =
        user_data;

    subscription->active =
        true;

    return subscription->id;
}


/* ============================================================
 * Unsubscribe
 * ============================================================ */

static bool ne_unsubscribe(
    NotificationEngine *engine,
    uint64_t subscription_id)
{
    if (!engine ||
        subscription_id == 0)
        return false;

    for (size_t i = 0;
         i < engine->subscription_count;
         ++i)
    {
        if (engine->subscriptions[i].id ==
            subscription_id)
        {
            engine->subscriptions[i].active =
                false;

            return true;
        }
    }

    return false;
}


/* ============================================================
 * History
 * ============================================================ */

static void ne_history_add(
    EventHistory *history,
    const NotificationEvent *event)
{
    if (!history ||
        !event)
        return;

    if (history->count <
        NE_MAX_HISTORY)
    {
        history->events[
            history->count++
        ] = *event;

        return;
    }

    history->events[
        history->next
    ] = *event;

    history->next =
        (history->next + 1) %
        NE_MAX_HISTORY;
}


/* ============================================================
 * Badge
 * ============================================================ */

static void ne_recalculate_badge(
    NotificationEngine *engine)
{
    uint64_t badge = 0;

    for (size_t i = 0;
         i < engine->conversation_count;
         ++i)
    {
        const ConversationNotificationState *state =
            &engine->conversations[i];

        if (state->archived)
            continue;

        badge +=
            state->unread_messages;

        badge +=
            state->unread_mentions;
    }

    if (badge >
        NE_MAX_BADGE)
    {
        badge =
            NE_MAX_BADGE;
    }

    engine->global_badge =
        (uint32_t)badge;

    engine->stats.badge_updates++;

    if (engine->output.persist)
    {
        /*
         * Badge persistence is intentionally represented
         * through the event persistence hook rather than
         * tied to a specific database.
         */
    }
}


/* ============================================================
 * Message state
 * ============================================================ */

static void ne_update_message_state(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    ConversationNotificationState *state =
        ne_get_conversation(
            engine,
            event->conversation_id
        );

    if (!state)
        return;

    if (state->muted)
        return;

    if (event->type ==
        NE_EVENT_MESSAGE_RECEIVED)
    {
        state->unread_messages++;

        state->last_message_event =
            event->id;

        engine->stats.messages++;
    }

    if (event->type ==
        NE_EVENT_MESSAGE_MENTION)
    {
        state->unread_messages++;
        state->unread_mentions++;

        state->last_message_event =
            event->id;

        engine->stats.mentions++;
    }

    if (event->type ==
        NE_EVENT_REACTION)
    {
        state->unread_reactions++;
    }

    ne_recalculate_badge(
        engine
    );
}


/* ============================================================
 * Call state
 * ============================================================ */

static void ne_update_call_state(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    ConversationNotificationState *state =
        ne_get_conversation(
            engine,
            event->conversation_id
        );

    if (!state)
        return;

    if (event->type ==
        NE_EVENT_CALL_INCOMING)
    {
        state->unread_calls++;

        engine->stats.calls++;

        ne_recalculate_badge(
            engine
        );
    }
}


/* ============================================================
 * Notification rate limiter
 * ============================================================ */

static void ne_rate_limiter_cleanup(
    NotificationEngine *engine,
    uint64_t now)
{
    uint64_t window =
        engine->policy.notification_window_ms;

    size_t write = 0;

    for (size_t i = 0;
         i < engine->rate_limiter.count;
         ++i)
    {
        uint64_t timestamp =
            engine->rate_limiter.timestamps[i];

        if (now - timestamp <= window)
        {
            engine->rate_limiter.timestamps[
                write++
            ] = timestamp;
        }
    }

    engine->rate_limiter.count =
        write;
}


/* ============================================================
 * Notification rate check
 * ============================================================ */

static bool ne_rate_limit_allows(
    NotificationEngine *engine)
{
    uint64_t now =
        ne_now_ms();

    ne_rate_limiter_cleanup(
        engine,
        now
    );

    if (engine->rate_limiter.count >=
        engine->policy.maximum_notifications_window)
    {
        return false;
    }

    if (engine->rate_limiter.count <
        sizeof(engine->rate_limiter.timestamps) /
        sizeof(engine->rate_limiter.timestamps[0]))
    {
        engine->rate_limiter.timestamps[
            engine->rate_limiter.count++
        ] = now;
    }

    return true;
}


/* ============================================================
 * Debounce
 * ============================================================ */

static bool ne_should_debounce(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    if (!engine ||
        !event)
        return true;

    ConversationNotificationState *state =
        ne_find_conversation(
            engine,
            event->conversation_id
        );

    if (!state)
        return false;

    uint64_t now =
        event->timestamp_ms;

    if (state->last_notification_ms == 0)
        return false;

    if (now <
        state->last_notification_ms)
        return false;

    return
        now -
        state->last_notification_ms <
        engine->policy.debounce_ms;
}


/* ============================================================
 * Notification decision
 * ============================================================ */

static bool ne_should_notify(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    if (!engine ||
        !event)
        return false;

    if (event->silent)
        return false;

    if (event->cancelled)
        return false;

    /*
     * Critical events bypass most normal filtering.
     */
    if (event->priority ==
        NE_PRIORITY_CRITICAL)
    {
        return true;
    }

    if (event->type ==
        NE_EVENT_MESSAGE_MENTION)
    {
        return engine->policy.notify_mentions;
    }

    if (event->type ==
        NE_EVENT_MESSAGE_RECEIVED)
    {
        if (!engine->policy.notify_direct_messages)
            return false;

        if (engine->policy.suppress_when_window_focused &&
            engine->application_focused)
        {
            return false;
        }

        if (engine->policy.suppress_when_in_meeting &&
            engine->in_meeting)
        {
            return false;
        }

        return true;
    }

    if (event->type ==
        NE_EVENT_REACTION)
    {
        return
            engine->policy.notify_reactions;
    }

    if (event->type ==
        NE_EVENT_CALL_INCOMING)
    {
        return true;
    }

    if (event->type ==
            NE_EVENT_PARTICIPANT_JOINED ||
        event->type ==
            NE_EVENT_PARTICIPANT_LEFT ||
        event->type ==
            NE_EVENT_PARTICIPANT_MUTED ||
        event->type ==
            NE_EVENT_PARTICIPANT_UNMUTED)
    {
        return
            engine->policy.notify_participant_events;
    }

    if (event->type ==
            NE_EVENT_SCREEN_SHARE_STARTED ||
        event->type ==
            NE_EVENT_SCREEN_SHARE_STOPPED ||
        event->type ==
            NE_EVENT_SCREEN_SHARE_CHANGED)
    {
        return
            engine->policy.notify_screen_share;
    }

    if (event->type ==
            NE_EVENT_NETWORK_CONNECTED ||
        event->type ==
            NE_EVENT_NETWORK_DISCONNECTED ||
        event->type ==
            NE_EVENT_NETWORK_DEGRADED ||
        event->type ==
            NE_EVENT_NETWORK_RECOVERED)
    {
        return
            engine->policy.notify_network;
    }

    if (event->type ==
            NE_EVENT_RECORDING_STARTED ||
        event->type ==
            NE_EVENT_RECORDING_STOPPED)
    {
        return
            engine->policy.notify_recording;
    }

    return false;
}


/* ============================================================
 * Select sound
 * ============================================================ */

static const char *
ne_sound_for_event(
    const NotificationEvent *event)
{
    if (!event)
        return "default";

    switch (event->type) {

        case NE_EVENT_CALL_INCOMING:
            return "incoming-call";

        case NE_EVENT_MESSAGE_MENTION:
            return "mention";

        case NE_EVENT_MESSAGE_RECEIVED:
            return "message";

        case NE_EVENT_REACTION:
            return "reaction";

        case NE_EVENT_NETWORK_DISCONNECTED:
            return "network-error";

        case NE_EVENT_NETWORK_RECOVERED:
            return "network-recovered";

        default:
            return "notification";
    }
}


/* ============================================================
 * Dispatch desktop
 * ============================================================ */

static void ne_dispatch_desktop(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    if (!engine->policy.enable_desktop)
        return;

    if (!engine->output.desktop)
        return;

    engine->output.desktop(
        event->title,
        event->text,
        event->priority,
        engine->output.user_data
    );

    engine->stats.desktop_notifications++;
}


/* ============================================================
 * Dispatch sound
 * ============================================================ */

static void ne_dispatch_sound(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    if (!engine->policy.enable_sound)
        return;

    if (!engine->output.sound)
        return;

    engine->output.sound(
        ne_sound_for_event(event),
        event->priority,
        engine->output.user_data
    );

    engine->stats.sound_notifications++;
}


/* ============================================================
 * Dispatch haptic
 * ============================================================ */

static void ne_dispatch_haptic(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    if (!engine->policy.enable_haptic)
        return;

    if (!engine->output.haptic)
        return;

    engine->output.haptic(
        event->priority,
        engine->output.user_data
    );

    engine->stats.haptic_notifications++;
}


/* ============================================================
 * Notification dispatch
 * ============================================================ */

static void ne_dispatch_notification(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    if (!ne_should_notify(
            engine,
            event))
    {
        return;
    }

    if (!ne_rate_limit_allows(
            engine))
    {
        return;
    }

    if (ne_should_debounce(
            engine,
            event))
    {
        return;
    }

    ConversationNotificationState *state =
        ne_get_conversation(
            engine,
            event->conversation_id
        );

    if (state)
    {
        state->last_notification_ms =
            event->timestamp_ms;
    }

    /*
     * Call notifications should be visually and audibly
     * distinct from normal messages.
     */
    if (engine->policy.enable_desktop)
    {
        ne_dispatch_desktop(
            engine,
            event
        );
    }

    /*
     * Calls and mentions are high-attention events.
     */
    if (event->priority >=
        NE_PRIORITY_HIGH)
    {
        ne_dispatch_sound(
            engine,
            event
        );

        ne_dispatch_haptic(
            engine,
            event
        );
    }
    else
    {
        ne_dispatch_sound(
            engine,
            event
        );
    }

    engine->stats.notifications_generated++;
}


/* ============================================================
 * Publish event
 * ============================================================ */

static uint64_t ne_publish(
    NotificationEngine *engine,
    NotificationEvent event)
{
    if (!engine ||
        !engine->running)
    {
        return 0;
    }

    event.magic =
        NE_EVENT_MAGIC;

    event.id =
        engine->next_event_id++;

    if (event.timestamp_ms == 0)
    {
        event.timestamp_ms =
            ne_now_ms();
    }

    if (!ne_queue_push(
            &engine->queue,
            &event))
    {
        engine->stats.events_dropped++;

        return 0;
    }

    engine->stats.events_published++;

    return event.id;
}


/* ============================================================
 * Process subscribers
 * ============================================================ */

static void ne_dispatch_subscribers(
    NotificationEngine *engine,
    const NotificationEvent *event)
{
    for (size_t i = 0;
         i < engine->subscription_count;
         ++i)
    {
        EventSubscription *subscription =
            &engine->subscriptions[i];

        if (!ne_subscription_matches(
                subscription,
                event))
        {
            continue;
        }

        subscription->handler(
            event,
            subscription->user_data
        );
    }
}


/* ============================================================
 * Process event
 * ============================================================ */

static void ne_process_event(
    NotificationEngine *engine,
    NotificationEvent *event)
{
    if (!engine ||
        !event)
        return;

    if (event->magic !=
        NE_EVENT_MAGIC)
    {
        return;
    }

    if (event->cancelled)
    {
        engine->stats.events_cancelled++;
        return;
    }

    /*
     * Update local state.
     */
    switch (event->type) {

        case NE_EVENT_MESSAGE_RECEIVED:
        case NE_EVENT_MESSAGE_MENTION:
        case NE_EVENT_REACTION:

            ne_update_message_state(
                engine,
                event
            );

            break;

        case NE_EVENT_CALL_INCOMING:

            ne_update_call_state(
                engine,
                event
            );

            break;

        case NE_EVENT_PARTICIPANT_JOINED:
        case NE_EVENT_PARTICIPANT_LEFT:
        case NE_EVENT_PARTICIPANT_MUTED:
        case NE_EVENT_PARTICIPANT_UNMUTED:

            engine->stats.participant_events++;

            break;

        case NE_EVENT_NETWORK_CONNECTED:
        case NE_EVENT_NETWORK_DISCONNECTED:
        case NE_EVENT_NETWORK_DEGRADED:
        case NE_EVENT_NETWORK_RECOVERED:

            engine->stats.network_events++;

            break;

        case NE_EVENT_SCREEN_SHARE_STARTED:
        case NE_EVENT_SCREEN_SHARE_STOPPED:
        case NE_EVENT_SCREEN_SHARE_CHANGED:

            engine->stats.screen_events++;

            break;

        default:
            break;
    }

    /*
     * Persist important events.
     */
    if (event->persistent &&
        engine->output.persist)
    {
        engine->output.persist(
            event,
            engine->output.user_data
        );
    }

    /*
     * Add to event history.
     */
    ne_history_add(
        &engine->history,
        event
    );

    /*
     * Notify subscribers.
     */
    ne_dispatch_subscribers(
        engine,
        event
    );

    /*
     * Produce user-facing notification.
     */
    ne_dispatch_notification(
        engine,
        event
    );

    engine->stats.events_processed++;
}


/* ============================================================
 * Process queue
 * ============================================================ */

static size_t ne_process(
    NotificationEngine *engine,
    size_t maximum_events)
{
    if (!engine)
        return 0;

    size_t processed = 0;

    NotificationEvent event;

    while (
        processed <
            maximum_events &&
        ne_queue_pop(
            &engine->queue,
            &event))
    {
        ne_process_event(
            engine,
            &event
        );

        processed++;
    }

    return processed;
}


/* ============================================================
 * Acknowledge conversation
 * ============================================================ */

static bool ne_acknowledge_conversation(
    NotificationEngine *engine,
    uint32_t conversation_id)
{
    ConversationNotificationState *state =
        ne_find_conversation(
            engine,
            conversation_id
        );

    if (!state)
        return false;

    state->unread_messages = 0;
    state->unread_mentions = 0;
    state->unread_reactions = 0;
    state->unread_calls = 0;

    ne_recalculate_badge(
        engine
    );

    return true;
}


/* ============================================================
 * Mute conversation
 * ============================================================ */

static bool ne_set_conversation_muted(
    NotificationEngine *engine,
    uint32_t conversation_id,
    bool muted)
{
    ConversationNotificationState *state =
        ne_get_conversation(
            engine,
            conversation_id
        );

    if (!state)
        return false;

    state->muted =
        muted;

    return true;
}


/* ============================================================
 * Archive conversation
 * ============================================================ */

static bool ne_set_conversation_archived(
    NotificationEngine *engine,
    uint32_t conversation_id,
    bool archived)
{
    ConversationNotificationState *state =
        ne_get_conversation(
            engine,
            conversation_id
        );

    if (!state)
        return false;

    state->archived =
        archived;

    ne_recalculate_badge(
        engine
    );

    return true;
}


/* ============================================================
 * Application state
 * ============================================================ */

static void ne_set_application_focused(
    NotificationEngine *engine,
    bool focused)
{
    if (!engine)
        return;

    engine->application_focused =
        focused;
}


static void ne_set_in_meeting(
    NotificationEngine *engine,
    bool in_meeting)
{
    if (!engine)
        return;

    engine->in_meeting =
        in_meeting;
}


/* ============================================================
 * Event builders
 * ============================================================ */

static NotificationEvent
ne_make_message_event(
    uint32_t conversation_id,
    const char *sender,
    const char *text,
    bool mention)
{
    NotificationEvent event;

    memset(
        &event,
        0,
        sizeof(event)
    );

    event.type =
        mention ?
        NE_EVENT_MESSAGE_MENTION :
        NE_EVENT_MESSAGE_RECEIVED;

    event.priority =
        mention ?
        NE_PRIORITY_HIGH :
        NE_PRIORITY_NORMAL;

    event.source =
        NE_SOURCE_SERVER;

    event.conversation_id =
        conversation_id;

    event.persistent =
        true;

    event.mention =
        mention;

    ne_copy_string(
        event.sender,
        sizeof(event.sender),
        sender
    );

    ne_copy_string(
        event.title,
        sizeof(event.title),
        sender
    );

    ne_copy_string(
        event.text,
        sizeof(event.text),
        text
    );

    return event;
}


static NotificationEvent
ne_make_call_event(
    uint32_t conversation_id,
    const char *caller)
{
    NotificationEvent event;

    memset(
        &event,
        0,
        sizeof(event)
    );

    event.type =
        NE_EVENT_CALL_INCOMING;

    event.priority =
        NE_PRIORITY_CRITICAL;

    event.source =
        NE_SOURCE_MEETING;

    event.conversation_id =
        conversation_id;

    event.requires_user_action =
        true;

    event.persistent =
        false;

    ne_copy_string(
        event.sender,
        sizeof(event.sender),
        caller
    );

    ne_copy_string(
        event.title,
        sizeof(event.title),
        "Incoming call"
    );

    ne_copy_string(
        event.text,
        sizeof(event.text),
        "Incoming call from caller"
    );

    return event;
}


static NotificationEvent
ne_make_network_event(
    NotificationEventType type,
    NotificationPriority priority,
    const char *text)
{
    NotificationEvent event;

    memset(
        &event,
        0,
        sizeof(event)
    );

    event.type =
        type;

    event.priority =
        priority;

    event.source =
        NE_SOURCE_NETWORK;

    event.persistent =
        false;

    ne_copy_string(
        event.title,
        sizeof(event.title),
        "Network"
    );

    ne_copy_string(
        event.text,
        sizeof(event.text),
        text
    );

    return event;
}


/* ============================================================
 * Console adapters
 * ============================================================ */

static void console_desktop_notification(
    const char *title,
    const char *body,
    NotificationPriority priority,
    void *user_data)
{
    (void)user_data;

    printf(
        "\n[DESKTOP][priority=%d]\n"
        "  %s\n"
        "  %s\n",
        priority,
        title,
        body
    );
}


static void console_sound_notification(
    const char *sound_name,
    NotificationPriority priority,
    void *user_data)
{
    (void)user_data;

    printf(
        "[SOUND][priority=%d] %s\n",
        priority,
        sound_name
    );
}


static void console_haptic_notification(
    NotificationPriority priority,
    void *user_data)
{
    (void)user_data;

    printf(
        "[HAPTIC][priority=%d]\n",
        priority
    );
}


static void console_persistence(
    const NotificationEvent *event,
    void *user_data)
{
    (void)user_data;

    printf(
        "[PERSIST] event=%llu type=%s\n",
        (unsigned long long)
        event->id,
        ne_event_name(event->type)
    );
}


/* ============================================================
 * Subscriber examples
 * ============================================================ */

static void message_logger(
    const NotificationEvent *event,
    void *user_data)
{
    (void)user_data;

    printf(
        "[EVENT BUS] #%llu %s: %s\n",
        (unsigned long long)
        event->id,
        ne_event_name(event->type),
        event->text
    );
}


static void meeting_logger(
    const NotificationEvent *event,
    void *user_data)
{
    (void)user_data;

    printf(
        "[MEETING] %s participant=%u\n",
        ne_event_name(event->type),
        event->participant_id
    );
}


/* ============================================================
 * Diagnostics
 * ============================================================ */

static void ne_print_stats(
    const NotificationEngine *engine)
{
    const NotificationStats *s =
        &engine->stats;

    printf(
        "\n"
        "============================================\n"
        " NOTIFICATION ENGINE STATISTICS\n"
        "============================================\n"
    );

    printf(
        "Events published:       %llu\n",
        (unsigned long long)
        s->events_published
    );

    printf(
        "Events processed:       %llu\n",
        (unsigned long long)
        s->events_processed
    );

    printf(
        "Events dropped:         %llu\n",
        (unsigned long long)
        s->events_dropped
    );

    printf(
        "Events cancelled:       %llu\n",
        (unsigned long long)
        s->events_cancelled
    );

    printf(
        "Notifications:          %llu\n",
        (unsigned long long)
        s->notifications_generated
    );

    printf(
        "Desktop notifications:  %llu\n",
        (unsigned long long)
        s->desktop_notifications
    );

    printf(
        "Sound notifications:    %llu\n",
        (unsigned long long)
        s->sound_notifications
    );

    printf(
        "Haptic notifications:   %llu\n",
        (unsigned long long)
        s->haptic_notifications
    );

    printf(
        "Badge updates:          %llu\n",
        (unsigned long long)
        s->badge_updates
    );

    printf(
        "Messages:               %llu\n",
        (unsigned long long)
        s->messages
    );

    printf(
        "Mentions:               %llu\n",
        (unsigned long long)
        s->mentions
    );

    printf(
        "Calls:                  %llu\n",
        (unsigned long long)
        s->calls
    );

    printf(
        "Participant events:     %llu\n",
        (unsigned long long)
        s->participant_events
    );

    printf(
        "Network events:         %llu\n",
        (unsigned long long)
        s->network_events
    );

    printf(
        "Screen events:          %llu\n",
        (unsigned long long)
        s->screen_events
    );

    printf(
        "Global badge:           %u\n",
        engine->global_badge
    );

    printf(
        "Queue depth:            %zu\n",
        engine->queue.count
    );

    printf(
        "Subscriptions:          %zu\n",
        engine->subscription_count
    );

    printf(
        "Conversations:           %zu\n",
        engine->conversation_count
    );

    printf(
        "============================================\n"
    );
}


/* ============================================================
 * Conversation diagnostics
 * ============================================================ */

static void ne_print_conversations(
    const NotificationEngine *engine)
{
    printf(
        "\nConversation notification state:\n"
    );

    for (size_t i = 0;
         i < engine->conversation_count;
         ++i)
    {
        const ConversationNotificationState *state =
            &engine->conversations[i];

        printf(
            "  conversation=%u "
            "messages=%u "
            "mentions=%u "
            "reactions=%u "
            "calls=%u "
            "muted=%s "
            "archived=%s\n",

            state->conversation_id,

            state->unread_messages,

            state->unread_mentions,

            state->unread_reactions,

            state->unread_calls,

            state->muted ?
            "yes" :
            "no",

            state->archived ?
            "yes" :
            "no"
        );
    }
}


/* ============================================================
 * Stress test
 * ============================================================ */

static void ne_stress_test(
    NotificationEngine *engine,
    size_t event_count)
{
    printf(
        "\nRunning notification stress test: "
        "%zu events\n",
        event_count
    );

    uint64_t start =
        ne_now_ms();

    for (size_t i = 0;
         i < event_count;
         ++i)
    {
        NotificationEvent event =
            ne_make_message_event(
                (uint32_t)
                (i % 100),
                "User",
                "Synthetic message",
                (i % 37) == 0
            );

        ne_publish(
            engine,
            event
        );
    }

    ne_process(
        engine,
        event_count
    );

    uint64_t end =
        ne_now_ms();

    double elapsed =
        (double)(end - start);

    printf(
        "Stress test elapsed: %.3f ms\n",
        elapsed
    );

    if (elapsed > 0)
    {
        printf(
            "Events/sec: %.0f\n",
            ((double)event_count /
             elapsed) *
            1000.0
        );
    }
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        " TEAMS-STYLE NOTIFICATION ENGINE\n"
        "============================================\n"
    );

    NotificationEngine engine;

    if (!ne_init(
            &engine))
    {
        fprintf(
            stderr,
            "Notification engine init failed.\n"
        );

        return EXIT_FAILURE;
    }

    /*
     * Configure output adapters.
     */
    engine.output.desktop =
        console_desktop_notification;

    engine.output.sound =
        console_sound_notification;

    engine.output.haptic =
        console_haptic_notification;

    engine.output.persist =
        console_persistence;

    /*
     * Subscribe to all messaging events.
     */
    ne_subscribe(
        &engine,
        NE_EVENT_NONE,
        NE_PRIORITY_LOW,
        NE_SOURCE_UNKNOWN,
        message_logger,
        NULL
    );

    /*
     * Subscribe specifically to meeting events.
     */
    ne_subscribe(
        &engine,
        NE_EVENT_PARTICIPANT_JOINED,
        NE_PRIORITY_LOW,
        NE_SOURCE_MEETING,
        meeting_logger,
        NULL
    );

    /*
     * --------------------------------------------------------
     * MESSAGE
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Normal message\n"
    );

    NotificationEvent message =
        ne_make_message_event(
            100,
            "Alice",
            "The meeting starts in five minutes.",
            false
        );

    ne_publish(
        &engine,
        message
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * MENTION
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Mention\n"
    );

    NotificationEvent mention =
        ne_make_message_event(
            100,
            "Bob",
            "@you please review the document",
            true
        );

    ne_publish(
        &engine,
        mention
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * INCOMING CALL
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Incoming call\n"
    );

    NotificationEvent call =
        ne_make_call_event(
            200,
            "Charlie"
        );

    ne_copy_string(
        call.text,
        sizeof(call.text),
        "Charlie is calling you."
    );

    ne_publish(
        &engine,
        call
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * SCREEN SHARE
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Screen share\n"
    );

    NotificationEvent screen;

    memset(
        &screen,
        0,
        sizeof(screen)
    );

    screen.type =
        NE_EVENT_SCREEN_SHARE_STARTED;

    screen.priority =
        NE_PRIORITY_NORMAL;

    screen.source =
        NE_SOURCE_SCREEN;

    screen.conversation_id =
        200;

    ne_copy_string(
        screen.title,
        sizeof(screen.title),
        "Screen sharing"
    );

    ne_copy_string(
        screen.text,
        sizeof(screen.text),
        "Alice started sharing her screen."
    );

    ne_publish(
        &engine,
        screen
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * NETWORK DEGRADATION
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Network degradation\n"
    );

    NotificationEvent network =
        ne_make_network_event(
            NE_EVENT_NETWORK_DEGRADED,
            NE_PRIORITY_HIGH,
            "Network quality has degraded."
        );

    ne_publish(
        &engine,
        network
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * NETWORK RECOVERY
     * --------------------------------------------------------
     */

    network =
        ne_make_network_event(
            NE_EVENT_NETWORK_RECOVERED,
            NE_PRIORITY_NORMAL,
            "Network quality has recovered."
        );

    ne_publish(
        &engine,
        network
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * PARTICIPANT
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Participant joined\n"
    );

    NotificationEvent participant;

    memset(
        &participant,
        0,
        sizeof(participant)
    );

    participant.type =
        NE_EVENT_PARTICIPANT_JOINED;

    participant.priority =
        NE_PRIORITY_NORMAL;

    participant.source =
        NE_SOURCE_MEETING;

    participant.conversation_id =
        200;

    participant.participant_id =
        42;

    ne_copy_string(
        participant.title,
        sizeof(participant.title),
        "Participant joined"
    );

    ne_copy_string(
        participant.text,
        sizeof(participant.text),
        "David joined the meeting."
    );

    ne_publish(
        &engine,
        participant
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * RECORDING
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Recording started\n"
    );

    NotificationEvent recording;

    memset(
        &recording,
        0,
        sizeof(recording)
    );

    recording.type =
        NE_EVENT_RECORDING_STARTED;

    recording.priority =
        NE_PRIORITY_HIGH;

    recording.source =
        NE_SOURCE_MEETING;

    recording.conversation_id =
        200;

    ne_copy_string(
        recording.title,
        sizeof(recording.title),
        "Recording started"
    );

    ne_copy_string(
        recording.text,
        sizeof(recording.text),
        "This meeting is now being recorded."
    );

    ne_publish(
        &engine,
        recording
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * MUTE CONVERSATION
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Muting conversation 100\n"
    );

    ne_set_conversation_muted(
        &engine,
        100,
        true
    );

    NotificationEvent muted_message =
        ne_make_message_event(
            100,
            "Alice",
            "This message should still be stored but "
            "the conversation is muted.",
            false
        );

    ne_publish(
        &engine,
        muted_message
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * ACKNOWLEDGE
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Acknowledge conversation 100\n"
    );

    ne_acknowledge_conversation(
        &engine,
        100
    );

    /*
     * --------------------------------------------------------
     * FOCUS STATE
     * --------------------------------------------------------
     */

    printf(
        "\n[TEST] Focused application\n"
    );

    engine.policy.suppress_when_window_focused =
        true;

    ne_set_application_focused(
        &engine,
        true
    );

    NotificationEvent focused_message =
        ne_make_message_event(
            300,
            "Eve",
            "You already have the application open.",
            false
        );

    ne_publish(
        &engine,
        focused_message
    );

    ne_process(
        &engine,
        32
    );

    /*
     * --------------------------------------------------------
     * DIAGNOSTICS
     * --------------------------------------------------------
     */

    ne_print_conversations(
        &engine
    );

    ne_print_stats(
        &engine
    );

    /*
     * --------------------------------------------------------
     * STRESS TEST
     * --------------------------------------------------------
     */

    /*
     * Disable console spam from the stress test.
     */
    engine.policy.enable_desktop =
        false;

    engine.policy.enable_sound =
        false;

    engine.policy.enable_haptic =
        false;

    ne_stress_test(
        &engine,
        5000
    );

    /*
     * Final statistics.
     */
    ne_print_stats(
        &engine
    );

    printf(
        "\nNotification engine shutdown.\n"
    );

    return EXIT_SUCCESS;
}







/*
 * teams_diagnostics_core.c
 *
 * Native C11 Diagnostics & Telemetry Engine
 *
 * Designed to sit above:
 *
 *   #1 Audio Engine
 *   #2 Video Engine
 *   #3 Adaptive Network Engine
 *   #4 Meeting Performance Manager
 *   #5 Search Engine
 *   #6 Storage Engine
 *   #7 Screen Capture Engine
 *   #8 Secure Transport
 *   #9 Notification/Event Engine
 *
 * Features:
 *
 *   - Counters
 *   - Gauges
 *   - Histograms
 *   - Rolling statistics
 *   - Event logging
 *   - Trace spans
 *   - Breadcrumbs
 *   - Subsystem health
 *   - Anomaly detection
 *   - Threshold alerts
 *   - Sampling
 *   - Privacy redaction
 *   - JSON export
 *   - Diagnostics snapshots
 *   - Crash breadcrumbs
 *   - Runtime statistics
 *   - Stress testing
 *
 * Build:
 *
 * gcc -std=c11 -O3 -march=native \
 *     -Wall -Wextra -pedantic \
 *     teams_diagnostics_core.c \
 *     -o teams_diagnostics -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <math.h>


/* ============================================================
 * Configuration
 * ============================================================ */

#define DIAG_MAX_METRICS          1024
#define DIAG_MAX_EVENTS           4096
#define DIAG_MAX_BREADCRUMBS      1024
#define DIAG_MAX_TRACES           1024
#define DIAG_MAX_SUBSYSTEMS       32
#define DIAG_MAX_NAME             96
#define DIAG_MAX_MESSAGE          512
#define DIAG_MAX_TAG              64

#define DIAG_HISTOGRAM_BUCKETS    32
#define DIAG_ROLLING_SAMPLES      256

#define DIAG_MAX_EXPORT           65536

#define DIAG_MAGIC                0x44494147u


/* ============================================================
 * Time
 * ============================================================ */

static uint64_t diag_now_ms(void)
{
    struct timespec ts;

    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0;

    return
        (uint64_t)ts.tv_sec * 1000ULL +
        (uint64_t)ts.tv_nsec / 1000000ULL;
}


/* ============================================================
 * Utility
 * ============================================================ */

static void diag_copy(
    char *dst,
    size_t capacity,
    const char *src)
{
    if (!dst || capacity == 0)
        return;

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, capacity - 1);
    dst[capacity - 1] = '\0';
}


static double diag_clamp(
    double value,
    double minimum,
    double maximum)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}


/* ============================================================
 * Metric types
 * ============================================================ */

typedef enum
{
    DIAG_METRIC_COUNTER = 0,
    DIAG_METRIC_GAUGE,
    DIAG_METRIC_HISTOGRAM
}
DiagMetricType;


/* ============================================================
 * Metric
 * ============================================================ */

typedef struct
{
    uint32_t magic;

    uint64_t id;

    char name[DIAG_MAX_NAME];

    DiagMetricType type;

    double value;

    double minimum;
    double maximum;

    uint64_t updates;

    /*
     * Histogram.
     */
    uint64_t buckets[
        DIAG_HISTOGRAM_BUCKETS
    ];

    uint64_t histogram_count;

    double histogram_sum;

    /*
     * Rolling window.
     */
    double rolling[
        DIAG_ROLLING_SAMPLES
    ];

    size_t rolling_count;
    size_t rolling_next;

}
DiagMetric;


/* ============================================================
 * Log severity
 * ============================================================ */

typedef enum
{
    DIAG_LOG_DEBUG = 0,
    DIAG_LOG_INFO,
    DIAG_LOG_NOTICE,
    DIAG_LOG_WARNING,
    DIAG_LOG_ERROR,
    DIAG_LOG_CRITICAL
}
DiagLogLevel;


/* ============================================================
 * Log event
 * ============================================================ */

typedef struct
{
    uint64_t id;

    uint64_t timestamp_ms;

    DiagLogLevel level;

    char subsystem[
        DIAG_MAX_NAME
    ];

    char message[
        DIAG_MAX_MESSAGE
    ];

    bool redacted;

}
DiagLogEvent;


/* ============================================================
 * Breadcrumb
 * ============================================================ */

typedef struct
{
    uint64_t timestamp_ms;

    char subsystem[
        DIAG_MAX_NAME
    ];

    char message[
        DIAG_MAX_MESSAGE
    ];

}
DiagBreadcrumb;


/* ============================================================
 * Trace span
 * ============================================================ */

typedef struct
{
    uint64_t id;

    uint64_t parent_id;

    uint64_t start_ms;

    uint64_t end_ms;

    uint64_t duration_ms;

    char name[
        DIAG_MAX_NAME
    ];

    char subsystem[
        DIAG_MAX_NAME
    ];

    bool active;

    bool error;

}
DiagTraceSpan;


/* ============================================================
 * Health state
 * ============================================================ */

typedef enum
{
    DIAG_HEALTH_UNKNOWN = 0,
    DIAG_HEALTH_HEALTHY,
    DIAG_HEALTH_DEGRADED,
    DIAG_HEALTH_UNHEALTHY,
    DIAG_HEALTH_CRITICAL
}
DiagHealthState;


/* ============================================================
 * Subsystem health
 * ============================================================ */

typedef struct
{
    char name[
        DIAG_MAX_NAME
    ];

    DiagHealthState state;

    double score;

    uint64_t last_update_ms;

    uint64_t error_count;

    uint64_t warning_count;

    uint64_t operation_count;

}
DiagSubsystemHealth;


/* ============================================================
 * Diagnostics statistics
 * ============================================================ */

typedef struct
{
    uint64_t metrics_created;

    uint64_t metric_updates;

    uint64_t log_events;

    uint64_t warnings;

    uint64_t errors;

    uint64_t critical_errors;

    uint64_t breadcrumbs;

    uint64_t traces_started;

    uint64_t traces_completed;

    uint64_t anomalies;

    uint64_t threshold_alerts;

    uint64_t exports;

    uint64_t redactions;

}
DiagStats;


/* ============================================================
 * Configuration
 * ============================================================ */

typedef struct
{
    bool enabled;

    bool export_enabled;

    bool privacy_redaction;

    bool breadcrumb_enabled;

    bool tracing_enabled;

    bool anomaly_detection;

    bool threshold_alerts;

    double sampling_rate;

    double cpu_warning_threshold;

    double cpu_critical_threshold;

    double memory_warning_threshold;

    double memory_critical_threshold;

    double packet_loss_warning;

    double packet_loss_critical;

    double latency_warning_ms;

    double latency_critical_ms;

}
DiagConfig;


/* ============================================================
 * Diagnostics engine
 * ============================================================ */

typedef struct
{
    DiagMetric metrics[
        DIAG_MAX_METRICS
    ];

    size_t metric_count;

    DiagLogEvent events[
        DIAG_MAX_EVENTS
    ];

    size_t event_count;

    size_t event_next;

    DiagBreadcrumb breadcrumbs[
        DIAG_MAX_BREADCRUMBS
    ];

    size_t breadcrumb_count;

    size_t breadcrumb_next;

    DiagTraceSpan traces[
        DIAG_MAX_TRACES
    ];

    size_t trace_count;

    DiagSubsystemHealth subsystems[
        DIAG_MAX_SUBSYSTEMS
    ];

    size_t subsystem_count;

    DiagConfig config;

    DiagStats stats;

    uint64_t next_metric_id;

    uint64_t next_event_id;

    uint64_t next_trace_id;

}
DiagnosticsEngine;


/* ============================================================
 * Default configuration
 * ============================================================ */

static DiagConfig diag_default_config(void)
{
    DiagConfig c;

    memset(&c, 0, sizeof(c));

    c.enabled = true;

    c.export_enabled = true;

    c.privacy_redaction = true;

    c.breadcrumb_enabled = true;

    c.tracing_enabled = true;

    c.anomaly_detection = true;

    c.threshold_alerts = true;

    c.sampling_rate = 1.0;

    c.cpu_warning_threshold = 75.0;

    c.cpu_critical_threshold = 95.0;

    c.memory_warning_threshold = 80.0;

    c.memory_critical_threshold = 95.0;

    c.packet_loss_warning = 3.0;

    c.packet_loss_critical = 10.0;

    c.latency_warning_ms = 150.0;

    c.latency_critical_ms = 400.0;

    return c;
}


/* ============================================================
 * Initialization
 * ============================================================ */

static bool diag_init(
    DiagnosticsEngine *engine)
{
    if (!engine)
        return false;

    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->config =
        diag_default_config();

    engine->next_metric_id = 1;
    engine->next_event_id = 1;
    engine->next_trace_id = 1;

    return true;
}


/* ============================================================
 * Metric lookup
 * ============================================================ */

static DiagMetric *
diag_find_metric(
    DiagnosticsEngine *engine,
    const char *name)
{
    if (!engine || !name)
        return NULL;

    for (size_t i = 0;
         i < engine->metric_count;
         ++i)
    {
        if (strcmp(
                engine->metrics[i].name,
                name) == 0)
        {
            return &engine->metrics[i];
        }
    }

    return NULL;
}


/* ============================================================
 * Metric creation
 * ============================================================ */

static DiagMetric *
diag_create_metric(
    DiagnosticsEngine *engine,
    const char *name,
    DiagMetricType type)
{
    if (!engine || !name)
        return NULL;

    DiagMetric *existing =
        diag_find_metric(
            engine,
            name
        );

    if (existing)
        return existing;

    if (engine->metric_count >=
        DIAG_MAX_METRICS)
    {
        return NULL;
    }

    DiagMetric *metric =
        &engine->metrics[
            engine->metric_count++
        ];

    memset(
        metric,
        0,
        sizeof(*metric)
    );

    metric->magic =
        DIAG_MAGIC;

    metric->id =
        engine->next_metric_id++;

    diag_copy(
        metric->name,
        sizeof(metric->name),
        name
    );

    metric->type =
        type;

    metric->minimum =
        HUGE_VAL;

    metric->maximum =
        -HUGE_VAL;

    engine->stats.metrics_created++;

    return metric;
}


/* ============================================================
 * Rolling sample
 * ============================================================ */

static void diag_add_rolling(
    DiagMetric *metric,
    double value)
{
    if (!metric)
        return;

    if (metric->rolling_count <
        DIAG_ROLLING_SAMPLES)
    {
        metric->rolling[
            metric->rolling_count++
        ] = value;

        return;
    }

    metric->rolling[
        metric->rolling_next
    ] = value;

    metric->rolling_next =
        (metric->rolling_next + 1) %
        DIAG_ROLLING_SAMPLES;
}


/* ============================================================
 * Counter increment
 * ============================================================ */

static void diag_counter_add(
    DiagnosticsEngine *engine,
    const char *name,
    double amount)
{
    if (!engine ||
        !engine->config.enabled)
        return;

    DiagMetric *metric =
        diag_create_metric(
            engine,
            name,
            DIAG_METRIC_COUNTER
        );

    if (!metric)
        return;

    metric->value += amount;

    metric->updates++;

    diag_add_rolling(
        metric,
        metric->value
    );

    engine->stats.metric_updates++;
}


/* ============================================================
 * Gauge set
 * ============================================================ */

static void diag_gauge_set(
    DiagnosticsEngine *engine,
    const char *name,
    double value)
{
    if (!engine ||
        !engine->config.enabled)
        return;

    DiagMetric *metric =
        diag_create_metric(
            engine,
            name,
            DIAG_METRIC_GAUGE
        );

    if (!metric)
        return;

    metric->value =
        value;

    if (value < metric->minimum)
        metric->minimum = value;

    if (value > metric->maximum)
        metric->maximum = value;

    metric->updates++;

    diag_add_rolling(
        metric,
        value
    );

    engine->stats.metric_updates++;
}


/* ============================================================
 * Histogram bucket selection
 * ============================================================ */

static size_t diag_histogram_bucket(
    double value)
{
    if (value <= 0.0)
        return 0;

    double logarithm =
        log2(value);

    int index =
        (int)floor(
            logarithm
        ) + 8;

    if (index < 0)
        index = 0;

    if (index >=
        DIAG_HISTOGRAM_BUCKETS)
    {
        index =
            DIAG_HISTOGRAM_BUCKETS - 1;
    }

    return (size_t)index;
}


/* ============================================================
 * Histogram observation
 * ============================================================ */

static void diag_histogram_observe(
    DiagnosticsEngine *engine,
    const char *name,
    double value)
{
    if (!engine ||
        !engine->config.enabled)
        return;

    DiagMetric *metric =
        diag_create_metric(
            engine,
            name,
            DIAG_METRIC_HISTOGRAM
        );

    if (!metric)
        return;

    size_t bucket =
        diag_histogram_bucket(
            value
        );

    metric->buckets[bucket]++;

    metric->histogram_count++;

    metric->histogram_sum +=
        value;

    metric->updates++;

    diag_add_rolling(
        metric,
        value
    );

    engine->stats.metric_updates++;
}


/* ============================================================
 * Metric mean
 * ============================================================ */

static double diag_metric_mean(
    const DiagMetric *metric)
{
    if (!metric)
        return 0.0;

    if (metric->type ==
        DIAG_METRIC_HISTOGRAM)
    {
        if (metric->histogram_count == 0)
            return 0.0;

        return
            metric->histogram_sum /
            (double)metric->histogram_count;
    }

    if (metric->rolling_count == 0)
        return metric->value;

    double sum = 0.0;

    for (size_t i = 0;
         i < metric->rolling_count;
         ++i)
    {
        sum += metric->rolling[i];
    }

    return
        sum /
        (double)metric->rolling_count;
}


/* ============================================================
 * Metric standard deviation
 * ============================================================ */

static double diag_metric_stddev(
    const DiagMetric *metric)
{
    if (!metric ||
        metric->rolling_count < 2)
    {
        return 0.0;
    }

    double mean =
        diag_metric_mean(metric);

    double sum = 0.0;

    for (size_t i = 0;
         i < metric->rolling_count;
         ++i)
    {
        double delta =
            metric->rolling[i] -
            mean;

        sum +=
            delta * delta;
    }

    return sqrt(
        sum /
        (double)
        metric->rolling_count
    );
}


/* ============================================================
 * Privacy redaction
 * ============================================================ */

static void diag_redact(
    DiagnosticsEngine *engine,
    char *text,
    size_t capacity)
{
    if (!engine ||
        !text ||
        !engine->config.privacy_redaction)
    {
        return;
    }

    /*
     * Lightweight privacy layer.
     *
     * Production implementation should use a dedicated
     * structured-data redaction system before telemetry
     * leaves the device.
     */

    const char *patterns[] =
    {
        "password=",
        "token=",
        "authorization=",
        "bearer ",
        "secret=",
        "private_key=",
        "session_key="
    };

    size_t count =
        sizeof(patterns) /
        sizeof(patterns[0]);

    for (size_t p = 0;
         p < count;
         ++p)
    {
        char *location =
            strstr(
                text,
                patterns[p]
            );

        if (!location)
            continue;

        char *value =
            strchr(
                location,
                '='
            );

        if (!value)
        {
            value =
                location +
                strlen(patterns[p]);
        }
        else
        {
            value++;
        }

        while (*value &&
               *value != ' ' &&
               *value != ';' &&
               *value != ',')
        {
            *value = '*';
            value++;
        }

        engine->stats.redactions++;
    }

    (void)capacity;
}


/* ============================================================
 * Log event
 * ============================================================ */

static void diag_log(
    DiagnosticsEngine *engine,
    DiagLogLevel level,
    const char *subsystem,
    const char *message)
{
    if (!engine ||
        !engine->config.enabled)
        return;

    DiagLogEvent event;

    memset(
        &event,
        0,
        sizeof(event)
    );

    event.id =
        engine->next_event_id++;

    event.timestamp_ms =
        diag_now_ms();

    event.level =
        level;

    diag_copy(
        event.subsystem,
        sizeof(event.subsystem),
        subsystem
    );

    diag_copy(
        event.message,
        sizeof(event.message),
        message
    );

    diag_redact(
        engine,
        event.message,
        sizeof(event.message)
    );

    event.redacted =
        engine->config.privacy_redaction;

    if (engine->event_count <
        DIAG_MAX_EVENTS)
    {
        engine->events[
            engine->event_count++
        ] = event;
    }
    else
    {
        engine->events[
            engine->event_next
        ] = event;

        engine->event_next =
            (engine->event_next + 1) %
            DIAG_MAX_EVENTS;
    }

    engine->stats.log_events++;

    if (level ==
        DIAG_LOG_WARNING)
    {
        engine->stats.warnings++;
    }

    if (level ==
        DIAG_LOG_ERROR)
    {
        engine->stats.errors++;
    }

    if (level ==
        DIAG_LOG_CRITICAL)
    {
        engine->stats.critical_errors++;
    }
}


/* ============================================================
 * Breadcrumb
 * ============================================================ */

static void diag_breadcrumb(
    DiagnosticsEngine *engine,
    const char *subsystem,
    const char *message)
{
    if (!engine ||
        !engine->config.breadcrumb_enabled)
        return;

    DiagBreadcrumb breadcrumb;

    memset(
        &breadcrumb,
        0,
        sizeof(breadcrumb)
    );

    breadcrumb.timestamp_ms =
        diag_now_ms();

    diag_copy(
        breadcrumb.subsystem,
        sizeof(breadcrumb.subsystem),
        subsystem
    );

    diag_copy(
        breadcrumb.message,
        sizeof(breadcrumb.message),
        message
    );

    diag_redact(
        engine,
        breadcrumb.message,
        sizeof(breadcrumb.message)
    );

    if (engine->breadcrumb_count <
        DIAG_MAX_BREADCRUMBS)
    {
        engine->breadcrumbs[
            engine->breadcrumb_count++
        ] = breadcrumb;
    }
    else
    {
        engine->breadcrumbs[
            engine->breadcrumb_next
        ] = breadcrumb;

        engine->breadcrumb_next =
            (engine->breadcrumb_next + 1) %
            DIAG_MAX_BREADCRUMBS;
    }

    engine->stats.breadcrumbs++;
}


/* ============================================================
 * Start trace
 * ============================================================ */

static uint64_t diag_trace_start(
    DiagnosticsEngine *engine,
    const char *subsystem,
    const char *name,
    uint64_t parent_id)
{
    if (!engine ||
        !engine->config.tracing_enabled)
    {
        return 0;
    }

    if (engine->trace_count >=
        DIAG_MAX_TRACES)
    {
        return 0;
    }

    DiagTraceSpan *span =
        &engine->traces[
            engine->trace_count++
        ];

    memset(
        span,
        0,
        sizeof(*span)
    );

    span->id =
        engine->next_trace_id++;

    span->parent_id =
        parent_id;

    span->start_ms =
        diag_now_ms();

    diag_copy(
        span->name,
        sizeof(span->name),
        name
    );

    diag_copy(
        span->subsystem,
        sizeof(span->subsystem),
        subsystem
    );

    span->active =
        true;

    engine->stats.traces_started++;

    return span->id;
}


/* ============================================================
 * Finish trace
 * ============================================================ */

static bool diag_trace_end(
    DiagnosticsEngine *engine,
    uint64_t trace_id,
    bool error)
{
    if (!engine ||
        trace_id == 0)
        return false;

    for (size_t i = 0;
         i < engine->trace_count;
         ++i)
    {
        DiagTraceSpan *span =
            &engine->traces[i];

        if (span->id != trace_id)
            continue;

        if (!span->active)
            return false;

        span->end_ms =
            diag_now_ms();

        if (span->end_ms >=
            span->start_ms)
        {
            span->duration_ms =
                span->end_ms -
                span->start_ms;
        }

        span->active =
            false;

        span->error =
            error;

        engine->stats.traces_completed++;

        char metric_name[
            DIAG_MAX_NAME
        ];

        snprintf(
            metric_name,
            sizeof(metric_name),
            "trace.%s.duration_ms",
            span->name
        );

        diag_histogram_observe(
            engine,
            metric_name,
            (double)
            span->duration_ms
        );

        return true;
    }

    return false;
}


/* ============================================================
 * Subsystem lookup
 * ============================================================ */

static DiagSubsystemHealth *
diag_find_subsystem(
    DiagnosticsEngine *engine,
    const char *name)
{
    for (size_t i = 0;
         i < engine->subsystem_count;
         ++i)
    {
        if (strcmp(
                engine->subsystems[i].name,
                name) == 0)
        {
            return
                &engine->subsystems[i];
        }
    }

    return NULL;
}


/* ============================================================
 * Subsystem creation
 * ============================================================ */

static DiagSubsystemHealth *
diag_create_subsystem(
    DiagnosticsEngine *engine,
    const char *name)
{
    DiagSubsystemHealth *existing =
        diag_find_subsystem(
            engine,
            name
        );

    if (existing)
        return existing;

    if (engine->subsystem_count >=
        DIAG_MAX_SUBSYSTEMS)
    {
        return NULL;
    }

    DiagSubsystemHealth *health =
        &engine->subsystems[
            engine->subsystem_count++
        ];

    memset(
        health,
        0,
        sizeof(*health)
    );

    diag_copy(
        health->name,
        sizeof(health->name),
        name
    );

    health->state =
        DIAG_HEALTH_UNKNOWN;

    health->score =
        100.0;

    health->last_update_ms =
        diag_now_ms();

    return health;
}


/* ============================================================
 * Set subsystem health
 * ============================================================ */

static void diag_set_health(
    DiagnosticsEngine *engine,
    const char *subsystem,
    DiagHealthState state,
    double score)
{
    if (!engine)
        return;

    DiagSubsystemHealth *health =
        diag_create_subsystem(
            engine,
            subsystem
        );

    if (!health)
        return;

    health->state =
        state;

    health->score =
        diag_clamp(
            score,
            0.0,
            100.0
        );

    health->last_update_ms =
        diag_now_ms();

    health->operation_count++;
}


/* ============================================================
 * Record subsystem error
 * ============================================================ */

static void diag_subsystem_error(
    DiagnosticsEngine *engine,
    const char *subsystem,
    bool critical)
{
    DiagSubsystemHealth *health =
        diag_create_subsystem(
            engine,
            subsystem
        );

    if (!health)
        return;

    health->error_count++;

    if (critical)
    {
        health->state =
            DIAG_HEALTH_CRITICAL;

        health->score =
            0.0;
    }
    else
    {
        health->warning_count++;

        health->state =
            DIAG_HEALTH_UNHEALTHY;

        health->score =
            diag_clamp(
                health->score - 10.0,
                0.0,
                100.0
            );
    }
}


/* ============================================================
 * CPU update
 * ============================================================ */

static void diag_update_cpu(
    DiagnosticsEngine *engine,
    double cpu_percent)
{
    diag_gauge_set(
        engine,
        "system.cpu.percent",
        cpu_percent
    );

    if (!engine->config.threshold_alerts)
        return;

    if (cpu_percent >=
        engine->config.cpu_critical_threshold)
    {
        diag_log(
            engine,
            DIAG_LOG_CRITICAL,
            "system",
            "CPU utilization critical."
        );

        diag_subsystem_error(
            engine,
            "cpu",
            true
        );

        engine->stats.threshold_alerts++;
    }
    else if (cpu_percent >=
             engine->config.cpu_warning_threshold)
    {
        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "system",
            "CPU utilization elevated."
        );

        engine->stats.threshold_alerts++;
    }
}


/* ============================================================
 * Memory update
 * ============================================================ */

static void diag_update_memory(
    DiagnosticsEngine *engine,
    double memory_percent)
{
    diag_gauge_set(
        engine,
        "system.memory.percent",
        memory_percent
    );

    if (!engine->config.threshold_alerts)
        return;

    if (memory_percent >=
        engine->config.memory_critical_threshold)
    {
        diag_log(
            engine,
            DIAG_LOG_CRITICAL,
            "system",
            "Memory pressure critical."
        );

        diag_subsystem_error(
            engine,
            "memory",
            true
        );

        engine->stats.threshold_alerts++;
    }
    else if (memory_percent >=
             engine->config.memory_warning_threshold)
    {
        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "system",
            "Memory pressure elevated."
        );

        engine->stats.threshold_alerts++;
    }
}


/* ============================================================
 * Network diagnostics
 * ============================================================ */

static void diag_update_network(
    DiagnosticsEngine *engine,
    double latency_ms,
    double packet_loss_percent,
    double jitter_ms)
{
    diag_gauge_set(
        engine,
        "network.latency_ms",
        latency_ms
    );

    diag_gauge_set(
        engine,
        "network.packet_loss_percent",
        packet_loss_percent
    );

    diag_gauge_set(
        engine,
        "network.jitter_ms",
        jitter_ms
    );

    diag_histogram_observe(
        engine,
        "network.latency_distribution_ms",
        latency_ms
    );

    if (!engine->config.threshold_alerts)
        return;

    if (packet_loss_percent >=
        engine->config.packet_loss_critical)
    {
        diag_log(
            engine,
            DIAG_LOG_CRITICAL,
            "network",
            "Packet loss is critical."
        );

        diag_subsystem_error(
            engine,
            "network",
            true
        );

        engine->stats.threshold_alerts++;
    }
    else if (packet_loss_percent >=
             engine->config.packet_loss_warning)
    {
        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "network",
            "Packet loss is elevated."
        );

        engine->stats.threshold_alerts++;
    }

    if (latency_ms >=
        engine->config.latency_critical_ms)
    {
        diag_log(
            engine,
            DIAG_LOG_CRITICAL,
            "network",
            "Network latency is critical."
        );

        engine->stats.threshold_alerts++;
    }
    else if (latency_ms >=
             engine->config.latency_warning_ms)
    {
        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "network",
            "Network latency is elevated."
        );

        engine->stats.threshold_alerts++;
    }
}


/* ============================================================
 * Audio diagnostics
 * ============================================================ */

static void diag_audio_frame(
    DiagnosticsEngine *engine,
    double processing_ms,
    bool underrun,
    bool overrun)
{
    diag_histogram_observe(
        engine,
        "audio.processing_ms",
        processing_ms
    );

    diag_counter_add(
        engine,
        "audio.frames_processed",
        1.0
    );

    if (underrun)
    {
        diag_counter_add(
            engine,
            "audio.underruns",
            1.0
        );

        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "audio",
            "Audio buffer underrun."
        );
    }

    if (overrun)
    {
        diag_counter_add(
            engine,
            "audio.overruns",
            1.0
        );

        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "audio",
            "Audio buffer overrun."
        );
    }
}


/* ============================================================
 * Video diagnostics
 * ============================================================ */

static void diag_video_frame(
    DiagnosticsEngine *engine,
    double processing_ms,
    bool dropped)
{
    diag_histogram_observe(
        engine,
        "video.processing_ms",
        processing_ms
    );

    diag_counter_add(
        engine,
        "video.frames_processed",
        1.0
    );

    if (dropped)
    {
        diag_counter_add(
            engine,
            "video.frames_dropped",
            1.0
        );
    }
}


/* ============================================================
 * Storage diagnostics
 * ============================================================ */

static void diag_storage_operation(
    DiagnosticsEngine *engine,
    double latency_ms,
    bool success)
{
    diag_histogram_observe(
        engine,
        "storage.operation_latency_ms",
        latency_ms
    );

    diag_counter_add(
        engine,
        "storage.operations",
        1.0
    );

    if (!success)
    {
        diag_counter_add(
            engine,
            "storage.errors",
            1.0
        );

        diag_subsystem_error(
            engine,
            "storage",
            false
        );
    }
}


/* ============================================================
 * Search diagnostics
 * ============================================================ */

static void diag_search_query(
    DiagnosticsEngine *engine,
    double latency_ms,
    uint64_t results)
{
    diag_histogram_observe(
        engine,
        "search.query_latency_ms",
        latency_ms
    );

    diag_counter_add(
        engine,
        "search.queries",
        1.0
    );

    diag_counter_add(
        engine,
        "search.results",
        (double)results
    );
}


/* ============================================================
 * Screen capture diagnostics
 * ============================================================ */

static void diag_screen_frame(
    DiagnosticsEngine *engine,
    double capture_ms,
    bool dirty)
{
    diag_histogram_observe(
        engine,
        "screen.capture_ms",
        capture_ms
    );

    diag_counter_add(
        engine,
        "screen.frames",
        1.0
    );

    if (!dirty)
    {
        diag_counter_add(
            engine,
            "screen.static_frames",
            1.0
        );
    }
}


/* ============================================================
 * Security diagnostics
 * ============================================================ */

static void diag_security_event(
    DiagnosticsEngine *engine,
    const char *operation,
    bool success)
{
    char metric[
        DIAG_MAX_NAME
    ];

    snprintf(
        metric,
        sizeof(metric),
        "security.%s",
        operation
    );

    diag_counter_add(
        engine,
        metric,
        1.0
    );

    if (!success)
    {
        diag_log(
            engine,
            DIAG_LOG_ERROR,
            "security",
            "Secure transport operation failed."
        );

        diag_subsystem_error(
            engine,
            "security",
            false
        );
    }
}


/* ============================================================
 * Anomaly detection
 * ============================================================ */

static bool diag_detect_anomaly(
    DiagnosticsEngine *engine,
    const char *metric_name,
    double value)
{
    if (!engine ||
        !engine->config.anomaly_detection)
    {
        return false;
    }

    DiagMetric *metric =
        diag_find_metric(
            engine,
            metric_name
        );

    if (!metric ||
        metric->rolling_count < 10)
    {
        return false;
    }

    double mean =
        diag_metric_mean(metric);

    double stddev =
        diag_metric_stddev(metric);

    if (stddev < 0.000001)
        return false;

    double z =
        fabs(
            (value - mean) /
            stddev
        );

    /*
     * Three-sigma anomaly.
     */
    if (z >= 3.0)
    {
        engine->stats.anomalies++;

        char message[
            DIAG_MAX_MESSAGE
        ];

        snprintf(
            message,
            sizeof(message),
            "Anomaly detected in %s: value=%.3f "
            "mean=%.3f sigma=%.3f",
            metric_name,
            value,
            mean,
            z
        );

        diag_log(
            engine,
            DIAG_LOG_WARNING,
            "anomaly",
            message
        );

        return true;
    }

    return false;
}


/* ============================================================
 * Diagnostics snapshot
 * ============================================================ */

typedef struct
{
    double cpu_percent;

    double memory_percent;

    double network_latency_ms;

    double packet_loss_percent;

    double network_jitter_ms;

    double audio_processing_ms;

    double video_processing_ms;

    uint64_t audio_underruns;

    uint64_t video_dropped;

    uint64_t storage_errors;

    uint64_t search_queries;

    uint64_t security_errors;

}
DiagnosticsSnapshot;


/* ============================================================
 * Build snapshot
 * ============================================================ */

static DiagnosticsSnapshot diag_snapshot(
    DiagnosticsEngine *engine)
{
    DiagnosticsSnapshot snapshot;

    memset(
        &snapshot,
        0,
        sizeof(snapshot)
    );

    DiagMetric *metric;

    metric =
        diag_find_metric(
            engine,
            "system.cpu.percent"
        );

    if (metric)
        snapshot.cpu_percent =
            metric->value;

    metric =
        diag_find_metric(
            engine,
            "system.memory.percent"
        );

    if (metric)
        snapshot.memory_percent =
            metric->value;

    metric =
        diag_find_metric(
            engine,
            "network.latency_ms"
        );

    if (metric)
        snapshot.network_latency_ms =
            metric->value;

    metric =
        diag_find_metric(
            engine,
            "network.packet_loss_percent"
        );

    if (metric)
        snapshot.packet_loss_percent =
            metric->value;

    metric =
        diag_find_metric(
            engine,
            "network.jitter_ms"
        );

    if (metric)
        snapshot.network_jitter_ms =
            metric->value;

    metric =
        diag_find_metric(
            engine,
            "audio.processing_ms"
        );

    if (metric)
        snapshot.audio_processing_ms =
            diag_metric_mean(metric);

    metric =
        diag_find_metric(
            engine,
            "video.processing_ms"
        );

    if (metric)
        snapshot.video_processing_ms =
            diag_metric_mean(metric);

    metric =
        diag_find_metric(
            engine,
            "audio.underruns"
        );

    if (metric)
        snapshot.audio_underruns =
            (uint64_t)metric->value;

    metric =
        diag_find_metric(
            engine,
            "video.frames_dropped"
        );

    if (metric)
        snapshot.video_dropped =
            (uint64_t)metric->value;

    metric =
        diag_find_metric(
            engine,
            "storage.errors"
        );

    if (metric)
        snapshot.storage_errors =
            (uint64_t)metric->value;

    metric =
        diag_find_metric(
            engine,
            "search.queries"
        );

    if (metric)
        snapshot.search_queries =
            (uint64_t)metric->value;

    metric =
        diag_find_metric(
            engine,
            "security.errors"
        );

    if (metric)
        snapshot.security_errors =
            (uint64_t)metric->value;

    return snapshot;
}


/* ============================================================
 * JSON escaping
 * ============================================================ */

static void diag_json_escape(
    FILE *file,
    const char *text)
{
    fputc(
        '"',
        file
    );

    for (const char *p = text;
         p && *p;
         ++p)
    {
        switch (*p)
        {
            case '"':
                fputs(
                    "\\\"",
                    file
                );
                break;

            case '\\':
                fputs(
                    "\\\\",
                    file
                );
                break;

            case '\n':
                fputs(
                    "\\n",
                    file
                );
                break;

            case '\r':
                fputs(
                    "\\r",
                    file
                );
                break;

            case '\t':
                fputs(
                    "\\t",
                    file
                );
                break;

            default:
                fputc(
                    *p,
                    file
                );
                break;
        }
    }

    fputc(
        '"',
        file
    );
}


/* ============================================================
 * Export JSON
 * ============================================================ */

static bool diag_export_json(
    DiagnosticsEngine *engine,
    const char *filename)
{
    if (!engine ||
        !filename ||
        !engine->config.export_enabled)
    {
        return false;
    }

    FILE *file =
        fopen(
            filename,
            "w"
        );

    if (!file)
        return false;

    fprintf(
        file,
        "{\n"
        "  \"schema\": \"native-diagnostics-v1\",\n"
        "  \"timestamp_ms\": %llu,\n",
        (unsigned long long)
        diag_now_ms()
    );

    /*
     * Metrics.
     */
    fprintf(
        file,
        "  \"metrics\": [\n"
    );

    for (size_t i = 0;
         i < engine->metric_count;
         ++i)
    {
        DiagMetric *metric =
            &engine->metrics[i];

        fprintf(
            file,
            "    {"
            "\"name\":"
        );

        diag_json_escape(
            file,
            metric->name
        );

        fprintf(
            file,
            ",\"type\":%d"
            ",\"value\":%.6f"
            ",\"updates\":%llu"
            "}%s\n",

            metric->type,

            metric->value,

            (unsigned long long)
            metric->updates,

            i + 1 <
            engine->metric_count ?
            "," :
            ""
        );
    }

    fprintf(
        file,
        "  ],\n"
    );

    /*
     * Logs.
     */
    fprintf(
        file,
        "  \"events\": [\n"
    );

    for (size_t i = 0;
         i < engine->event_count;
         ++i)
    {
        DiagLogEvent *event =
            &engine->events[i];

        fprintf(
            file,
            "    {"
            "\"id\":%llu,"
            "\"timestamp_ms\":%llu,"
            "\"level\":%d,"
            "\"subsystem\":",

            (unsigned long long)
            event->id,

            (unsigned long long)
            event->timestamp_ms,

            event->level
        );

        diag_json_escape(
            file,
            event->subsystem
        );

        fprintf(
            file,
            ",\"message\":"
        );

        diag_json_escape(
            file,
            event->message
        );

        fprintf(
            file,
            "}%s\n",
            i + 1 <
            engine->event_count ?
            "," :
            ""
        );
    }

    fprintf(
        file,
        "  ],\n"
    );

    /*
     * Breadcrumbs.
     */
    fprintf(
        file,
        "  \"breadcrumbs\": [\n"
    );

    for (size_t i = 0;
         i < engine->breadcrumb_count;
         ++i)
    {
        DiagBreadcrumb *b =
            &engine->breadcrumbs[i];

        fprintf(
            file,
            "    {"
            "\"timestamp_ms\":%llu,"
            "\"subsystem\":",

            (unsigned long long)
            b->timestamp_ms
        );

        diag_json_escape(
            file,
            b->subsystem
        );

        fprintf(
            file,
            ",\"message\":"
        );

        diag_json_escape(
            file,
            b->message
        );

        fprintf(
            file,
            "}%s\n",
            i + 1 <
            engine->breadcrumb_count ?
            "," :
            ""
        );
    }

    fprintf(
        file,
        "  ],\n"
    );

    /*
     * Subsystem health.
     */
    fprintf(
        file,
        "  \"subsystems\": [\n"
    );

    for (size_t i = 0;
         i < engine->subsystem_count;
         ++i)
    {
        DiagSubsystemHealth *health =
            &engine->subsystems[i];

        fprintf(
            file,
            "    {"
            "\"name\":"
        );

        diag_json_escape(
            file,
            health->name
        );

        fprintf(
            file,
            ",\"state\":%d"
            ",\"score\":%.2f"
            ",\"errors\":%llu"
            "}%s\n",

            health->state,

            health->score,

            (unsigned long long)
            health->error_count,

            i + 1 <
            engine->subsystem_count ?
            "," :
            ""
        );
    }

    fprintf(
        file,
        "  ],\n"
    );

    /*
     * Statistics.
     */
    fprintf(
        file,
        "  \"statistics\": {\n"
        "    \"metrics_created\": %llu,\n"
        "    \"metric_updates\": %llu,\n"
        "    \"log_events\": %llu,\n"
        "    \"warnings\": %llu,\n"
        "    \"errors\": %llu,\n"
        "    \"critical_errors\": %llu,\n"
        "    \"breadcrumbs\": %llu,\n"
        "    \"traces_started\": %llu,\n"
        "    \"traces_completed\": %llu,\n"
        "    \"anomalies\": %llu,\n"
        "    \"threshold_alerts\": %llu,\n"
        "    \"redactions\": %llu\n"
        "  }\n"
        "}\n",

        (unsigned long long)
        engine->stats.metrics_created,

        (unsigned long long)
        engine->stats.metric_updates,

        (unsigned long long)
        engine->stats.log_events,

        (unsigned long long)
        engine->stats.warnings,

        (unsigned long long)
        engine->stats.errors,

        (unsigned long long)
        engine->stats.critical_errors,

        (unsigned long long)
        engine->stats.breadcrumbs,

        (unsigned long long)
        engine->stats.traces_started,

        (unsigned long long)
        engine->stats.traces_completed,

        (unsigned long long)
        engine->stats.anomalies,

        (unsigned long long)
        engine->stats.threshold_alerts,

        (unsigned long long)
        engine->stats.redactions
    );

    fclose(file);

    engine->stats.exports++;

    return true;
}


/* ============================================================
 * Health score
 * ============================================================ */

static double diag_overall_health(
    const DiagnosticsEngine *engine)
{
    if (!engine ||
        engine->subsystem_count == 0)
    {
        return 100.0;
    }

    double sum = 0.0;

    for (size_t i = 0;
         i < engine->subsystem_count;
         ++i)
    {
        sum +=
            engine->subsystems[i].score;
    }

    return
        sum /
        (double)
        engine->subsystem_count;
}


/* ============================================================
 * Diagnostics report
 * ============================================================ */

static void diag_print_report(
    DiagnosticsEngine *engine)
{
    DiagnosticsSnapshot s =
        diag_snapshot(engine);

    printf(
        "\n"
        "====================================================\n"
        " NATIVE DIAGNOSTICS & TELEMETRY\n"
        "====================================================\n"
    );

    printf(
        "Overall health:        %.1f / 100\n",
        diag_overall_health(engine)
    );

    printf(
        "\nSYSTEM\n"
        "  CPU:                 %.1f%%\n"
        "  Memory:              %.1f%%\n",
        s.cpu_percent,
        s.memory_percent
    );

    printf(
        "\nNETWORK\n"
        "  Latency:             %.1f ms\n"
        "  Packet loss:         %.2f%%\n"
        "  Jitter:              %.1f ms\n",
        s.network_latency_ms,
        s.packet_loss_percent,
        s.network_jitter_ms
    );

    printf(
        "\nAUDIO\n"
        "  Processing:          %.2f ms\n"
        "  Underruns:           %llu\n",
        s.audio_processing_ms,
        (unsigned long long)
        s.audio_underruns
    );

    printf(
        "\nVIDEO\n"
        "  Processing:          %.2f ms\n"
        "  Dropped frames:      %llu\n",
        s.video_processing_ms,
        (unsigned long long)
        s.video_dropped
    );

    printf(
        "\nSTORAGE\n"
        "  Errors:              %llu\n",
        (unsigned long long)
        s.storage_errors
    );

    printf(
        "\nSEARCH\n"
        "  Queries:             %llu\n",
        (unsigned long long)
        s.search_queries
    );

    printf(
        "\nSECURITY\n"
        "  Errors:              %llu\n",
        (unsigned long long)
        s.security_errors
    );

    printf(
        "\nTELEMETRY\n"
        "  Metrics:             %zu\n"
        "  Events:              %zu\n"
        "  Breadcrumbs:         %zu\n"
        "  Traces:              %zu\n"
        "  Anomalies:           %llu\n"
        "  Threshold alerts:    %llu\n",

        engine->metric_count,

        engine->event_count,

        engine->breadcrumb_count,

        engine->trace_count,

        (unsigned long long)
        engine->stats.anomalies,

        (unsigned long long)
        engine->stats.threshold_alerts
    );

    printf(
        "====================================================\n"
    );
}


/* ============================================================
 * Print subsystem health
 * ============================================================ */

static void diag_print_health(
    const DiagnosticsEngine *engine)
{
    printf(
        "\nSubsystem health:\n"
    );

    for (size_t i = 0;
         i < engine->subsystem_count;
         ++i)
    {
        const DiagSubsystemHealth *health =
            &engine->subsystems[i];

        const char *state;

        switch (health->state)
        {
            case DIAG_HEALTH_HEALTHY:
                state = "HEALTHY";
                break;

            case DIAG_HEALTH_DEGRADED:
                state = "DEGRADED";
                break;

            case DIAG_HEALTH_UNHEALTHY:
                state = "UNHEALTHY";
                break;

            case DIAG_HEALTH_CRITICAL:
                state = "CRITICAL";
                break;

            default:
                state = "UNKNOWN";
                break;
        }

        printf(
            "  %-16s %-10s %.1f/100 "
            "errors=%llu\n",

            health->name,

            state,

            health->score,

            (unsigned long long)
            health->error_count
        );
    }
}


/* ============================================================
 * Print recent events
 * ============================================================ */

static void diag_print_recent_events(
    const DiagnosticsEngine *engine,
    size_t count)
{
    if (count >
        engine->event_count)
    {
        count =
            engine->event_count;
    }

    printf(
        "\nRecent diagnostics events:\n"
    );

    size_t start =
        engine->event_count -
        count;

    for (size_t i = start;
         i < engine->event_count;
         ++i)
    {
        const DiagLogEvent *event =
            &engine->events[i];

        printf(
            "  [%llu] [%d] [%s] %s\n",

            (unsigned long long)
            event->timestamp_ms,

            event->level,

            event->subsystem,

            event->message
        );
    }
}


/* ============================================================
 * Test trace
 * ============================================================ */

static void test_trace(
    DiagnosticsEngine *engine)
{
    uint64_t trace =
        diag_trace_start(
            engine,
            "meeting",
            "process_frame",
            0
        );

    diag_breadcrumb(
        engine,
        "video",
        "Video frame received."
    );

    /*
     * Synthetic work.
     */
    volatile double value = 0.0;

    for (int i = 0;
         i < 100000;
         ++i)
    {
        value +=
            sqrt(
                (double)i
            );
    }

    (void)value;

    diag_trace_end(
        engine,
        trace,
        false
    );
}


/* ============================================================
 * Synthetic meeting workload
 * ============================================================ */

static void simulate_meeting(
    DiagnosticsEngine *engine)
{
    printf(
        "\nSimulating meeting telemetry...\n"
    );

    for (int second = 0;
         second < 60;
         ++second)
    {
        double cpu;
        double memory;
        double latency;
        double loss;
        double jitter;

        /*
         * Phase 1:
         * Healthy meeting.
         */
        if (second < 15)
        {
            cpu = 35.0;
            memory = 45.0;
            latency = 35.0;
            loss = 0.2;
            jitter = 4.0;
        }

        /*
         * Phase 2:
         * CPU pressure.
         */
        else if (second < 25)
        {
            cpu = 82.0;
            memory = 60.0;
            latency = 45.0;
            loss = 0.5;
            jitter = 6.0;
        }

        /*
         * Phase 3:
         * Network degradation.
         */
        else if (second < 40)
        {
            cpu = 70.0;
            memory = 65.0;
            latency = 230.0;
            loss = 6.5;
            jitter = 38.0;
        }

        /*
         * Phase 4:
         * Severe meeting conditions.
         */
        else if (second < 50)
        {
            cpu = 96.0;
            memory = 91.0;
            latency = 470.0;
            loss = 13.0;
            jitter = 70.0;
        }

        /*
         * Phase 5:
         * Recovery.
         */
        else
        {
            cpu = 40.0;
            memory = 50.0;
            latency = 40.0;
            loss = 0.3;
            jitter = 5.0;
        }

        diag_update_cpu(
            engine,
            cpu
        );

        diag_update_memory(
            engine,
            memory
        );

        diag_update_network(
            engine,
            latency,
            loss,
            jitter
        );

        /*
         * Audio.
         */
        double audio_ms =
            1.5 +
            ((double)(second % 4) * 0.15);

        bool underrun =
            second >= 40 &&
            second < 50 &&
            second % 3 == 0;

        diag_audio_frame(
            engine,
            audio_ms,
            underrun,
            false
        );

        /*
         * Video.
         */
        double video_ms =
            5.0 +
            cpu / 20.0;

        bool dropped =
            loss > 5.0 ||
            cpu > 90.0;

        diag_video_frame(
            engine,
            video_ms,
            dropped
        );

        /*
         * Storage.
         */
        diag_storage_operation(
            engine,
            1.0 +
            (second % 5) * 0.4,
            true
        );

        /*
         * Search.
         */
        diag_search_query(
            engine,
            4.0 +
            (second % 3),
            5 + (second % 12)
        );

        /*
         * Screen capture.
         */
        diag_screen_frame(
            engine,
            3.0 +
            (second % 2),
            second % 5 != 0
        );

        /*
         * Security.
         */
        diag_security_event(
            engine,
            "packet_verify",
            true
        );

        /*
         * Trace.
         */
        if (second % 10 == 0)
        {
            test_trace(
                engine
            );
        }
    }
}


/* ============================================================
 * Stress test
 * ============================================================ */

static void stress_test(
    DiagnosticsEngine *engine,
    size_t operations)
{
    printf(
        "\nRunning diagnostics stress test: "
        "%zu operations\n",
        operations
    );

    uint64_t start =
        diag_now_ms();

    for (size_t i = 0;
         i < operations;
         ++i)
    {
        diag_counter_add(
            engine,
            "stress.operations",
            1.0
        );

        diag_gauge_set(
            engine,
            "stress.value",
            (double)
            (i % 1000)
        );

        diag_histogram_observe(
            engine,
            "stress.latency_ms",
            (double)
            (i % 50)
        );
    }

    uint64_t end =
        diag_now_ms();

    double elapsed =
        (double)
        (end - start);

    printf(
        "Elapsed: %.3f ms\n",
        elapsed
    );

    if (elapsed > 0.0)
    {
        printf(
            "Operations/sec: %.0f\n",
            ((double)operations /
             elapsed) *
            1000.0
        );
    }
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "====================================================\n"
        " TEAMS-STYLE DIAGNOSTICS ENGINE\n"
        "====================================================\n"
    );

    DiagnosticsEngine engine;

    if (!diag_init(
            &engine))
    {
        fprintf(
            stderr,
            "Diagnostics initialization failed.\n"
        );

        return EXIT_FAILURE;
    }


    /* --------------------------------------------------------
     * Register subsystems.
     * -------------------------------------------------------- */

    diag_set_health(
        &engine,
        "audio",
        DIAG_HEALTH_HEALTHY,
        100.0
    );

    diag_set_health(
        &engine,
        "video",
        DIAG_HEALTH_HEALTHY,
        100.0
    );

    diag_set_health(
        &engine,
        "network",
        DIAG_HEALTH_HEALTHY,
        100.0
    );

    diag_set_health(
        &engine,
        "storage",
        DIAG_HEALTH_HEALTHY,
        100.0
    );

    diag_set_health(
        &engine,
        "search",
        DIAG_HEALTH_HEALTHY,
        100.0
    );

    diag_set_health(
        &engine,
        "screen",
        DIAG_HEALTH_HEALTHY,
        100.0
    );

    diag_set_health(
        &engine,
        "security",
        DIAG_HEALTH_HEALTHY,
        100.0
    );


    /* --------------------------------------------------------
     * Startup breadcrumbs.
     * -------------------------------------------------------- */

    diag_breadcrumb(
        &engine,
        "application",
        "Native client starting."
    );

    diag_log(
        &engine,
        DIAG_LOG_INFO,
        "application",
        "Diagnostics subsystem initialized."
    );


    /* --------------------------------------------------------
     * Basic metrics.
     * -------------------------------------------------------- */

    diag_counter_add(
        &engine,
        "application.starts",
        1.0
    );

    diag_gauge_set(
        &engine,
        "meeting.participants",
        12.0
    );


    /* --------------------------------------------------------
     * Privacy test.
     * -------------------------------------------------------- */

    diag_log(
        &engine,
        DIAG_LOG_INFO,
        "authentication",
        "token=SUPER_SECRET_VALUE authentication completed."
    );


    /* --------------------------------------------------------
     * Meeting simulation.
     * -------------------------------------------------------- */

    simulate_meeting(
        &engine
    );


    /* --------------------------------------------------------
     * Explicit anomaly test.
     * -------------------------------------------------------- */

    for (int i = 0;
         i < 30;
         ++i)
    {
        diag_gauge_set(
            &engine,
            "test.signal",
            10.0 +
            (double)(i % 3)
        );
    }

    diag_gauge_set(
        &engine,
        "test.signal",
        500.0
    );

    diag_detect_anomaly(
        &engine,
        "test.signal",
        500.0
    );


    /* --------------------------------------------------------
     * Stress test.
     * -------------------------------------------------------- */

    stress_test(
        &engine,
        100000
    );


    /* --------------------------------------------------------
     * Reports.
     * -------------------------------------------------------- */

    diag_print_report(
        &engine
    );

    diag_print_health(
        &engine
    );

    diag_print_recent_events(
        &engine,
        20
    );


    /* --------------------------------------------------------
     * JSON export.
     * -------------------------------------------------------- */

    if (diag_export_json(
            &engine,
            "teams_diagnostics.json"))
    {
        printf(
            "\nDiagnostics exported to "
            "teams_diagnostics.json\n"
        );
    }
    else
    {
        printf(
            "\nDiagnostics JSON export failed.\n"
        );
    }


    /* --------------------------------------------------------
     * Shutdown.
     * -------------------------------------------------------- */

    diag_breadcrumb(
        &engine,
        "application",
        "Native client shutting down."
    );

    diag_log(
        &engine,
        DIAG_LOG_INFO,
        "application",
        "Diagnostics subsystem shutdown."
    );

    printf(
        "\nDiagnostics engine complete.\n"
    );

    return EXIT_SUCCESS;
}
Compile
gcc -std=c11 -O3 -march=native \
    -Wall -Wextra -pedantic \
    teams_diagnostics_core.c \
    -o teams_diagnostics -lm

Run:

./teams_diagnostics

It also produces:

teams_diagnostics.json
The important part: all 10 systems now form one architecture

You effectively have a native C systems layer like this:

                         TEAMS-STYLE CLIENT
                                │
                 ┌──────────────┴──────────────┐
                 │                             │
                 ▼                             ▼
              UI LAYER                    C CORE
                                               │
        ┌──────────────────────────────────────┼─────────────────────┐
        │                                      │                     │
        ▼                                      ▼                     ▼
   #1 AUDIO                                #2 VIDEO              #3 NETWORK
        │                                      │                     │
        └──────────────────┬───────────────────┘                     │
                           ▼                                         │
                    #4 PERFORMANCE ◄─────────────────────────────────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
          #5 SEARCH     #6 STORAGE   #7 SCREEN
              │            │            │
              └────────────┼────────────┘
                           ▼
                       #8 SECURITY
                           │
                           ▼
                    #9 EVENT ENGINE
                           │
                           ▼
                  #10 DIAGNOSTICS
                           │
             ┌─────────────┼──────────────┐
             ▼             ▼              ▼
          Metrics       Tracing        Breadcrumbs
             │             │              │
             └─────────────┼──────────────┘
                           ▼
                  Health / Telemetry
                           │
                  ┌────────┴────────┐
                  ▼                 ▼
              Local JSON       Remote exporter

The really useful design decision is that #10 doesn't need to know the implementation details of the other systems. Each subsystem exposes a small telemetry interface.

For example, #1 can report:

diag_audio_frame(
    &diagnostics,
    processing_time_ms,
    underrun,
    overrun
);

#2:

diag_video_frame(
    &diagnostics,
    processing_time_ms,
    frame_dropped
);

#3:

diag_update_network(
    &diagnostics,
    latency_ms,
    packet_loss,
    jitter_ms
);

#6:

diag_storage_operation(
    &diagnostics,
    operation_latency_ms,
    success
);

#8:

diag_security_event(
    &diagnostics,
    "packet_verify",
    success
);




