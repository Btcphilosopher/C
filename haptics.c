/*
 * hapticfs.c
 *
 * HapticFS
 * --------
 * A native C haptic filesystem/runtime.
 *
 * Concept:
 *
 *     /haptics/
 *         ui/
 *             click.hfs
 *             hover.hfs
 *             confirm.hfs
 *
 *         physics/
 *             metal_hit.hfs
 *             wood_hit.hfs
 *             glass_hit.hfs
 *
 *         environment/
 *             engine.hfs
 *             rain.hfs
 *             wind.hfs
 *
 *         robotics/
 *             motor.hfs
 *             gear.hfs
 *             collision.hfs
 *
 * HapticFS treats haptic effects as assets.
 *
 * Pipeline:
 *
 *     HapticFS
 *         |
 *     Haptic Asset
 *         |
 *     Renderer
 *         |
 *     Actuator Profile
 *         |
 *     Quest / Controller / Glove / Wearable
 *
 * C11
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define HFS_MAX_FILES          256
#define HFS_MAX_POINTS         128
#define HFS_MAX_TRANSIENTS     64
#define HFS_MAX_ACTUATORS      16
#define HFS_MAX_PATH           256
#define HFS_MAX_NAME           96

#define HFS_PI 3.14159265358979323846f

/* ============================================================
   BASIC TYPES
   ============================================================ */

typedef struct
{
    float time;
    float value;

} HFSPoint;

/* ============================================================
   AMPLITUDE / FREQUENCY ENVELOPE
   ============================================================ */

typedef struct
{
    HFSPoint points[
        HFS_MAX_POINTS];

    int count;

} HFSEnvelope;

/* ============================================================
   TRANSIENT
   ============================================================ */

typedef struct
{
    float time;

    float amplitude;

    float frequency;

    float duration;

} HFSTransient;

/* ============================================================
   HAPTIC EFFECT
   ============================================================ */

typedef struct
{
    char name[
        HFS_MAX_NAME];

    float duration;

    HFSEnvelope amplitude;

    HFSEnvelope frequency;

    HFSTransient transients[
        HFS_MAX_TRANSIENTS];

    int transient_count;

    bool loop;

} HFSEffect;

/* ============================================================
   FILE TYPES
   ============================================================ */

typedef enum
{
    HFS_FILE_EFFECT = 0,
    HFS_FILE_PROFILE,
    HFS_FILE_DIRECTORY

} HFSFileType;

/* ============================================================
   HAPTIC FILE
   ============================================================ */

typedef struct
{
    uint32_t id;

    char path[
        HFS_MAX_PATH];

    HFSFileType type;

    HFSEffect effect;

    uint64_t size;

    bool loaded;

} HFSFile;

/* ============================================================
   ACTUATOR PROFILE
   ============================================================ */

typedef struct
{
    char name[
        HFS_MAX_NAME];

    float minimum_frequency;

    float maximum_frequency;

    float maximum_amplitude;

    bool supports_frequency;

    bool supports_pcm;

    bool supports_parametric;

} HFSActuatorProfile;

/* ============================================================
   ACTUATOR
   ============================================================ */

typedef struct
{
    uint32_t id;

    char name[
        HFS_MAX_NAME];

    HFSActuatorProfile profile;

    bool enabled;

} HFSActuator;

/* ============================================================
   RENDERED SAMPLE
   ============================================================ */

typedef struct
{
    float amplitude;

    float frequency;

} HFSSample;

/* ============================================================
   HAPTIC PLAYBACK
   ============================================================ */

typedef struct
{
    const HFSEffect *effect;

    float time;

    float gain;

    bool playing;

    bool looping;

} HFSPlayback;

/* ============================================================
   FILESYSTEM
   ============================================================ */

typedef struct
{
    HFSFile files[
        HFS_MAX_FILES];

    int file_count;

    HFSActuator actuators[
        HFS_MAX_ACTUATORS];

    int actuator_count;

    uint32_t next_file_id;

    uint32_t next_actuator_id;

} HapticFS;

/* ============================================================
   CLAMP
   ============================================================ */

static float hfs_clamp(
    float x,
    float minimum,
    float maximum)
{
    if (x < minimum)
        return minimum;

    if (x > maximum)
        return maximum;

    return x;
}

/* ============================================================
   ENVELOPE INITIALISATION
   ============================================================ */

static void hfs_envelope_init(
    HFSEnvelope *envelope)
{
    memset(
        envelope,
        0,
        sizeof(*envelope));
}

/* ============================================================
   ADD ENVELOPE POINT
   ============================================================ */

static bool hfs_envelope_add(
    HFSEnvelope *envelope,
    float time,
    float value)
{
    if (envelope->count >=
        HFS_MAX_POINTS)
    {
        return false;
    }

    envelope->points[
        envelope->count++] =
        (HFSPoint){
            time,
            value
        };

    return true;
}

/* ============================================================
   LINEAR INTERPOLATION
   ============================================================ */

static float hfs_envelope_sample(
    const HFSEnvelope *envelope,
    float time)
{
    if (envelope->count == 0)
        return 0.0f;

    if (time <=
        envelope->points[0].time)
    {
        return
            envelope->points[0].value;
    }

    for (int i = 1;
         i < envelope->count;
         ++i)
    {
        HFSPoint a =
            envelope->points[i - 1];

        HFSPoint b =
            envelope->points[i];

        if (time <= b.time)
        {
            float span =
                b.time -
                a.time;

            if (span <= 0.0f)
                return b.value;

            float t =
                (time - a.time) /
                span;

            return
                a.value +
                (b.value -
                 a.value) *
                t;
        }
    }

    return envelope
        ->points[
            envelope->count - 1]
        .value;
}

/* ============================================================
   EFFECT INITIALISATION
   ============================================================ */

static void hfs_effect_init(
    HFSEffect *effect,
    const char *name,
    float duration)
{
    memset(
        effect,
        0,
        sizeof(*effect));

    strncpy(
        effect->name,
        name,
        HFS_MAX_NAME - 1);

    effect->duration =
        duration;

    hfs_envelope_init(
        &effect->amplitude);

    hfs_envelope_init(
        &effect->frequency);
}

/* ============================================================
   ADD TRANSIENT
   ============================================================ */

static bool hfs_add_transient(
    HFSEffect *effect,
    float time,
    float amplitude,
    float frequency,
    float duration)
{
    if (effect->transient_count >=
        HFS_MAX_TRANSIENTS)
    {
        return false;
    }

    effect->transients[
        effect->transient_count++] =
        (HFSTransient){
            time,
            amplitude,
            frequency,
            duration
        };

    return true;
}

/* ============================================================
   CREATE FILE
   ============================================================ */

static HFSFile *hfs_create_effect(
    HapticFS *fs,
    const char *path,
    HFSEffect *effect)
{
    if (fs->file_count >=
        HFS_MAX_FILES)
    {
        return NULL;
    }

    HFSFile *file =
        &fs->files[
            fs->file_count++];

    memset(
        file,
        0,
        sizeof(*file));

    file->id =
        ++fs->next_file_id;

    strncpy(
        file->path,
        path,
        HFS_MAX_PATH - 1);

    file->type =
        HFS_FILE_EFFECT;

    file->effect =
        *effect;

    file->loaded =
        true;

    file->size =
        sizeof(HFSEffect);

    return file;
}

/* ============================================================
   FIND FILE
   ============================================================ */

static HFSFile *hfs_find(
    HapticFS *fs,
    const char *path)
{
    for (int i = 0;
         i < fs->file_count;
         ++i)
    {
        if (strcmp(
                fs->files[i].path,
                path) == 0)
        {
            return
                &fs->files[i];
        }
    }

    return NULL;
}

/* ============================================================
   ACTUATOR PROFILE
   ============================================================ */

static HFSActuatorProfile hfs_quest_profile(void)
{
    HFSActuatorProfile profile;

    memset(
        &profile,
        0,
        sizeof(profile));

    strncpy(
        profile.name,
        "Meta Quest Controller",
        HFS_MAX_NAME - 1);

    profile.minimum_frequency =
        40.0f;

    profile.maximum_frequency =
        250.0f;

    profile.maximum_amplitude =
        1.0f;

    profile.supports_frequency =
        true;

    profile.supports_pcm =
        true;

    profile.supports_parametric =
        true;

    return profile;
}

/* ============================================================
   REGISTER ACTUATOR
   ============================================================ */

static uint32_t hfs_register_actuator(
    HapticFS *fs,
    const char *name,
    HFSActuatorProfile profile)
{
    if (fs->actuator_count >=
        HFS_MAX_ACTUATORS)
    {
        return 0;
    }

    HFSActuator *actuator =
        &fs->actuators[
            fs->actuator_count++];

    memset(
        actuator,
        0,
        sizeof(*actuator));

    actuator->id =
        ++fs->next_actuator_id;

    strncpy(
        actuator->name,
        name,
        HFS_MAX_NAME - 1);

    actuator->profile =
        profile;

    actuator->enabled =
        true;

    return actuator->id;
}

/* ============================================================
   PLAYBACK
   ============================================================ */

static void hfs_play(
    HFSPlayback *playback,
    const HFSEffect *effect,
    float gain)
{
    playback->effect =
        effect;

    playback->time =
        0.0f;

    playback->gain =
        hfs_clamp(
            gain,
            0.0f,
            1.0f);

    playback->playing =
        true;

    playback->looping =
        effect->loop;
}

/* ============================================================
   STOP
   ============================================================ */

static void hfs_stop(
    HFSPlayback *playback)
{
    playback->playing =
        false;

    playback->time =
        0.0f;
}

/* ============================================================
   TRANSIENT SAMPLE
   ============================================================ */

static float hfs_transient_amplitude(
    const HFSEffect *effect,
    float time)
{
    float result =
        0.0f;

    for (int i = 0;
         i < effect->transient_count;
         ++i)
    {
        const HFSTransient *t =
            &effect->transients[i];

        float start =
            t->time;

        float end =
            start +
            t->duration;

        if (time >= start &&
            time <= end)
        {
            float local =
                (time - start) /
                t->duration;

            /*
             * Smooth impact envelope.
             */

            float envelope =
                sinf(
                    local *
                    HFS_PI);

            result +=
                t->amplitude *
                envelope;
        }
    }

    return result;
}

/* ============================================================
   RENDER EFFECT
   ============================================================ */

static HFSSample hfs_render(
    const HFSEffect *effect,
    float time,
    float gain)
{
    HFSSample sample;

    memset(
        &sample,
        0,
        sizeof(sample));

    if (effect == NULL)
        return sample;

    if (time < 0.0f)
        return sample;

    if (time > effect->duration)
        return sample;

    float amplitude =
        hfs_envelope_sample(
            &effect->amplitude,
            time);

    float frequency =
        hfs_envelope_sample(
            &effect->frequency,
            time);

    amplitude +=
        hfs_transient_amplitude(
            effect,
            time);

    amplitude =
        hfs_clamp(
            amplitude * gain,
            0.0f,
            1.0f);

    sample.amplitude =
        amplitude;

    sample.frequency =
        frequency;

    return sample;
}

/* ============================================================
   HARDWARE ADAPTATION
   ============================================================ */

static HFSSample hfs_adapt_to_actuator(
    HFSSample sample,
    HFSActuatorProfile *profile)
{
    sample.amplitude =
        hfs_clamp(
            sample.amplitude,
            0.0f,
            profile->maximum_amplitude);

    if (!profile->supports_frequency)
    {
        sample.frequency =
            profile->minimum_frequency;
    }
    else
    {
        sample.frequency =
            hfs_clamp(
                sample.frequency,
                profile->minimum_frequency,
                profile->maximum_frequency);
    }

    return sample;
}

/* ============================================================
   PLAYBACK UPDATE
   ============================================================ */

static bool hfs_update(
    HFSPlayback *playback,
    HFSActuator *actuator,
    float dt)
{
    if (!playback->playing)
        return false;

    playback->time +=
        dt;

    const HFSEffect *effect =
        playback->effect;

    if (playback->time >
        effect->duration)
    {
        if (playback->looping)
        {
            playback->time =
                fmodf(
                    playback->time,
                    effect->duration);
        }
        else
        {
            playback->playing =
                false;

            return false;
        }
    }

    HFSSample sample =
        hfs_render(
            effect,
            playback->time,
            playback->gain);

    sample =
        hfs_adapt_to_actuator(
            sample,
            &actuator->profile);

    /*
     * Hardware abstraction point.
     *
     * A real Quest backend would translate
     * this sample into OpenXR haptic output.
     */

    printf(
        "\r[%s] amp=%0.3f freq=%6.1f Hz",
        effect->name,
        sample.amplitude,
        sample.frequency);

    fflush(stdout);

    return true;
}

/* ============================================================
   SAVE EFFECT
   ============================================================ */

static bool hfs_save_binary(
    const HFSFile *file,
    const char *filename)
{
    FILE *fp =
        fopen(
            filename,
            "wb");

    if (!fp)
        return false;

    /*
     * Simple HFS binary header.
     */

    const char magic[4] =
    {
        'H',
        'F',
        'S',
        '1'
    };

    fwrite(
        magic,
        1,
        4,
        fp);

    fwrite(
        &file->effect,
        sizeof(HFSEffect),
        1,
        fp);

    fclose(fp);

    return true;
}

/* ============================================================
   LOAD EFFECT
   ============================================================ */

static bool hfs_load_binary(
    HFSEffect *effect,
    const char *filename)
{
    FILE *fp =
        fopen(
            filename,
            "rb");

    if (!fp)
        return false;

    char magic[4];

    if (fread(
            magic,
            1,
            4,
            fp) != 4)
    {
        fclose(fp);
        return false;
    }

    if (memcmp(
            magic,
            "HFS1",
            4) != 0)
    {
        fclose(fp);
        return false;
    }

    if (fread(
            effect,
            sizeof(HFSEffect),
            1,
            fp) != 1)
    {
        fclose(fp);
        return false;
    }

    fclose(fp);

    return true;
}

/* ============================================================
   FILESYSTEM LIST
   ============================================================ */

static void hfs_ls(
    HapticFS *fs,
    const char *prefix)
{
    printf(
        "\nHapticFS: %s\n",
        prefix);

    for (int i = 0;
         i < fs->file_count;
         ++i)
    {
        HFSFile *file =
            &fs->files[i];

        if (strncmp(
                file->path,
                prefix,
                strlen(prefix)) == 0)
        {
            printf(
                "  %-40s %.2f ms\n",
                file->path,
                file->effect.duration *
                    1000.0f);
        }
    }
}

/* ============================================================
   CREATE STANDARD LIBRARY
   ============================================================ */

static void hfs_create_library(
    HapticFS *fs)
{
    /*
     * UI CLICK
     */

    HFSEffect click;

    hfs_effect_init(
        &click,
        "UI Click",
        0.080f);

    hfs_envelope_add(
        &click.amplitude,
        0.000f,
        0.00f);

    hfs_envelope_add(
        &click.amplitude,
        0.005f,
        1.00f);

    hfs_envelope_add(
        &click.amplitude,
        0.080f,
        0.00f);

    hfs_envelope_add(
        &click.frequency,
        0.000f,
        160.0f);

    hfs_envelope_add(
        &click.frequency,
        0.080f,
        110.0f);

    hfs_create_effect(
        fs,
        "/haptics/ui/click.hfs",
        &click);

    /*
     * METAL IMPACT
     */

    HFSEffect metal;

    hfs_effect_init(
        &metal,
        "Metal Impact",
        0.250f);

    hfs_envelope_add(
        &metal.amplitude,
        0.000f,
        0.00f);

    hfs_envelope_add(
        &metal.amplitude,
        0.008f,
        1.00f);

    hfs_envelope_add(
        &metal.amplitude,
        0.060f,
        0.60f);

    hfs_envelope_add(
        &metal.amplitude,
        0.250f,
        0.00f);

    hfs_envelope_add(
        &metal.frequency,
        0.000f,
        210.0f);

    hfs_envelope_add(
        &metal.frequency,
        0.250f,
        80.0f);

    hfs_add_transient(
        &metal,
        0.008f,
        1.0f,
        220.0f,
        0.025f);

    hfs_create_effect(
        fs,
        "/haptics/physics/metal_hit.hfs",
        &metal);

    /*
     * ENGINE
     */

    HFSEffect engine;

    hfs_effect_init(
        &engine,
        "Engine",
        2.0f);

    engine.loop =
        true;

    hfs_envelope_add(
        &engine.amplitude,
        0.0f,
        0.20f);

    hfs_envelope_add(
        &engine.amplitude,
        0.5f,
        0.45f);

    hfs_envelope_add(
        &engine.amplitude,
        1.0f,
        0.60f);

    hfs_envelope_add(
        &engine.amplitude,
        2.0f,
        0.40f);

    hfs_envelope_add(
        &engine.frequency,
        0.0f,
        70.0f);

    hfs_envelope_add(
        &engine.frequency,
        2.0f,
        120.0f);

    hfs_create_effect(
        fs,
        "/haptics/environment/engine.hfs",
        &engine);

    /*
     * ROBOT MOTOR
     */

    HFSEffect motor;

    hfs_effect_init(
        &motor,
        "Robot Motor",
        1.0f);

    motor.loop =
        true;

    hfs_envelope_add(
        &motor.amplitude,
        0.0f,
        0.15f);

    hfs_envelope_add(
        &motor.amplitude,
        0.2f,
        0.55f);

    hfs_envelope_add(
        &motor.amplitude,
        0.6f,
        0.35f);

    hfs_envelope_add(
        &motor.amplitude,
        1.0f,
        0.50f);

    hfs_envelope_add(
        &motor.frequency,
        0.0f,
        90.0f);

    hfs_envelope_add(
        &motor.frequency,
        1.0f,
        180.0f);

    hfs_create_effect(
        fs,
        "/haptics/robotics/motor.hfs",
        &motor);
}

/* ============================================================
   FILESYSTEM INITIALISATION
   ============================================================ */

static void hfs_init(
    HapticFS *fs)
{
    memset(
        fs,
        0,
        sizeof(*fs));

    fs->next_file_id =
        100;

    fs->next_actuator_id =
        500;

    HFSActuatorProfile quest =
        hfs_quest_profile();

    hfs_register_actuator(
        fs,
        "Right Controller",
        quest);

    hfs_register_actuator(
        fs,
        "Left Controller",
        quest);

    hfs_create_library(
        fs);
}

/* ============================================================
   DEMONSTRATION
   ============================================================ */

int main(void)
{
    printf(
        "\n");
    printf(
        "============================================\n");
    printf(
        "              HAPTICFS\n");
    printf(
        "============================================\n");

    HapticFS fs;

    hfs_init(
        &fs);

    hfs_ls(
        &fs,
        "/haptics");

    /*
     * Find a haptic asset.
     */

    HFSFile *file =
        hfs_find(
            &fs,
            "/haptics/physics/metal_hit.hfs");

    if (!file)
    {
        printf(
            "Haptic asset not found.\n");

        return 1;
    }

    printf(
        "\nLoaded: %s\n",
        file->effect.name);

    printf(
        "Duration: %.2f ms\n",
        file->effect.duration *
            1000.0f);

    /*
     * Play the effect.
     */

    HFSPlayback playback;

    memset(
        &playback,
        0,
        sizeof(playback));

    hfs_play(
        &playback,
        &file->effect,
        1.0f);

    HFSActuator *actuator =
        &fs.actuators[0];

    printf(
        "\n\nPlaying haptic...\n");

    /*
     * Simulated high-frequency
     * rendering loop.
     */

    while (playback.playing)
    {
        hfs_update(
            &playback,
            actuator,
            0.005f);
    }

    printf(
        "\n\nPlayback complete.\n");

    /*
     * Save an asset.
     */

    if (hfs_save_binary(
            file,
            "metal_hit.hfs"))
    {
        printf(
            "Saved metal_hit.hfs\n");
    }

    printf(
        "============================================\n");

    return 0;
}













/*
 * haptic_physics.c
 *
 * Haptic Physics Engine
 * ---------------------
 *
 * C11 prototype for Meta Quest / OpenXR VR.
 *
 * Features:
 *   - 3D vectors
 *   - Material definitions
 *   - Physical bodies
 *   - Collision detection
 *   - Collision impulse estimation
 *   - Impact velocity
 *   - Contact force estimation
 *   - Impact angle
 *   - Material-dependent haptic signatures
 *   - Procedural haptic envelopes
 *   - Continuous contact vibration
 *   - Friction vibration
 *   - Haptic priority
 *   - Left/right actuator routing
 *   - Haptic event queue
 *   - Quest/OpenXR backend abstraction
 *
 * Build:
 *   cc -std=c11 -O2 haptic_physics.c -lm -o haptic_physics
 *
 * Production:
 *   Replace the console backend with OpenXR:
 *
 *   xrApplyHapticFeedback(...)
 *
 * and optionally Meta's parametric/PCM haptic APIs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HP_MAX_BODIES       128
#define HP_MAX_EVENTS       256
#define HP_MAX_MATERIALS     64
#define HP_MAX_ACTIVE_HAPTICS 32

#define HP_SAMPLE_RATE      1000.0f
#define HP_MAX_AMPLITUDE       1.0f
#define HP_MIN_AMPLITUDE       0.0f

#define HP_EPSILON 0.000001f


/* ============================================================
 * VECTOR MATH
 * ============================================================ */

typedef struct {
    float x;
    float y;
    float z;
} Vec3;

static Vec3 vec3(float x, float y, float z)
{
    Vec3 v = {x, y, z};
    return v;
}

static Vec3 vadd(Vec3 a, Vec3 b)
{
    return vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}

static Vec3 vsub(Vec3 a, Vec3 b)
{
    return vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}

static Vec3 vmul(Vec3 a, float s)
{
    return vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}

static float vdot(Vec3 a, Vec3 b)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}

static float vlength(Vec3 a)
{
    return sqrtf(vdot(a, a));
}

static Vec3 vnormalize(Vec3 a)
{
    float l = vlength(a);

    if (l < HP_EPSILON)
        return vec3(0.0f, 0.0f, 0.0f);

    return vmul(a, 1.0f / l);
}

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}


/* ============================================================
 * MATERIALS
 * ============================================================ */

typedef enum {
    MATERIAL_STEEL,
    MATERIAL_ALUMINIUM,
    MATERIAL_WOOD,
    MATERIAL_RUBBER,
    MATERIAL_GLASS,
    MATERIAL_CONCRETE,
    MATERIAL_PLASTIC,
    MATERIAL_CARBON,
    MATERIAL_LEATHER,
    MATERIAL_CERAMIC,
    MATERIAL_SANDPAPER,
    MATERIAL_COUNT
} MaterialType;


typedef struct {

    const char *name;

    MaterialType type;

    /*
     * Physical properties
     */

    float hardness;

    float restitution;

    float friction;

    float density;

    /*
     * Haptic properties
     */

    float impact_amplitude;

    float impact_frequency;

    float impact_duration;

    float resonance;

    float damping;

    float texture;

} HapticMaterial;


/* ============================================================
 * MATERIAL DATABASE
 * ============================================================ */

static HapticMaterial materials[MATERIAL_COUNT] = {

    {
        "Steel",
        MATERIAL_STEEL,
        0.95f,
        0.35f,
        0.65f,
        7850.0f,
        1.00f,
        190.0f,
        0.085f,
        0.90f,
        0.35f,
        0.18f
    },

    {
        "Aluminium",
        MATERIAL_ALUMINIUM,
        0.72f,
        0.45f,
        0.55f,
        2700.0f,
        0.82f,
        145.0f,
        0.075f,
        0.70f,
        0.45f,
        0.15f
    },

    {
        "Wood",
        MATERIAL_WOOD,
        0.42f,
        0.30f,
        0.70f,
        650.0f,
        0.58f,
        85.0f,
        0.11f,
        0.35f,
        0.72f,
        0.30f
    },

    {
        "Rubber",
        MATERIAL_RUBBER,
        0.25f,
        0.72f,
        1.00f,
        1100.0f,
        0.35f,
        55.0f,
        0.16f,
        0.20f,
        0.90f,
        0.42f
    },

    {
        "Glass",
        MATERIAL_GLASS,
        0.92f,
        0.58f,
        0.40f,
        2500.0f,
        0.95f,
        260.0f,
        0.065f,
        0.95f,
        0.18f,
        0.12f
    },

    {
        "Concrete",
        MATERIAL_CONCRETE,
        0.88f,
        0.22f,
        0.85f,
        2400.0f,
        0.90f,
        120.0f,
        0.10f,
        0.55f,
        0.55f,
        0.20f
    },

    {
        "Plastic",
        MATERIAL_PLASTIC,
        0.38f,
        0.42f,
        0.50f,
        950.0f,
        0.48f,
        100.0f,
        0.095f,
        0.30f,
        0.70f,
        0.25f
    },

    {
        "Carbon Fibre",
        MATERIAL_CARBON,
        0.93f,
        0.32f,
        0.48f,
        1600.0f,
        0.88f,
        210.0f,
        0.075f,
        0.82f,
        0.32f,
        0.16f
    },

    {
        "Leather",
        MATERIAL_LEATHER,
        0.20f,
        0.20f,
        0.75f,
        860.0f,
        0.25f,
        45.0f,
        0.20f,
        0.15f,
        0.95f,
        0.48f
    },

    {
        "Ceramic",
        MATERIAL_CERAMIC,
        0.90f,
        0.50f,
        0.55f,
        2400.0f,
        0.91f,
        230.0f,
        0.07f,
        0.88f,
        0.22f,
        0.14f
    },

    {
        "Sandpaper",
        MATERIAL_SANDPAPER,
        0.65f,
        0.05f,
        1.20f,
        1500.0f,
        0.20f,
        70.0f,
        0.25f,
        0.12f,
        0.92f,
        0.95f
    }
};


/* ============================================================
 * PHYSICAL BODY
 * ============================================================ */

typedef struct {

    int id;

    char name[64];

    Vec3 position;
    Vec3 velocity;

    float mass;
    float radius;

    MaterialType material;

    bool dynamic;
    bool haptic_enabled;

} PhysicsBody;


/* ============================================================
 * HAPTIC HAND
 * ============================================================ */

typedef enum {

    HAPTIC_LEFT,
    HAPTIC_RIGHT,
    HAPTIC_BOTH

} HapticHand;


/* ============================================================
 * HAPTIC EVENT
 * ============================================================ */

typedef enum {

    HAPTIC_EVENT_IMPACT,
    HAPTIC_EVENT_FRICTION,
    HAPTIC_EVENT_CONTACT,
    HAPTIC_EVENT_BOUNCE

} HapticEventType;


typedef struct {

    HapticEventType type;

    HapticHand hand;

    MaterialType material_a;
    MaterialType material_b;

    Vec3 position;
    Vec3 normal;

    float impact_velocity;
    float force;
    float angle;

    float amplitude;
    float frequency;
    float duration;

    float priority;

} HapticEvent;


/* ============================================================
 * HAPTIC ENVELOPE
 * ============================================================ */

typedef struct {

    float attack;
    float decay;

    float sustain;
    float release;

} HapticEnvelope;


/* ============================================================
 * ACTIVE HAPTIC
 * ============================================================ */

typedef struct {

    bool active;

    HapticEvent event;

    float elapsed;

} ActiveHaptic;


/* ============================================================
 * HAPTIC PHYSICS ENGINE
 * ============================================================ */

typedef struct {

    PhysicsBody bodies[HP_MAX_BODIES];

    int body_count;

    HapticEvent events[HP_MAX_EVENTS];

    int event_count;

    ActiveHaptic active[HP_MAX_ACTIVE_HAPTICS];

    float time;

} HapticPhysicsEngine;


/* ============================================================
 * ENGINE INITIALISATION
 * ============================================================ */

static void hp_init(HapticPhysicsEngine *engine)
{
    memset(engine, 0, sizeof(*engine));
}


/* ============================================================
 * BODY CREATION
 * ============================================================ */

static int hp_add_body(
    HapticPhysicsEngine *engine,
    const char *name,
    Vec3 position,
    Vec3 velocity,
    float mass,
    float radius,
    MaterialType material,
    bool dynamic
)
{
    if (engine->body_count >= HP_MAX_BODIES)
        return -1;

    int id = engine->body_count;

    PhysicsBody *body =
        &engine->bodies[id];

    memset(body, 0, sizeof(*body));

    body->id = id;

    snprintf(
        body->name,
        sizeof(body->name),
        "%s",
        name
    );

    body->position = position;
    body->velocity = velocity;

    body->mass = mass;
    body->radius = radius;

    body->material = material;

    body->dynamic = dynamic;
    body->haptic_enabled = true;

    engine->body_count++;

    return id;
}


/* ============================================================
 * COLLISION TEST
 * ============================================================ */

static bool hp_collision(
    const PhysicsBody *a,
    const PhysicsBody *b,
    Vec3 *normal,
    float *penetration
)
{
    Vec3 delta =
        vsub(b->position, a->position);

    float distance =
        vlength(delta);

    float combined =
        a->radius + b->radius;

    if (distance > combined)
        return false;

    if (distance < HP_EPSILON) {

        *normal =
            vec3(0.0f, 1.0f, 0.0f);

        *penetration =
            combined;

        return true;
    }

    *normal =
        vmul(delta, 1.0f / distance);

    *penetration =
        combined - distance;

    return true;
}


/* ============================================================
 * IMPACT VELOCITY
 * ============================================================ */

static float hp_relative_velocity(
    const PhysicsBody *a,
    const PhysicsBody *b,
    Vec3 normal
)
{
    Vec3 relative =
        vsub(b->velocity, a->velocity);

    return fabsf(vdot(relative, normal));
}


/* ============================================================
 * EFFECTIVE MATERIAL
 * ============================================================ */

static HapticMaterial hp_combined_material(
    MaterialType a,
    MaterialType b
)
{
    HapticMaterial ma = materials[a];
    HapticMaterial mb = materials[b];

    HapticMaterial result = ma;

    result.hardness =
        (ma.hardness + mb.hardness) * 0.5f;

    result.restitution =
        (ma.restitution + mb.restitution) * 0.5f;

    result.friction =
        (ma.friction + mb.friction) * 0.5f;

    result.impact_amplitude =
        sqrtf(
            ma.impact_amplitude *
            mb.impact_amplitude
        );

    result.impact_frequency =
        sqrtf(
            ma.impact_frequency *
            mb.impact_frequency
        );

    result.resonance =
        (ma.resonance + mb.resonance) * 0.5f;

    result.damping =
        (ma.damping + mb.damping) * 0.5f;

    result.texture =
        (ma.texture + mb.texture) * 0.5f;

    return result;
}


/* ============================================================
 * IMPACT FORCE
 * ============================================================ */

static float hp_impact_force(
    const PhysicsBody *a,
    const PhysicsBody *b,
    float velocity
)
{
    float inverse_mass_a =
        a->dynamic && a->mass > 0.0f
        ? 1.0f / a->mass
        : 0.0f;

    float inverse_mass_b =
        b->dynamic && b->mass > 0.0f
        ? 1.0f / b->mass
        : 0.0f;

    float effective_mass =
        1.0f /
        fmaxf(
            inverse_mass_a +
            inverse_mass_b,
            0.0001f
        );

    HapticMaterial mat =
        hp_combined_material(
            a->material,
            b->material
        );

    float impulse =
        effective_mass *
        velocity *
        (1.0f + mat.restitution);

    return fabsf(impulse);
}


/* ============================================================
 * IMPACT ANGLE
 * ============================================================ */

static float hp_impact_angle(
    Vec3 velocity,
    Vec3 normal
)
{
    Vec3 v =
        vnormalize(velocity);

    float d =
        fabsf(vdot(v, normal));

    d = clampf(d, 0.0f, 1.0f);

    return acosf(d);
}


/* ============================================================
 * MATERIAL → HAPTIC SIGNATURE
 * ============================================================ */

static void hp_material_signature(
    MaterialType a,
    MaterialType b,
    float force,
    float velocity,
    float angle,
    float *amplitude,
    float *frequency,
    float *duration
)
{
    HapticMaterial m =
        hp_combined_material(a, b);

    float force_factor =
        clampf(force / 30.0f, 0.0f, 1.0f);

    float velocity_factor =
        clampf(velocity / 8.0f, 0.0f, 1.0f);

    /*
     * Glancing collisions should feel weaker.
     */

    float angle_factor =
        clampf(
            cosf(angle),
            0.0f,
            1.0f
        );

    *amplitude =
        m.impact_amplitude *
        (0.30f +
         0.70f * force_factor) *
        (0.40f +
         0.60f * velocity_factor) *
        angle_factor;

    *amplitude =
        clampf(
            *amplitude,
            0.0f,
            1.0f
        );

    /*
     * Harder/faster impacts have
     * shorter and sharper pulses.
     */

    *frequency =
        m.impact_frequency *
        (0.75f +
         velocity_factor * 0.50f);

    *duration =
        m.impact_duration *
        (1.20f -
         0.45f * velocity_factor);

    *duration =
        clampf(
            *duration,
            0.025f,
            0.300f
        );
}


/* ============================================================
 * QUEUE HAPTIC EVENT
 * ============================================================ */

static void hp_queue_event(
    HapticPhysicsEngine *engine,
    const HapticEvent *event
)
{
    if (engine->event_count >= HP_MAX_EVENTS)
        return;

    engine->events[
        engine->event_count++
    ] = *event;
}


/* ============================================================
 * CREATE IMPACT EVENT
 * ============================================================ */

static void hp_create_impact(
    HapticPhysicsEngine *engine,
    PhysicsBody *a,
    PhysicsBody *b,
    Vec3 position,
    Vec3 normal
)
{
    float velocity =
        hp_relative_velocity(
            a,
            b,
            normal
        );

    if (velocity < 0.05f)
        return;

    float force =
        hp_impact_force(
            a,
            b,
            velocity
        );

    Vec3 relative =
        vsub(
            b->velocity,
            a->velocity
        );

    float angle =
        hp_impact_angle(
            relative,
            normal
        );

    float amplitude;
    float frequency;
    float duration;

    hp_material_signature(
        a->material,
        b->material,
        force,
        velocity,
        angle,
        &amplitude,
        &frequency,
        &duration
    );

    HapticEvent event;

    memset(
        &event,
        0,
        sizeof(event)
    );

    event.type =
        HAPTIC_EVENT_IMPACT;

    event.hand =
        HAPTIC_RIGHT;

    event.material_a =
        a->material;

    event.material_b =
        b->material;

    event.position =
        position;

    event.normal =
        normal;

    event.impact_velocity =
        velocity;

    event.force =
        force;

    event.angle =
        angle;

    event.amplitude =
        amplitude;

    event.frequency =
        frequency;

    event.duration =
        duration;

    event.priority =
        amplitude;

    hp_queue_event(
        engine,
        &event
    );
}


/* ============================================================
 * SIMPLE HAPTIC ENVELOPE
 * ============================================================ */

static float hp_envelope(
    float t,
    float duration
)
{
    if (t < 0.0f)
        return 0.0f;

    if (t >= duration)
        return 0.0f;

    float attack =
        fminf(
            0.012f,
            duration * 0.15f
        );

    float release =
        fminf(
            0.050f,
            duration * 0.35f
        );

    float sustain_start =
        attack;

    float release_start =
        duration - release;

    if (t < sustain_start)
        return t / attack;

    if (t > release_start)
        return
            (duration - t) /
            release;

    return 1.0f;
}


/* ============================================================
 * HAPTIC WAVEFORM
 * ============================================================ */

static float hp_waveform(
    const HapticEvent *event,
    float t
)
{
    float env =
        hp_envelope(
            t,
            event->duration
        );

    float sine =
        sinf(
            2.0f *
            (float)M_PI *
            event->frequency *
            t
        );

    /*
     * Rectified tactile waveform.
     */

    float tactile =
        0.5f +
        0.5f * sine;

    return
        env *
        tactile *
        event->amplitude;
}


/* ============================================================
 * ACTIVATE HAPTIC
 * ============================================================ */

static void hp_activate(
    HapticPhysicsEngine *engine,
    const HapticEvent *event
)
{
    int slot = -1;

    for (int i = 0;
         i < HP_MAX_ACTIVE_HAPTICS;
         ++i)
    {
        if (!engine->active[i].active)
        {
            slot = i;
            break;
        }
    }

    /*
     * If all slots are occupied,
     * replace the lowest-priority effect.
     */

    if (slot < 0) {

        float lowest =
            999999.0f;

        for (int i = 0;
             i < HP_MAX_ACTIVE_HAPTICS;
             ++i)
        {
            float p =
                engine->active[i]
                    .event.priority;

            if (p < lowest) {
                lowest = p;
                slot = i;
            }
        }

        if (event->priority <= lowest)
            return;
    }

    engine->active[slot].active = true;

    engine->active[slot].event =
        *event;

    engine->active[slot].elapsed =
        0.0f;
}


/* ============================================================
 * PROCESS EVENT QUEUE
 * ============================================================ */

static void hp_process_events(
    HapticPhysicsEngine *engine
)
{
    for (int i = 0;
         i < engine->event_count;
         ++i)
    {
        hp_activate(
            engine,
            &engine->events[i]
        );
    }

    engine->event_count = 0;
}


/* ============================================================
 * HAPTIC MIXER
 * ============================================================ */

static float hp_mix_hand(
    HapticPhysicsEngine *engine,
    HapticHand hand,
    float dt
)
{
    float output = 0.0f;

    for (int i = 0;
         i < HP_MAX_ACTIVE_HAPTICS;
         ++i)
    {
        ActiveHaptic *h =
            &engine->active[i];

        if (!h->active)
            continue;

        bool routed =
            h->event.hand == hand ||
            h->event.hand == HAPTIC_BOTH;

        h->elapsed += dt;

        if (h->elapsed >=
            h->event.duration)
        {
            h->active = false;
            continue;
        }

        if (routed) {

            float sample =
                hp_waveform(
                    &h->event,
                    h->elapsed
                );

            output += sample;
        }
    }

    return clampf(
        output,
        0.0f,
        1.0f
    );
}


/* ============================================================
 * QUEST / OPENXR BACKEND
 * ============================================================ */

/*
 * This is intentionally an abstraction.
 *
 * In a real Quest application this layer would translate the
 * generated haptic signal into the OpenXR haptic API.
 *
 * Typical OpenXR route:
 *
 *   XrSession
 *      ↓
 *   XrAction
 *      ↓
 *   XrHapticActionInfo
 *      ↓
 *   XrHapticVibration
 *      ↓
 *   xrApplyHapticFeedback()
 */

typedef struct {

    bool connected;

    float left_amplitude;
    float right_amplitude;

} QuestHapticBackend;


static void quest_haptics_init(
    QuestHapticBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected = true;
}


static void quest_haptics_submit(
    QuestHapticBackend *backend,
    float left,
    float right
)
{
    backend->left_amplitude =
        clampf(left, 0.0f, 1.0f);

    backend->right_amplitude =
        clampf(right, 0.0f, 1.0f);

    /*
     * Production implementation:
     *
     * XrHapticVibration vibration = {
     *     .type = XR_TYPE_HAPTIC_VIBRATION,
     *     .next = NULL,
     *     .duration = ...,
     *     .frequency = ...,
     *     .amplitude = ...
     * };
     *
     * xrApplyHapticFeedback(...);
     */

    printf(
        "\rQuest Haptics  "
        "L=%0.2f  R=%0.2f",
        backend->left_amplitude,
        backend->right_amplitude
    );

    fflush(stdout);
}


/* ============================================================
 * PHYSICS STEP
 * ============================================================ */

static void hp_step_physics(
    HapticPhysicsEngine *engine,
    float dt
)
{
    engine->time += dt;

    /*
     * Integrate bodies.
     */

    for (int i = 0;
         i < engine->body_count;
         ++i)
    {
        PhysicsBody *body =
            &engine->bodies[i];

        if (!body->dynamic)
            continue;

        body->position =
            vadd(
                body->position,
                vmul(
                    body->velocity,
                    dt
                )
            );
    }

    /*
     * Detect collisions.
     */

    for (int i = 0;
         i < engine->body_count;
         ++i)
    {
        for (int j = i + 1;
             j < engine->body_count;
             ++j)
        {
            PhysicsBody *a =
                &engine->bodies[i];

            PhysicsBody *b =
                &engine->bodies[j];

            if (!a->dynamic &&
                !b->dynamic)
                continue;

            Vec3 normal;
            float penetration;

            if (!hp_collision(
                    a,
                    b,
                    &normal,
                    &penetration))
            {
                continue;
            }

            Vec3 contact =
                vadd(
                    a->position,
                    vmul(
                        normal,
                        a->radius
                    )
                );

            if (a->haptic_enabled ||
                b->haptic_enabled)
            {
                hp_create_impact(
                    engine,
                    a,
                    b,
                    contact,
                    normal
                );
            }

            /*
             * Simple positional correction.
             */

            if (penetration > 0.0f) {

                if (a->dynamic &&
                    b->dynamic)
                {
                    a->position =
                        vsub(
                            a->position,
                            vmul(
                                normal,
                                penetration * 0.5f
                            )
                        );

                    b->position =
                        vadd(
                            b->position,
                            vmul(
                                normal,
                                penetration * 0.5f
                            )
                        );
                }
                else if (a->dynamic)
                {
                    a->position =
                        vsub(
                            a->position,
                            vmul(
                                normal,
                                penetration
                            )
                        );
                }
                else if (b->dynamic)
                {
                    b->position =
                        vadd(
                            b->position,
                            vmul(
                                normal,
                                penetration
                            )
                        );
                }
            }
        }
    }
}


/* ============================================================
 * HAPTIC ENGINE STEP
 * ============================================================ */

static void hp_step(
    HapticPhysicsEngine *engine,
    QuestHapticBackend *backend,
    float dt
)
{
    hp_step_physics(
        engine,
        dt
    );

    hp_process_events(
        engine
    );

    float left =
        hp_mix_hand(
            engine,
            HAPTIC_LEFT,
            dt
        );

    float right =
        hp_mix_hand(
            engine,
            HAPTIC_RIGHT,
            dt
        );

    quest_haptics_submit(
        backend,
        left,
        right
    );
}


/* ============================================================
 * DEMONSTRATION SCENE
 * ============================================================ */

static void create_demo_scene(
    HapticPhysicsEngine *engine
)
{
    /*
     * Static steel plate.
     */

    hp_add_body(
        engine,
        "Steel Plate",
        vec3(0.0f, 0.0f, 0.0f),
        vec3(0.0f, 0.0f, 0.0f),
        1000.0f,
        1.0f,
        MATERIAL_STEEL,
        false
    );

    /*
     * Falling steel tool.
     */

    hp_add_body(
        engine,
        "Steel Hammer",
        vec3(0.0f, 3.5f, 0.0f),
        vec3(0.0f, -6.0f, 0.0f),
        2.5f,
        0.35f,
        MATERIAL_STEEL,
        true
    );

    /*
     * Wooden block.
     */

    hp_add_body(
        engine,
        "Wood Block",
        vec3(2.0f, 0.8f, 0.0f),
        vec3(-1.5f, 0.0f, 0.0f),
        3.0f,
        0.65f,
        MATERIAL_WOOD,
        true
    );

    /*
     * Concrete obstacle.
     */

    hp_add_body(
        engine,
        "Concrete Wall",
        vec3(-2.5f, 0.7f, 0.0f),
        vec3(0.0f, 0.0f, 0.0f),
        10000.0f,
        0.7f,
        MATERIAL_CONCRETE,
        false
    );
}


/* ============================================================
 * DEBUG EVENT PRINTING
 * ============================================================ */

static void print_event(
    const HapticEvent *event
)
{
    printf(
        "\n\nHAPTIC EVENT\n"
        "------------\n"
        "Material A : %s\n"
        "Material B : %s\n"
        "Velocity   : %.2f m/s\n"
        "Force      : %.2f N\n"
        "Angle      : %.2f degrees\n"
        "Amplitude  : %.2f\n"
        "Frequency  : %.1f Hz\n"
        "Duration   : %.3f s\n",
        materials[event->material_a].name,
        materials[event->material_b].name,
        event->impact_velocity,
        event->force,
        event->angle * 180.0f / (float)M_PI,
        event->amplitude,
        event->frequency,
        event->duration
    );
}


/* ============================================================
 * DIRECT MATERIAL TEST
 * ============================================================ */

static void test_material(
    MaterialType a,
    MaterialType b,
    float velocity
)
{
    float force =
        velocity * 2.5f;

    float angle =
        0.15f;

    float amplitude;
    float frequency;
    float duration;

    hp_material_signature(
        a,
        b,
        force,
        velocity,
        angle,
        &amplitude,
        &frequency,
        &duration
    );

    printf(
        "\n%-12s + %-12s"
        " | V=%5.2f"
        " | A=%0.2f"
        " | F=%6.1fHz"
        " | D=%0.3fs",
        materials[a].name,
        materials[b].name,
        velocity,
        amplitude,
        frequency,
        duration
    );
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "========================================\n"
        "       HAPTIC PHYSICS ENGINE\n"
        "       Meta Quest C Prototype\n"
        "========================================\n\n"
    );

    HapticPhysicsEngine engine;

    hp_init(
        &engine
    );

    QuestHapticBackend backend;

    quest_haptics_init(
        &backend
    );

    create_demo_scene(
        &engine
    );

    printf(
        "Bodies: %d\n",
        engine.body_count
    );

    printf(
        "\nMaterial response tests:\n"
    );

    test_material(
        MATERIAL_STEEL,
        MATERIAL_STEEL,
        2.0f
    );

    test_material(
        MATERIAL_STEEL,
        MATERIAL_CONCRETE,
        4.0f
    );

    test_material(
        MATERIAL_WOOD,
        MATERIAL_STEEL,
        3.0f
    );

    test_material(
        MATERIAL_RUBBER,
        MATERIAL_STEEL,
        3.0f
    );

    test_material(
        MATERIAL_GLASS,
        MATERIAL_STEEL,
        4.0f
    );

    printf("\n\nRunning physics...\n");

    /*
     * 5 seconds of simulation.
     */

    const float dt =
        1.0f / 90.0f;

    int frames =
        (int)(5.0f / dt);

    for (int frame = 0;
         frame < frames;
         ++frame)
    {
        hp_step(
            &engine,
            &backend,
            dt
        );

        /*
         * Print queued events after
         * physics processing.
         */

        if (engine.event_count > 0) {

            for (int i = 0;
                 i < engine.event_count;
                 ++i)
            {
                print_event(
                    &engine.events[i]
                );
            }
        }
    }

    printf(
        "\n\nSimulation complete.\n"
    );

    return 0;
}





/*
 * haptic_material_engine.c
 *
 * HAPTIC MATERIAL ENGINE
 * ======================
 *
 * Procedural tactile-material synthesis for Meta Quest.
 *
 * The engine converts material properties + interaction state
 * into a continuously changing haptic signal.
 *
 * Supported sensations:
 *
 *   - Impact
 *   - Tap
 *   - Sliding
 *   - Friction
 *   - Roughness
 *   - Grain
 *   - Scratching
 *   - Stick-slip
 *   - Resonance
 *   - Continuous contact
 *   - Material transitions
 *
 * Designed as a C11 foundation for:
 *
 *      VR OBJECT
 *          |
 *          v
 *    MATERIAL ENGINE
 *          |
 *          v
 *    HAPTIC SIGNAL
 *          |
 *          v
 *   QUEST / OPENXR
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_material_engine.c -lm \
 *       -o haptic_material_engine
 *
 * This file contains a hardware-independent renderer.
 *
 * A production Quest implementation can connect the output to
 * OpenXR parametric or PCM haptics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HME_SAMPLE_RATE       1000.0f
#define HME_MAX_MATERIALS       64
#define HME_MAX_CONTACTS        32
#define HME_MAX_SAMPLES       2048

#define HME_MIN_FREQUENCY      20.0f
#define HME_MAX_FREQUENCY     350.0f

#define HME_EPSILON           0.000001f


/* ============================================================
 * BASIC MATH
 * ============================================================ */

static float clampf(
    float x,
    float min,
    float max
)
{
    if (x < min)
        return min;

    if (x > max)
        return max;

    return x;
}


static float lerpf(
    float a,
    float b,
    float t
)
{
    return a + (b - a) * t;
}


static float smoothstep(
    float edge0,
    float edge1,
    float x
)
{
    float t =
        clampf(
            (x - edge0) /
            (edge1 - edge0),
            0.0f,
            1.0f
        );

    return
        t * t *
        (3.0f - 2.0f * t);
}


static float fractf(float x)
{
    return x - floorf(x);
}


/* ============================================================
 * DETERMINISTIC NOISE
 * ============================================================ */

static uint32_t noise_hash(
    uint32_t x
)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;

    return x;
}


static float noise1(
    uint32_t seed
)
{
    uint32_t h =
        noise_hash(seed);

    return
        ((float)(h & 0xFFFFFF) /
         16777215.0f) * 2.0f
        - 1.0f;
}


static float smooth_noise(
    float x,
    uint32_t seed
)
{
    int i =
        (int)floorf(x);

    float f =
        fractf(x);

    float a =
        noise1(
            (uint32_t)i + seed
        );

    float b =
        noise1(
            (uint32_t)(i + 1) + seed
        );

    f =
        f * f *
        (3.0f - 2.0f * f);

    return
        lerpf(a, b, f);
}


/* ============================================================
 * MATERIAL TYPES
 * ============================================================ */

typedef enum {

    HME_MAT_STEEL,
    HME_MAT_ALUMINIUM,
    HME_MAT_WOOD,
    HME_MAT_RUBBER,
    HME_MAT_GLASS,
    HME_MAT_CONCRETE,
    HME_MAT_PLASTIC,
    HME_MAT_CARBON,
    HME_MAT_LEATHER,
    HME_MAT_CERAMIC,
    HME_MAT_SANDPAPER,
    HME_MAT_FABRIC,
    HME_MAT_ICE,
    HME_MAT_STONE,
    HME_MAT_BRASS,
    HME_MAT_COUNT

} HME_MaterialType;


/* ============================================================
 * MATERIAL DEFINITION
 * ============================================================ */

typedef struct {

    char name[48];

    HME_MaterialType type;

    /*
     * Physical / tactile properties.
     */

    float hardness;

    float friction;

    float roughness;

    float elasticity;

    float damping;

    float density;

    /*
     * Surface structure.
     */

    float grain;

    float grain_scale;

    float texture;

    /*
     * Resonance.
     */

    float resonance_frequency;

    float resonance_strength;

    /*
     * Haptic output.
     */

    float contact_gain;

    float impact_gain;

    float friction_gain;

    float texture_gain;

    float minimum_frequency;

    float maximum_frequency;

} HME_Material;


/* ============================================================
 * MATERIAL DATABASE
 * ============================================================ */

static HME_Material g_materials[
    HME_MAT_COUNT
] = {

    {
        "Steel",
        HME_MAT_STEEL,

        0.96f,
        0.62f,
        0.16f,
        0.25f,
        0.32f,
        7850.0f,

        0.18f,
        14.0f,
        0.20f,

        190.0f,
        0.85f,

        0.65f,
        1.00f,
        0.70f,
        0.65f,

        90.0f,
        260.0f
    },

    {
        "Aluminium",
        HME_MAT_ALUMINIUM,

        0.74f,
        0.52f,
        0.20f,
        0.40f,
        0.45f,
        2700.0f,

        0.22f,
        11.0f,
        0.24f,

        145.0f,
        0.65f,

        0.55f,
        0.80f,
        0.55f,
        0.60f,

        70.0f,
        220.0f
    },

    {
        "Wood",
        HME_MAT_WOOD,

        0.42f,
        0.68f,
        0.42f,
        0.58f,
        0.70f,
        650.0f,

        0.80f,
        5.0f,
        0.72f,

        82.0f,
        0.32f,

        0.45f,
        0.58f,
        0.72f,
        0.90f,

        45.0f,
        130.0f
    },

    {
        "Rubber",
        HME_MAT_RUBBER,

        0.22f,
        0.92f,
        0.25f,
        0.88f,
        0.92f,
        1100.0f,

        0.20f,
        8.0f,
        0.30f,

        55.0f,
        0.15f,

        0.50f,
        0.30f,
        0.90f,
        0.40f,

        25.0f,
        80.0f
    },

    {
        "Glass",
        HME_MAT_GLASS,

        0.94f,
        0.38f,
        0.08f,
        0.62f,
        0.20f,
        2500.0f,

        0.05f,
        25.0f,
        0.12f,

        260.0f,
        0.92f,

        0.55f,
        0.96f,
        0.42f,
        0.35f,

        120.0f,
        320.0f
    },

    {
        "Concrete",
        HME_MAT_CONCRETE,

        0.90f,
        0.88f,
        0.72f,
        0.18f,
        0.62f,
        2400.0f,

        0.58f,
        3.0f,
        0.78f,

        120.0f,
        0.55f,

        0.70f,
        0.90f,
        0.92f,
        0.88f,

        40.0f,
        170.0f
    },

    {
        "Plastic",
        HME_MAT_PLASTIC,

        0.38f,
        0.50f,
        0.24f,
        0.52f,
        0.72f,
        950.0f,

        0.18f,
        9.0f,
        0.35f,

        100.0f,
        0.30f,

        0.42f,
        0.48f,
        0.52f,
        0.55f,

        40.0f,
        140.0f
    },

    {
        "Carbon Fibre",
        HME_MAT_CARBON,

        0.94f,
        0.46f,
        0.13f,
        0.25f,
        0.38f,
        1600.0f,

        0.12f,
        20.0f,
        0.18f,

        215.0f,
        0.82f,

        0.62f,
        0.88f,
        0.50f,
        0.50f,

        90.0f,
        280.0f
    },

    {
        "Leather",
        HME_MAT_LEATHER,

        0.18f,
        0.78f,
        0.38f,
        0.76f,
        0.92f,
        860.0f,

        0.55f,
        4.0f,
        0.70f,

        45.0f,
        0.12f,

        0.35f,
        0.25f,
        0.82f,
        0.78f,

        25.0f,
        90.0f
    },

    {
        "Ceramic",
        HME_MAT_CERAMIC,

        0.91f,
        0.51f,
        0.13f,
        0.40f,
        0.28f,
        2400.0f,

        0.10f,
        18.0f,
        0.18f,

        230.0f,
        0.86f,

        0.62f,
        0.92f,
        0.48f,
        0.42f,

        100.0f,
        300.0f
    },

    {
        "Sandpaper",
        HME_MAT_SANDPAPER,

        0.65f,
        1.00f,
        0.98f,
        0.08f,
        0.80f,
        1500.0f,

        0.90f,
        42.0f,
        1.00f,

        70.0f,
        0.08f,

        0.55f,
        0.20f,
        1.00f,
        1.00f,

        35.0f,
        150.0f
    },

    {
        "Fabric",
        HME_MAT_FABRIC,

        0.12f,
        0.72f,
        0.66f,
        0.80f,
        0.95f,
        400.0f,

        0.75f,
        12.0f,
        0.88f,

        38.0f,
        0.05f,

        0.30f,
        0.15f,
        0.75f,
        0.95f,

        20.0f,
        70.0f
    },

    {
        "Ice",
        HME_MAT_ICE,

        0.72f,
        0.12f,
        0.04f,
        0.30f,
        0.30f,
        917.0f,

        0.04f,
        30.0f,
        0.08f,

        180.0f,
        0.65f,

        0.40f,
        0.70f,
        0.15f,
        0.20f,

        80.0f,
        250.0f
    },

    {
        "Stone",
        HME_MAT_STONE,

        0.92f,
        0.84f,
        0.65f,
        0.12f,
        0.68f,
        2600.0f,

        0.48f,
        2.5f,
        0.76f,

        130.0f,
        0.58f,

        0.70f,
        0.92f,
        0.88f,
        0.82f,

        45.0f,
        180.0f
    },

    {
        "Brass",
        HME_MAT_BRASS,

        0.82f,
        0.55f,
        0.11f,
        0.35f,
        0.28f,
        8500.0f,

        0.08f,
        16.0f,
        0.15f,

        170.0f,
        0.82f,

        0.64f,
        0.94f,
        0.56f,
        0.48f,

        80.0f,
        240.0f
    }
};


/* ============================================================
 * CONTACT MODE
 * ============================================================ */

typedef enum {

    HME_CONTACT_NONE,
    HME_CONTACT_TOUCH,
    HME_CONTACT_PRESS,
    HME_CONTACT_SLIDE,
    HME_CONTACT_SCRATCH,
    HME_CONTACT_STICK_SLIP,
    HME_CONTACT_ROLL

} HME_ContactMode;


/* ============================================================
 * CONTACT STATE
 * ============================================================ */

typedef struct {

    bool active;

    int material_a;
    int material_b;

    HME_ContactMode mode;

    /*
     * Contact mechanics.
     */

    float normal_force;

    float tangential_velocity;

    float normal_velocity;

    float pressure;

    float contact_area;

    float angle;

    /*
     * Surface interaction.
     */

    float sliding_distance;

    float accumulated_energy;

    float stick_time;

    float slip_time;

    /*
     * Position in the material.
     */

    float texture_position;

    /*
     * Time.
     */

    float time;

    /*
     * Which controller.
     */

    int hand;

} HME_Contact;


/* ============================================================
 * HAPTIC SAMPLE
 * ============================================================ */

typedef struct {

    float amplitude;

    float frequency;

    float transient;

} HME_Sample;


/* ============================================================
 * MATERIAL ENGINE
 * ============================================================ */

typedef struct {

    HME_Contact contacts[
        HME_MAX_CONTACTS
    ];

    int contact_count;

    float global_gain;

    uint32_t noise_seed;

} HME_Engine;


/* ============================================================
 * ENGINE INITIALISATION
 * ============================================================ */

static void hme_init(
    HME_Engine *engine
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->global_gain =
        1.0f;

    engine->noise_seed =
        0xA5317F21;
}


/* ============================================================
 * CREATE CONTACT
 * ============================================================ */

static int hme_create_contact(
    HME_Engine *engine,
    int material_a,
    int material_b,
    int hand
)
{
    if (
        engine->contact_count >=
        HME_MAX_CONTACTS
    )
        return -1;

    int id =
        engine->contact_count;

    HME_Contact *c =
        &engine->contacts[id];

    memset(
        c,
        0,
        sizeof(*c)
    );

    c->active = true;

    c->material_a =
        material_a;

    c->material_b =
        material_b;

    c->mode =
        HME_CONTACT_TOUCH;

    c->hand =
        hand;

    engine->contact_count++;

    return id;
}


/* ============================================================
 * GET COMBINED MATERIAL
 * ============================================================ */

static HME_Material hme_combine_materials(
    int a,
    int b
)
{
    HME_Material A =
        g_materials[a];

    HME_Material B =
        g_materials[b];

    HME_Material R =
        A;

    /*
     * Geometric averaging works reasonably
     * for many multiplicative tactile properties.
     */

    R.hardness =
        sqrtf(
            A.hardness *
            B.hardness
        );

    R.friction =
        sqrtf(
            A.friction *
            B.friction
        );

    R.roughness =
        sqrtf(
            A.roughness *
            B.roughness
        );

    R.elasticity =
        sqrtf(
            A.elasticity *
            B.elasticity
        );

    R.damping =
        sqrtf(
            A.damping *
            B.damping
        );

    R.grain =
        sqrtf(
            A.grain *
            B.grain
        );

    R.texture =
        sqrtf(
            A.texture *
            B.texture
        );

    R.contact_gain =
        sqrtf(
            A.contact_gain *
            B.contact_gain
        );

    R.friction_gain =
        sqrtf(
            A.friction_gain *
            B.friction_gain
        );

    R.texture_gain =
        sqrtf(
            A.texture_gain *
            B.texture_gain
        );

    R.resonance_frequency =
        sqrtf(
            A.resonance_frequency *
            B.resonance_frequency
        );

    R.resonance_strength =
        sqrtf(
            A.resonance_strength *
            B.resonance_strength
        );

    R.minimum_frequency =
        fmaxf(
            A.minimum_frequency,
            B.minimum_frequency
        );

    R.maximum_frequency =
        fminf(
            A.maximum_frequency,
            B.maximum_frequency
        );

    return R;
}


/* ============================================================
 * MATERIAL CONTACT RESPONSE
 * ============================================================ */

static float hme_contact_response(
    const HME_Material *m,
    const HME_Contact *c
)
{
    float pressure =
        clampf(
            c->pressure,
            0.0f,
            1.0f
        );

    float force =
        clampf(
            c->normal_force / 20.0f,
            0.0f,
            1.0f
        );

    return
        m->contact_gain *
        (0.25f +
         0.75f * pressure) *
        (0.30f +
         0.70f * force);
}


/* ============================================================
 * FRICTION MODEL
 * ============================================================ */

static float hme_friction_response(
    const HME_Material *m,
    const HME_Contact *c
)
{
    float velocity =
        fabsf(
            c->tangential_velocity
        );

    float velocity_factor =
        clampf(
            velocity / 2.5f,
            0.0f,
            1.0f
        );

    float friction =
        m->friction;

    /*
     * Coulomb-like friction response.
     */

    float force =
        c->normal_force *
        friction;

    float force_factor =
        clampf(
            force / 20.0f,
            0.0f,
            1.0f
        );

    return
        m->friction_gain *
        velocity_factor *
        force_factor;
}


/* ============================================================
 * PROCEDURAL SURFACE TEXTURE
 * ============================================================ */

static float hme_surface_texture(
    const HME_Material *m,
    const HME_Contact *c,
    uint32_t seed
)
{
    float x =
        c->texture_position *
        m->grain_scale;

    /*
     * Multi-scale surface noise.
     */

    float low =
        smooth_noise(
            x * 0.25f,
            seed
        );

    float mid =
        smooth_noise(
            x,
            seed + 100
        );

    float high =
        smooth_noise(
            x * 5.0f,
            seed + 200
        );

    float texture =
        low * 0.25f +
        mid * 0.45f +
        high * 0.30f;

    /*
     * Grain direction.
     */

    float grain =
        sinf(
            2.0f *
            (float)M_PI *
            x
        );

    texture =
        texture *
        m->texture +
        grain *
        m->grain *
        0.25f;

    return
        clampf(
            texture,
            -1.0f,
            1.0f
        );
}


/* ============================================================
 * STICK-SLIP MODEL
 * ============================================================ */

static float hme_stick_slip(
    const HME_Material *m,
    HME_Contact *c
)
{
    float velocity =
        fabsf(
            c->tangential_velocity
        );

    float static_limit =
        m->friction *
        (0.5f +
         c->normal_force);

    /*
     * Very low tangential movement + high
     * friction produces a sticking state.
     */

    bool sticking =
        velocity < 0.12f &&
        static_limit > 0.30f;

    if (sticking) {

        c->stick_time +=
            1.0f /
            HME_SAMPLE_RATE;

        c->slip_time = 0.0f;

        return
            0.0f;
    }

    /*
     * Slip state.
     */

    c->slip_time +=
        1.0f /
        HME_SAMPLE_RATE;

    c->stick_time = 0.0f;

    /*
     * Release transient.
     */

    float slip_phase =
        fmodf(
            c->slip_time *
            (8.0f +
             m->friction * 24.0f),
            1.0f
        );

    float pulse =
        expf(
            -18.0f *
            slip_phase
        );

    return
        pulse *
        m->friction_gain *
        0.55f;
}


/* ============================================================
 * RESONANCE
 * ============================================================ */

static float hme_resonance(
    const HME_Material *m,
    const HME_Contact *c
)
{
    float velocity =
        fabsf(
            c->normal_velocity
        );

    float excitation =
        clampf(
            velocity / 5.0f,
            0.0f,
            1.0f
        );

    float oscillation =
        sinf(
            2.0f *
            (float)M_PI *
            m->resonance_frequency *
            c->time
        );

    return
        oscillation *
        m->resonance_strength *
        excitation;
}


/* ============================================================
 * SCRATCH RESPONSE
 * ============================================================ */

static float hme_scratch(
    const HME_Material *m,
    HME_Contact *c,
    uint32_t seed
)
{
    float velocity =
        fabsf(
            c->tangential_velocity
        );

    float velocity_factor =
        clampf(
            velocity / 4.0f,
            0.0f,
            1.0f
        );

    float texture =
        hme_surface_texture(
            m,
            c,
            seed
        );

    /*
     * Scratching emphasises high-frequency
     * texture components.
     */

    float scratch =
        fabsf(texture) *
        velocity_factor *
        m->roughness *
        m->texture_gain;

    return scratch;
}


/* ============================================================
 * ROLLING RESPONSE
 * ============================================================ */

static float hme_rolling(
    const HME_Material *m,
    HME_Contact *c,
    uint32_t seed
)
{
    float velocity =
        fabsf(
            c->tangential_velocity
        );

    float rotation =
        c->texture_position *
        m->grain_scale;

    float periodic =
        sinf(
            rotation *
            2.0f *
            (float)M_PI
        );

    float noise =
        smooth_noise(
            rotation * 2.0f,
            seed
        );

    return
        (0.55f * fabsf(periodic) +
         0.45f * fabsf(noise))
        *
        velocity /
        3.0f
        *
        m->texture_gain;
}


/* ============================================================
 * CONTACT SIGNAL
 * ============================================================ */

static HME_Sample hme_render_contact(
    HME_Engine *engine,
    HME_Contact *c,
    float dt
)
{
    HME_Material m =
        hme_combine_materials(
            c->material_a,
            c->material_b
        );

    c->time += dt;

    /*
     * Surface advances according to movement.
     */

    c->texture_position +=
        c->tangential_velocity *
        dt;

    c->sliding_distance +=
        fabsf(
            c->tangential_velocity
        ) *
        dt;

    /*
     * Base contact.
     */

    float contact =
        hme_contact_response(
            &m,
            c
        );

    /*
     * Friction.
     */

    float friction =
        hme_friction_response(
            &m,
            c
        );

    /*
     * Surface texture.
     */

    float texture =
        hme_surface_texture(
            &m,
            c,
            engine->noise_seed
        );

    /*
     * Stick-slip.
     */

    float stick_slip =
        hme_stick_slip(
            &m,
            c
        );

    /*
     * Resonance.
     */

    float resonance =
        hme_resonance(
            &m,
            c
        );

    /*
     * Default frequency.
     */

    float frequency =
        lerpf(
            m.minimum_frequency,
            m.maximum_frequency,
            clampf(
                c->normal_force /
                20.0f,
                0.0f,
                1.0f
            )
        );

    float amplitude =
        contact;

    /*
     * Mode-specific rendering.
     */

    switch (c->mode) {

        case HME_CONTACT_TOUCH:

            amplitude =
                contact;

            frequency =
                m.resonance_frequency;

            break;


        case HME_CONTACT_PRESS:

            amplitude =
                contact *
                (0.75f +
                 0.25f * m.hardness);

            frequency =
                lerpf(
                    40.0f,
                    150.0f,
                    m.hardness
                );

            break;


        case HME_CONTACT_SLIDE:

            amplitude =
                contact * 0.35f +
                friction * 0.65f +
                fabsf(texture) *
                m.texture_gain;

            frequency =
                lerpf(
                    m.minimum_frequency,
                    m.maximum_frequency,
                    clampf(
                        fabsf(
                            c->tangential_velocity
                        ) / 3.0f,
                        0.0f,
                        1.0f
                    )
                );

            break;


        case HME_CONTACT_SCRATCH:

            amplitude =
                friction * 0.25f +
                hme_scratch(
                    &m,
                    c,
                    engine->noise_seed
                ) * 0.75f;

            frequency =
                lerpf(
                    m.minimum_frequency,
                    m.maximum_frequency,
                    0.85f
                );

            break;


        case HME_CONTACT_STICK_SLIP:

            amplitude =
                stick_slip +
                friction * 0.25f;

            frequency =
                m.resonance_frequency;

            break;


        case HME_CONTACT_ROLL:

            amplitude =
                hme_rolling(
                    &m,
                    c,
                    engine->noise_seed
                );

            frequency =
                lerpf(
                    35.0f,
                    m.maximum_frequency,
                    clampf(
                        fabsf(
                            c->tangential_velocity
                        ) / 5.0f,
                        0.0f,
                        1.0f
                    )
                );

            break;


        default:

            amplitude = 0.0f;
            frequency = 0.0f;

            break;
    }

    /*
     * Add material resonance.
     */

    amplitude +=
        fabsf(resonance) *
        0.20f;

    /*
     * Texture modulation.
     */

    amplitude *=
        1.0f +
        texture *
        0.18f;

    /*
     * Apply global gain.
     */

    amplitude *=
        engine->global_gain;

    amplitude =
        clampf(
            amplitude,
            0.0f,
            1.0f
        );

    frequency =
        clampf(
            frequency,
            HME_MIN_FREQUENCY,
            HME_MAX_FREQUENCY
        );

    HME_Sample output;

    output.amplitude =
        amplitude;

    output.frequency =
        frequency;

    output.transient =
        fabsf(stick_slip);

    return output;
}


/* ============================================================
 * IMPACT SYNTHESIS
 * ============================================================ */

static HME_Sample hme_render_impact(
    HME_Engine *engine,
    int material_a,
    int material_b,
    float velocity,
    float force,
    float time
)
{
    HME_Material m =
        hme_combine_materials(
            material_a,
            material_b
        );

    float force_factor =
        clampf(
            force / 30.0f,
            0.0f,
            1.0f
        );

    float velocity_factor =
        clampf(
            velocity / 8.0f,
            0.0f,
            1.0f
        );

    float envelope =
        expf(
            -time *
            (15.0f +
             m.damping * 30.0f)
        );

    float ringing =
        fabsf(
            sinf(
                2.0f *
                (float)M_PI *
                m.resonance_frequency *
                time
            )
        );

    float noise =
        fabsf(
            smooth_noise(
                time * 700.0f,
                engine->noise_seed
            )
        );

    float amplitude =
        m.impact_gain *
        (0.30f +
         0.70f * force_factor) *
        (0.30f +
         0.70f * velocity_factor) *
        envelope;

    amplitude *=
        0.60f +
        0.25f * ringing +
        0.15f * noise;

    HME_Sample result;

    result.amplitude =
        clampf(
            amplitude,
            0.0f,
            1.0f
        );

    result.frequency =
        m.resonance_frequency;

    result.transient =
        clampf(
            amplitude * 1.2f,
            0.0f,
            1.0f
        );

    return result;
}


/* ============================================================
 * CONTACT UPDATE
 * ============================================================ */

static void hme_update_contact(
    HME_Engine *engine,
    int id,
    HME_ContactMode mode,
    float normal_force,
    float tangential_velocity,
    float normal_velocity,
    float pressure,
    float contact_area,
    float angle
)
{
    if (
        id < 0 ||
        id >= engine->contact_count
    )
        return;

    HME_Contact *c =
        &engine->contacts[id];

    c->mode =
        mode;

    c->normal_force =
        normal_force;

    c->tangential_velocity =
        tangential_velocity;

    c->normal_velocity =
        normal_velocity;

    c->pressure =
        pressure;

    c->contact_area =
        contact_area;

    c->angle =
        angle;
}


/* ============================================================
 * CONTACT RENDER
 * ============================================================ */

static HME_Sample hme_update(
    HME_Engine *engine,
    float dt
)
{
    HME_Sample left = {
        0.0f,
        0.0f,
        0.0f
    };

    HME_Sample right = {
        0.0f,
        0.0f,
        0.0f
    };

    for (
        int i = 0;
        i < engine->contact_count;
        ++i
    )
    {
        HME_Contact *c =
            &engine->contacts[i];

        if (!c->active)
            continue;

        HME_Sample s =
            hme_render_contact(
                engine,
                c,
                dt
            );

        if (c->hand == 0) {

            left.amplitude =
                clampf(
                    left.amplitude +
                    s.amplitude,
                    0.0f,
                    1.0f
                );

            left.frequency =
                s.frequency;

            left.transient =
                fmaxf(
                    left.transient,
                    s.transient
                );
        }

        else {

            right.amplitude =
                clampf(
                    right.amplitude +
                    s.amplitude,
                    0.0f,
                    1.0f
                );

            right.frequency =
                s.frequency;

            right.transient =
                fmaxf(
                    right.transient,
                    s.transient
                );
        }
    }

    /*
     * This demo combines both channels.
     */

    HME_Sample result;

    result.amplitude =
        fmaxf(
            left.amplitude,
            right.amplitude
        );

    result.frequency =
        right.amplitude >
        left.amplitude
        ? right.frequency
        : left.frequency;

    result.transient =
        fmaxf(
            left.transient,
            right.transient
        );

    return result;
}


/* ============================================================
 * MATERIAL DESCRIPTION
 * ============================================================ */

static void hme_print_material(
    int id
)
{
    if (
        id < 0 ||
        id >= HME_MAT_COUNT
    )
        return;

    HME_Material *m =
        &g_materials[id];

    printf(
        "\n"
        "Material: %s\n"
        "-----------------------------\n"
        "Hardness       : %.2f\n"
        "Friction       : %.2f\n"
        "Roughness      : %.2f\n"
        "Elasticity     : %.2f\n"
        "Damping        : %.2f\n"
        "Grain          : %.2f\n"
        "Texture        : %.2f\n"
        "Resonance      : %.1f Hz\n"
        "Resonance Gain : %.2f\n"
        "Frequency      : %.1f - %.1f Hz\n",
        m->name,
        m->hardness,
        m->friction,
        m->roughness,
        m->elasticity,
        m->damping,
        m->grain,
        m->texture,
        m->resonance_frequency,
        m->resonance_strength,
        m->minimum_frequency,
        m->maximum_frequency
    );
}


/* ============================================================
 * MATERIAL COMPARISON
 * ============================================================ */

static void hme_compare_materials(
    int a,
    int b
)
{
    HME_Material m =
        hme_combine_materials(
            a,
            b
        );

    printf(
        "\n"
        "Material Pair\n"
        "=============================\n"
        "%s + %s\n\n"
        "Hardness       %.3f\n"
        "Friction       %.3f\n"
        "Roughness      %.3f\n"
        "Texture        %.3f\n"
        "Resonance      %.1f Hz\n"
        "Resonance Gain %.3f\n",
        g_materials[a].name,
        g_materials[b].name,
        m.hardness,
        m.friction,
        m.roughness,
        m.texture,
        m.resonance_frequency,
        m.resonance_strength
    );
}


/* ============================================================
 * DEMO: SLIDING STEEL ON STEEL
 * ============================================================ */

static void demo_steel_slide(
    HME_Engine *engine
)
{
    printf(
        "\n"
        "=== STEEL ON STEEL ===\n"
    );

    int contact =
        hme_create_contact(
            engine,
            HME_MAT_STEEL,
            HME_MAT_STEEL,
            1
        );

    for (
        int i = 0;
        i < 1000;
        ++i
    )
    {
        float t =
            (float)i /
            HME_SAMPLE_RATE;

        float speed =
            1.2f +
            0.4f *
            sinf(
                t * 2.0f *
                (float)M_PI
            );

        hme_update_contact(
            engine,
            contact,
            HME_CONTACT_SLIDE,
            8.0f,
            speed,
            0.0f,
            0.60f,
            0.005f,
            0.0f
        );

        HME_Sample s =
            hme_update(
                engine,
                1.0f /
                HME_SAMPLE_RATE
            );

        if (
            i % 100 == 0
        )
        {
            printf(
                "t=%5.2f "
                "amp=%0.3f "
                "freq=%6.1f "
                "trans=%0.3f\n",
                t,
                s.amplitude,
                s.frequency,
                s.transient
            );
        }
    }

    engine->contacts[contact]
        .active = false;
}


/* ============================================================
 * DEMO: SANDPAPER
 * ============================================================ */

static void demo_sandpaper(
    HME_Engine *engine
)
{
    printf(
        "\n"
        "=== SANDPAPER ===\n"
    );

    int contact =
        hme_create_contact(
            engine,
            HME_MAT_SANDPAPER,
            HME_MAT_STEEL,
            1
        );

    for (
        int i = 0;
        i < 600;
        ++i
    )
    {
        float speed =
            0.5f +
            ((float)i /
             600.0f) *
            2.5f;

        hme_update_contact(
            engine,
            contact,
            HME_CONTACT_SCRATCH,
            7.0f,
            speed,
            0.0f,
            0.65f,
            0.003f,
            0.0f
        );

        HME_Sample s =
            hme_update(
                engine,
                1.0f /
                HME_SAMPLE_RATE
            );

        if (
            i % 100 == 0
        )
        {
            printf(
                "speed=%0.2f "
                "amp=%0.3f "
                "freq=%6.1f\n",
                speed,
                s.amplitude,
                s.frequency
            );
        }
    }

    engine->contacts[contact]
        .active = false;
}


/* ============================================================
 * DEMO: WOOD GRAIN
 * ============================================================ */

static void demo_wood(
    HME_Engine *engine
)
{
    printf(
        "\n"
        "=== WOOD GRAIN ===\n"
    );

    int contact =
        hme_create_contact(
            engine,
            HME_MAT_WOOD,
            HME_MAT_WOOD,
            0
        );

    for (
        int i = 0;
        i < 800;
        ++i
    )
    {
        float speed =
            0.8f;

        hme_update_contact(
            engine,
            contact,
            HME_CONTACT_SLIDE,
            5.0f,
            speed,
            0.0f,
            0.50f,
            0.01f,
            0.0f
        );

        HME_Sample s =
            hme_update(
                engine,
                1.0f /
                HME_SAMPLE_RATE
            );

        if (
            i % 100 == 0
        )
        {
            printf(
                "position=%0.2f "
                "amp=%0.3f "
                "freq=%6.1f\n",
                engine->contacts[contact]
                    .texture_position,
                s.amplitude,
                s.frequency
            );
        }
    }

    engine->contacts[contact]
        .active = false;
}


/* ============================================================
 * DEMO: STICK-SLIP
 * ============================================================ */

static void demo_stick_slip(
    HME_Engine *engine
)
{
    printf(
        "\n"
        "=== STICK-SLIP ===\n"
    );

    int contact =
        hme_create_contact(
            engine,
            HME_MAT_RUBBER,
            HME_MAT_STEEL,
            1
        );

    for (
        int i = 0;
        i < 1000;
        ++i
    )
    {
        float t =
            (float)i /
            HME_SAMPLE_RATE;

        /*
         * Slow movement creates
         * repeated stick-slip events.
         */

        float velocity =
            fabsf(
                sinf(
                    t *
                    2.0f *
                    (float)M_PI *
                    2.0f
                )
            ) * 0.5f;

        hme_update_contact(
            engine,
            contact,
            HME_CONTACT_STICK_SLIP,
            10.0f,
            velocity,
            0.0f,
            0.70f,
            0.008f,
            0.0f
        );

        HME_Sample s =
            hme_update(
                engine,
                1.0f /
                HME_SAMPLE_RATE
            );

        if (
            i % 100 == 0
        )
        {
            printf(
                "t=%0.2f "
                "velocity=%0.3f "
                "amp=%0.3f "
                "trans=%0.3f\n",
                t,
                velocity,
                s.amplitude,
                s.transient
            );
        }
    }

    engine->contacts[contact]
        .active = false;
}


/* ============================================================
 * IMPACT TEST
 * ============================================================ */

static void demo_impact(
    HME_Engine *engine
)
{
    printf(
        "\n"
        "=== MATERIAL IMPACTS ===\n"
    );

    int pairs[][2] = {

        {
            HME_MAT_STEEL,
            HME_MAT_STEEL
        },

        {
            HME_MAT_WOOD,
            HME_MAT_STEEL
        },

        {
            HME_MAT_GLASS,
            HME_MAT_STEEL
        },

        {
            HME_MAT_RUBBER,
            HME_MAT_STEEL
        },

        {
            HME_MAT_CERAMIC,
            HME_MAT_STEEL
        }

    };

    int count =
        sizeof(pairs) /
        sizeof(pairs[0]);

    for (
        int p = 0;
        p < count;
        ++p
    )
    {
        printf(
            "\n%s + %s\n",
            g_materials[
                pairs[p][0]
            ].name,
            g_materials[
                pairs[p][1]
            ].name
        );

        for (
            int i = 0;
            i < 10;
            ++i
        )
        {
            float t =
                (float)i *
                0.005f;

            HME_Sample s =
                hme_render_impact(
                    engine,
                    pairs[p][0],
                    pairs[p][1],
                    4.0f,
                    15.0f,
                    t
                );

            printf(
                "  t=%0.3f "
                "A=%0.3f "
                "F=%6.1f "
                "T=%0.3f\n",
                t,
                s.amplitude,
                s.frequency,
                s.transient
            );
        }
    }
}


/* ============================================================
 * QUEST OUTPUT ABSTRACTION
 * ============================================================ */

typedef struct {

    bool connected;

    float left_amplitude;
    float right_amplitude;

    float left_frequency;
    float right_frequency;

} HME_QuestBackend;


static void hme_quest_init(
    HME_QuestBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected =
        true;
}


static void hme_quest_submit(
    HME_QuestBackend *backend,
    HME_Sample left,
    HME_Sample right
)
{
    backend->left_amplitude =
        left.amplitude;

    backend->right_amplitude =
        right.amplitude;

    backend->left_frequency =
        left.frequency;

    backend->right_frequency =
        right.frequency;

    /*
     * Production Quest implementation:
     *
     * 1. Convert the material-generated
     *    parametric signal to OpenXR data.
     *
     * 2. Submit through:
     *
     *      xrApplyHapticFeedback()
     *
     * 3. For supported VCM controllers,
     *    transmit amplitude/frequency data
     *    through the parametric haptics path.
     *
     * 4. For older hardware, gracefully
     *    degrade to amplitude-only output.
     */

    printf(
        "\r"
        "LEFT  A=%0.2f F=%6.1f Hz   "
        "RIGHT A=%0.2f F=%6.1f Hz",
        left.amplitude,
        left.frequency,
        right.amplitude,
        right.frequency
    );

    fflush(stdout);
}


/* ============================================================
 * FULL REAL-TIME LOOP EXAMPLE
 * ============================================================ */

static void demo_realtime(
    HME_Engine *engine,
    HME_QuestBackend *backend
)
{
    printf(
        "\n\n"
        "=== REAL-TIME MATERIAL SIMULATION ===\n"
    );

    int contact =
        hme_create_contact(
            engine,
            HME_MAT_STEEL,
            HME_MAT_STEEL,
            1
        );

    const float dt =
        1.0f /
        HME_SAMPLE_RATE;

    for (
        int frame = 0;
        frame < 2000;
        ++frame
    )
    {
        float t =
            frame * dt;

        /*
         * Simulated controller movement.
         */

        float speed =
            1.5f +
            0.8f *
            sinf(
                t * 2.0f *
                (float)M_PI
            );

        /*
         * Simulated pressure.
         */

        float pressure =
            0.50f +
            0.20f *
            sinf(
                t * 1.3f
            );

        hme_update_contact(
            engine,
            contact,
            HME_CONTACT_SLIDE,
            pressure * 12.0f,
            speed,
            0.0f,
            pressure,
            0.005f,
            0.0f
        );

        HME_Sample left =
            hme_update(
                engine,
                dt
            );

        HME_Sample right =
            left;

        /*
         * Submit to Quest abstraction.
         */

        hme_quest_submit(
            backend,
            left,
            right
        );
    }

    printf(
        "\n\n"
    );

    engine->contacts[contact]
        .active = false;
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        "       HAPTIC MATERIAL ENGINE\n"
        "       C11 / META QUEST\n"
        "============================================\n"
    );

    HME_Engine engine;

    hme_init(
        &engine
    );

    HME_QuestBackend backend;

    hme_quest_init(
        &backend
    );

    /*
     * Inspect materials.
     */

    hme_print_material(
        HME_MAT_STEEL
    );

    hme_print_material(
        HME_MAT_WOOD
    );

    hme_print_material(
        HME_MAT_SANDPAPER
    );

    /*
     * Test combined materials.
     */

    hme_compare_materials(
        HME_MAT_STEEL,
        HME_MAT_GLASS
    );

    /*
     * Procedural tests.
     */

    demo_impact(
        &engine
    );

    demo_steel_slide(
        &engine
    );

    demo_sandpaper(
        &engine
    );

    demo_wood(
        &engine
    );

    demo_stick_slip(
        &engine
    );

    /*
     * Full realtime demonstration.
     */

    demo_realtime(
        &engine,
        &backend
    );

    printf(
        "Material engine complete.\n"
    );

    return 0;
}




/*
 * haptic_audio_engine.c
 *
 * ============================================================
 * HAPTIC AUDIO -> VIBRATION ENGINE
 * ============================================================
 *
 * Converts real-time audio into procedural haptic feedback.
 *
 * AUDIO
 *   |
 *   +--> RMS / envelope
 *   |
 *   +--> FFT / spectrum
 *   |
 *   +--> Bass energy
 *   |
 *   +--> Mid energy
 *   |
 *   +--> High-frequency energy
 *   |
 *   +--> Spectral centroid
 *   |
 *   +--> Transient detector
 *   |
 *   v
 * HAPTIC MAPPER
 *   |
 *   +--> amplitude
 *   +--> frequency
 *   +--> transient
 *   +--> rumble
 *   +--> texture
 *   |
 *   v
 * META QUEST / OPENXR
 *
 * C11
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_audio_engine.c -lm \
 *      -o haptic_audio_engine
 *
 * This is a hardware-independent implementation.
 *
 * The Quest backend can translate the resulting signal into
 * OpenXR haptic commands.
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* ============================================================
 * CONFIGURATION
 * ============================================================ */

#define HAE_SAMPLE_RATE       48000.0f

#define HAE_FFT_SIZE          1024
#define HAE_HALF_FFT          (HAE_FFT_SIZE / 2)

#define HAE_AUDIO_BLOCK       256

#define HAE_MIN_HAPTIC_FREQ   20.0f
#define HAE_MAX_HAPTIC_FREQ   300.0f

#define HAE_MAX_OUTPUT        1.0f


/* ============================================================
 * BASIC MATH
 * ============================================================ */

static float clampf(
    float x,
    float lo,
    float hi
)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


static float lerpf(
    float a,
    float b,
    float t
)
{
    return a + (b - a) * t;
}


static float smoothstep(
    float a,
    float b,
    float x
)
{
    float t =
        clampf(
            (x - a) /
            (b - a),
            0.0f,
            1.0f
        );

    return
        t * t *
        (3.0f - 2.0f * t);
}


/* ============================================================
 * COMPLEX NUMBER
 * ============================================================ */

typedef struct {

    float real;
    float imag;

} HAE_Complex;


/* ============================================================
 * FFT
 * ============================================================ */

static void hae_fft(
    HAE_Complex *x,
    int n
)
{
    /*
     * Bit reversal.
     */

    int j = 0;

    for (
        int i = 1;
        i < n;
        ++i
    )
    {
        int bit =
            n >> 1;

        while (
            j & bit
        )
        {
            j ^= bit;
            bit >>= 1;
        }

        j ^= bit;

        if (i < j)
        {
            HAE_Complex tmp =
                x[i];

            x[i] =
                x[j];

            x[j] =
                tmp;
        }
    }

    /*
     * Cooley-Tukey FFT.
     */

    for (
        int length = 2;
        length <= n;
        length <<= 1
    )
    {
        float angle =
            -2.0f *
            (float)M_PI /
            (float)length;

        HAE_Complex wlen;

        wlen.real =
            cosf(angle);

        wlen.imag =
            sinf(angle);

        for (
            int i = 0;
            i < n;
            i += length
        )
        {
            HAE_Complex w;

            w.real = 1.0f;
            w.imag = 0.0f;

            for (
                int j = 0;
                j < length / 2;
                ++j
            )
            {
                HAE_Complex u =
                    x[i + j];

                HAE_Complex v;

                v.real =
                    x[
                        i +
                        j +
                        length / 2
                    ].real *
                    w.real -
                    x[
                        i +
                        j +
                        length / 2
                    ].imag *
                    w.imag;

                v.imag =
                    x[
                        i +
                        j +
                        length / 2
                    ].real *
                    w.imag +
                    x[
                        i +
                        j +
                        length / 2
                    ].imag *
                    w.real;

                x[i + j].real =
                    u.real + v.real;

                x[i + j].imag =
                    u.imag + v.imag;

                x[
                    i +
                    j +
                    length / 2
                ].real =
                    u.real - v.real;

                x[
                    i +
                    j +
                    length / 2
                ].imag =
                    u.imag - v.imag;

                float next_real =
                    w.real *
                    wlen.real -
                    w.imag *
                    wlen.imag;

                float next_imag =
                    w.real *
                    wlen.imag +
                    w.imag *
                    wlen.real;

                w.real =
                    next_real;

                w.imag =
                    next_imag;
            }
        }
    }
}


/* ============================================================
 * HANN WINDOW
 * ============================================================ */

static float hae_hann(
    int i,
    int n
)
{
    return
        0.5f *
        (
            1.0f -
            cosf(
                2.0f *
                (float)M_PI *
                (float)i /
                (float)(n - 1)
            )
        );
}


/* ============================================================
 * AUDIO FEATURES
 * ============================================================ */

typedef struct {

    float rms;

    float peak;

    float bass;

    float low_mid;

    float mid;

    float high_mid;

    float high;

    float spectral_centroid;

    float spectral_flux;

    float transient;

    float zero_crossing_rate;

} HAE_AudioFeatures;


/* ============================================================
 * ENGINE STATE
 * ============================================================ */

typedef struct {

    float samples[
        HAE_FFT_SIZE
    ];

    HAE_Complex fft[
        HAE_FFT_SIZE
    ];

    float spectrum[
        HAE_HALF_FFT
    ];

    float previous_spectrum[
        HAE_HALF_FFT
    ];

    HAE_AudioFeatures features;

    float envelope;

    float previous_rms;

    float transient_memory;

    uint32_t frame;

} HAE_Engine;


/* ============================================================
 * ENGINE INITIALISATION
 * ============================================================ */

static void hae_init(
    HAE_Engine *e
)
{
    memset(
        e,
        0,
        sizeof(*e)
    );
}


/* ============================================================
 * LOAD AUDIO BLOCK
 * ============================================================ */

static void hae_push_audio(
    HAE_Engine *e,
    const float *input,
    int count
)
{
    if (count <= 0)
        return;

    if (count > HAE_FFT_SIZE)
        count = HAE_FFT_SIZE;

    /*
     * Shift existing samples.
     */

    memmove(
        e->samples,
        e->samples + count,
        sizeof(float) *
        (HAE_FFT_SIZE - count)
    );

    memcpy(
        e->samples +
        HAE_FFT_SIZE -
        count,
        input,
        sizeof(float) *
        count
    );
}


/* ============================================================
 * RMS
 * ============================================================ */

static float hae_rms(
    const float *x,
    int n
)
{
    double sum = 0.0;

    for (
        int i = 0;
        i < n;
        ++i
    )
    {
        sum +=
            (double)x[i] *
            (double)x[i];
    }

    return
        sqrtf(
            (float)(
                sum /
                (double)n
            )
        );
}


/* ============================================================
 * PEAK
 * ============================================================ */

static float hae_peak(
    const float *x,
    int n
)
{
    float peak = 0.0f;

    for (
        int i = 0;
        i < n;
        ++i
    )
    {
        float a =
            fabsf(x[i]);

        if (a > peak)
            peak = a;
    }

    return peak;
}


/* ============================================================
 * ZERO CROSSING RATE
 * ============================================================ */

static float hae_zcr(
    const float *x,
    int n
)
{
    int crossings = 0;

    for (
        int i = 1;
        i < n;
        ++i
    )
    {
        if (
            (x[i - 1] < 0.0f &&
             x[i] >= 0.0f)
            ||
            (x[i - 1] >= 0.0f &&
             x[i] < 0.0f)
        )
        {
            crossings++;
        }
    }

    return
        (float)crossings /
        (float)(n - 1);
}


/* ============================================================
 * SPECTRUM
 * ============================================================ */

static void hae_analyse_spectrum(
    HAE_Engine *e
)
{
    for (
        int i = 0;
        i < HAE_FFT_SIZE;
        ++i
    )
    {
        e->fft[i].real =
            e->samples[i] *
            hae_hann(
                i,
                HAE_FFT_SIZE
            );

        e->fft[i].imag =
            0.0f;
    }

    hae_fft(
        e->fft,
        HAE_FFT_SIZE
    );

    for (
        int i = 0;
        i < HAE_HALF_FFT;
        ++i
    )
    {
        float real =
            e->fft[i].real;

        float imag =
            e->fft[i].imag;

        float magnitude =
            sqrtf(
                real * real +
                imag * imag
            );

        magnitude /=
            (float)HAE_FFT_SIZE;

        e->spectrum[i] =
            magnitude;
    }
}


/* ============================================================
 * BAND ENERGY
 * ============================================================ */

static float hae_band_energy(
    const HAE_Engine *e,
    float low,
    float high
)
{
    int start =
        (int)(
            low *
            HAE_FFT_SIZE /
            HAE_SAMPLE_RATE
        );

    int end =
        (int)(
            high *
            HAE_FFT_SIZE /
            HAE_SAMPLE_RATE
        );

    start =
        start < 0
        ? 0
        : start;

    end =
        end >= HAE_HALF_FFT
        ? HAE_HALF_FFT - 1
        : end;

    float sum = 0.0f;

    int count = 0;

    for (
        int i = start;
        i <= end;
        ++i
    )
    {
        sum +=
            e->spectrum[i];

        count++;
    }

    if (count == 0)
        return 0.0f;

    return
        sum /
        (float)count;
}


/* ============================================================
 * SPECTRAL CENTROID
 * ============================================================ */

static float hae_centroid(
    const HAE_Engine *e
)
{
    double weighted = 0.0;
    double total = 0.0;

    for (
        int i = 1;
        i < HAE_HALF_FFT;
        ++i
    )
    {
        float magnitude =
            e->spectrum[i];

        float frequency =
            (float)i *
            HAE_SAMPLE_RATE /
            (float)HAE_FFT_SIZE;

        weighted +=
            frequency *
            magnitude;

        total +=
            magnitude;
    }

    if (total < 0.000001)
        return 0.0f;

    return
        (float)(
            weighted /
            total
        );
}


/* ============================================================
 * SPECTRAL FLUX
 * ============================================================ */

static float hae_flux(
    HAE_Engine *e
)
{
    float flux = 0.0f;

    for (
        int i = 0;
        i < HAE_HALF_FFT;
        ++i
    )
    {
        float current =
            e->spectrum[i];

        float previous =
            e->previous_spectrum[i];

        float difference =
            current -
            previous;

        if (difference > 0.0f)
            flux += difference;
    }

    return flux;
}


/* ============================================================
 * FEATURE EXTRACTION
 * ============================================================ */

static void hae_extract_features(
    HAE_Engine *e
)
{
    HAE_AudioFeatures *f =
        &e->features;

    f->rms =
        hae_rms(
            e->samples,
            HAE_FFT_SIZE
        );

    f->peak =
        hae_peak(
            e->samples,
            HAE_FFT_SIZE
        );

    f->zero_crossing_rate =
        hae_zcr(
            e->samples,
            HAE_FFT_SIZE
        );

    f->bass =
        hae_band_energy(
            e,
            20.0f,
            160.0f
        );

    f->low_mid =
        hae_band_energy(
            e,
            160.0f,
            400.0f
        );

    f->mid =
        hae_band_energy(
            e,
            400.0f,
            2000.0f
        );

    f->high_mid =
        hae_band_energy(
            e,
            2000.0f,
            6000.0f
        );

    f->high =
        hae_band_energy(
            e,
            6000.0f,
            16000.0f
        );

    f->spectral_centroid =
        hae_centroid(e);

    f->spectral_flux =
        hae_flux(e);

    /*
     * RMS onset detector.
     */

    float rms_delta =
        f->rms -
        e->previous_rms;

    float flux =
        f->spectral_flux;

    float onset =
        clampf(
            rms_delta * 15.0f +
            flux * 4.0f,
            0.0f,
            1.0f
        );

    /*
     * Smooth transient memory.
     */

    e->transient_memory =
        fmaxf(
            onset,
            e->transient_memory *
            0.92f
        );

    f->transient =
        e->transient_memory;

    e->previous_rms =
        f->rms;

    memcpy(
        e->previous_spectrum,
        e->spectrum,
        sizeof(e->spectrum)
    );
}


/* ============================================================
 * NORMALISE SPECTRUM
 * ============================================================ */

static float hae_normalise_energy(
    float energy
)
{
    /*
     * Audio magnitude is generally much lower
     * than 1.0 after FFT normalisation.
     */

    return
        clampf(
            energy * 12.0f,
            0.0f,
            1.0f
        );
}


/* ============================================================
 * HAPTIC OUTPUT
 * ============================================================ */

typedef struct {

    float amplitude;

    float frequency;

    float transient;

    float rumble;

    float texture;

    float left;

    float right;

} HAE_HapticSignal;


/* ============================================================
 * AUDIO -> HAPTICS MAPPING
 * ============================================================ */

static HAE_HapticSignal hae_map_haptics(
    const HAE_AudioFeatures *f
)
{
    HAE_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float bass =
        hae_normalise_energy(
            f->bass
        );

    float low_mid =
        hae_normalise_energy(
            f->low_mid
        );

    float mid =
        hae_normalise_energy(
            f->mid
        );

    float high_mid =
        hae_normalise_energy(
            f->high_mid
        );

    float high =
        hae_normalise_energy(
            f->high
        );

    /*
     * --------------------------------------------------------
     * RUMBLE
     * --------------------------------------------------------
     *
     * Bass becomes low-frequency vibration.
     */

    h.rumble =
        clampf(
            bass * 1.35f +
            low_mid * 0.30f,
            0.0f,
            1.0f
        );

    /*
     * --------------------------------------------------------
     * TEXTURE
     * --------------------------------------------------------
     *
     * High-frequency energy produces tactile texture.
     */

    h.texture =
        clampf(
            high_mid * 0.55f +
            high * 0.45f,
            0.0f,
            1.0f
        );

    /*
     * --------------------------------------------------------
     * TRANSIENT
     * --------------------------------------------------------
     */

    h.transient =
        f->transient;

    /*
     * --------------------------------------------------------
     * MASTER AMPLITUDE
     * --------------------------------------------------------
     */

    h.amplitude =
        clampf(
            bass * 0.55f +
            low_mid * 0.20f +
            mid * 0.10f +
            high_mid * 0.10f +
            high * 0.05f,
            0.0f,
            1.0f
        );

    /*
     * --------------------------------------------------------
     * FREQUENCY
     * --------------------------------------------------------
     *
     * Spectral centroid controls tactile pitch.
     */

    float centroid =
        clampf(
            f->spectral_centroid,
            20.0f,
            16000.0f
        );

    float centroid_norm =
        logf(
            centroid / 20.0f
        ) /
        logf(
            16000.0f / 20.0f
        );

    h.frequency =
        lerpf(
            30.0f,
            280.0f,
            centroid_norm
        );

    /*
     * Strong bass should remain tactilely low.
     */

    h.frequency =
        lerpf(
            h.frequency,
            45.0f,
            bass * 0.70f
        );

    /*
     * High-frequency events increase texture
     * rather than simply producing a very high
     * controller vibration frequency.
     */

    h.amplitude =
        clampf(
            h.amplitude +
            h.transient * 0.40f,
            0.0f,
            1.0f
        );

    /*
     * Default centred output.
     */

    h.left =
        h.amplitude;

    h.right =
        h.amplitude;

    return h;
}


/* ============================================================
 * TRANSIENT IMPACT BOOST
 * ============================================================ */

static HAE_HapticSignal hae_add_impact(
    HAE_HapticSignal h
)
{
    if (h.transient <= 0.001f)
        return h;

    float boost =
        h.transient;

    h.amplitude =
        clampf(
            h.amplitude +
            boost * 0.45f,
            0.0f,
            1.0f
        );

    h.frequency =
        lerpf(
            h.frequency,
            180.0f,
            boost * 0.55f
        );

    return h;
}


/* ============================================================
 * HAPTIC DYNAMIC RANGE
 * ============================================================ */

typedef struct {

    float attack;

    float release;

    float envelope;

} HAE_Dynamics;


static void hae_dynamics_init(
    HAE_Dynamics *d
)
{
    d->attack =
        0.35f;

    d->release =
        0.08f;

    d->envelope =
        0.0f;
}


static float hae_apply_dynamics(
    HAE_Dynamics *d,
    float input
)
{
    float coefficient;

    if (
        input >
        d->envelope
    )
    {
        coefficient =
            d->attack;
    }
    else
    {
        coefficient =
            d->release;
    }

    d->envelope +=
        (input -
         d->envelope) *
        coefficient;

    return
        d->envelope;
}


/* ============================================================
 * HAPTIC LIMITER
 * ============================================================ */

static float hae_limit(
    float x
)
{
    /*
     * Soft-knee saturation.
     */

    if (x <= 0.75f)
        return x;

    float excess =
        x - 0.75f;

    return
        0.75f +
        tanhf(
            excess * 3.0f
        ) *
        0.25f;
}


/* ============================================================
 * STEREO / HAND DISTRIBUTION
 * ============================================================ */

static void hae_distribute(
    HAE_HapticSignal *h,
    float pan
)
{
    /*
     * pan:
     *
     * -1 = left
     *  0 = centre
     * +1 = right
     */

    pan =
        clampf(
            pan,
            -1.0f,
            1.0f
        );

    float left_gain =
        0.5f *
        (1.0f - pan);

    float right_gain =
        0.5f *
        (1.0f + pan);

    h->left =
        h->amplitude *
        left_gain *
        2.0f;

    h->right =
        h->amplitude *
        right_gain *
        2.0f;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct {

    bool connected;

    float left_amplitude;

    float right_amplitude;

    float left_frequency;

    float right_frequency;

} HAE_QuestBackend;


static void hae_quest_init(
    HAE_QuestBackend *q
)
{
    memset(
        q,
        0,
        sizeof(*q)
    );

    q->connected =
        true;
}


/* ============================================================
 * SUBMIT TO QUEST
 * ============================================================ */

static void hae_quest_submit(
    HAE_QuestBackend *q,
    const HAE_HapticSignal *h
)
{
    if (!q->connected)
        return;

    q->left_amplitude =
        clampf(
            h->left,
            0.0f,
            1.0f
        );

    q->right_amplitude =
        clampf(
            h->right,
            0.0f,
            1.0f
        );

    q->left_frequency =
        clampf(
            h->frequency,
            HAE_MIN_HAPTIC_FREQ,
            HAE_MAX_HAPTIC_FREQ
        );

    q->right_frequency =
        q->left_frequency;

    /*
     * Production Quest layer:
     *
     *      HAE_HapticSignal
     *              |
     *              v
     *      Quest haptic backend
     *              |
     *              v
     *      xrApplyHapticFeedback()
     *
     * For supported Quest hardware the
     * frequency/amplitude representation can
     * be translated into the appropriate
     * parametric haptic format.
     *
     * An amplitude-only fallback can discard
     * frequency while retaining the envelope.
     */

    printf(
        "\r"
        "HAPTIC "
        "A=%0.2f "
        "F=%6.1f Hz "
        "L=%0.2f "
        "R=%0.2f "
        "T=%0.2f",
        h->amplitude,
        h->frequency,
        h->left,
        h->right,
        h->transient
    );

    fflush(stdout);
}


/* ============================================================
 * SYNTHETIC AUDIO
 * ============================================================ */

static void generate_engine_sound(
    float *buffer,
    int count,
    float *phase,
    float time
)
{
    for (
        int i = 0;
        i < count;
        ++i
    )
    {
        float t =
            time +
            (float)i /
            HAE_SAMPLE_RATE;

        /*
         * Engine fundamental.
         */

        float rpm =
            1800.0f +
            500.0f *
            sinf(
                t * 0.5f
            );

        float fundamental =
            rpm / 60.0f;

        /*
         * Harmonics.
         */

        float s =
            0.45f *
            sinf(
                *phase
            );

        s +=
            0.22f *
            sinf(
                *phase * 2.0f
            );

        s +=
            0.12f *
            sinf(
                *phase * 3.0f
            );

        s +=
            0.07f *
            sinf(
                *phase * 6.0f
            );

        /*
         * Mechanical vibration noise.
         */

        s +=
            0.04f *
            sinf(
                2.0f *
                (float)M_PI *
                90.0f *
                t
            );

        buffer[i] =
            s;

        *phase +=
            2.0f *
            (float)M_PI *
            fundamental /
            HAE_SAMPLE_RATE;
    }
}


/* ============================================================
 * SYNTHETIC IMPACT
 * ============================================================ */

static void add_impact(
    float *buffer,
    int count,
    int position
)
{
    if (
        position < 0 ||
        position >= count
    )
        return;

    for (
        int i = position;
        i < count;
        ++i
    )
    {
        float t =
            (float)(i - position) /
            HAE_SAMPLE_RATE;

        float envelope =
            expf(
                -t * 120.0f
            );

        float impact =
            sinf(
                2.0f *
                (float)M_PI *
                120.0f *
                t
            );

        buffer[i] +=
            impact *
            envelope *
            0.9f;
    }
}


/* ============================================================
 * AUDIO CLASS
 * ============================================================ */

typedef enum {

    HAE_AUDIO_UNKNOWN,
    HAE_AUDIO_ENGINE,
    HAE_AUDIO_FOOTSTEP,
    HAE_AUDIO_IMPACT,
    HAE_AUDIO_MUSIC,
    HAE_AUDIO_WEAPON,
    HAE_AUDIO_MACHINE,
    HAE_AUDIO_ENVIRONMENT

} HAE_AudioClass;


/* ============================================================
 * SIMPLE AUDIO CLASSIFIER
 * ============================================================ */

static HAE_AudioClass hae_classify(
    const HAE_AudioFeatures *f
)
{
    if (
        f->transient >
        0.70f
    )
    {
        return
            HAE_AUDIO_IMPACT;
    }

    if (
        f->bass >
        f->high * 2.0f &&
        f->spectral_centroid <
        500.0f
    )
    {
        return
            HAE_AUDIO_ENGINE;
    }

    if (
        f->high >
        f->bass * 2.0f
    )
    {
        return
            HAE_AUDIO_ENVIRONMENT;
    }

    return
        HAE_AUDIO_UNKNOWN;
}


/* ============================================================
 * CLASS-SPECIFIC MAPPING
 * ============================================================ */

static HAE_HapticSignal hae_class_map(
    HAE_HapticSignal h,
    HAE_AudioClass type
)
{
    switch (type)
    {
        case HAE_AUDIO_ENGINE:

            /*
             * Engine = strong continuous rumble.
             */

            h.amplitude *=
                1.20f;

            h.frequency =
                clampf(
                    h.frequency,
                    30.0f,
                    90.0f
                );

            break;


        case HAE_AUDIO_FOOTSTEP:

            /*
             * Footsteps = transient impulse.
             */

            h.amplitude *=
                1.10f;

            h.frequency =
                90.0f;

            break;


        case HAE_AUDIO_IMPACT:

            /*
             * Impacts = short strong pulse.
             */

            h.amplitude =
                clampf(
                    h.amplitude * 1.4f,
                    0.0f,
                    1.0f
                );

            h.frequency =
                150.0f;

            break;


        case HAE_AUDIO_MACHINE:

            h.amplitude *=
                0.90f;

            h.frequency =
                clampf(
                    h.frequency,
                    45.0f,
                    140.0f
                );

            break;


        case HAE_AUDIO_ENVIRONMENT:

            h.amplitude *=
                0.45f;

            break;


        default:

            break;
    }

    h.amplitude =
        clampf(
            h.amplitude,
            0.0f,
            1.0f
        );

    return h;
}


/* ============================================================
 * PROCESS AUDIO FRAME
 * ============================================================ */

static HAE_HapticSignal hae_process(
    HAE_Engine *engine,
    HAE_Dynamics *dynamics,
    const float *audio,
    int count
)
{
    hae_push_audio(
        engine,
        audio,
        count
    );

    hae_analyse_spectrum(
        engine
    );

    hae_extract_features(
        engine
    );

    HAE_HapticSignal h =
        hae_map_haptics(
            &engine->features
        );

    /*
     * Transient enhancement.
     */

    h =
        hae_add_impact(
            h
        );

    /*
     * Envelope smoothing.
     */

    h.amplitude =
        hae_apply_dynamics(
            dynamics,
            h.amplitude
        );

    /*
     * Limit output.
     */

    h.amplitude =
        hae_limit(
            h.amplitude
        );

    /*
     * Recalculate stereo.
     */

    h.left =
        h.amplitude;

    h.right =
        h.amplitude;

    return h;
}


/* ============================================================
 * PRINT AUDIO ANALYSIS
 * ============================================================ */

static void print_features(
    const HAE_AudioFeatures *f
)
{
    printf(
        "\n"
        "Audio Analysis\n"
        "-----------------------------\n"
        "RMS               : %.4f\n"
        "Peak              : %.4f\n"
        "Bass              : %.5f\n"
        "Low Mid           : %.5f\n"
        "Mid               : %.5f\n"
        "High Mid          : %.5f\n"
        "High              : %.5f\n"
        "Centroid          : %.1f Hz\n"
        "Spectral Flux     : %.5f\n"
        "Transient         : %.3f\n"
        "Zero Crossing     : %.4f\n",
        f->rms,
        f->peak,
        f->bass,
        f->low_mid,
        f->mid,
        f->high_mid,
        f->high,
        f->spectral_centroid,
        f->spectral_flux,
        f->transient,
        f->zero_crossing_rate
    );
}


/* ============================================================
 * MAIN DEMO
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        "       HAPTIC AUDIO ENGINE\n"
        "       C11 / META QUEST\n"
        "============================================\n"
    );

    HAE_Engine engine;

    hae_init(
        &engine
    );

    HAE_Dynamics dynamics;

    hae_dynamics_init(
        &dynamics
    );

    HAE_QuestBackend quest;

    hae_quest_init(
        &quest
    );

    float audio[
        HAE_AUDIO_BLOCK
    ];

    float phase =
        0.0f;

    /*
     * Simulated real-time engine audio.
     */

    for (
        int frame = 0;
        frame < 500;
        ++frame
    )
    {
        memset(
            audio,
            0,
            sizeof(audio)
        );

        float time =
            (float)frame *
            (float)HAE_AUDIO_BLOCK /
            HAE_SAMPLE_RATE;

        generate_engine_sound(
            audio,
            HAE_AUDIO_BLOCK,
            &phase,
            time
        );

        /*
         * Add occasional mechanical impacts.
         */

        if (
            frame == 100 ||
            frame == 250 ||
            frame == 400
        )
        {
            add_impact(
                audio,
                HAE_AUDIO_BLOCK,
                20
            );
        }

        HAE_HapticSignal h =
            hae_process(
                &engine,
                &dynamics,
                audio,
                HAE_AUDIO_BLOCK
            );

        HAE_AudioClass type =
            hae_classify(
                &engine.features
            );

        h =
            hae_class_map(
                h,
                type
            );

        /*
         * Slight stereo movement.
         */

        float pan =
            0.30f *
            sinf(
                time * 0.7f
            );

        hae_distribute(
            &h,
            pan
        );

        /*
         * Send to Quest backend.
         */

        hae_quest_submit(
            &quest,
            &h
        );

        engine.frame++;
    }

    printf(
        "\n\n"
    );

    print_features(
        &engine.features
    );

    printf(
        "\nHaptic audio engine complete.\n"
    );

    return 0;
}






/*
 * haptic_physics_engine.c
 *
 * ============================================================
 * HAPTIC PHYSICS ENGINE
 * ============================================================
 *
 * Converts physical interactions directly into haptics.
 *
 * Supported:
 *
 *   - Collision impulses
 *   - Mass
 *   - Velocity
 *   - Acceleration
 *   - Friction
 *   - Static friction
 *   - Kinetic friction
 *   - Surface roughness
 *   - Elasticity
 *   - Deformation
 *   - Impact resonance
 *   - Sliding vibration
 *   - Stick-slip
 *   - Object hardness
 *   - Contact pressure
 *   - Damage / breaking
 *
 * Designed as a hardware-independent C11 engine.
 *
 * Production architecture:
 *
 *      OpenXR / Quest
 *             |
 *        VR hand pose
 *             |
 *             v
 *       Physics Engine
 *             |
 *             v
 *      Contact Resolver
 *             |
 *             v
 *      Haptic Physics
 *             |
 *       ┌─────┼──────┐
 *       v     v      v
 *     Impact Slide  Break
 *       \     |     /
 *        \    |    /
 *         v   v   v
 *        Haptic Signal
 *             |
 *             v
 *       Quest Backend
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_physics_engine.c -lm \
 *      -o haptic_physics_engine
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HPE_MAX_OBJECTS       128
#define HPE_MAX_CONTACTS       64
#define HPE_MAX_HAPTICS        32

#define HPE_DT                 (1.0f / 1000.0f)

#define HPE_MAX_FORCE          100.0f
#define HPE_MAX_VELOCITY        20.0f

#define HPE_MIN_FREQ            20.0f
#define HPE_MAX_FREQ           300.0f


/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct {

    float x;
    float y;
    float z;

} HPE_Vec3;


static HPE_Vec3 hpe_vec3(
    float x,
    float y,
    float z
)
{
    HPE_Vec3 v = {
        x, y, z
    };

    return v;
}


static HPE_Vec3 hpe_add(
    HPE_Vec3 a,
    HPE_Vec3 b
)
{
    return hpe_vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}


static HPE_Vec3 hpe_sub(
    HPE_Vec3 a,
    HPE_Vec3 b
)
{
    return hpe_vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}


static HPE_Vec3 hpe_scale(
    HPE_Vec3 a,
    float s
)
{
    return hpe_vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}


static float hpe_dot(
    HPE_Vec3 a,
    HPE_Vec3 b
)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}


static float hpe_length(
    HPE_Vec3 a
)
{
    return sqrtf(
        hpe_dot(a, a)
    );
}


static HPE_Vec3 hpe_normalize(
    HPE_Vec3 a
)
{
    float l =
        hpe_length(a);

    if (l < 0.000001f)
        return hpe_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    return hpe_scale(
        a,
        1.0f / l
    );
}


static float clampf(
    float x,
    float lo,
    float hi
)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


static float lerpf(
    float a,
    float b,
    float t
)
{
    return
        a +
        (b - a) * t;
}


/* ============================================================
 * MATERIAL
 * ============================================================ */

typedef struct {

    char name[32];

    /*
     * Mechanical properties.
     */

    float mass_density;

    float hardness;

    float elasticity;

    float damping;

    float friction_static;

    float friction_dynamic;

    float roughness;

    float restitution;

    /*
     * Haptic properties.
     */

    float impact_gain;

    float friction_gain;

    float texture_gain;

    float resonance_frequency;

    float resonance_gain;

} HPE_Material;


/* ============================================================
 * MATERIAL LIBRARY
 * ============================================================ */

static HPE_Material g_materials[] = {

    {
        "Steel",

        7850.0f,
        0.95f,
        0.25f,
        0.25f,
        0.60f,
        0.45f,
        0.12f,
        0.30f,

        1.00f,
        0.75f,
        0.55f,
        190.0f,
        0.90f
    },

    {
        "Wood",

        650.0f,
        0.42f,
        0.58f,
        0.70f,
        0.68f,
        0.52f,
        0.50f,
        0.42f,

        0.55f,
        0.70f,
        0.90f,
        82.0f,
        0.35f
    },

    {
        "Rubber",

        1100.0f,
        0.20f,
        0.88f,
        0.92f,
        0.95f,
        0.82f,
        0.30f,
        0.75f,

        0.35f,
        0.50f,
        0.75f,
        55.0f,
        0.15f
    },

    {
        "Glass",

        2500.0f,
        0.92f,
        0.60f,
        0.20f,
        0.38f,
        0.28f,
        0.08f,
        0.20f,

        0.95f,
        0.40f,
        0.30f,
        260.0f,
        0.95f
    },

    {
        "Concrete",

        2400.0f,
        0.90f,
        0.18f,
        0.65f,
        0.88f,
        0.72f,
        0.70f,
        0.15f,

        0.90f,
        0.90f,
        0.95f,
        120.0f,
        0.60f
    },

    {
        "Plastic",

        950.0f,
        0.35f,
        0.55f,
        0.70f,
        0.52f,
        0.40f,
        0.25f,
        0.50f,

        0.50f,
        0.55f,
        0.55f,
        100.0f,
        0.30f
    },

    {
        "Ceramic",

        2400.0f,
        0.90f,
        0.40f,
        0.30f,
        0.52f,
        0.40f,
        0.12f,
        0.25f,

        0.95f,
        0.45f,
        0.45f,
        230.0f,
        0.90f
    },

    {
        "Sandpaper",

        1500.0f,
        0.65f,
        0.10f,
        0.80f,
        1.00f,
        0.92f,
        0.98f,
        0.05f,

        0.30f,
        1.00f,
        1.00f,
        70.0f,
        0.10f
    }
};

#define HPE_MATERIAL_COUNT \
    ((int)(sizeof(g_materials) / sizeof(g_materials[0])))


/* ============================================================
 * RIGID BODY
 * ============================================================ */

typedef struct {

    int id;

    char name[64];

    int material;

    float mass;

    float inverse_mass;

    HPE_Vec3 position;

    HPE_Vec3 velocity;

    HPE_Vec3 acceleration;

    HPE_Vec3 force;

    float radius;

    float restitution;

    bool dynamic;

    bool grounded;

} HPE_RigidBody;


/* ============================================================
 * CONTACT
 * ============================================================ */

typedef struct {

    bool active;

    int object_a;

    int object_b;

    HPE_Vec3 point;

    HPE_Vec3 normal;

    float penetration;

    float normal_velocity;

    float tangential_velocity;

    float normal_force;

    float friction_force;

    float impulse;

    float pressure;

    float contact_area;

    float lifetime;

    float slip_distance;

    bool sticking;

} HPE_Contact;


/* ============================================================
 * HAPTIC SIGNAL
 * ============================================================ */

typedef struct {

    float amplitude;

    float frequency;

    float transient;

    float duration;

    float impact;

    float friction;

    float texture;

    float pressure;

    float left;

    float right;

} HPE_HapticSignal;


/* ============================================================
 * HAPTIC EVENT
 * ============================================================ */

typedef enum {

    HPE_EVENT_NONE,
    HPE_EVENT_IMPACT,
    HPE_EVENT_SLIDE,
    HPE_EVENT_STICK,
    HPE_EVENT_SLIP,
    HPE_EVENT_BREAK,
    HPE_EVENT_PRESSURE

} HPE_HapticEventType;


typedef struct {

    HPE_HapticEventType type;

    int object_a;

    int object_b;

    float magnitude;

    float velocity;

    float force;

    float time;

} HPE_HapticEvent;


/* ============================================================
 * PHYSICS WORLD
 * ============================================================ */

typedef struct {

    HPE_RigidBody objects[
        HPE_MAX_OBJECTS
    ];

    int object_count;

    HPE_Contact contacts[
        HPE_MAX_CONTACTS
    ];

    int contact_count;

    HPE_HapticEvent events[
        HPE_MAX_HAPTICS
    ];

    int event_count;

    float time;

} HPE_World;


/* ============================================================
 * WORLD INITIALISATION
 * ============================================================ */

static void hpe_world_init(
    HPE_World *world
)
{
    memset(
        world,
        0,
        sizeof(*world)
    );
}


/* ============================================================
 * CREATE BODY
 * ============================================================ */

static int hpe_create_body(
    HPE_World *world,
    const char *name,
    int material,
    float mass,
    HPE_Vec3 position,
    float radius,
    bool dynamic
)
{
    if (
        world->object_count >=
        HPE_MAX_OBJECTS
    )
        return -1;

    int id =
        world->object_count;

    HPE_RigidBody *b =
        &world->objects[id];

    memset(
        b,
        0,
        sizeof(*b)
    );

    b->id =
        id;

    snprintf(
        b->name,
        sizeof(b->name),
        "%s",
        name
    );

    b->material =
        material;

    b->mass =
        mass;

    b->inverse_mass =
        mass > 0.0f
        ? 1.0f / mass
        : 0.0f;

    b->position =
        position;

    b->radius =
        radius;

    b->dynamic =
        dynamic;

    b->restitution =
        g_materials[
            material
        ].restitution;

    world->object_count++;

    return id;
}


/* ============================================================
 * APPLY FORCE
 * ============================================================ */

static void hpe_apply_force(
    HPE_RigidBody *body,
    HPE_Vec3 force
)
{
    if (!body->dynamic)
        return;

    body->force =
        hpe_add(
            body->force,
            force
        );
}


/* ============================================================
 * PHYSICS INTEGRATION
 * ============================================================ */

static void hpe_integrate(
    HPE_World *world,
    float dt
)
{
    for (
        int i = 0;
        i < world->object_count;
        ++i
    )
    {
        HPE_RigidBody *b =
            &world->objects[i];

        if (!b->dynamic)
            continue;

        b->acceleration =
            hpe_scale(
                b->force,
                b->inverse_mass
            );

        b->velocity =
            hpe_add(
                b->velocity,
                hpe_scale(
                    b->acceleration,
                    dt
                )
            );

        float speed =
            hpe_length(
                b->velocity
            );

        if (
            speed >
            HPE_MAX_VELOCITY
        )
        {
            b->velocity =
                hpe_scale(
                    hpe_normalize(
                        b->velocity
                    ),
                    HPE_MAX_VELOCITY
                );
        }

        b->position =
            hpe_add(
                b->position,
                hpe_scale(
                    b->velocity,
                    dt
                )
            );

        b->force =
            hpe_vec3(
                0.0f,
                0.0f,
                0.0f
            );
    }
}


/* ============================================================
 * MATERIAL COMBINATION
 * ============================================================ */

static void hpe_material_pair(
    int a,
    int b,
    float *friction,
    float *restitution,
    float *hardness,
    float *damping,
    float *roughness,
    float *resonance
)
{
    HPE_Material A =
        g_materials[a];

    HPE_Material B =
        g_materials[b];

    /*
     * Geometric combination.
     */

    *friction =
        sqrtf(
            A.friction_dynamic *
            B.friction_dynamic
        );

    *restitution =
        sqrtf(
            A.restitution *
            B.restitution
        );

    *hardness =
        sqrtf(
            A.hardness *
            B.hardness
        );

    *damping =
        sqrtf(
            A.damping *
            B.damping
        );

    *roughness =
        sqrtf(
            A.roughness *
            B.roughness
        );

    *resonance =
        sqrtf(
            A.resonance_frequency *
            B.resonance_frequency
        );
}


/* ============================================================
 * CONTACT GENERATION
 * ============================================================ */

static int hpe_create_contact(
    HPE_World *world,
    int a,
    int b,
    HPE_Vec3 point,
    HPE_Vec3 normal,
    float penetration
)
{
    if (
        world->contact_count >=
        HPE_MAX_CONTACTS
    )
        return -1;

    int id =
        world->contact_count;

    HPE_Contact *c =
        &world->contacts[id];

    memset(
        c,
        0,
        sizeof(*c)
    );

    c->active =
        true;

    c->object_a =
        a;

    c->object_b =
        b;

    c->point =
        point;

    c->normal =
        hpe_normalize(
            normal
        );

    c->penetration =
        penetration;

    world->contact_count++;

    return id;
}


/* ============================================================
 * CONTACT VELOCITY
 * ============================================================ */

static void hpe_calculate_contact_velocity(
    HPE_World *world,
    HPE_Contact *c
)
{
    HPE_RigidBody *A =
        &world->objects[
            c->object_a
        ];

    HPE_RigidBody *B =
        &world->objects[
            c->object_b
        ];

    HPE_Vec3 relative =
        hpe_sub(
            A->velocity,
            B->velocity
        );

    c->normal_velocity =
        hpe_dot(
            relative,
            c->normal
        );

    HPE_Vec3 normal_component =
        hpe_scale(
            c->normal,
            c->normal_velocity
        );

    HPE_Vec3 tangent =
        hpe_sub(
            relative,
            normal_component
        );

    c->tangential_velocity =
        hpe_length(
            tangent
        );
}


/* ============================================================
 * IMPULSE CALCULATION
 * ============================================================ */

static float hpe_collision_impulse(
    HPE_World *world,
    HPE_Contact *c
)
{
    HPE_RigidBody *A =
        &world->objects[
            c->object_a
        ];

    HPE_RigidBody *B =
        &world->objects[
            c->object_b
        ];

    if (
        c->normal_velocity >=
        0.0f
    )
        return 0.0f;

    float friction;
    float restitution;
    float hardness;
    float damping;
    float roughness;
    float resonance;

    hpe_material_pair(
        A->material,
        B->material,
        &friction,
        &restitution,
        &hardness,
        &damping,
        &roughness,
        &resonance
    );

    float denominator =
        A->inverse_mass +
        B->inverse_mass;

    if (
        denominator <
        0.000001f
    )
        return 0.0f;

    float impulse =
        -(1.0f + restitution) *
        c->normal_velocity /
        denominator;

    impulse =
        clampf(
            impulse,
            0.0f,
            HPE_MAX_FORCE
        );

    c->impulse =
        impulse;

    /*
     * Approximate force over one timestep.
     */

    c->normal_force =
        impulse /
        HPE_DT;

    c->normal_force =
        clampf(
            c->normal_force,
            0.0f,
            HPE_MAX_FORCE
        );

    return impulse;
}


/* ============================================================
 * FRICTION
 * ============================================================ */

static void hpe_calculate_friction(
    HPE_World *world,
    HPE_Contact *c
)
{
    HPE_RigidBody *A =
        &world->objects[
            c->object_a
        ];

    HPE_RigidBody *B =
        &world->objects[
            c->object_b
        ];

    float friction;
    float restitution;
    float hardness;
    float damping;
    float roughness;
    float resonance;

    hpe_material_pair(
        A->material,
        B->material,
        &friction,
        &restitution,
        &hardness,
        &damping,
        &roughness,
        &resonance
    );

    /*
     * Coulomb friction approximation.
     */

    c->friction_force =
        friction *
        c->normal_force;

    /*
     * Stick/slip threshold.
     */

    float static_threshold =
        friction *
        c->normal_force;

    c->sticking =
        c->tangential_velocity <
        0.05f &&
        static_threshold >
        0.5f;
}


/* ============================================================
 * CONTACT PRESSURE
 * ============================================================ */

static void hpe_calculate_pressure(
    HPE_World *world,
    HPE_Contact *c
)
{
    HPE_RigidBody *A =
        &world->objects[
            c->object_a
        ];

    float radius =
        A->radius;

    float area =
        (float)M_PI *
        radius *
        radius;

    /*
     * Minimum contact area avoids
     * extreme numerical pressure.
     */

    area =
        fmaxf(
            area * 0.15f,
            0.0001f
        );

    c->contact_area =
        area;

    float pressure =
        c->normal_force /
        area;

    c->pressure =
        clampf(
            pressure /
            100000.0f,
            0.0f,
            1.0f
        );
}


/* ============================================================
 * CONTACT RESOLUTION
 * ============================================================ */

static void hpe_resolve_contact(
    HPE_World *world,
    HPE_Contact *c
)
{
    HPE_RigidBody *A =
        &world->objects[
            c->object_a
        ];

    HPE_RigidBody *B =
        &world->objects[
            c->object_b
        ];

    /*
     * Normal impulse.
     */

    float impulse =
        hpe_collision_impulse(
            world,
            c
        );

    if (
        impulse <=
        0.000001f
    )
        return;

    /*
     * Apply impulse.
     */

    HPE_Vec3 J =
        hpe_scale(
            c->normal,
            impulse
        );

    if (A->dynamic)
    {
        A->velocity =
            hpe_add(
                A->velocity,
                hpe_scale(
                    J,
                    A->inverse_mass
                )
            );
    }

    if (B->dynamic)
    {
        B->velocity =
            hpe_sub(
                B->velocity,
                hpe_scale(
                    J,
                    B->inverse_mass
                )
            );
    }

    /*
     * Friction.
     */

    hpe_calculate_friction(
        world,
        c
    );

    /*
     * Pressure.
     */

    hpe_calculate_pressure(
        world,
        c
    );
}


/* ============================================================
 * HAPTIC EVENT QUEUE
 * ============================================================ */

static void hpe_push_event(
    HPE_World *world,
    HPE_HapticEventType type,
    HPE_Contact *c,
    float magnitude
)
{
    if (
        world->event_count >=
        HPE_MAX_HAPTICS
    )
        return;

    HPE_HapticEvent *e =
        &world->events[
            world->event_count++
        ];

    e->type =
        type;

    e->object_a =
        c->object_a;

    e->object_b =
        c->object_b;

    e->magnitude =
        magnitude;

    e->velocity =
        fabsf(
            c->normal_velocity
        );

    e->force =
        c->normal_force;

    e->time =
        world->time;
}


/* ============================================================
 * GENERATE CONTACT EVENTS
 * ============================================================ */

static void hpe_generate_events(
    HPE_World *world
)
{
    for (
        int i = 0;
        i < world->contact_count;
        ++i
    )
    {
        HPE_Contact *c =
            &world->contacts[i];

        if (!c->active)
            continue;

        /*
         * Strong collision.
         */

        if (
            c->impulse >
            0.25f
        )
        {
            hpe_push_event(
                world,
                HPE_EVENT_IMPACT,
                c,
                c->impulse
            );
        }

        /*
         * Sliding.
         */

        if (
            c->tangential_velocity >
            0.10f
        )
        {
            hpe_push_event(
                world,
                HPE_EVENT_SLIDE,
                c,
                c->friction_force
            );
        }

        /*
         * Stick.
         */

        if (c->sticking)
        {
            hpe_push_event(
                world,
                HPE_EVENT_STICK,
                c,
                c->pressure
            );
        }

        /*
         * Pressure.
         */

        if (
            c->pressure >
            0.30f
        )
        {
            hpe_push_event(
                world,
                HPE_EVENT_PRESSURE,
                c,
                c->pressure
            );
        }
    }
}


/* ============================================================
 * IMPACT HAPTIC
 * ============================================================ */

static HPE_HapticSignal hpe_impact_haptic(
    HPE_World *world,
    const HPE_HapticEvent *event
)
{
    HPE_RigidBody *A =
        &world->objects[
            event->object_a
        ];

    HPE_RigidBody *B =
        &world->objects[
            event->object_b
        ];

    HPE_Material *ma =
        &g_materials[
            A->material
        ];

    HPE_Material *mb =
        &g_materials[
            B->material
        ];

    float impact =
        clampf(
            event->magnitude /
            10.0f,
            0.0f,
            1.0f
        );

    float hardness =
        sqrtf(
            ma->hardness *
            mb->hardness
        );

    float resonance =
        sqrtf(
            ma->resonance_frequency *
            mb->resonance_frequency
        );

    float gain =
        sqrtf(
            ma->impact_gain *
            mb->impact_gain
        );

    HPE_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    h.impact =
        impact;

    h.amplitude =
        clampf(
            impact *
            gain *
            (0.60f +
             0.40f * hardness),
            0.0f,
            1.0f
        );

    h.frequency =
        clampf(
            resonance,
            HPE_MIN_FREQ,
            HPE_MAX_FREQ
        );

    h.transient =
        h.amplitude;

    h.duration =
        lerpf(
            0.020f,
            0.120f,
            impact
        );

    h.left =
        h.amplitude;

    h.right =
        h.amplitude;

    return h;
}


/* ============================================================
 * SLIDING HAPTIC
 * ============================================================ */

static HPE_HapticSignal hpe_slide_haptic(
    HPE_World *world,
    const HPE_HapticEvent *event
)
{
    HPE_RigidBody *A =
        &world->objects[
            event->object_a
        ];

    HPE_RigidBody *B =
        &world->objects[
            event->object_b
        ];

    HPE_Material *ma =
        &g_materials[
            A->material
        ];

    HPE_Material *mb =
        &g_materials[
            B->material
        ];

    float friction =
        sqrtf(
            ma->friction_gain *
            mb->friction_gain
        );

    float roughness =
        sqrtf(
            ma->roughness *
            mb->roughness
        );

    float velocity =
        clampf(
            event->velocity /
            4.0f,
            0.0f,
            1.0f
        );

    HPE_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    h.friction =
        velocity;

    h.texture =
        roughness;

    h.amplitude =
        clampf(
            velocity *
            friction *
            (0.35f +
             0.65f * roughness),
            0.0f,
            1.0f
        );

    /*
     * Faster sliding creates higher-frequency
     * tactile texture.
     */

    h.frequency =
        lerpf(
            35.0f,
            240.0f,
            velocity
        );

    h.duration =
        0.020f;

    h.left =
        h.amplitude;

    h.right =
        h.amplitude;

    return h;
}


/* ============================================================
 * PRESSURE HAPTIC
 * ============================================================ */

static HPE_HapticSignal hpe_pressure_haptic(
    HPE_World *world,
    const HPE_HapticEvent *event
)
{
    float pressure =
        clampf(
            event->magnitude,
            0.0f,
            1.0f
        );

    HPE_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    h.pressure =
        pressure;

    h.amplitude =
        pressure *
        0.55f;

    h.frequency =
        lerpf(
            35.0f,
            80.0f,
            pressure
        );

    h.duration =
        0.030f;

    h.left =
        h.amplitude;

    h.right =
        h.amplitude;

    return h;
}


/* ============================================================
 * EVENT -> HAPTIC
 * ============================================================ */

static HPE_HapticSignal hpe_event_to_haptic(
    HPE_World *world,
    const HPE_HapticEvent *event
)
{
    switch (event->type)
    {
        case HPE_EVENT_IMPACT:

            return
                hpe_impact_haptic(
                    world,
                    event
                );

        case HPE_EVENT_SLIDE:

            return
                hpe_slide_haptic(
                    world,
                    event
                );

        case HPE_EVENT_PRESSURE:

            return
                hpe_pressure_haptic(
                    world,
                    event
                );

        default:
        {
            HPE_HapticSignal h;

            memset(
                &h,
                0,
                sizeof(h)
            );

            return h;
        }
    }
}


/* ============================================================
 * HAPTIC MIXER
 * ============================================================ */

static HPE_HapticSignal hpe_mix_haptics(
    HPE_World *world
)
{
    HPE_HapticSignal output;

    memset(
        &output,
        0,
        sizeof(output)
    );

    for (
        int i = 0;
        i < world->event_count;
        ++i
    )
    {
        HPE_HapticSignal h =
            hpe_event_to_haptic(
                world,
                &world->events[i]
            );

        /*
         * Sum energy, not raw amplitudes.
         */

        output.amplitude =
            fminf(
                1.0f,
                sqrtf(
                    output.amplitude *
                    output.amplitude +
                    h.amplitude *
                    h.amplitude
                )
            );

        output.transient =
            fmaxf(
                output.transient,
                h.transient
            );

        output.impact =
            fmaxf(
                output.impact,
                h.impact
            );

        output.friction =
            fmaxf(
                output.friction,
                h.friction
            );

        output.texture =
            fmaxf(
                output.texture,
                h.texture
            );

        output.pressure =
            fmaxf(
                output.pressure,
                h.pressure
            );

        if (
            h.amplitude >
            output.amplitude * 0.5f
        )
        {
            output.frequency =
                h.frequency;
        }

        output.duration =
            fmaxf(
                output.duration,
                h.duration
            );
    }

    output.left =
        output.amplitude;

    output.right =
        output.amplitude;

    return output;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct {

    bool connected;

    float left_amplitude;

    float right_amplitude;

    float left_frequency;

    float right_frequency;

} HPE_QuestBackend;


static void hpe_quest_init(
    HPE_QuestBackend *q
)
{
    memset(
        q,
        0,
        sizeof(*q)
    );

    q->connected =
        true;
}


/* ============================================================
 * SUBMIT HAPTICS
 * ============================================================ */

static void hpe_quest_submit(
    HPE_QuestBackend *q,
    HPE_HapticSignal h
)
{
    if (!q->connected)
        return;

    q->left_amplitude =
        clampf(
            h.left,
            0.0f,
            1.0f
        );

    q->right_amplitude =
        clampf(
            h.right,
            0.0f,
            1.0f
        );

    q->left_frequency =
        clampf(
            h.frequency,
            HPE_MIN_FREQ,
            HPE_MAX_FREQ
        );

    q->right_frequency =
        q->left_frequency;

    /*
     * Production implementation:
     *
     *     HPE_HapticSignal
     *             |
     *             v
     *     XrHapticParametricVibrationEXT
     *             |
     *             v
     *     xrApplyHapticFeedback()
     *
     * On VCM-capable controllers, amplitude and
     * frequency can be represented with parametric
     * haptics.
     *
     * Older hardware can use amplitude-only output.
     */

    printf(
        "\r"
        "PHYSICS HAPTIC "
        "A=%0.2f "
        "F=%6.1f Hz "
        "Impact=%0.2f "
        "Friction=%0.2f "
        "Texture=%0.2f",
        h.amplitude,
        h.frequency,
        h.impact,
        h.friction,
        h.texture
    );

    fflush(stdout);
}


/* ============================================================
 * SIMPLE COLLISION TEST
 * ============================================================ */

static bool hpe_sphere_collision(
    HPE_RigidBody *a,
    HPE_RigidBody *b,
    HPE_Vec3 *normal,
    float *penetration
)
{
    HPE_Vec3 delta =
        hpe_sub(
            a->position,
            b->position
        );

    float distance =
        hpe_length(
            delta
        );

    float radius =
        a->radius +
        b->radius;

    if (
        distance >= radius
    )
    {
        return false;
    }

    if (
        distance <
        0.000001f
    )
    {
        *normal =
            hpe_vec3(
                0.0f,
                1.0f,
                0.0f
            );

        *penetration =
            radius;

        return true;
    }

    *normal =
        hpe_scale(
            delta,
            1.0f / distance
        );

    *penetration =
        radius -
        distance;

    return true;
}


/* ============================================================
 * WORLD COLLISION SCAN
 * ============================================================ */

static void hpe_detect_collisions(
    HPE_World *world
)
{
    /*
     * Clear previous contacts.
     */

    world->contact_count =
        0;

    for (
        int a = 0;
        a < world->object_count;
        ++a
    )
    {
        for (
            int b = a + 1;
            b < world->object_count;
            ++b
        )
        {
            HPE_RigidBody *A =
                &world->objects[a];

            HPE_RigidBody *B =
                &world->objects[b];

            if (
                !A->dynamic &&
                !B->dynamic
            )
                continue;

            HPE_Vec3 normal;

            float penetration;

            if (
                hpe_sphere_collision(
                    A,
                    B,
                    &normal,
                    &penetration
                )
            )
            {
                HPE_Vec3 point =
                    hpe_add(
                        B->position,
                        hpe_scale(
                            normal,
                            B->radius
                        )
                    );

                hpe_create_contact(
                    world,
                    a,
                    b,
                    point,
                    normal,
                    penetration
                );
            }
        }
    }
}


/* ============================================================
 * PHYSICS STEP
 * ============================================================ */

static HPE_HapticSignal hpe_step(
    HPE_World *world,
    float dt
)
{
    world->time += dt;

    /*
     * Integrate bodies.
     */

    hpe_integrate(
        world,
        dt
    );

    /*
     * Detect collisions.
     */

    hpe_detect_collisions(
        world
    );

    /*
     * Calculate contact velocities.
     */

    for (
        int i = 0;
        i < world->contact_count;
        ++i
    )
    {
        hpe_calculate_contact_velocity(
            world,
            &world->contacts[i]
        );
    }

    /*
     * Resolve physics.
     */

    for (
        int i = 0;
        i < world->contact_count;
        ++i
    )
    {
        hpe_resolve_contact(
            world,
            &world->contacts[i]
        );
    }

    /*
     * Generate haptic events.
     */

    world->event_count =
        0;

    hpe_generate_events(
        world
    );

    /*
     * Mix all haptic events.
     */

    return
        hpe_mix_haptics(
            world
        );
}


/* ============================================================
 * OBJECT STATUS
 * ============================================================ */

static void hpe_print_object(
    HPE_World *world,
    int id
)
{
    if (
        id < 0 ||
        id >= world->object_count
    )
        return;

    HPE_RigidBody *b =
        &world->objects[id];

    printf(
        "\n"
        "Object: %s\n"
        "-------------------------\n"
        "Material : %s\n"
        "Mass     : %.2f kg\n"
        "Position : %.2f %.2f %.2f\n"
        "Velocity : %.2f %.2f %.2f\n",
        b->name,
        g_materials[
            b->material
        ].name,
        b->mass,
        b->position.x,
        b->position.y,
        b->position.z,
        b->velocity.x,
        b->velocity.y,
        b->velocity.z
    );
}


/* ============================================================
 * DEMO: STEEL BALL IMPACT
 * ============================================================ */

static void demo_steel_impact(
    HPE_World *world,
    HPE_QuestBackend *quest
)
{
    printf(
        "\n"
        "\n=== STEEL IMPACT ===\n"
    );

    int floor =
        hpe_create_body(
            world,
            "Steel Floor",
            0,
            1000.0f,
            hpe_vec3(
                0.0f,
                0.0f,
                0.0f
            ),
            2.0f,
            false
        );

    int ball =
        hpe_create_body(
            world,
            "Steel Ball",
            0,
            2.0f,
            hpe_vec3(
                0.0f,
                3.0f,
                0.0f
            ),
            0.5f,
            true
        );

    world->objects[ball]
        .velocity =
        hpe_vec3(
            0.0f,
            -8.0f,
            0.0f
        );

    for (
        int i = 0;
        i < 30;
        ++i
    )
    {
        HPE_HapticSignal h =
            hpe_step(
                world,
                HPE_DT
            );

        hpe_quest_submit(
            quest,
            h
        );
    }

    printf(
        "\n"
    );

    hpe_print_object(
        world,
        ball
    );
}


/* ============================================================
 * DEMO: RUBBER SLIDE
 * ============================================================ */

static void demo_rubber_slide(
    HPE_World *world,
    HPE_QuestBackend *quest
)
{
    printf(
        "\n"
        "\n=== RUBBER / STEEL SLIDING ===\n"
    );

    HPE_World local;

    hpe_world_init(
        &local
    );

    int floor =
        hpe_create_body(
            &local,
            "Steel Surface",
            0,
            1000.0f,
            hpe_vec3(
                0.0f,
                0.0f,
                0.0f
            ),
            2.0f,
            false
        );

    (void)floor;

    int object =
        hpe_create_body(
            &local,
            "Rubber Object",
            2,
            1.0f,
            hpe_vec3(
                -1.0f,
                1.0f,
                0.0f
            ),
            0.5f,
            true
        );

    local.objects[object]
        .velocity =
        hpe_vec3(
            3.0f,
            0.0f,
            0.0f
        );

    for (
        int i = 0;
        i < 200;
        ++i
    )
    {
        HPE_HapticSignal h =
            hpe_step(
                &local,
                HPE_DT
            );

        if (
            h.amplitude >
            0.001f
        )
        {
            hpe_quest_submit(
                quest,
                h
            );
        }
    }

    printf(
        "\n"
    );
}


/* ============================================================
 * DEMO: GLASS IMPACT
 * ============================================================ */

static void demo_glass(
    HPE_QuestBackend *quest
)
{
    printf(
        "\n"
        "\n=== GLASS IMPACT ===\n"
    );

    HPE_World world;

    hpe_world_init(
        &world
    );

    int floor =
        hpe_create_body(
            &world,
            "Steel Surface",
            0,
            1000.0f,
            hpe_vec3(
                0.0f,
                0.0f,
                0.0f
            ),
            2.0f,
            false
        );

    (void)floor;

    int glass =
        hpe_create_body(
            &world,
            "Glass Object",
            3,
            0.5f,
            hpe_vec3(
                0.0f,
                3.0f,
                0.0f
            ),
            0.5f,
            true
        );

    world.objects[glass]
        .velocity =
        hpe_vec3(
            0.0f,
            -12.0f,
            0.0f
        );

    for (
        int i = 0;
        i < 40;
        ++i
    )
    {
        HPE_HapticSignal h =
            hpe_step(
                &world,
                HPE_DT
            );

        hpe_quest_submit(
            quest,
            h
        );
    }

    printf(
        "\n"
    );
}


/* ============================================================
 * DEMO: DIFFERENT MATERIALS
 * ============================================================ */

static void demo_material_table(void)
{
    printf(
        "\n"
        "============================================\n"
        "MATERIAL HAPTIC PROFILES\n"
        "============================================\n"
    );

    for (
        int i = 0;
        i < HPE_MATERIAL_COUNT;
        ++i
    )
    {
        HPE_Material *m =
            &g_materials[i];

        printf(
            "%-12s "
            "hard=%0.2f "
            "friction=%0.2f "
            "rough=%0.2f "
            "res=%5.1f Hz\n",
            m->name,
            m->hardness,
            m->friction_dynamic,
            m->roughness,
            m->resonance_frequency
        );
    }
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        "       HAPTIC PHYSICS ENGINE\n"
        "       C11 / META QUEST\n"
        "============================================\n"
    );

    HPE_World world;

    hpe_world_init(
        &world
    );

    HPE_QuestBackend quest;

    hpe_quest_init(
        &quest
    );

    /*
     * Show material library.
     */

    demo_material_table();

    /*
     * Steel collision.
     */

    demo_steel_impact(
        &world,
        &quest
    );

    /*
     * Rubber / steel interaction.
     */

    demo_rubber_slide(
        &world,
        &quest
    );

    /*
     * Glass collision.
     */

    demo_glass(
        &quest
    );

    printf(
        "\n\n"
        "============================================\n"
        "HAPTIC PHYSICS ENGINE COMPLETE\n"
        "============================================\n"
    );

    return 0;
}




/*
 * haptic_hand_tracking.c
 *
 * ============================================================
 * HAPTIC HAND TRACKING ENGINE
 * ============================================================
 *
 * Converts tracked hand/finger motion into haptic signals.
 *
 * Inputs:
 *   - Palm position
 *   - Palm velocity
 *   - Palm acceleration
 *   - Finger positions
 *   - Finger velocities
 *   - Pinch distance
 *   - Grip strength
 *   - Contact state
 *   - Contact pressure
 *   - Impact velocity
 *   - Sliding velocity
 *
 * Outputs:
 *   - Impact haptics
 *   - Contact haptics
 *   - Pinch haptics
 *   - Grip haptics
 *   - Sliding texture
 *   - Finger-specific intensity
 *   - Left/right hand distribution
 *
 * C11
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_hand_tracking.c -lm \
 *      -o haptic_hand_tracking
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HHT_FINGER_COUNT       5
#define HHT_HISTORY_SIZE       8

#define HHT_MIN_FREQUENCY     20.0f
#define HHT_MAX_FREQUENCY    300.0f

#define HHT_MAX_HAND_SPEED     8.0f
#define HHT_MAX_ACCELERATION  80.0f

#define HHT_PINCH_START        0.030f
#define HHT_PINCH_END          0.012f

#define HHT_IMPACT_THRESHOLD   2.0f
#define HHT_CONTACT_THRESHOLD  0.08f


/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} HHT_Vec3;


static HHT_Vec3 hht_vec3(
    float x,
    float y,
    float z
)
{
    HHT_Vec3 v = {x, y, z};
    return v;
}


static HHT_Vec3 hht_add(
    HHT_Vec3 a,
    HHT_Vec3 b
)
{
    return hht_vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}


static HHT_Vec3 hht_sub(
    HHT_Vec3 a,
    HHT_Vec3 b
)
{
    return hht_vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}


static HHT_Vec3 hht_scale(
    HHT_Vec3 a,
    float s
)
{
    return hht_vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}


static float hht_dot(
    HHT_Vec3 a,
    HHT_Vec3 b
)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}


static float hht_length(
    HHT_Vec3 a
)
{
    return sqrtf(
        hht_dot(a, a)
    );
}


static HHT_Vec3 hht_normalize(
    HHT_Vec3 a
)
{
    float length =
        hht_length(a);

    if (length < 0.000001f)
        return hht_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    return hht_scale(
        a,
        1.0f / length
    );
}


static float hht_clamp(
    float x,
    float low,
    float high
)
{
    if (x < low)
        return low;

    if (x > high)
        return high;

    return x;
}


static float hht_saturate(
    float x
)
{
    return hht_clamp(
        x,
        0.0f,
        1.0f
    );
}


/* ============================================================
 * FINGER
 * ============================================================ */

typedef enum
{
    HHT_THUMB,
    HHT_INDEX,
    HHT_MIDDLE,
    HHT_RING,
    HHT_LITTLE

} HHT_Finger;


typedef struct
{
    HHT_Vec3 position;

    HHT_Vec3 velocity;

    HHT_Vec3 acceleration;

    float curl;

    float extension;

    float pressure;

    float contact;

    bool touching;

} HHT_FingerState;


/* ============================================================
 * HAND
 * ============================================================ */

typedef struct
{
    bool tracked;

    HHT_Vec3 palm_position;

    HHT_Vec3 palm_velocity;

    HHT_Vec3 palm_acceleration;

    HHT_Vec3 wrist_position;

    HHT_Vec3 wrist_velocity;

    HHT_FingerState finger[
        HHT_FINGER_COUNT
    ];

    float pinch_distance;

    float pinch_strength;

    float grip_strength;

    float contact_strength;

    float impact_velocity;

    float impact_acceleration;

} HHT_Hand;


/* ============================================================
 * HAND HISTORY
 * ============================================================ */

typedef struct
{
    HHT_Vec3 position[
        HHT_HISTORY_SIZE
    ];

    float time[
        HHT_HISTORY_SIZE
    ];

    int cursor;

    int count;

} HHT_HandHistory;


/* ============================================================
 * HAPTIC SIGNAL
 * ============================================================ */

typedef struct
{
    float amplitude;

    float frequency;

    float duration;

    float transient;

    float impact;

    float contact;

    float pinch;

    float grip;

    float texture;

    float pressure;

    float left;

    float right;

} HHT_HapticSignal;


/* ============================================================
 * HAND SIDE
 * ============================================================ */

typedef enum
{
    HHT_LEFT_HAND,
    HHT_RIGHT_HAND

} HHT_HandSide;


/* ============================================================
 * HAPTIC ENGINE
 * ============================================================ */

typedef struct
{
    HHT_Hand left;

    HHT_Hand right;

    HHT_HandHistory left_history;

    HHT_HandHistory right_history;

    float time;

    float sample_rate;

} HHT_Engine;


/* ============================================================
 * HAND INITIALISATION
 * ============================================================ */

static void hht_hand_init(
    HHT_Hand *hand
)
{
    memset(
        hand,
        0,
        sizeof(*hand)
    );

    for (int i = 0;
         i < HHT_FINGER_COUNT;
         ++i)
    {
        hand->finger[i]
            .extension = 1.0f;
    }
}


/* ============================================================
 * ENGINE INITIALISATION
 * ============================================================ */

static void hht_init(
    HHT_Engine *engine,
    float sample_rate
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    hht_hand_init(
        &engine->left
    );

    hht_hand_init(
        &engine->right
    );

    engine->sample_rate =
        sample_rate;
}


/* ============================================================
 * HISTORY
 * ============================================================ */

static void hht_history_push(
    HHT_HandHistory *history,
    HHT_Vec3 position,
    float time
)
{
    history->position[
        history->cursor
    ] = position;

    history->time[
        history->cursor
    ] = time;

    history->cursor =
        (history->cursor + 1)
        % HHT_HISTORY_SIZE;

    if (
        history->count <
        HHT_HISTORY_SIZE
    )
    {
        history->count++;
    }
}


/* ============================================================
 * VELOCITY ESTIMATION
 * ============================================================ */

static HHT_Vec3 hht_estimate_velocity(
    HHT_HandHistory *history
)
{
    if (history->count < 2)
        return hht_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    int current =
        (history->cursor -
         1 +
         HHT_HISTORY_SIZE)
        % HHT_HISTORY_SIZE;

    int previous =
        (history->cursor -
         2 +
         HHT_HISTORY_SIZE)
        % HHT_HISTORY_SIZE;

    float dt =
        history->time[current] -
        history->time[previous];

    if (dt <= 0.000001f)
        return hht_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    HHT_Vec3 delta =
        hht_sub(
            history->position[current],
            history->position[previous]
        );

    return hht_scale(
        delta,
        1.0f / dt
    );
}


/* ============================================================
 * ACCELERATION ESTIMATION
 * ============================================================ */

static HHT_Vec3 hht_estimate_acceleration(
    HHT_HandHistory *history
)
{
    if (history->count < 3)
        return hht_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    int c =
        (history->cursor -
         1 +
         HHT_HISTORY_SIZE)
        % HHT_HISTORY_SIZE;

    int p =
        (history->cursor -
         2 +
         HHT_HISTORY_SIZE)
        % HHT_HISTORY_SIZE;

    int pp =
        (history->cursor -
         3 +
         HHT_HISTORY_SIZE)
        % HHT_HISTORY_SIZE;

    float dt1 =
        history->time[c] -
        history->time[p];

    float dt2 =
        history->time[p] -
        history->time[pp];

    if (
        dt1 <= 0.000001f ||
        dt2 <= 0.000001f
    )
    {
        return hht_vec3(
            0.0f,
            0.0f,
            0.0f
        );
    }

    HHT_Vec3 v1 =
        hht_scale(
            hht_sub(
                history->position[c],
                history->position[p]
            ),
            1.0f / dt1
        );

    HHT_Vec3 v2 =
        hht_scale(
            hht_sub(
                history->position[p],
                history->position[pp]
            ),
            1.0f / dt2
        );

    return hht_scale(
        hht_sub(v1, v2),
        1.0f / dt1
    );
}


/* ============================================================
 * PINCH
 * ============================================================ */

static float hht_compute_pinch(
    HHT_Hand *hand
)
{
    HHT_Vec3 thumb =
        hand->finger[
            HHT_THUMB
        ].position;

    HHT_Vec3 index =
        hand->finger[
            HHT_INDEX
        ].position;

    float distance =
        hht_length(
            hht_sub(
                thumb,
                index
            )
        );

    hand->pinch_distance =
        distance;

    float strength;

    if (
        distance >=
        HHT_PINCH_START
    )
    {
        strength = 0.0f;
    }
    else if (
        distance <=
        HHT_PINCH_END
    )
    {
        strength = 1.0f;
    }
    else
    {
        strength =
            1.0f -
            (distance -
             HHT_PINCH_END) /
            (HHT_PINCH_START -
             HHT_PINCH_END);
    }

    hand->pinch_strength =
        hht_saturate(
            strength
        );

    return hand->pinch_strength;
}


/* ============================================================
 * GRIP
 * ============================================================ */

static float hht_compute_grip(
    HHT_Hand *hand
)
{
    float total = 0.0f;

    /*
     * Index through little finger.
     */

    for (
        int i = HHT_INDEX;
        i < HHT_FINGER_COUNT;
        ++i
    )
    {
        total +=
            hand->finger[i]
                .curl;
    }

    float grip =
        total /
        4.0f;

    hand->grip_strength =
        hht_saturate(
            grip
        );

    return hand->grip_strength;
}


/* ============================================================
 * CONTACT
 * ============================================================ */

static float hht_compute_contact(
    HHT_Hand *hand
)
{
    float total = 0.0f;

    for (
        int i = 0;
        i < HHT_FINGER_COUNT;
        ++i
    )
    {
        total +=
            hand->finger[i]
                .contact;
    }

    float contact =
        total /
        (float)HHT_FINGER_COUNT;

    hand->contact_strength =
        hht_saturate(
            contact
        );

    return hand->contact_strength;
}


/* ============================================================
 * IMPACT DETECTION
 * ============================================================ */

static float hht_detect_impact(
    HHT_Hand *hand
)
{
    float acceleration =
        hht_length(
            hand->palm_acceleration
        );

    float velocity =
        hht_length(
            hand->palm_velocity
        );

    float impact =
        0.0f;

    if (
        acceleration >
        HHT_IMPACT_THRESHOLD
    )
    {
        impact =
            acceleration /
            HHT_MAX_ACCELERATION;
    }

    /*
     * High velocity combined with
     * sudden acceleration creates
     * stronger tactile impact.
     */

    impact *=
        hht_saturate(
            velocity /
            HHT_MAX_HAND_SPEED
        );

    hand->impact_velocity =
        velocity;

    hand->impact_acceleration =
        acceleration;

    return hht_saturate(
        impact
    );
}


/* ============================================================
 * FINGER VELOCITY
 * ============================================================ */

static float hht_finger_velocity(
    HHT_FingerState *finger
)
{
    return hht_length(
        finger->velocity
    );
}


/* ============================================================
 * FINGER CONTACT MIX
 * ============================================================ */

static float hht_finger_contact_mix(
    HHT_Hand *hand
)
{
    float result = 0.0f;

    for (
        int i = 0;
        i < HHT_FINGER_COUNT;
        ++i
    )
    {
        float contact =
            hht_saturate(
                hand->finger[i]
                    .contact
            );

        float velocity =
            hht_saturate(
                hht_finger_velocity(
                    &hand->finger[i]
                ) / 2.0f
            );

        result +=
            contact *
            (0.25f +
             0.75f * velocity);
    }

    return hht_saturate(
        result /
        HHT_FINGER_COUNT
    );
}


/* ============================================================
 * TEXTURE
 * ============================================================ */

static float hht_texture(
    HHT_Hand *hand
)
{
    float texture =
        0.0f;

    for (
        int i = 0;
        i < HHT_FINGER_COUNT;
        ++i
    )
    {
        float contact =
            hand->finger[i]
                .contact;

        float speed =
            hht_finger_velocity(
                &hand->finger[i]
            );

        /*
         * Sliding contact.
         */

        texture +=
            contact *
            hht_saturate(
                speed / 1.5f
            );
    }

    return hht_saturate(
        texture /
        HHT_FINGER_COUNT
    );
}


/* ============================================================
 * PINCH HAPTIC
 * ============================================================ */

static HHT_HapticSignal hht_pinch_haptic(
    HHT_Hand *hand
)
{
    HHT_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float pinch =
        hand->pinch_strength;

    h.pinch =
        pinch;

    h.amplitude =
        0.15f +
        0.45f * pinch;

    h.frequency =
        55.0f +
        35.0f * pinch;

    h.duration =
        0.025f;

    /*
     * Pinch becomes especially
     * noticeable near closure.
     */

    if (pinch > 0.92f)
    {
        h.transient =
            0.35f;
    }

    return h;
}


/* ============================================================
 * GRIP HAPTIC
 * ============================================================ */

static HHT_HapticSignal hht_grip_haptic(
    HHT_Hand *hand
)
{
    HHT_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float grip =
        hand->grip_strength;

    h.grip =
        grip;

    h.amplitude =
        0.08f +
        0.35f * grip;

    h.frequency =
        40.0f +
        25.0f * grip;

    h.duration =
        0.030f;

    return h;
}


/* ============================================================
 * CONTACT HAPTIC
 * ============================================================ */

static HHT_HapticSignal hht_contact_haptic(
    HHT_Hand *hand
)
{
    HHT_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float contact =
        hand->contact_strength;

    h.contact =
        contact;

    h.amplitude =
        0.10f +
        0.35f * contact;

    h.frequency =
        45.0f +
        45.0f * contact;

    h.duration =
        0.025f;

    return h;
}


/* ============================================================
 * IMPACT HAPTIC
 * ============================================================ */

static HHT_HapticSignal hht_impact_haptic(
    HHT_Hand *hand
)
{
    HHT_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float impact =
        hht_detect_impact(
            hand
        );

    h.impact =
        impact;

    h.transient =
        impact;

    h.amplitude =
        impact;

    /*
     * Short, sharp impact.
     */

    h.frequency =
        90.0f +
        170.0f *
        impact;

    h.duration =
        0.015f +
        0.055f *
        (1.0f - impact);

    return h;
}


/* ============================================================
 * TEXTURE HAPTIC
 * ============================================================ */

static HHT_HapticSignal hht_texture_haptic(
    HHT_Hand *hand
)
{
    HHT_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float texture =
        hht_texture(
            hand
        );

    h.texture =
        texture;

    h.amplitude =
        texture *
        0.45f;

    h.frequency =
        40.0f +
        240.0f *
        texture;

    h.duration =
        0.020f;

    return h;
}


/* ============================================================
 * FINGER-SPECIFIC HAPTICS
 * ============================================================ */

static float hht_finger_weight(
    int finger
)
{
    /*
     * Thumb and index are particularly
     * important for interaction.
     */

    static const float weights[
        HHT_FINGER_COUNT
    ] =
    {
        1.00f, /* thumb  */
        1.00f, /* index  */
        0.80f, /* middle */
        0.55f, /* ring   */
        0.45f  /* little */
    };

    if (
        finger < 0 ||
        finger >= HHT_FINGER_COUNT
    )
        return 0.0f;

    return weights[finger];
}


static float hht_finger_activity(
    HHT_Hand *hand
)
{
    float activity = 0.0f;

    for (
        int i = 0;
        i < HHT_FINGER_COUNT;
        ++i
    )
    {
        float contact =
            hand->finger[i]
                .contact;

        float velocity =
            hht_saturate(
                hht_finger_velocity(
                    &hand->finger[i]
                ) / 3.0f
            );

        activity +=
            contact *
            velocity *
            hht_finger_weight(i);
    }

    return hht_saturate(
        activity /
        3.8f
    );
}


/* ============================================================
 * MIX
 * ============================================================ */

static HHT_HapticSignal hht_mix(
    HHT_HapticSignal a,
    HHT_HapticSignal b
)
{
    HHT_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    h.amplitude =
        hht_clamp(
            sqrtf(
                a.amplitude *
                a.amplitude +
                b.amplitude *
                b.amplitude
            ),
            0.0f,
            1.0f
        );

    h.frequency =
        b.amplitude >
        a.amplitude
        ? b.frequency
        : a.frequency;

    h.duration =
        fmaxf(
            a.duration,
            b.duration
        );

    h.transient =
        fmaxf(
            a.transient,
            b.transient
        );

    h.impact =
        fmaxf(
            a.impact,
            b.impact
        );

    h.contact =
        fmaxf(
            a.contact,
            b.contact
        );

    h.pinch =
        fmaxf(
            a.pinch,
            b.pinch
        );

    h.grip =
        fmaxf(
            a.grip,
            b.grip
        );

    h.texture =
        fmaxf(
            a.texture,
            b.texture
        );

    h.pressure =
        fmaxf(
            a.pressure,
            b.pressure
        );

    return h;
}


/* ============================================================
 * HAND -> HAPTICS
 * ============================================================ */

static HHT_HapticSignal hht_process_hand(
    HHT_Hand *hand
)
{
    HHT_HapticSignal output;

    memset(
        &output,
        0,
        sizeof(output)
    );

    if (!hand->tracked)
        return output;

    /*
     * Calculate interaction states.
     */

    hht_compute_pinch(
        hand
    );

    hht_compute_grip(
        hand
    );

    hht_compute_contact(
        hand
    );

    /*
     * Individual generators.
     */

    HHT_HapticSignal pinch =
        hht_pinch_haptic(
            hand
        );

    HHT_HapticSignal grip =
        hht_grip_haptic(
            hand
        );

    HHT_HapticSignal contact =
        hht_contact_haptic(
            hand
        );

    HHT_HapticSignal impact =
        hht_impact_haptic(
            hand
        );

    HHT_HapticSignal texture =
        hht_texture_haptic(
            hand
        );

    output =
        hht_mix(
            pinch,
            grip
        );

    output =
        hht_mix(
            output,
            contact
        );

    output =
        hht_mix(
            output,
            impact
        );

    output =
        hht_mix(
            output,
            texture
        );

    /*
     * Finger activity adds subtle tactile
     * energy during physical interaction.
     */

    float finger_activity =
        hht_finger_activity(
            hand
        );

    output.amplitude =
        hht_clamp(
            output.amplitude +
            finger_activity * 0.15f,
            0.0f,
            1.0f
        );

    output.left =
        output.amplitude;

    output.right =
        output.amplitude;

    output.frequency =
        hht_clamp(
            output.frequency,
            HHT_MIN_FREQUENCY,
            HHT_MAX_FREQUENCY
        );

    return output;
}


/* ============================================================
 * HAND TRACKING UPDATE
 * ============================================================ */

static void hht_update_hand(
    HHT_Engine *engine,
    HHT_Hand *hand,
    HHT_HandHistory *history,
    HHT_Vec3 palm_position,
    float dt
)
{
    if (dt <= 0.0f)
        return;

    hand->tracked =
        true;

    /*
     * Save position.
     */

    hht_history_push(
        history,
        palm_position,
        engine->time
    );

    hand->palm_position =
        palm_position;

    /*
     * Estimate velocity.
     */

    hand->palm_velocity =
        hht_estimate_velocity(
            history
        );

    /*
     * Estimate acceleration.
     */

    hand->palm_acceleration =
        hht_estimate_acceleration(
            history
        );

    /*
     * Clamp pathological tracking
     * spikes.
     */

    float velocity =
        hht_length(
            hand->palm_velocity
        );

    if (
        velocity >
        HHT_MAX_HAND_SPEED
    )
    {
        hand->palm_velocity =
            hht_scale(
                hht_normalize(
                    hand->palm_velocity
                ),
                HHT_MAX_HAND_SPEED
            );
    }

    float acceleration =
        hht_length(
            hand->palm_acceleration
        );

    if (
        acceleration >
        HHT_MAX_ACCELERATION
    )
    {
        hand->palm_acceleration =
            hht_scale(
                hht_normalize(
                    hand->palm_acceleration
                ),
                HHT_MAX_ACCELERATION
            );
    }
}


/* ============================================================
 * FINGER UPDATE
 * ============================================================ */

static void hht_update_finger(
    HHT_Hand *hand,
    int finger,
    HHT_Vec3 position,
    HHT_Vec3 velocity,
    float curl,
    float contact,
    float pressure
)
{
    if (
        finger < 0 ||
        finger >= HHT_FINGER_COUNT
    )
        return;

    HHT_FingerState *f =
        &hand->finger[finger];

    f->position =
        position;

    f->velocity =
        velocity;

    f->curl =
        hht_saturate(
            curl
        );

    f->extension =
        1.0f -
        f->curl;

    f->contact =
        hht_saturate(
            contact
        );

    f->pressure =
        hht_saturate(
            pressure
        );

    f->touching =
        f->contact >
        HHT_CONTACT_THRESHOLD;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct
{
    bool connected;

    float left_amplitude;
    float right_amplitude;

    float left_frequency;
    float right_frequency;

} HHT_QuestBackend;


static void hht_quest_init(
    HHT_QuestBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected =
        true;
}


/* ============================================================
 * QUEST SUBMISSION
 * ============================================================ */

static void hht_quest_submit(
    HHT_QuestBackend *backend,
    HHT_HapticSignal signal
)
{
    if (!backend->connected)
        return;

    backend->left_amplitude =
        hht_saturate(
            signal.left
        );

    backend->right_amplitude =
        hht_saturate(
            signal.right
        );

    backend->left_frequency =
        hht_clamp(
            signal.frequency,
            HHT_MIN_FREQUENCY,
            HHT_MAX_FREQUENCY
        );

    backend->right_frequency =
        backend->left_frequency;

    /*
     * Production Quest path:
     *
     *     HHT_HapticSignal
     *             |
     *             v
     *     OpenXR haptic structure
     *             |
     *             v
     *     xrApplyHapticFeedback()
     *
     * This backend deliberately remains independent
     * of the OpenXR headers so the physics/interaction
     * layer can be compiled independently.
     */

    printf(
        "\r"
        "HAND HAPTIC "
        "A=%0.2f "
        "F=%6.1fHz "
        "Impact=%0.2f "
        "Pinch=%0.2f "
        "Grip=%0.2f "
        "Texture=%0.2f",
        signal.amplitude,
        signal.frequency,
        signal.impact,
        signal.pinch,
        signal.grip,
        signal.texture
    );

    fflush(stdout);
}


/* ============================================================
 * SYNTHETIC HAND MOTION
 * ============================================================ */

static void hht_generate_demo_hand(
    HHT_Engine *engine,
    float t
)
{
    /*
     * Simulated hand trajectory.
     */

    HHT_Vec3 palm =
        hht_vec3(
            0.20f * sinf(t * 3.0f),
            0.10f * cosf(t * 2.0f),
            0.30f * sinf(t * 1.5f)
        );

    hht_update_hand(
        engine,
        &engine->right,
        &engine->right_history,
        palm,
        1.0f /
        engine->sample_rate
    );

    /*
     * Thumb.
     */

    HHT_Vec3 thumb =
        hht_vec3(
            palm.x + 0.025f,
            palm.y,
            palm.z
        );

    /*
     * Index finger.
     */

    float pinch_cycle =
        0.5f +
        0.5f *
        sinf(t * 2.5f);

    float index_distance =
        0.012f +
        0.030f *
        (1.0f -
         pinch_cycle);

    HHT_Vec3 index =
        hht_vec3(
            palm.x +
            index_distance,
            palm.y,
            palm.z
        );

    hht_update_finger(
        &engine->right,
        HHT_THUMB,
        thumb,
        hht_vec3(
            0.0f,
            0.0f,
            0.0f
        ),
        pinch_cycle,
        pinch_cycle,
        pinch_cycle
    );

    hht_update_finger(
        &engine->right,
        HHT_INDEX,
        index,
        hht_vec3(
            0.0f,
            0.0f,
            0.0f
        ),
        pinch_cycle,
        pinch_cycle,
        pinch_cycle
    );

    /*
     * Other fingers progressively curl.
     */

    for (
        int i = HHT_MIDDLE;
        i < HHT_FINGER_COUNT;
        ++i
    )
    {
        float curl =
            0.25f +
            0.65f *
            pinch_cycle;

        HHT_Vec3 p =
            hht_vec3(
                palm.x,
                palm.y +
                0.02f * i,
                palm.z
            );

        hht_update_finger(
            &engine->right,
            i,
            p,
            hht_vec3(
                0.0f,
                0.0f,
                0.0f
            ),
            curl,
            pinch_cycle * 0.5f,
            pinch_cycle * 0.3f
        );
    }
}


/* ============================================================
 * DEBUG STATE
 * ============================================================ */

static void hht_print_state(
    HHT_Hand *hand
)
{
    printf(
        "\n\n"
        "--------------------------------------------\n"
        "HAND TRACKING STATE\n"
        "--------------------------------------------\n"
        "Tracked          : %s\n"
        "Palm velocity    : %.3f m/s\n"
        "Palm acceleration: %.3f m/s²\n"
        "Pinch            : %.3f\n"
        "Grip             : %.3f\n"
        "Contact          : %.3f\n"
        "Impact velocity  : %.3f m/s\n"
        "--------------------------------------------\n",
        hand->tracked
            ? "YES"
            : "NO",

        hht_length(
            hand->palm_velocity
        ),

        hht_length(
            hand->palm_acceleration
        ),

        hand->pinch_strength,

        hand->grip_strength,

        hand->contact_strength,

        hand->impact_velocity
    );
}


/* ============================================================
 * MAIN DEMO
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        "       HAPTIC HAND TRACKING ENGINE\n"
        "       C11 / META QUEST\n"
        "============================================\n"
    );

    HHT_Engine engine;

    hht_init(
        &engine,
        90.0f
    );

    HHT_QuestBackend quest;

    hht_quest_init(
        &quest
    );

    /*
     * Simulate approximately two seconds
     * of tracked hand movement.
     */

    const int frames =
        180;

    for (
        int frame = 0;
        frame < frames;
        ++frame
    )
    {
        float dt =
            1.0f /
            engine.sample_rate;

        engine.time += dt;

        /*
         * Generate synthetic tracking.
         *
         * Replace this with actual Quest
         * hand-tracking data.
         */

        hht_generate_demo_hand(
            &engine,
            engine.time
        );

        /*
         * Generate haptics.
         */

        HHT_HapticSignal signal =
            hht_process_hand(
                &engine.right
            );

        /*
         * Send to Quest backend.
         */

        hht_quest_submit(
            &quest,
            signal
        );
    }

    printf(
        "\n"
    );

    hht_print_state(
        &engine.right
    );

    printf(
        "\n"
        "============================================\n"
        "HAND TRACKING ENGINE COMPLETE\n"
        "============================================\n"
    );

    return 0;
}




/*
 * haptic_gesture_engine.c
 *
 * ============================================================
 * HAPTIC GESTURE RECOGNITION ENGINE
 * ============================================================
 *
 * Recognises hand gestures and converts them into haptic events.
 *
 * Gestures:
 *
 *   - PINCH
 *   - PINCH_RELEASE
 *   - GRAB
 *   - RELEASE
 *   - POINT
 *   - TAP
 *   - DOUBLE_TAP
 *   - FLICK
 *   - SWIPE
 *   - OPEN_HAND
 *   - FIST
 *
 * Each recognised gesture generates a tactile signature.
 *
 * C11
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_gesture_engine.c -lm \
 *      -o haptic_gesture_engine
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HGE_FINGER_COUNT       5

#define HGE_THUMB             0
#define HGE_INDEX             1
#define HGE_MIDDLE            2
#define HGE_RING              3
#define HGE_LITTLE            4

#define HGE_PINCH_ON           0.85f
#define HGE_PINCH_OFF          0.35f

#define HGE_GRAB_ON            0.75f
#define HGE_GRAB_OFF           0.35f

#define HGE_POINT_THRESHOLD     0.75f

#define HGE_TAP_SPEED          1.2f
#define HGE_FLICK_SPEED        2.0f
#define HGE_SWIPE_SPEED        0.8f

#define HGE_DOUBLE_TAP_TIME    0.35f

#define HGE_HAPTIC_MIN_FREQ   20.0f
#define HGE_HAPTIC_MAX_FREQ  300.0f


/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} HGE_Vec3;


static HGE_Vec3 hge_vec3(
    float x,
    float y,
    float z
)
{
    HGE_Vec3 v = {x, y, z};
    return v;
}


static HGE_Vec3 hge_add(
    HGE_Vec3 a,
    HGE_Vec3 b
)
{
    return hge_vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}


static HGE_Vec3 hge_sub(
    HGE_Vec3 a,
    HGE_Vec3 b
)
{
    return hge_vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}


static HGE_Vec3 hge_scale(
    HGE_Vec3 a,
    float s
)
{
    return hge_vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}


static float hge_dot(
    HGE_Vec3 a,
    HGE_Vec3 b
)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}


static float hge_length(
    HGE_Vec3 a
)
{
    return sqrtf(
        hge_dot(a, a)
    );
}


static HGE_Vec3 hge_normalize(
    HGE_Vec3 a
)
{
    float length =
        hge_length(a);

    if (length < 0.000001f)
        return hge_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    return hge_scale(
        a,
        1.0f / length
    );
}


static float hge_clamp(
    float x,
    float lo,
    float hi
)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


static float hge_saturate(
    float x
)
{
    return hge_clamp(
        x,
        0.0f,
        1.0f
    );
}


/* ============================================================
 * FINGER STATE
 * ============================================================ */

typedef struct
{
    HGE_Vec3 position;

    HGE_Vec3 velocity;

    float curl;

    float extension;

    float contact;

} HGE_Finger;


/* ============================================================
 * HAND SAMPLE
 * ============================================================ */

typedef struct
{
    bool tracked;

    HGE_Vec3 palm_position;

    HGE_Vec3 palm_velocity;

    HGE_Vec3 palm_acceleration;

    HGE_Vec3 palm_forward;

    HGE_Finger finger[
        HGE_FINGER_COUNT
    ];

    float pinch_strength;

    float grip_strength;

    float timestamp;

} HGE_Hand;


/* ============================================================
 * GESTURE TYPE
 * ============================================================ */

typedef enum
{
    HGE_GESTURE_NONE,

    HGE_GESTURE_PINCH,

    HGE_GESTURE_PINCH_RELEASE,

    HGE_GESTURE_GRAB,

    HGE_GESTURE_RELEASE,

    HGE_GESTURE_POINT,

    HGE_GESTURE_TAP,

    HGE_GESTURE_DOUBLE_TAP,

    HGE_GESTURE_FLICK,

    HGE_GESTURE_SWIPE,

    HGE_GESTURE_OPEN_HAND,

    HGE_GESTURE_FIST

} HGE_GestureType;


/* ============================================================
 * GESTURE EVENT
 * ============================================================ */

typedef struct
{
    HGE_GestureType type;

    HGE_Vec3 direction;

    float magnitude;

    float velocity;

    float timestamp;

} HGE_GestureEvent;


/* ============================================================
 * HAPTIC SIGNAL
 * ============================================================ */

typedef struct
{
    float amplitude;

    float frequency;

    float duration;

    float transient;

    float left;

    float right;

} HGE_HapticSignal;


/* ============================================================
 * GESTURE STATE
 * ============================================================ */

typedef struct
{
    bool pinch_active;

    bool grab_active;

    bool point_active;

    bool open_hand_active;

    bool fist_active;

    bool previous_tap;

    float last_tap_time;

    float gesture_cooldown;

} HGE_GestureState;


/* ============================================================
 * ENGINE
 * ============================================================ */

typedef struct
{
    HGE_Hand hand;

    HGE_GestureState state;

    HGE_GestureEvent event;

    float time;

} HGE_Engine;


/* ============================================================
 * INITIALISE
 * ============================================================ */

static void hge_init(
    HGE_Engine *engine
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->state.last_tap_time =
        -100.0f;
}


/* ============================================================
 * CALCULATE GRIP
 * ============================================================ */

static float hge_calculate_grip(
    HGE_Hand *hand
)
{
    float grip = 0.0f;

    for (
        int i = HGE_INDEX;
        i < HGE_FINGER_COUNT;
        ++i
    )
    {
        grip +=
            hand->finger[i]
                .curl;
    }

    grip /= 4.0f;

    hand->grip_strength =
        hge_saturate(grip);

    return hand->grip_strength;
}


/* ============================================================
 * CALCULATE PINCH
 * ============================================================ */

static float hge_calculate_pinch(
    HGE_Hand *hand
)
{
    HGE_Vec3 thumb =
        hand->finger[
            HGE_THUMB
        ].position;

    HGE_Vec3 index =
        hand->finger[
            HGE_INDEX
        ].position;

    float distance =
        hge_length(
            hge_sub(
                thumb,
                index
            )
        );

    /*
     * 3 cm -> open
     * 0.8 cm -> closed
     */

    float pinch =
        1.0f -
        hge_saturate(
            (distance - 0.008f) /
            0.022f
        );

    hand->pinch_strength =
        hge_saturate(
            pinch
        );

    return hand->pinch_strength;
}


/* ============================================================
 * DETECT OPEN HAND
 * ============================================================ */

static bool hge_detect_open_hand(
    HGE_Hand *hand
)
{
    float extension = 0.0f;

    for (
        int i = HGE_INDEX;
        i < HGE_FINGER_COUNT;
        ++i
    )
    {
        extension +=
            hand->finger[i]
                .extension;
    }

    extension /= 4.0f;

    return
        extension > 0.75f &&
        hand->grip_strength < 0.30f;
}


/* ============================================================
 * DETECT FIST
 * ============================================================ */

static bool hge_detect_fist(
    HGE_Hand *hand
)
{
    return
        hand->grip_strength >
        0.82f;
}


/* ============================================================
 * DETECT POINT
 * ============================================================ */

static bool hge_detect_point(
    HGE_Hand *hand
)
{
    float index_extension =
        hand->finger[
            HGE_INDEX
        ].extension;

    float other_curl =
        (
            hand->finger[
                HGE_MIDDLE
            ].curl +

            hand->finger[
                HGE_RING
            ].curl +

            hand->finger[
                HGE_LITTLE
            ].curl
        ) / 3.0f;

    return
        index_extension >
        HGE_POINT_THRESHOLD &&

        other_curl >
        0.60f &&

        hand->pinch_strength <
        0.30f;
}


/* ============================================================
 * DIRECTION
 * ============================================================ */

static HGE_Vec3 hge_movement_direction(
    HGE_Hand *hand
)
{
    return hge_normalize(
        hand->palm_velocity
    );
}


/* ============================================================
 * EVENT RESET
 * ============================================================ */

static void hge_clear_event(
    HGE_Engine *engine
)
{
    engine->event.type =
        HGE_GESTURE_NONE;

    engine->event.magnitude =
        0.0f;

    engine->event.velocity =
        0.0f;
}


/* ============================================================
 * EMIT EVENT
 * ============================================================ */

static void hge_emit(
    HGE_Engine *engine,
    HGE_GestureType type,
    float magnitude,
    float velocity
)
{
    engine->event.type =
        type;

    engine->event.magnitude =
        hge_saturate(
            magnitude
        );

    engine->event.velocity =
        velocity;

    engine->event.direction =
        hge_movement_direction(
            &engine->hand
        );

    engine->event.timestamp =
        engine->time;
}


/* ============================================================
 * PINCH DETECTION
 * ============================================================ */

static bool hge_check_pinch(
    HGE_Engine *engine
)
{
    bool pinch =
        engine->hand.pinch_strength >
        HGE_PINCH_ON;

    bool previous =
        engine->state.pinch_active;

    engine->state.pinch_active =
        pinch;

    if (
        pinch &&
        !previous
    )
    {
        hge_emit(
            engine,
            HGE_GESTURE_PINCH,
            engine->hand.pinch_strength,
            0.0f
        );

        return true;
    }

    if (
        !pinch &&
        previous
    )
    {
        hge_emit(
            engine,
            HGE_GESTURE_PINCH_RELEASE,
            0.7f,
            0.0f
        );

        return true;
    }

    return false;
}


/* ============================================================
 * GRAB DETECTION
 * ============================================================ */

static bool hge_check_grab(
    HGE_Engine *engine
)
{
    bool grab =
        engine->hand.grip_strength >
        HGE_GRAB_ON;

    bool previous =
        engine->state.grab_active;

    engine->state.grab_active =
        grab;

    if (
        grab &&
        !previous
    )
    {
        hge_emit(
            engine,
            HGE_GESTURE_GRAB,
            engine->hand.grip_strength,
            0.0f
        );

        return true;
    }

    if (
        !grab &&
        previous &&
        engine->hand.grip_strength <
        HGE_GRAB_OFF
    )
    {
        hge_emit(
            engine,
            HGE_GESTURE_RELEASE,
            0.8f,
            0.0f
        );

        return true;
    }

    return false;
}


/* ============================================================
 * POINT DETECTION
 * ============================================================ */

static bool hge_check_point(
    HGE_Engine *engine
)
{
    bool point =
        hge_detect_point(
            &engine->hand
        );

    bool previous =
        engine->state.point_active;

    engine->state.point_active =
        point;

    if (
        point &&
        !previous
    )
    {
        hge_emit(
            engine,
            HGE_GESTURE_POINT,
            0.4f,
            0.0f
        );

        return true;
    }

    return false;
}


/* ============================================================
 * OPEN HAND / FIST
 * ============================================================ */

static bool hge_check_hand_state(
    HGE_Engine *engine
)
{
    bool open =
        hge_detect_open_hand(
            &engine->hand
        );

    bool fist =
        hge_detect_fist(
            &engine->hand
        );

    if (
        open &&
        !engine->state.open_hand_active
    )
    {
        engine->state.open_hand_active =
            true;

        engine->state.fist_active =
            false;

        hge_emit(
            engine,
            HGE_GESTURE_OPEN_HAND,
            0.35f,
            0.0f
        );

        return true;
    }

    if (
        !open
    )
    {
        engine->state.open_hand_active =
            false;
    }

    if (
        fist &&
        !engine->state.fist_active
    )
    {
        engine->state.fist_active =
            true;

        hge_emit(
            engine,
            HGE_GESTURE_FIST,
            engine->hand.grip_strength,
            0.0f
        );

        return true;
    }

    if (!fist)
    {
        engine->state.fist_active =
            false;
    }

    return false;
}


/* ============================================================
 * TAP DETECTION
 * ============================================================ */

static bool hge_check_tap(
    HGE_Engine *engine
)
{
    float speed =
        hge_length(
            engine->hand.palm_velocity
        );

    float acceleration =
        hge_length(
            engine->hand.palm_acceleration
        );

    /*
     * A tap is characterised by a brief
     * high acceleration followed by
     * contact / deceleration.
     */

    if (
        speed > HGE_TAP_SPEED &&
        acceleration > 8.0f
    )
    {
        float since_last =
            engine->time -
            engine->state.last_tap_time;

        if (
            since_last <
            HGE_DOUBLE_TAP_TIME
        )
        {
            hge_emit(
                engine,
                HGE_GESTURE_DOUBLE_TAP,
                1.0f,
                speed
            );

            engine->state.last_tap_time =
                -100.0f;

            return true;
        }

        hge_emit(
            engine,
            HGE_GESTURE_TAP,
            hge_saturate(
                speed / 4.0f
            ),
            speed
        );

        engine->state.last_tap_time =
            engine->time;

        return true;
    }

    return false;
}


/* ============================================================
 * FLICK DETECTION
 * ============================================================ */

static bool hge_check_flick(
    HGE_Engine *engine
)
{
    float speed =
        hge_length(
            engine->hand.palm_velocity
        );

    if (
        speed <
        HGE_FLICK_SPEED
    )
        return false;

    /*
     * High-speed short gesture.
     */

    float magnitude =
        hge_saturate(
            speed /
            6.0f
        );

    hge_emit(
        engine,
        HGE_GESTURE_FLICK,
        magnitude,
        speed
    );

    return true;
}


/* ============================================================
 * SWIPE DETECTION
 * ============================================================ */

static bool hge_check_swipe(
    HGE_Engine *engine
)
{
    HGE_Vec3 velocity =
        engine->hand.palm_velocity;

    float speed =
        hge_length(
            velocity
        );

    if (
        speed <
        HGE_SWIPE_SPEED
    )
        return false;

    /*
     * Require horizontal dominance.
     */

    float horizontal =
        sqrtf(
            velocity.x *
            velocity.x +

            velocity.z *
            velocity.z
        );

    if (
        horizontal <
        speed * 0.70f
    )
        return false;

    hge_emit(
        engine,
        HGE_GESTURE_SWIPE,
        hge_saturate(
            speed / 4.0f
        ),
        speed
    );

    return true;
}


/* ============================================================
 * GESTURE UPDATE
 * ============================================================ */

static HGE_GestureEvent hge_update(
    HGE_Engine *engine,
    HGE_Hand hand,
    float dt
)
{
    engine->time += dt;

    engine->hand =
        hand;

    hge_clear_event(
        engine
    );

    if (!hand.tracked)
        return engine->event;

    /*
     * Calculate derived states.
     */

    hge_calculate_pinch(
        &engine->hand
    );

    hge_calculate_grip(
        &engine->hand
    );

    /*
     * Gesture priority:
     *
     * 1. pinch transitions
     * 2. grab transitions
     * 3. tap
     * 4. flick
     * 5. swipe
     * 6. point
     * 7. hand state
     */

    if (
        hge_check_pinch(
            engine
        )
    )
        return engine->event;

    if (
        hge_check_grab(
            engine
        )
    )
        return engine->event;

    if (
        hge_check_tap(
            engine
        )
    )
        return engine->event;

    if (
        hge_check_flick(
            engine
        )
    )
        return engine->event;

    if (
        hge_check_swipe(
            engine
        )
    )
        return engine->event;

    if (
        hge_check_point(
            engine
        )
    )
        return engine->event;

    hge_check_hand_state(
        engine
    );

    return engine->event;
}


/* ============================================================
 * GESTURE HAPTIC GENERATOR
 * ============================================================ */

static HGE_HapticSignal hge_gesture_haptic(
    HGE_GestureEvent *event
)
{
    HGE_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    switch (
        event->type
    )
    {
        case HGE_GESTURE_PINCH:

            /*
             * Small crisp confirmation.
             */

            h.amplitude =
                0.42f;

            h.frequency =
                75.0f;

            h.duration =
                0.025f;

            h.transient =
                0.65f;

            break;


        case HGE_GESTURE_PINCH_RELEASE:

            h.amplitude =
                0.22f;

            h.frequency =
                55.0f;

            h.duration =
                0.035f;

            break;


        case HGE_GESTURE_GRAB:

            h.amplitude =
                0.48f;

            h.frequency =
                58.0f;

            h.duration =
                0.050f;

            h.transient =
                0.55f;

            break;


        case HGE_GESTURE_RELEASE:

            h.amplitude =
                0.25f;

            h.frequency =
                45.0f;

            h.duration =
                0.040f;

            break;


        case HGE_GESTURE_POINT:

            h.amplitude =
                0.18f;

            h.frequency =
                90.0f;

            h.duration =
                0.025f;

            break;


        case HGE_GESTURE_TAP:

            h.amplitude =
                0.60f *
                event->magnitude;

            h.frequency =
                130.0f;

            h.duration =
                0.020f;

            h.transient =
                0.90f;

            break;


        case HGE_GESTURE_DOUBLE_TAP:

            /*
             * Stronger two-stage confirmation.
             */

            h.amplitude =
                0.78f;

            h.frequency =
                155.0f;

            h.duration =
                0.045f;

            h.transient =
                1.0f;

            break;


        case HGE_GESTURE_FLICK:

            h.amplitude =
                0.25f +
                0.45f *
                event->magnitude;

            h.frequency =
                110.0f +
                100.0f *
                event->magnitude;

            h.duration =
                0.025f;

            h.transient =
                0.50f;

            break;


        case HGE_GESTURE_SWIPE:

            /*
             * Longer texture.
             */

            h.amplitude =
                0.18f +
                0.25f *
                event->magnitude;

            h.frequency =
                50.0f +
                120.0f *
                event->magnitude;

            h.duration =
                0.060f;

            break;


        case HGE_GESTURE_OPEN_HAND:

            h.amplitude =
                0.15f;

            h.frequency =
                42.0f;

            h.duration =
                0.030f;

            break;


        case HGE_GESTURE_FIST:

            h.amplitude =
                0.38f;

            h.frequency =
                50.0f;

            h.duration =
                0.040f;

            break;


        default:

            break;
    }

    h.amplitude =
        hge_clamp(
            h.amplitude,
            0.0f,
            1.0f
        );

    h.frequency =
        hge_clamp(
            h.frequency,
            HGE_HAPTIC_MIN_FREQ,
            HGE_HAPTIC_MAX_FREQ
        );

    h.left =
        h.amplitude;

    h.right =
        h.amplitude;

    return h;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct
{
    bool connected;

    float amplitude;

    float frequency;

} HGE_QuestBackend;


static void hge_quest_init(
    HGE_QuestBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected =
        true;
}


/* ============================================================
 * QUEST HAPTIC SUBMISSION
 * ============================================================ */

static void hge_quest_submit(
    HGE_QuestBackend *backend,
    HGE_HapticSignal signal,
    HGE_GestureEvent *event
)
{
    if (!backend->connected)
        return;

    backend->amplitude =
        signal.amplitude;

    backend->frequency =
        signal.frequency;

    /*
     * Production:
     *
     *     HGE_HapticSignal
     *             |
     *             v
     *     OpenXR haptic structure
     *             |
     *             v
     *     xrApplyHapticFeedback()
     */

    printf(
        "\r"
        "GESTURE %-16s "
        "A=%0.2f "
        "F=%6.1fHz "
        "V=%0.2f",
        event->type ==
            HGE_GESTURE_PINCH
            ? "PINCH" :

        event->type ==
            HGE_GESTURE_PINCH_RELEASE
            ? "PINCH_RELEASE" :

        event->type ==
            HGE_GESTURE_GRAB
            ? "GRAB" :

        event->type ==
            HGE_GESTURE_RELEASE
            ? "RELEASE" :

        event->type ==
            HGE_GESTURE_POINT
            ? "POINT" :

        event->type ==
            HGE_GESTURE_TAP
            ? "TAP" :

        event->type ==
            HGE_GESTURE_DOUBLE_TAP
            ? "DOUBLE_TAP" :

        event->type ==
            HGE_GESTURE_FLICK
            ? "FLICK" :

        event->type ==
            HGE_GESTURE_SWIPE
            ? "SWIPE" :

        event->type ==
            HGE_GESTURE_OPEN_HAND
            ? "OPEN_HAND" :

        event->type ==
            HGE_GESTURE_FIST
            ? "FIST" :

        "NONE",

        signal.amplitude,

        signal.frequency,

        event->velocity
    );

    fflush(stdout);
}


/* ============================================================
 * DEMO HAND GENERATOR
 * ============================================================ */

static HGE_Hand hge_demo_hand(
    float time
)
{
    HGE_Hand hand;

    memset(
        &hand,
        0,
        sizeof(hand)
    );

    hand.tracked =
        true;

    hand.timestamp =
        time;

    /*
     * Palm motion.
     */

    hand.palm_position =
        hge_vec3(
            0.20f *
                sinf(time * 2.0f),

            0.05f *
                cosf(time * 1.5f),

            0.15f *
                cosf(time * 2.5f)
        );

    hand.palm_velocity =
        hge_vec3(
            0.40f *
                cosf(time * 2.0f),

            -0.075f *
                sinf(time * 1.5f),

            -0.375f *
                sinf(time * 2.5f)
        );

    /*
     * Simulate cyclic pinch.
     */

    float pinch =
        0.5f +
        0.5f *
        sinf(
            time * 2.0f
        );

    hand.pinch_strength =
        pinch;

    /*
     * Thumb.
     */

    hand.finger[
        HGE_THUMB
    ].position =
        hge_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    hand.finger[
        HGE_THUMB
    ].curl =
        pinch;

    hand.finger[
        HGE_THUMB
    ].extension =
        1.0f - pinch;

    /*
     * Index.
     */

    hand.finger[
        HGE_INDEX
    ].position =
        hge_vec3(
            0.012f +
            0.025f *
            (1.0f - pinch),

            0.0f,
            0.0f
        );

    hand.finger[
        HGE_INDEX
    ].curl =
        pinch;

    hand.finger[
        HGE_INDEX
    ].extension =
        1.0f - pinch;

    /*
     * Remaining fingers.
     */

    for (
        int i = HGE_MIDDLE;
        i < HGE_FINGER_COUNT;
        ++i
    )
    {
        hand.finger[i].curl =
            0.15f +
            0.75f *
            pinch;

        hand.finger[i].extension =
            1.0f -
            hand.finger[i].curl;
    }

    return hand;
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "============================================\n"
        "      HAPTIC GESTURE ENGINE\n"
        "      C11 / META QUEST\n"
        "============================================\n"
    );

    HGE_Engine engine;

    hge_init(
        &engine
    );

    HGE_QuestBackend quest;

    hge_quest_init(
        &quest
    );

    /*
     * Simulate 5 seconds at 90 Hz.
     */

    const int frames =
        450;

    const float dt =
        1.0f / 90.0f;

    for (
        int i = 0;
        i < frames;
        ++i
    )
    {
        HGE_Hand hand =
            hge_demo_hand(
                engine.time
            );

        HGE_GestureEvent event =
            hge_update(
                &engine,
                hand,
                dt
            );

        if (
            event.type !=
            HGE_GESTURE_NONE
        )
        {
            HGE_HapticSignal signal =
                hge_gesture_haptic(
                    &event
                );

            hge_quest_submit(
                &quest,
                signal,
                &event
            );
        }
    }

    printf(
        "\n\n"
        "============================================\n"
        "HAPTIC GESTURE ENGINE COMPLETE\n"
        "============================================\n"
    );

    return 0;
}






/*
 * haptic_material_engine.c
 *
 * ============================================================
 * HAPTIC MATERIAL & SURFACE ENGINE
 * ============================================================
 *
 * Generates tactile responses from virtual materials.
 *
 * Materials:
 *   STEEL
 *   ALUMINIUM
 *   GLASS
 *   WOOD
 *   RUBBER
 *   FABRIC
 *   STONE
 *   PLASTIC
 *   ICE
 *   CERAMIC
 *
 * Interaction parameters:
 *   - impact velocity
 *   - contact pressure
 *   - normal force
 *   - friction
 *   - sliding velocity
 *   - surface roughness
 *   - hardness
 *   - elasticity
 *   - mass
 *
 * C11
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_material_engine.c -lm \
 *      -o haptic_material_engine
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HME_MIN_FREQUENCY       20.0f
#define HME_MAX_FREQUENCY      300.0f

#define HME_MAX_AMPLITUDE        1.0f

#define HME_IMPACT_SCALE         6.0f
#define HME_PRESSURE_SCALE       1.0f
#define HME_SLIDE_SCALE          3.0f

#define HME_TEXTURE_SCALE        2.5f


/* ============================================================
 * UTILITIES
 * ============================================================ */

static float hme_clamp(
    float x,
    float lo,
    float hi
)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


static float hme_saturate(
    float x
)
{
    return hme_clamp(
        x,
        0.0f,
        1.0f
    );
}


static float hme_lerp(
    float a,
    float b,
    float t
)
{
    return a +
        (b - a) *
        hme_saturate(t);
}


/* ============================================================
 * MATERIAL TYPES
 * ============================================================ */

typedef enum
{
    HME_MATERIAL_STEEL,
    HME_MATERIAL_ALUMINIUM,
    HME_MATERIAL_GLASS,
    HME_MATERIAL_WOOD,
    HME_MATERIAL_RUBBER,
    HME_MATERIAL_FABRIC,
    HME_MATERIAL_STONE,
    HME_MATERIAL_PLASTIC,
    HME_MATERIAL_ICE,
    HME_MATERIAL_CERAMIC,

    HME_MATERIAL_COUNT

} HME_MaterialType;


/* ============================================================
 * MATERIAL DESCRIPTION
 * ============================================================ */

typedef struct
{
    const char *name;

    /*
     * Physical characteristics.
     */

    float hardness;

    float elasticity;

    float density;

    float friction;

    float roughness;

    float damping;

    /*
     * Haptic characteristics.
     */

    float impact_amplitude;

    float impact_frequency;

    float texture_amplitude;

    float texture_frequency;

    float pressure_amplitude;

    float slide_amplitude;

} HME_Material;


/* ============================================================
 * MATERIAL DATABASE
 * ============================================================ */

static const HME_Material
hme_materials[
    HME_MATERIAL_COUNT
] =
{
    /*
     * STEEL
     */

    {
        "Steel",

        0.98f,
        0.35f,
        0.95f,
        0.65f,
        0.25f,
        0.15f,

        0.95f,
        185.0f,

        0.55f,
        220.0f,

        0.65f,

        0.60f
    },

    /*
     * ALUMINIUM
     */

    {
        "Aluminium",

        0.85f,
        0.45f,
        0.40f,
        0.60f,
        0.28f,
        0.22f,

        0.75f,
        150.0f,

        0.48f,
        195.0f,

        0.55f,

        0.55f
    },

    /*
     * GLASS
     */

    {
        "Glass",

        0.94f,
        0.25f,
        0.35f,
        0.35f,
        0.12f,
        0.10f,

        0.90f,
        240.0f,

        0.30f,
        270.0f,

        0.60f,

        0.35f
    },

    /*
     * WOOD
     */

    {
        "Wood",

        0.55f,
        0.40f,
        0.25f,
        0.60f,
        0.60f,
        0.55f,

        0.55f,
        85.0f,

        0.70f,
        115.0f,

        0.50f,

        0.65f
    },

    /*
     * RUBBER
     */

    {
        "Rubber",

        0.25f,
        0.90f,
        0.45f,
        0.95f,
        0.70f,
        0.85f,

        0.30f,
        45.0f,

        0.60f,
        65.0f,

        0.80f,

        0.85f
    },

    /*
     * FABRIC
     */

    {
        "Fabric",

        0.10f,
        0.75f,
        0.10f,
        0.75f,
        0.85f,
        0.90f,

        0.12f,
        30.0f,

        0.80f,
        75.0f,

        0.35f,

        0.70f
    },

    /*
     * STONE
     */

    {
        "Stone",

        0.95f,
        0.18f,
        0.80f,
        0.80f,
        0.90f,
        0.30f,

        0.90f,
        125.0f,

        0.85f,
        155.0f,

        0.70f,

        0.85f
    },

    /*
     * PLASTIC
     */

    {
        "Plastic",

        0.45f,
        0.55f,
        0.20f,
        0.50f,
        0.40f,
        0.60f,

        0.45f,
        75.0f,

        0.50f,
        110.0f,

        0.45f,

        0.55f
    },

    /*
     * ICE
     */

    {
        "Ice",

        0.75f,
        0.15f,
        0.15f,
        0.08f,
        0.08f,
        0.20f,

        0.72f,
        205.0f,

        0.20f,
        260.0f,

        0.35f,

        0.12f
    },

    /*
     * CERAMIC
     */

    {
        "Ceramic",

        0.90f,
        0.20f,
        0.50f,
        0.55f,
        0.45f,
        0.18f,

        0.88f,
        225.0f,

        0.50f,
        245.0f,

        0.65f,

        0.50f
    }
};


/* ============================================================
 * CONTACT STATE
 * ============================================================ */

typedef struct
{
    bool active;

    float normal_force;

    float pressure;

    float impact_velocity;

    float penetration;

    float sliding_velocity;

    float tangential_velocity;

    float contact_area;

} HME_Contact;


/* ============================================================
 * HAPTIC SIGNAL
 * ============================================================ */

typedef struct
{
    float amplitude;

    float frequency;

    float duration;

    float impact;

    float pressure;

    float friction;

    float texture;

    float resonance;

    float left;

    float right;

} HME_HapticSignal;


/* ============================================================
 * MATERIAL ENGINE
 * ============================================================ */

typedef struct
{
    HME_MaterialType material;

    HME_Contact contact;

    float time;

    float texture_phase;

} HME_Engine;


/* ============================================================
 * INITIALISE
 * ============================================================ */

static void hme_init(
    HME_Engine *engine,
    HME_MaterialType material
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->material =
        material;
}


/* ============================================================
 * GET MATERIAL
 * ============================================================ */

static const HME_Material *
hme_get_material(
    HME_MaterialType material
)
{
    if (
        material < 0 ||
        material >=
        HME_MATERIAL_COUNT
    )
    {
        material =
            HME_MATERIAL_STEEL;
    }

    return
        &hme_materials[
            material
        ];
}


/* ============================================================
 * IMPACT RESPONSE
 * ============================================================ */

static HME_HapticSignal
hme_generate_impact(
    HME_Engine *engine
)
{
    HME_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    const HME_Material *m =
        hme_get_material(
            engine->material
        );

    float velocity =
        hme_saturate(
            engine->contact
                .impact_velocity /
            HME_IMPACT_SCALE
        );

    /*
     * Hard materials produce sharper,
     * stronger transients.
     */

    float impact =
        velocity *
        m->hardness *
        m->impact_amplitude;

    /*
     * Damping reduces the final response.
     */

    impact *=
        1.0f -
        m->damping * 0.35f;

    h.impact =
        hme_saturate(
            impact
        );

    h.amplitude =
        h.impact;

    /*
     * Hardness raises resonant frequency.
     */

    float hardness_frequency =
        m->impact_frequency *
        (
            0.75f +
            0.50f *
            m->hardness
        );

    /*
     * Higher collision velocity produces
     * a slightly higher-frequency transient.
     */

    h.frequency =
        hardness_frequency +
        velocity * 35.0f;

    h.frequency =
        hme_clamp(
            h.frequency,
            HME_MIN_FREQUENCY,
            HME_MAX_FREQUENCY
        );

    h.duration =
        hme_lerp(
            0.080f,
            0.015f,
            m->hardness
        );

    h.resonance =
        m->elasticity *
        h.impact;

    return h;
}


/* ============================================================
 * PRESSURE RESPONSE
 * ============================================================ */

static HME_HapticSignal
hme_generate_pressure(
    HME_Engine *engine
)
{
    HME_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    const HME_Material *m =
        hme_get_material(
            engine->material
        );

    float pressure =
        hme_saturate(
            engine->contact
                .pressure /
            HME_PRESSURE_SCALE
        );

    /*
     * Soft materials deform more under
     * the same pressure.
     */

    float response =
        pressure *
        m->pressure_amplitude *
        (
            0.60f +
            0.40f *
            m->elasticity
        );

    h.pressure =
        hme_saturate(
            response
        );

    h.amplitude =
        h.pressure *
        0.65f;

    /*
     * Pressure response is generally
     * lower frequency than impact.
     */

    h.frequency =
        hme_lerp(
            35.0f,
            105.0f,
            m->hardness
        );

    h.duration =
        0.030f;

    return h;
}


/* ============================================================
 * SLIDING RESPONSE
 * ============================================================ */

static HME_HapticSignal
hme_generate_slide(
    HME_Engine *engine
)
{
    HME_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    const HME_Material *m =
        hme_get_material(
            engine->material
        );

    float velocity =
        hme_saturate(
            engine->contact
                .sliding_velocity /
            HME_SLIDE_SCALE
        );

    /*
     * Friction creates low-frequency
     * tactile force.
     */

    float friction =
        velocity *
        m->friction *
        m->slide_amplitude;

    h.friction =
        hme_saturate(
            friction
        );

    h.amplitude =
        h.friction *
        0.60f;

    /*
     * Surface roughness controls texture
     * amplitude.
     */

    float texture =
        velocity *
        m->roughness *
        m->texture_amplitude;

    h.texture =
        hme_saturate(
            texture
        );

    /*
     * Sliding frequency depends on
     * velocity and texture scale.
     */

    h.frequency =
        m->texture_frequency *
        (
            0.35f +
            velocity *
            0.90f
        );

    h.frequency =
        hme_clamp(
            h.frequency,
            HME_MIN_FREQUENCY,
            HME_MAX_FREQUENCY
        );

    h.duration =
        0.025f;

    return h;
}


/* ============================================================
 * MATERIAL RESONANCE
 * ============================================================ */

static float
hme_material_resonance(
    const HME_Material *m,
    float velocity
)
{
    /*
     * Approximate resonance model.
     *
     * This is not a physical FEM simulation;
     * it is a perceptual haptic model.
     */

    float resonance =
        m->elasticity *
        m->hardness *
        hme_saturate(
            velocity /
            4.0f
        );

    resonance *=
        1.0f -
        m->damping;

    return hme_saturate(
        resonance
    );
}


/* ============================================================
 * MIX SIGNALS
 * ============================================================ */

static HME_HapticSignal
hme_mix(
    HME_HapticSignal a,
    HME_HapticSignal b
)
{
    HME_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    h.amplitude =
        hme_clamp(
            a.amplitude +
            b.amplitude,
            0.0f,
            1.0f
        );

    /*
     * Use the strongest component's
     * frequency.
     */

    h.frequency =
        a.amplitude >
        b.amplitude
        ? a.frequency
        : b.frequency;

    h.duration =
        fmaxf(
            a.duration,
            b.duration
        );

    h.impact =
        fmaxf(
            a.impact,
            b.impact
        );

    h.pressure =
        fmaxf(
            a.pressure,
            b.pressure
        );

    h.friction =
        fmaxf(
            a.friction,
            b.friction
        );

    h.texture =
        fmaxf(
            a.texture,
            b.texture
        );

    h.resonance =
        fmaxf(
            a.resonance,
            b.resonance
        );

    return h;
}


/* ============================================================
 * COMPLETE MATERIAL RESPONSE
 * ============================================================ */

static HME_HapticSignal
hme_process(
    HME_Engine *engine
)
{
    HME_HapticSignal output;

    memset(
        &output,
        0,
        sizeof(output)
    );

    if (
        !engine->contact.active
    )
    {
        return output;
    }

    const HME_Material *m =
        hme_get_material(
            engine->material
        );

    /*
     * Impact.
     */

    HME_HapticSignal impact =
        hme_generate_impact(
            engine
        );

    /*
     * Pressure.
     */

    HME_HapticSignal pressure =
        hme_generate_pressure(
            engine
        );

    /*
     * Sliding.
     */

    HME_HapticSignal slide =
        hme_generate_slide(
            engine
        );

    output =
        hme_mix(
            impact,
            pressure
        );

    output =
        hme_mix(
            output,
            slide
        );

    /*
     * Material resonance.
     */

    output.resonance =
        hme_material_resonance(
            m,
            engine->contact
                .impact_velocity
        );

    /*
     * Resonance contributes to
     * the final tactile amplitude.
     */

    output.amplitude =
        hme_clamp(
            output.amplitude +
            output.resonance * 0.20f,
            0.0f,
            1.0f
        );

    /*
     * Stereo output.
     *
     * The caller can replace this with
     * actual contact localization.
     */

    output.left =
        output.amplitude;

    output.right =
        output.amplitude;

    return output;
}


/* ============================================================
 * CONTACT UPDATE
 * ============================================================ */

static void
hme_set_contact(
    HME_Engine *engine,
    bool active,
    float normal_force,
    float pressure,
    float impact_velocity,
    float sliding_velocity,
    float contact_area
)
{
    engine->contact.active =
        active;

    engine->contact.normal_force =
        fmaxf(
            0.0f,
            normal_force
        );

    engine->contact.pressure =
        fmaxf(
            0.0f,
            pressure
        );

    engine->contact.impact_velocity =
        fmaxf(
            0.0f,
            impact_velocity
        );

    engine->contact.sliding_velocity =
        fmaxf(
            0.0f,
            sliding_velocity
        );

    engine->contact.tangential_velocity =
        engine->contact
            .sliding_velocity;

    engine->contact.contact_area =
        hme_clamp(
            contact_area,
            0.0f,
            1.0f
        );

    /*
     * Penetration approximation.
     */

    engine->contact.penetration =
        engine->contact.normal_force *
        0.001f;
}


/* ============================================================
 * MATERIAL SWITCHING
 * ============================================================ */

static void
hme_set_material(
    HME_Engine *engine,
    HME_MaterialType material
)
{
    if (
        material >= 0 &&
        material <
        HME_MATERIAL_COUNT
    )
    {
        engine->material =
            material;
    }
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct
{
    bool connected;

    float amplitude;

    float frequency;

    float duration;

} HME_QuestBackend;


static void
hme_quest_init(
    HME_QuestBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected =
        true;
}


/* ============================================================
 * QUEST OUTPUT
 * ============================================================ */

static void
hme_quest_submit(
    HME_QuestBackend *backend,
    HME_HapticSignal *signal,
    const HME_Material *material
)
{
    if (
        !backend->connected
    )
        return;

    backend->amplitude =
        signal->amplitude;

    backend->frequency =
        signal->frequency;

    backend->duration =
        signal->duration;

    /*
     * Production Quest implementation:
     *
     *   signal
     *      |
     *      v
     *   OpenXR
     *      |
     *      v
     *   xrApplyHapticFeedback()
     *
     * More advanced implementations can
     * translate the generated signal into
     * the Quest haptic/PCM pipeline.
     */

    printf(
        "%-10s "
        "A=%0.2f "
        "F=%6.1fHz "
        "Impact=%0.2f "
        "Texture=%0.2f "
        "Friction=%0.2f\n",

        material->name,

        signal->amplitude,

        signal->frequency,

        signal->impact,

        signal->texture,

        signal->friction
    );
}


/* ============================================================
 * DEMO MATERIAL TEST
 * ============================================================ */

static void
hme_test_material(
    HME_QuestBackend *quest,
    HME_MaterialType type
)
{
    HME_Engine engine;

    hme_init(
        &engine,
        type
    );

    /*
     * Simulate a collision.
     */

    hme_set_contact(
        &engine,

        true,

        0.80f,     /* normal force    */
        0.65f,     /* pressure        */
        3.20f,     /* impact velocity */
        0.00f,     /* sliding         */
        0.35f      /* contact area    */
    );

    HME_HapticSignal impact =
        hme_process(
            &engine
        );

    hme_quest_submit(
        quest,
        &impact,
        hme_get_material(type)
    );

    /*
     * Now simulate sliding across
     * the surface.
     */

    hme_set_contact(
        &engine,

        true,

        0.45f,

        0.40f,

        0.00f,

        1.80f,

        0.25f
    );

    HME_HapticSignal slide =
        hme_process(
            &engine
        );

    hme_quest_submit(
        quest,
        &slide,
        hme_get_material(type)
    );
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "================================================\n"
        "       HAPTIC MATERIAL ENGINE\n"
        "       C11 / META QUEST\n"
        "================================================\n\n"
    );

    HME_QuestBackend quest;

    hme_quest_init(
        &quest
    );

    /*
     * Test every material.
     */

    for (
        int i = 0;
        i < HME_MATERIAL_COUNT;
        ++i
    )
    {
        hme_test_material(
            &quest,
            (HME_MaterialType)i
        );
    }

    printf(
        "\n"
        "================================================\n"
        "MATERIAL ENGINE COMPLETE\n"
        "================================================\n"
    );

    return 0;
}






/*
 * haptic_spatial_audio.c
 *
 * ============================================================
 * HAPTIC SPATIAL AUDIO ENGINE
 * ============================================================
 *
 * Converts spatial audio events into directional haptic signals.
 *
 * Features:
 *
 *   - 3D sound source positioning
 *   - Listener orientation
 *   - Distance attenuation
 *   - Left/right spatialisation
 *   - Front/back discrimination
 *   - Doppler-like approach intensity
 *   - Directional haptic routing
 *   - Impact / transient detection
 *   - Continuous spatial rumble
 *   - Movement-based haptics
 *   - Multiple simultaneous sound sources
 *   - Haptic mixing
 *   - Quest backend abstraction
 *
 * C11
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_spatial_audio.c -lm \
 *      -o haptic_spatial_audio
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HSA_MAX_SOURCES       32

#define HSA_MAX_DISTANCE      100.0f

#define HSA_MIN_DISTANCE        0.25f

#define HSA_PI                  3.14159265358979323846f

#define HSA_MAX_AMPLITUDE       1.0f

#define HSA_MIN_FREQUENCY      20.0f
#define HSA_MAX_FREQUENCY     300.0f


/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} HSA_Vec3;


/* ============================================================
 * VECTOR FUNCTIONS
 * ============================================================ */

static HSA_Vec3
hsa_vec3(
    float x,
    float y,
    float z
)
{
    HSA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;

    return v;
}


static HSA_Vec3
hsa_add(
    HSA_Vec3 a,
    HSA_Vec3 b
)
{
    return hsa_vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}


static HSA_Vec3
hsa_sub(
    HSA_Vec3 a,
    HSA_Vec3 b
)
{
    return hsa_vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}


static HSA_Vec3
hsa_scale(
    HSA_Vec3 v,
    float s
)
{
    return hsa_vec3(
        v.x * s,
        v.y * s,
        v.z * s
    );
}


static float
hsa_dot(
    HSA_Vec3 a,
    HSA_Vec3 b
)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}


static float
hsa_length(
    HSA_Vec3 v
)
{
    return sqrtf(
        hsa_dot(
            v,
            v
        )
    );
}


static HSA_Vec3
hsa_normalize(
    HSA_Vec3 v
)
{
    float length =
        hsa_length(v);

    if (length < 0.00001f)
        return hsa_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    return hsa_scale(
        v,
        1.0f / length
    );
}


/* ============================================================
 * UTILITY
 * ============================================================ */

static float
hsa_clamp(
    float x,
    float lo,
    float hi
)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


static float
hsa_saturate(
    float x
)
{
    return hsa_clamp(
        x,
        0.0f,
        1.0f
    );
}


static float
hsa_lerp(
    float a,
    float b,
    float t
)
{
    t = hsa_saturate(t);

    return a +
        (b - a) * t;
}


/* ============================================================
 * LISTENER
 * ============================================================ */

typedef struct
{
    HSA_Vec3 position;

    /*
     * Forward direction.
     */

    HSA_Vec3 forward;

    /*
     * Up direction.
     */

    HSA_Vec3 up;

} HSA_Listener;


/* ============================================================
 * SOUND SOURCE
 * ============================================================ */

typedef struct
{
    bool active;

    uint32_t id;

    HSA_Vec3 position;

    HSA_Vec3 velocity;

    /*
     * Acoustic properties.
     */

    float volume;

    float frequency;

    float bandwidth;

    /*
     * Dynamic properties.
     */

    float transient;

    float intensity;

    float previous_distance;

    /*
     * Optional source classification.
     */

    bool impact;

    bool looping;

} HSA_Source;


/* ============================================================
 * HAPTIC OUTPUT
 * ============================================================ */

typedef struct
{
    float left;

    float right;

    float amplitude;

    float frequency;

    float duration;

    float front;

    float rear;

    float approach;

    float distance;

    float direction;

} HSA_HapticSignal;


/* ============================================================
 * SPATIAL INFORMATION
 * ============================================================ */

typedef struct
{
    float distance;

    float azimuth;

    float elevation;

    float left_gain;

    float right_gain;

    float front_gain;

    float rear_gain;

    float approach;

} HSA_SpatialInfo;


/* ============================================================
 * ENGINE
 * ============================================================ */

typedef struct
{
    HSA_Listener listener;

    HSA_Source sources[
        HSA_MAX_SOURCES
    ];

    int source_count;

    float master_gain;

    float time;

} HSA_Engine;


/* ============================================================
 * INITIALISE
 * ============================================================ */

static void
hsa_init(
    HSA_Engine *engine
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->listener.position =
        hsa_vec3(
            0.0f,
            1.7f,
            0.0f
        );

    engine->listener.forward =
        hsa_vec3(
            0.0f,
            0.0f,
            -1.0f
        );

    engine->listener.up =
        hsa_vec3(
            0.0f,
            1.0f,
            0.0f
        );

    engine->master_gain =
        1.0f;
}


/* ============================================================
 * SET LISTENER
 * ============================================================ */

static void
hsa_set_listener(
    HSA_Engine *engine,
    HSA_Vec3 position,
    HSA_Vec3 forward,
    HSA_Vec3 up
)
{
    engine->listener.position =
        position;

    engine->listener.forward =
        hsa_normalize(
            forward
        );

    engine->listener.up =
        hsa_normalize(
            up
        );
}


/* ============================================================
 * ADD SOURCE
 * ============================================================ */

static int
hsa_add_source(
    HSA_Engine *engine,
    HSA_Vec3 position,
    HSA_Vec3 velocity,
    float volume,
    float frequency
)
{
    if (
        engine->source_count >=
        HSA_MAX_SOURCES
    )
    {
        return -1;
    }

    int index =
        engine->source_count++;

    HSA_Source *source =
        &engine->sources[index];

    memset(
        source,
        0,
        sizeof(*source)
    );

    source->active =
        true;

    source->id =
        (uint32_t)(index + 1);

    source->position =
        position;

    source->velocity =
        velocity;

    source->volume =
        hsa_saturate(
            volume
        );

    source->frequency =
        frequency;

    source->bandwidth =
        1.0f;

    source->intensity =
        1.0f;

    source->previous_distance =
        hsa_length(
            hsa_sub(
                position,
                engine->listener.position
            )
        );

    return index;
}


/* ============================================================
 * UPDATE SOURCE
 * ============================================================ */

static void
hsa_update_source(
    HSA_Source *source,
    float dt
)
{
    if (!source->active)
        return;

    source->position =
        hsa_add(
            source->position,
            hsa_scale(
                source->velocity,
                dt
            )
        );
}


/* ============================================================
 * BUILD LOCAL BASIS
 * ============================================================ */

static void
hsa_basis(
    HSA_Listener *listener,
    HSA_Vec3 *right
)
{
    /*
     * right = forward × up
     */

    HSA_Vec3 f =
        listener->forward;

    HSA_Vec3 u =
        listener->up;

    *right =
        hsa_normalize(
            hsa_vec3(
                f.y * u.z -
                f.z * u.y,

                f.z * u.x -
                f.x * u.z,

                f.x * u.y -
                f.y * u.x
            )
        );
}


/* ============================================================
 * SPATIAL ANALYSIS
 * ============================================================ */

static HSA_SpatialInfo
hsa_spatialise(
    HSA_Engine *engine,
    HSA_Source *source
)
{
    HSA_SpatialInfo result;

    memset(
        &result,
        0,
        sizeof(result)
    );

    HSA_Vec3 relative =
        hsa_sub(
            source->position,
            engine->listener.position
        );

    result.distance =
        hsa_length(
            relative
        );

    HSA_Vec3 direction =
        hsa_normalize(
            relative
        );

    HSA_Vec3 right;

    hsa_basis(
        &engine->listener,
        &right
    );

    float forward =
        hsa_dot(
            direction,
            engine->listener.forward
        );

    float lateral =
        hsa_dot(
            direction,
            right
        );

    float vertical =
        hsa_dot(
            direction,
            engine->listener.up
        );

    /*
     * Azimuth.
     */

    result.azimuth =
        atan2f(
            lateral,
            forward
        );

    /*
     * Elevation.
     */

    result.elevation =
        asinf(
            hsa_clamp(
                vertical,
                -1.0f,
                1.0f
            )
        );

    /*
     * Distance attenuation.
     */

    float d =
        hsa_clamp(
            result.distance,
            HSA_MIN_DISTANCE,
            HSA_MAX_DISTANCE
        );

    float distance_gain =
        1.0f /
        (
            1.0f +
            0.08f *
            d *
            d
        );

    /*
     * Equal-power stereo panning.
     *
     * lateral:
     *
     * -1 = left
     *  0 = centre
     * +1 = right
     */

    float pan =
        hsa_clamp(
            lateral,
            -1.0f,
            1.0f
        );

    float pan_angle =
        (pan + 1.0f) *
        HSA_PI *
        0.25f;

    result.left_gain =
        cosf(
            pan_angle
        ) *
        distance_gain;

    result.right_gain =
        sinf(
            pan_angle
        ) *
        distance_gain;

    /*
     * Front/rear energy.
     */

    result.front_gain =
        hsa_saturate(
            forward
        ) *
        distance_gain;

    result.rear_gain =
        hsa_saturate(
            -forward
        ) *
        distance_gain;

    /*
     * APPROACH DETECTION
     *
     * If the current distance is smaller
     * than the previous distance, the source
     * is approaching.
     */

    float distance_delta =
        source->previous_distance -
        result.distance;

    result.approach =
        hsa_saturate(
            distance_delta *
            3.0f
        );

    source->previous_distance =
        result.distance;

    return result;
}


/* ============================================================
 * DIRECTIONAL FREQUENCY
 * ============================================================ */

static float
hsa_direction_frequency(
    HSA_SpatialInfo *spatial,
    HSA_Source *source
)
{
    /*
     * Lower frequencies are useful for
     * broad directional rumble.
     *
     * Higher frequencies are useful for
     * impacts/transients.
     */

    float front =
        spatial->front_gain;

    float rear =
        spatial->rear_gain;

    float directional =
        front * 1.0f +
        rear  * 0.65f;

    float frequency =
        source->frequency *
        (
            0.55f +
            0.45f *
            directional
        );

    return hsa_clamp(
        frequency,
        HSA_MIN_FREQUENCY,
        HSA_MAX_FREQUENCY
    );
}


/* ============================================================
 * SOURCE → HAPTICS
 * ============================================================ */

static HSA_HapticSignal
hsa_source_to_haptic(
    HSA_Engine *engine,
    HSA_Source *source
)
{
    HSA_HapticSignal h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    HSA_SpatialInfo spatial =
        hsa_spatialise(
            engine,
            source
        );

    /*
     * Ignore distant sources.
     */

    if (
        spatial.distance >
        HSA_MAX_DISTANCE
    )
    {
        return h;
    }

    /*
     * Base intensity.
     */

    float amplitude =
        source->volume *
        source->intensity;

    /*
     * Distance attenuation.
     */

    float distance_gain =
        1.0f /
        (
            1.0f +
            0.08f *
            spatial.distance *
            spatial.distance
        );

    amplitude *=
        distance_gain;

    /*
     * Approaching sounds become
     * increasingly tactile.
     */

    amplitude +=
        spatial.approach *
        0.35f;

    /*
     * Transients produce stronger
     * pulses.
     */

    amplitude +=
        source->transient *
        0.50f;

    amplitude =
        hsa_saturate(
            amplitude
        );

    h.amplitude =
        amplitude;

    /*
     * Directional frequency.
     */

    h.frequency =
        hsa_direction_frequency(
            &spatial,
            source
        );

    /*
     * Left/right routing.
     */

    h.left =
        amplitude *
        spatial.left_gain;

    h.right =
        amplitude *
        spatial.right_gain;

    /*
     * Front/rear information.
     */

    h.front =
        spatial.front_gain;

    h.rear =
        spatial.rear_gain;

    h.approach =
        spatial.approach;

    h.distance =
        spatial.distance;

    h.direction =
        spatial.azimuth;

    /*
     * Impacts are short.
     * Continuous sounds are longer.
     */

    if (source->impact)
    {
        h.duration =
            0.025f +
            source->transient *
            0.045f;
    }
    else
    {
        h.duration =
            0.040f;
    }

    return h;
}


/* ============================================================
 * HAPTIC MIXER
 * ============================================================ */

static HSA_HapticSignal
hsa_mix(
    HSA_HapticSignal a,
    HSA_HapticSignal b
)
{
    HSA_HapticSignal out;

    memset(
        &out,
        0,
        sizeof(out)
    );

    /*
     * Sum directional energy.
     */

    out.left =
        hsa_clamp(
            a.left +
            b.left,
            0.0f,
            1.0f
        );

    out.right =
        hsa_clamp(
            a.right +
            b.right,
            0.0f,
            1.0f
        );

    out.amplitude =
        fmaxf(
            out.left,
            out.right
        );

    /*
     * Strongest frequency dominates.
     */

    out.frequency =
        a.amplitude >
        b.amplitude
        ? a.frequency
        : b.frequency;

    out.duration =
        fmaxf(
            a.duration,
            b.duration
        );

    out.front =
        fmaxf(
            a.front,
            b.front
        );

    out.rear =
        fmaxf(
            a.rear,
            b.rear
        );

    out.approach =
        fmaxf(
            a.approach,
            b.approach
        );

    out.distance =
        fminf(
            a.distance,
            b.distance
        );

    return out;
}


/* ============================================================
 * PROCESS ALL SOURCES
 * ============================================================ */

static HSA_HapticSignal
hsa_process(
    HSA_Engine *engine,
    float dt
)
{
    HSA_HapticSignal output;

    memset(
        &output,
        0,
        sizeof(output)
    );

    engine->time +=
        dt;

    for (
        int i = 0;
        i < engine->source_count;
        ++i
    )
    {
        HSA_Source *source =
            &engine->sources[i];

        if (!source->active)
            continue;

        hsa_update_source(
            source,
            dt
        );

        HSA_HapticSignal signal =
            hsa_source_to_haptic(
                engine,
                source
            );

        output =
            hsa_mix(
                output,
                signal
            );
    }

    output.left *=
        engine->master_gain;

    output.right *=
        engine->master_gain;

    output.left =
        hsa_saturate(
            output.left
        );

    output.right =
        hsa_saturate(
            output.right
        );

    output.amplitude =
        fmaxf(
            output.left,
            output.right
        );

    return output;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct
{
    bool connected;

    float left_amplitude;

    float right_amplitude;

    float frequency;

    float duration;

} HSA_QuestBackend;


/* ============================================================
 * QUEST INITIALISATION
 * ============================================================ */

static void
hsa_quest_init(
    HSA_QuestBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected =
        true;
}


/* ============================================================
 * QUEST HAPTIC OUTPUT
 * ============================================================ */

static void
hsa_quest_submit(
    HSA_QuestBackend *backend,
    HSA_HapticSignal *signal
)
{
    if (
        !backend->connected
    )
    {
        return;
    }

    backend->left_amplitude =
        signal->left;

    backend->right_amplitude =
        signal->right;

    backend->frequency =
        signal->frequency;

    backend->duration =
        signal->duration;

    /*
     * Production implementation:
     *
     *   HSA_HapticSignal
     *          |
     *          v
     *   left/right routing
     *          |
     *          v
     *   OpenXR haptic action
     *          |
     *          v
     *   xrApplyHapticFeedback()
     *
     * For Quest-specific advanced haptics,
     * the signal can additionally be converted
     * into Meta's PCM/parametric haptic format.
     */

    printf(
        "HAPTIC  "
        "L=%0.2f  "
        "R=%0.2f  "
        "F=%6.1fHz  "
        "D=%0.3fs  "
        "Approach=%0.2f  "
        "Dir=%+0.2frad\n",

        signal->left,
        signal->right,
        signal->frequency,
        signal->duration,
        signal->approach,
        signal->direction
    );
}


/* ============================================================
 * SOURCE EVENT
 * ============================================================ */

static void
hsa_trigger_impact(
    HSA_Source *source,
    float intensity
)
{
    source->impact =
        true;

    source->transient =
        hsa_saturate(
            intensity
        );

    source->intensity =
        hsa_saturate(
            0.7f +
            intensity * 0.3f
        );
}


/* ============================================================
 * DEMO
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "================================================\n"
        "       HAPTIC SPATIAL AUDIO ENGINE\n"
        "       C11 / META QUEST\n"
        "================================================\n\n"
    );

    HSA_Engine engine;

    hsa_init(
        &engine
    );

    HSA_QuestBackend quest;

    hsa_quest_init(
        &quest
    );

    /*
     * Listener looking forward.
     */

    hsa_set_listener(
        &engine,

        hsa_vec3(
            0.0f,
            1.7f,
            0.0f
        ),

        hsa_vec3(
            0.0f,
            0.0f,
            -1.0f
        ),

        hsa_vec3(
            0.0f,
            1.0f,
            0.0f
        )
    );


    /*
     * Source 1:
     *
     * Engine on the LEFT.
     */

    int left_engine =
        hsa_add_source(
            &engine,

            hsa_vec3(
                -3.0f,
                1.5f,
                -4.0f
            ),

            hsa_vec3(
                0.8f,
                0.0f,
                0.0f
            ),

            0.90f,

            80.0f
        );


    /*
     * Source 2:
     *
     * High-frequency object on RIGHT.
     */

    int right_source =
        hsa_add_source(
            &engine,

            hsa_vec3(
                3.0f,
                1.5f,
                -5.0f
            ),

            hsa_vec3(
                -0.4f,
                0.0f,
                0.0f
            ),

            0.75f,

            180.0f
        );


    /*
     * Source 3:
     *
     * Object behind listener.
     */

    int rear_source =
        hsa_add_source(
            &engine,

            hsa_vec3(
                0.0f,
                1.5f,
                5.0f
            ),

            hsa_vec3(
                0.0f,
                0.0f,
                -1.2f
            ),

            0.65f,

            60.0f
        );


    /*
     * Generate an impact from
     * the right-hand source.
     */

    if (right_source >= 0)
    {
        hsa_trigger_impact(
            &engine.sources[
                right_source
            ],
            1.0f
        );
    }


    /*
     * Simulate several frames.
     */

    for (
        int frame = 0;
        frame < 20;
        ++frame
    )
    {
        HSA_HapticSignal signal =
            hsa_process(
                &engine,
                0.016f
            );

        hsa_quest_submit(
            &quest,
            &signal
        );
    }


    /*
     * Prevent compiler warnings for
     * demonstration-only source.
     */

    (void)left_engine;
    (void)rear_source;


    printf(
        "\n"
        "================================================\n"
        "SPATIAL HAPTIC ENGINE COMPLETE\n"
        "================================================\n"
    );

    return 0;
}



/*
 * haptic_robot_teleoperation.c
 *
 * ============================================================
 * HAPTIC ROBOTICS / TELEOPERATION ENGINE
 * ============================================================
 *
 * Meta Quest / OpenXR-oriented C11 prototype.
 *
 * Features:
 *
 *   - VR hand/controller pose
 *   - VR → robot workspace mapping
 *   - Position scaling
 *   - Velocity limiting
 *   - Acceleration limiting
 *   - Virtual robot workspace
 *   - Collision boundary detection
 *   - Gripper control
 *   - Contact detection
 *   - Force estimation
 *   - Object stiffness modelling
 *   - Directional haptic feedback
 *   - Contact vibration
 *   - Collision vibration
 *   - Gripper vibration
 *   - Simulated robot arm
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_robot_teleoperation.c -lm \
 *      -o haptic_robot_teleoperation
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HRT_PI 3.14159265358979323846f

#define HRT_MAX_AMPLITUDE 1.0f

#define HRT_MAX_VELOCITY  2.0f

#define HRT_MAX_ACCEL     8.0f

#define HRT_WORKSPACE_X   1.20f
#define HRT_WORKSPACE_Y   1.00f
#define HRT_WORKSPACE_Z   1.20f

#define HRT_SAFETY_MARGIN 0.08f

#define HRT_CONTACT_DISTANCE 0.035f


/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} HRT_Vec3;


static HRT_Vec3
hrt_vec3(
    float x,
    float y,
    float z
)
{
    return (HRT_Vec3){
        x,
        y,
        z
    };
}


static HRT_Vec3
hrt_add(
    HRT_Vec3 a,
    HRT_Vec3 b
)
{
    return hrt_vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}


static HRT_Vec3
hrt_sub(
    HRT_Vec3 a,
    HRT_Vec3 b
)
{
    return hrt_vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}


static HRT_Vec3
hrt_scale(
    HRT_Vec3 a,
    float s
)
{
    return hrt_vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}


static float
hrt_dot(
    HRT_Vec3 a,
    HRT_Vec3 b
)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}


static float
hrt_length(
    HRT_Vec3 a
)
{
    return sqrtf(
        hrt_dot(
            a,
            a
        )
    );
}


static HRT_Vec3
hrt_normalize(
    HRT_Vec3 a
)
{
    float l =
        hrt_length(a);

    if (l < 0.00001f)
        return hrt_vec3(
            0.0f,
            0.0f,
            0.0f
        );

    return hrt_scale(
        a,
        1.0f / l
    );
}


static float
hrt_clamp(
    float x,
    float min,
    float max
)
{
    if (x < min)
        return min;

    if (x > max)
        return max;

    return x;
}


static float
hrt_saturate(
    float x
)
{
    return hrt_clamp(
        x,
        0.0f,
        1.0f
    );
}


/* ============================================================
 * POSE
 * ============================================================ */

typedef struct
{
    HRT_Vec3 position;

    HRT_Vec3 velocity;

    HRT_Vec3 forward;

    HRT_Vec3 up;

} HRT_Pose;


/* ============================================================
 * GRIPPER
 * ============================================================ */

typedef struct
{
    float command;

    float position;

    float force;

    bool holding;

} HRT_Gripper;


/* ============================================================
 * ROBOT STATE
 * ============================================================ */

typedef struct
{
    HRT_Pose end_effector;

    HRT_Gripper gripper;

    float max_velocity;

    float max_acceleration;

    float stiffness;

    bool collision;

    bool contact;

} HRT_Robot;


/* ============================================================
 * VR CONTROLLER
 * ============================================================ */

typedef struct
{
    HRT_Pose pose;

    float trigger;

    float grip;

    bool primary_button;

    bool secondary_button;

} HRT_Controller;


/* ============================================================
 * HAPTIC SIGNAL
 * ============================================================ */

typedef struct
{
    float amplitude;

    float frequency;

    float duration;

    float left;

    float right;

    float impact;

    float contact;

    float collision;

    float grip;

    HRT_Vec3 direction;

} HRT_Haptic;


/* ============================================================
 * ROBOT OBJECT
 * ============================================================ */

typedef struct
{
    HRT_Vec3 position;

    float radius;

    float stiffness;

    float friction;

    bool graspable;

} HRT_Object;


/* ============================================================
 * ENGINE
 * ============================================================ */

typedef struct
{
    HRT_Controller controller;

    HRT_Robot robot;

    HRT_Object object;

    HRT_Vec3 workspace_min;

    HRT_Vec3 workspace_max;

    float position_scale;

    float time;

} HRT_Engine;


/* ============================================================
 * INITIALISE
 * ============================================================ */

static void
hrt_init(
    HRT_Engine *engine
)
{
    memset(
        engine,
        0,
        sizeof(*engine)
    );

    engine->position_scale =
        1.0f;

    engine->workspace_min =
        hrt_vec3(
            -HRT_WORKSPACE_X,
            0.0f,
            -HRT_WORKSPACE_Z
        );

    engine->workspace_max =
        hrt_vec3(
            HRT_WORKSPACE_X,
            HRT_WORKSPACE_Y,
            HRT_WORKSPACE_Z
        );

    engine->robot.max_velocity =
        1.0f;

    engine->robot.max_acceleration =
        4.0f;

    engine->robot.stiffness =
        100.0f;

    engine->robot.end_effector.position =
        hrt_vec3(
            0.0f,
            0.5f,
            0.0f
        );

    engine->robot.end_effector.forward =
        hrt_vec3(
            0.0f,
            0.0f,
            -1.0f
        );

    engine->robot.end_effector.up =
        hrt_vec3(
            0.0f,
            1.0f,
            0.0f
        );

    /*
     * Virtual object.
     */

    engine->object.position =
        hrt_vec3(
            0.0f,
            0.45f,
            -0.45f
        );

    engine->object.radius =
        0.12f;

    engine->object.stiffness =
        300.0f;

    engine->object.friction =
        0.4f;

    engine->object.graspable =
        true;
}


/* ============================================================
 * MAP VR → ROBOT
 * ============================================================ */

static HRT_Vec3
hrt_map_controller_to_robot(
    HRT_Engine *engine,
    HRT_Vec3 vr_position
)
{
    /*
     * The user's hand is mapped into the robot
     * workspace using a configurable scale.
     */

    HRT_Vec3 result =
        hrt_scale(
            vr_position,
            engine->position_scale
        );

    result.x =
        hrt_clamp(
            result.x,
            engine->workspace_min.x,
            engine->workspace_max.x
        );

    result.y =
        hrt_clamp(
            result.y,
            engine->workspace_min.y,
            engine->workspace_max.y
        );

    result.z =
        hrt_clamp(
            result.z,
            engine->workspace_min.z,
            engine->workspace_max.z
        );

    return result;
}


/* ============================================================
 * WORKSPACE PROXIMITY
 * ============================================================ */

static float
hrt_workspace_proximity(
    HRT_Engine *engine,
    HRT_Vec3 p
)
{
    float dx =
        fminf(
            p.x - engine->workspace_min.x,
            engine->workspace_max.x - p.x
        );

    float dy =
        fminf(
            p.y - engine->workspace_min.y,
            engine->workspace_max.y - p.y
        );

    float dz =
        fminf(
            p.z - engine->workspace_min.z,
            engine->workspace_max.z - p.z
        );

    float minimum =
        fminf(
            dx,
            fminf(dy, dz)
        );

    return hrt_saturate(
        1.0f -
        minimum /
        HRT_SAFETY_MARGIN
    );
}


/* ============================================================
 * LIMIT VELOCITY
 * ============================================================ */

static HRT_Vec3
hrt_limit_velocity(
    HRT_Vec3 velocity,
    float maximum
)
{
    float magnitude =
        hrt_length(
            velocity
        );

    if (
        magnitude <= maximum
    )
    {
        return velocity;
    }

    return hrt_scale(
        velocity,
        maximum / magnitude
    );
}


/* ============================================================
 * UPDATE ROBOT
 * ============================================================ */

static void
hrt_update_robot(
    HRT_Engine *engine,
    float dt
)
{
    HRT_Vec3 target =
        hrt_map_controller_to_robot(
            engine,
            engine->controller.pose.position
        );

    HRT_Vec3 current =
        engine->robot.end_effector.position;

    HRT_Vec3 error =
        hrt_sub(
            target,
            current
        );

    /*
     * Simple proportional position controller.
     */

    HRT_Vec3 desired_velocity =
        hrt_scale(
            error,
            12.0f
        );

    desired_velocity =
        hrt_limit_velocity(
            desired_velocity,
            engine->robot.max_velocity
        );

    /*
     * Acceleration limiting.
     */

    HRT_Vec3 current_velocity =
        engine->robot.end_effector.velocity;

    HRT_Vec3 velocity_delta =
        hrt_sub(
            desired_velocity,
            current_velocity
        );

    float delta_length =
        hrt_length(
            velocity_delta
        );

    float maximum_delta =
        engine->robot.max_acceleration *
        dt;

    if (
        delta_length >
        maximum_delta
    )
    {
        velocity_delta =
            hrt_scale(
                velocity_delta,
                maximum_delta /
                delta_length
            );
    }

    current_velocity =
        hrt_add(
            current_velocity,
            velocity_delta
        );

    engine->robot.end_effector.velocity =
        current_velocity;

    engine->robot.end_effector.position =
        hrt_add(
            current,
            hrt_scale(
                current_velocity,
                dt
            )
        );

    /*
     * Gripper.
     */

    engine->robot.gripper.command =
        engine->controller.trigger;

    engine->robot.gripper.position =
        1.0f -
        engine->controller.trigger;
}


/* ============================================================
 * OBJECT CONTACT
 * ============================================================ */

static float
hrt_object_penetration(
    HRT_Engine *engine
)
{
    HRT_Vec3 delta =
        hrt_sub(
            engine->robot.end_effector.position,
            engine->object.position
        );

    float distance =
        hrt_length(
            delta
        );

    float penetration =
        engine->object.radius -
        distance;

    if (penetration <= 0.0f)
        return 0.0f;

    return penetration;
}


/* ============================================================
 * CONTACT FORCE
 * ============================================================ */

static HRT_Vec3
hrt_contact_force(
    HRT_Engine *engine
)
{
    HRT_Vec3 delta =
        hrt_sub(
            engine->robot.end_effector.position,
            engine->object.position
        );

    float distance =
        hrt_length(
            delta
        );

    if (
        distance <
        0.0001f
    )
    {
        return hrt_vec3(
            0.0f,
            1.0f,
            0.0f
        );
    }

    float penetration =
        engine->object.radius -
        distance;

    if (penetration <= 0.0f)
    {
        return hrt_vec3(
            0.0f,
            0.0f,
            0.0f
        );
    }

    HRT_Vec3 normal =
        hrt_scale(
            delta,
            1.0f / distance
        );

    return hrt_scale(
        normal,
        penetration *
        engine->object.stiffness
    );
}


/* ============================================================
 * GRASP DETECTION
 * ============================================================ */

static void
hrt_update_grasp(
    HRT_Engine *engine
)
{
    float penetration =
        hrt_object_penetration(
            engine
        );

    bool close =
        engine->controller.trigger >
        0.75f;

    bool near =
        penetration >
        -HRT_CONTACT_DISTANCE;

    if (
        close &&
        near &&
        engine->object.graspable
    )
    {
        engine->robot.gripper.holding =
            true;

        engine->robot.gripper.force =
            engine->controller.trigger *
            10.0f;
    }

    if (
        engine->controller.trigger <
        0.20f
    )
    {
        engine->robot.gripper.holding =
            false;

        engine->robot.gripper.force =
            0.0f;
    }
}


/* ============================================================
 * HAPTIC CONTACT
 * ============================================================ */

static HRT_Haptic
hrt_contact_haptic(
    HRT_Engine *engine
)
{
    HRT_Haptic h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    HRT_Vec3 force =
        hrt_contact_force(
            engine
        );

    float force_magnitude =
        hrt_length(
            force
        );

    /*
     * Contact.
     */

    engine->robot.contact =
        force_magnitude >
        0.1f;

    if (
        engine->robot.contact
    )
    {
        h.contact =
            hrt_saturate(
                force_magnitude /
                40.0f
            );

        h.amplitude =
            h.contact *
            0.65f;

        h.frequency =
            45.0f +
            h.contact *
            130.0f;

        h.duration =
            0.040f;

        h.direction =
            hrt_normalize(
                force
            );

        /*
         * Split force into directional
         * controller channels.
         */

        float x =
            h.direction.x;

        h.left =
            h.amplitude *
            hrt_saturate(
                0.5f - x * 0.5f
            );

        h.right =
            h.amplitude *
            hrt_saturate(
                0.5f + x * 0.5f
            );
    }

    return h;
}


/* ============================================================
 * COLLISION HAPTIC
 * ============================================================ */

static HRT_Haptic
hrt_collision_haptic(
    HRT_Engine *engine
)
{
    HRT_Haptic h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    float proximity =
        hrt_workspace_proximity(
            engine,
            engine->robot.end_effector.position
        );

    /*
     * Hard workspace boundary.
     */

    if (
        proximity > 0.75f
    )
    {
        engine->robot.collision =
            proximity >= 1.0f;

        h.collision =
            proximity;

        h.amplitude =
            0.10f +
            proximity *
            0.55f;

        h.frequency =
            80.0f +
            proximity *
            140.0f;

        h.duration =
            0.025f;

        /*
         * Global warning.
         */

        h.left =
            h.amplitude;

        h.right =
            h.amplitude;
    }

    return h;
}


/* ============================================================
 * GRIPPER HAPTIC
 * ============================================================ */

static HRT_Haptic
hrt_gripper_haptic(
    HRT_Engine *engine
)
{
    HRT_Haptic h;

    memset(
        &h,
        0,
        sizeof(h)
    );

    if (
        engine->robot.gripper.holding
    )
    {
        h.grip =
            hrt_saturate(
                engine->robot.gripper.force /
                10.0f
            );

        h.amplitude =
            h.grip *
            0.45f;

        h.frequency =
            65.0f;

        h.duration =
            0.030f;

        h.left =
            h.amplitude;

        h.right =
            h.amplitude;
    }

    return h;
}


/* ============================================================
 * HAPTIC MIXER
 * ============================================================ */

static HRT_Haptic
hrt_mix(
    HRT_Haptic a,
    HRT_Haptic b
)
{
    HRT_Haptic out;

    memset(
        &out,
        0,
        sizeof(out)
    );

    out.left =
        hrt_clamp(
            a.left +
            b.left,
            0.0f,
            1.0f
        );

    out.right =
        hrt_clamp(
            a.right +
            b.right,
            0.0f,
            1.0f
        );

    out.amplitude =
        fmaxf(
            out.left,
            out.right
        );

    out.frequency =
        a.amplitude >
        b.amplitude
        ? a.frequency
        : b.frequency;

    out.duration =
        fmaxf(
            a.duration,
            b.duration
        );

    out.contact =
        fmaxf(
            a.contact,
            b.contact
        );

    out.collision =
        fmaxf(
            a.collision,
            b.collision
        );

    out.grip =
        fmaxf(
            a.grip,
            b.grip
        );

    return out;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct
{
    bool connected;

} HRT_QuestBackend;


static void
hrt_quest_init(
    HRT_QuestBackend *backend
)
{
    backend->connected =
        true;
}


/* ============================================================
 * SUBMIT HAPTICS
 * ============================================================ */

static void
hrt_quest_submit(
    HRT_QuestBackend *backend,
    HRT_Haptic *h
)
{
    if (
        !backend->connected
    )
    {
        return;
    }

    /*
     * Production implementation:
     *
     *   HRT_Haptic
     *       |
     *       +---- left amplitude
     *       |
     *       +---- right amplitude
     *       |
     *       +---- frequency
     *       |
     *       +---- duration
     *       |
     *       v
     *   OpenXR / Meta haptic layer
     */

    printf(
        "HAPTIC  "
        "L=%0.2f "
        "R=%0.2f "
        "F=%6.1fHz "
        "contact=%0.2f "
        "collision=%0.2f "
        "grip=%0.2f\n",

        h->left,
        h->right,
        h->frequency,
        h->contact,
        h->collision,
        h->grip
    );
}


/* ============================================================
 * SIMULATE VR HAND
 * ============================================================ */

static void
hrt_simulate_controller(
    HRT_Engine *engine,
    float t
)
{
    /*
     * Move the virtual hand in a circular
     * trajectory around the object.
     */

    engine->controller.pose.position =
        hrt_vec3(
            sinf(t) * 0.30f,
            0.45f +
                sinf(t * 0.7f) * 0.08f,
            -0.45f +
                cosf(t) * 0.30f
        );

    engine->controller.pose.forward =
        hrt_vec3(
            0.0f,
            0.0f,
            -1.0f
        );

    /*
     * Simulated trigger.
     */

    engine->controller.trigger =
        0.5f +
        0.5f *
        sinf(t * 0.5f);

    /*
     * Occasionally close the gripper.
     */

    if (
        fmodf(
            t,
            4.0f
        ) > 2.0f
    )
    {
        engine->controller.trigger =
            0.95f;
    }
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "================================================\n"
        "       HAPTIC ROBOT TELEOPERATION\n"
        "       C11 / META QUEST\n"
        "================================================\n\n"
    );

    HRT_Engine engine;

    hrt_init(
        &engine
    );

    HRT_QuestBackend quest;

    hrt_quest_init(
        &quest
    );

    /*
     * Simulate 10 seconds of VR control.
     */

    const float dt =
        0.016f;

    for (
        int frame = 0;
        frame < 600;
        ++frame
    )
    {
        float t =
            frame * dt;

        engine.time =
            t;

        /*
         * VR input.
         */

        hrt_simulate_controller(
            &engine,
            t
        );

        /*
         * Robot motion.
         */

        hrt_update_robot(
            &engine,
            dt
        );

        /*
         * Gripper.
         */

        hrt_update_grasp(
            &engine
        );

        /*
         * Generate haptics.
         */

        HRT_Haptic contact =
            hrt_contact_haptic(
                &engine
            );

        HRT_Haptic collision =
            hrt_collision_haptic(
                &engine
            );

        HRT_Haptic grip =
            hrt_gripper_haptic(
                &engine
            );

        HRT_Haptic result =
            hrt_mix(
                contact,
                collision
            );

        result =
            hrt_mix(
                result,
                grip
            );

        /*
         * Send to Quest.
         */

        hrt_quest_submit(
            &quest,
            &result
        );
    }

    printf(
        "\n"
        "================================================\n"
        "ROBOT TELEOPERATION COMPLETE\n"
        "================================================\n"
    );

    return 0;
}








/*
 * haptic_spatial_os.c
 *
 * ============================================================
 * HAPTIC SPATIAL OPERATING SYSTEM
 * ============================================================
 *
 * System-level haptic runtime for VR.
 *
 * C11
 *
 * Features:
 *
 *   - Central haptic event bus
 *   - Multiple haptic channels
 *   - Priority handling
 *   - Event lifetime management
 *   - Left/right spatial routing
 *   - Haptic mixing
 *   - Material channel
 *   - Audio channel
 *   - Gesture channel
 *   - Robotics channel
 *   - UI channel
 *   - Physics channel
 *   - Safety limiter
 *   - Amplitude smoothing
 *   - Frequency smoothing
 *   - Controller-independent API
 *   - Quest/OpenXR backend abstraction
 *
 * Build:
 *
 *   cc -std=c11 -O2 haptic_spatial_os.c -lm \
 *      -o haptic_spatial_os
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>


/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define HSO_MAX_EVENTS       128
#define HSO_MAX_CHANNELS      8

#define HSO_MAX_AMPLITUDE     1.0f

#define HSO_MIN_FREQUENCY    20.0f
#define HSO_MAX_FREQUENCY   300.0f

#define HSO_MAX_DURATION      2.0f

#define HSO_ATTACK_RATE      18.0f
#define HSO_RELEASE_RATE      8.0f


/* ============================================================
 * CHANNELS
 * ============================================================ */

typedef enum
{
    HSO_CHANNEL_UI = 0,

    HSO_CHANNEL_GESTURE,

    HSO_CHANNEL_MATERIAL,

    HSO_CHANNEL_AUDIO,

    HSO_CHANNEL_PHYSICS,

    HSO_CHANNEL_ROBOTICS,

    HSO_CHANNEL_ENVIRONMENT,

    HSO_CHANNEL_SYSTEM

} HSO_Channel;


/* ============================================================
 * EVENT TYPES
 * ============================================================ */

typedef enum
{
    HSO_EVENT_NONE = 0,

    HSO_EVENT_TAP,

    HSO_EVENT_CLICK,

    HSO_EVENT_GRAB,

    HSO_EVENT_RELEASE,

    HSO_EVENT_IMPACT,

    HSO_EVENT_CONTACT,

    HSO_EVENT_SLIDE,

    HSO_EVENT_MATERIAL,

    HSO_EVENT_AUDIO,

    HSO_EVENT_COLLISION,

    HSO_EVENT_ROBOT_FORCE,

    HSO_EVENT_WARNING,

    HSO_EVENT_CONFIRMATION,

    HSO_EVENT_ERROR,

    HSO_EVENT_NOTIFICATION

} HSO_EventType;


/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} HSO_Vec3;


static HSO_Vec3
hso_vec3(
    float x,
    float y,
    float z
)
{
    return (HSO_Vec3){
        x,
        y,
        z
    };
}


static HSO_Vec3
hso_add(
    HSO_Vec3 a,
    HSO_Vec3 b
)
{
    return hso_vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}


static HSO_Vec3
hso_scale(
    HSO_Vec3 a,
    float s
)
{
    return hso_vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}


static float
hso_length(
    HSO_Vec3 a
)
{
    return sqrtf(
        a.x * a.x +
        a.y * a.y +
        a.z * a.z
    );
}


/* ============================================================
 * UTILITY
 * ============================================================ */

static float
hso_clamp(
    float x,
    float lo,
    float hi
)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


static float
hso_saturate(
    float x
)
{
    return hso_clamp(
        x,
        0.0f,
        1.0f
    );
}


/* ============================================================
 * HAPTIC SIGNAL
 * ============================================================ */

typedef struct
{
    /*
     * Overall signal.
     */

    float amplitude;

    float frequency;

    float duration;

    /*
     * Stereo controller routing.
     */

    float left;

    float right;

    /*
     * Semantic components.
     */

    float impact;

    float contact;

    float texture;

    float pressure;

    float friction;

    float resonance;

    /*
     * Spatial direction.
     */

    HSO_Vec3 direction;

} HSO_Signal;


/* ============================================================
 * EVENT
 * ============================================================ */

typedef struct
{
    bool active;

    uint32_t id;

    HSO_EventType type;

    HSO_Channel channel;

    /*
     * Priority:
     *
     * 0 = background
     * 100 = critical
     */

    int priority;

    /*
     * Time remaining.
     */

    float lifetime;

    /*
     * Signal.
     */

    HSO_Signal signal;

    /*
     * Source information.
     */

    HSO_Vec3 position;

    HSO_Vec3 velocity;

} HSO_Event;


/* ============================================================
 * MIXED OUTPUT
 * ============================================================ */

typedef struct
{
    float left;

    float right;

    float amplitude;

    float frequency;

    float duration;

    float activity;

} HSO_Output;


/* ============================================================
 * HAPTIC OS
 * ============================================================ */

typedef struct
{
    HSO_Event events[
        HSO_MAX_EVENTS
    ];

    uint32_t next_event_id;

    /*
     * Channel gains.
     */

    float channel_gain[
        HSO_MAX_CHANNELS
    ];

    /*
     * Global safety limiter.
     */

    float master_gain;

    float safety_limit;

    /*
     * Output smoothing.
     */

    float output_left;

    float output_right;

    float output_frequency;

    /*
     * Runtime time.
     */

    float time;

} HSO_Runtime;


/* ============================================================
 * INITIALISE
 * ============================================================ */

static void
hso_init(
    HSO_Runtime *runtime
)
{
    memset(
        runtime,
        0,
        sizeof(*runtime)
    );

    runtime->next_event_id =
        1;

    runtime->master_gain =
        1.0f;

    runtime->safety_limit =
        1.0f;

    for (
        int i = 0;
        i < HSO_MAX_CHANNELS;
        ++i
    )
    {
        runtime->channel_gain[i] =
            1.0f;
    }
}


/* ============================================================
 * CHANNEL NAME
 * ============================================================ */

static const char *
hso_channel_name(
    HSO_Channel channel
)
{
    switch (channel)
    {
        case HSO_CHANNEL_UI:
            return "UI";

        case HSO_CHANNEL_GESTURE:
            return "GESTURE";

        case HSO_CHANNEL_MATERIAL:
            return "MATERIAL";

        case HSO_CHANNEL_AUDIO:
            return "AUDIO";

        case HSO_CHANNEL_PHYSICS:
            return "PHYSICS";

        case HSO_CHANNEL_ROBOTICS:
            return "ROBOTICS";

        case HSO_CHANNEL_ENVIRONMENT:
            return "ENVIRONMENT";

        case HSO_CHANNEL_SYSTEM:
            return "SYSTEM";

        default:
            return "UNKNOWN";
    }
}


/* ============================================================
 * FIND FREE EVENT
 * ============================================================ */

static int
hso_find_free_event(
    HSO_Runtime *runtime
)
{
    for (
        int i = 0;
        i < HSO_MAX_EVENTS;
        ++i
    )
    {
        if (!runtime->events[i].active)
            return i;
    }

    /*
     * If full, replace the lowest-priority
     * active event.
     */

    int candidate = -1;

    int lowest_priority = 1000000;

    for (
        int i = 0;
        i < HSO_MAX_EVENTS;
        ++i
    )
    {
        if (
            runtime->events[i].priority <
            lowest_priority
        )
        {
            candidate = i;

            lowest_priority =
                runtime->events[i].priority;
        }
    }

    return candidate;
}


/* ============================================================
 * SUBMIT EVENT
 * ============================================================ */

static uint32_t
hso_submit(
    HSO_Runtime *runtime,
    HSO_EventType type,
    HSO_Channel channel,
    int priority,
    HSO_Signal signal,
    float lifetime
)
{
    int slot =
        hso_find_free_event(
            runtime
        );

    if (slot < 0)
        return 0;

    HSO_Event *event =
        &runtime->events[slot];

    memset(
        event,
        0,
        sizeof(*event)
    );

    event->active =
        true;

    event->id =
        runtime->next_event_id++;

    event->type =
        type;

    event->channel =
        channel;

    event->priority =
        priority;

    event->lifetime =
        hso_clamp(
            lifetime,
            0.001f,
            HSO_MAX_DURATION
        );

    event->signal =
        signal;

    return event->id;
}


/* ============================================================
 * SIMPLE SIGNAL
 * ============================================================ */

static HSO_Signal
hso_signal(
    float amplitude,
    float frequency,
    float duration
)
{
    HSO_Signal s;

    memset(
        &s,
        0,
        sizeof(s)
    );

    s.amplitude =
        hso_saturate(
            amplitude
        );

    s.frequency =
        hso_clamp(
            frequency,
            HSO_MIN_FREQUENCY,
            HSO_MAX_FREQUENCY
        );

    s.duration =
        duration;

    s.left =
        s.amplitude;

    s.right =
        s.amplitude;

    return s;
}


/* ============================================================
 * SPATIAL SIGNAL
 * ============================================================ */

static HSO_Signal
hso_directional_signal(
    float amplitude,
    float frequency,
    float azimuth
)
{
    HSO_Signal s =
        hso_signal(
            amplitude,
            frequency,
            0.04f
        );

    /*
     * azimuth:
     *
     * -1 = left
     *  0 = centre
     * +1 = right
     */

    azimuth =
        hso_clamp(
            azimuth,
            -1.0f,
            1.0f
        );

    s.left =
        s.amplitude *
        (0.5f - azimuth * 0.5f);

    s.right =
        s.amplitude *
        (0.5f + azimuth * 0.5f);

    return s;
}


/* ============================================================
 * EXAMPLE EVENT: UI CLICK
 * ============================================================ */

static uint32_t
hso_ui_click(
    HSO_Runtime *runtime
)
{
    HSO_Signal s =
        hso_signal(
            0.25f,
            150.0f,
            0.025f
        );

    return hso_submit(
        runtime,
        HSO_EVENT_CLICK,
        HSO_CHANNEL_UI,
        30,
        s,
        0.035f
    );
}


/* ============================================================
 * EXAMPLE EVENT: MATERIAL CONTACT
 * ============================================================ */

static uint32_t
hso_material_contact(
    HSO_Runtime *runtime,
    float force,
    float roughness,
    float azimuth
)
{
    HSO_Signal s =
        hso_directional_signal(
            hso_saturate(
                force
            ),
            70.0f +
            roughness * 150.0f,
            azimuth
        );

    s.contact =
        force;

    s.texture =
        roughness;

    return hso_submit(
        runtime,
        HSO_EVENT_CONTACT,
        HSO_CHANNEL_MATERIAL,
        45,
        s,
        0.050f
    );
}


/* ============================================================
 * EXAMPLE EVENT: IMPACT
 * ============================================================ */

static uint32_t
hso_impact(
    HSO_Runtime *runtime,
    float energy,
    float azimuth
)
{
    HSO_Signal s =
        hso_directional_signal(
            hso_saturate(
                energy
            ),
            100.0f +
            energy * 160.0f,
            azimuth
        );

    s.impact =
        energy;

    s.duration =
        0.025f +
        energy * 0.04f;

    return hso_submit(
        runtime,
        HSO_EVENT_IMPACT,
        HSO_CHANNEL_PHYSICS,
        70,
        s,
        s.duration
    );
}


/* ============================================================
 * EXAMPLE EVENT: ROBOT FORCE
 * ============================================================ */

static uint32_t
hso_robot_force(
    HSO_Runtime *runtime,
    float force,
    float azimuth
)
{
    HSO_Signal s =
        hso_directional_signal(
            hso_saturate(
                force
            ),
            50.0f +
            force * 100.0f,
            azimuth
        );

    s.pressure =
        force;

    return hso_submit(
        runtime,
        HSO_EVENT_ROBOT_FORCE,
        HSO_CHANNEL_ROBOTICS,
        80,
        s,
        0.060f
    );
}


/* ============================================================
 * EXAMPLE EVENT: SYSTEM WARNING
 * ============================================================ */

static uint32_t
hso_warning(
    HSO_Runtime *runtime
)
{
    HSO_Signal s =
        hso_signal(
            0.65f,
            190.0f,
            0.08f
        );

    return hso_submit(
        runtime,
        HSO_EVENT_WARNING,
        HSO_CHANNEL_SYSTEM,
        95,
        s,
        0.10f
    );
}


/* ============================================================
 * PRIORITY WEIGHT
 * ============================================================ */

static float
hso_priority_weight(
    int priority
)
{
    /*
     * Important events receive slightly
     * stronger representation in the mix.
     */

    return
        0.75f +
        hso_saturate(
            priority / 100.0f
        ) *
        0.25f;
}


/* ============================================================
 * MIX EVENT
 * ============================================================ */

static void
hso_mix_event(
    HSO_Runtime *runtime,
    HSO_Event *event,
    HSO_Output *output
)
{
    if (!event->active)
        return;

    int channel =
        event->channel;

    if (
        channel < 0 ||
        channel >= HSO_MAX_CHANNELS
    )
    {
        return;
    }

    float gain =
        runtime->channel_gain[channel];

    float priority =
        hso_priority_weight(
            event->priority
        );

    float weight =
        gain *
        priority;

    output->left +=
        event->signal.left *
        weight;

    output->right +=
        event->signal.right *
        weight;

    /*
     * Strongest event controls frequency.
     */

    if (
        event->signal.amplitude >
        output->amplitude
    )
    {
        output->frequency =
            event->signal.frequency;
    }

    output->amplitude =
        fmaxf(
            output->amplitude,
            event->signal.amplitude
        );

    output->duration =
        fmaxf(
            output->duration,
            event->signal.duration
        );

    output->activity +=
        event->signal.amplitude;
}


/* ============================================================
 * LIMIT OUTPUT
 * ============================================================ */

static void
hso_limit(
    HSO_Runtime *runtime,
    HSO_Output *output
)
{
    output->left *=
        runtime->master_gain;

    output->right *=
        runtime->master_gain;

    float maximum =
        fmaxf(
            output->left,
            output->right
        );

    if (
        maximum >
        runtime->safety_limit
    )
    {
        float scale =
            runtime->safety_limit /
            maximum;

        output->left *=
            scale;

        output->right *=
            scale;
    }

    output->left =
        hso_saturate(
            output->left
        );

    output->right =
        hso_saturate(
            output->right
        );

    output->amplitude =
        fmaxf(
            output->left,
            output->right
        );
}


/* ============================================================
 * SMOOTH OUTPUT
 * ============================================================ */

static float
hso_smooth(
    float current,
    float target,
    float rate,
    float dt
)
{
    float difference =
        target - current;

    float maximum_change =
        rate * dt;

    difference =
        hso_clamp(
            difference,
            -maximum_change,
            maximum_change
        );

    return current +
        difference;
}


static void
hso_smooth_output(
    HSO_Runtime *runtime,
    HSO_Output *output,
    float dt
)
{
    float left_rate =
        output->left >
        runtime->output_left
        ? HSO_ATTACK_RATE
        : HSO_RELEASE_RATE;

    float right_rate =
        output->right >
        runtime->output_right
        ? HSO_ATTACK_RATE
        : HSO_RELEASE_RATE;

    runtime->output_left =
        hso_smooth(
            runtime->output_left,
            output->left,
            left_rate,
            dt
        );

    runtime->output_right =
        hso_smooth(
            runtime->output_right,
            output->right,
            right_rate,
            dt
        );

    runtime->output_frequency =
        hso_smooth(
            runtime->output_frequency,
            output->frequency,
            300.0f,
            dt
        );

    output->left =
        runtime->output_left;

    output->right =
        runtime->output_right;

    output->frequency =
        runtime->output_frequency;

    output->amplitude =
        fmaxf(
            output->left,
            output->right
        );
}


/* ============================================================
 * UPDATE EVENTS
 * ============================================================ */

static void
hso_update_events(
    HSO_Runtime *runtime,
    float dt
)
{
    for (
        int i = 0;
        i < HSO_MAX_EVENTS;
        ++i
    )
    {
        HSO_Event *event =
            &runtime->events[i];

        if (!event->active)
            continue;

        event->lifetime -=
            dt;

        if (
            event->lifetime <= 0.0f
        )
        {
            event->active =
                false;
        }
    }
}


/* ============================================================
 * RENDER HAPTIC FRAME
 * ============================================================ */

static HSO_Output
hso_render(
    HSO_Runtime *runtime,
    float dt
)
{
    HSO_Output output;

    memset(
        &output,
        0,
        sizeof(output)
    );

    for (
        int i = 0;
        i < HSO_MAX_EVENTS;
        ++i
    )
    {
        hso_mix_event(
            runtime,
            &runtime->events[i],
            &output
        );
    }

    hso_limit(
        runtime,
        &output
    );

    hso_smooth_output(
        runtime,
        &output,
        dt
    );

    hso_update_events(
        runtime,
        dt
    );

    runtime->time +=
        dt;

    return output;
}


/* ============================================================
 * QUEST BACKEND
 * ============================================================ */

typedef struct
{
    bool connected;

    float left;

    float right;

    float frequency;

    float duration;

} HSO_QuestBackend;


/* ============================================================
 * QUEST INITIALISE
 * ============================================================ */

static void
hso_quest_init(
    HSO_QuestBackend *backend
)
{
    memset(
        backend,
        0,
        sizeof(*backend)
    );

    backend->connected =
        true;
}


/* ============================================================
 * QUEST SUBMIT
 * ============================================================ */

static void
hso_quest_submit(
    HSO_QuestBackend *backend,
    HSO_Output *output
)
{
    if (
        !backend->connected
    )
    {
        return;
    }

    backend->left =
        output->left;

    backend->right =
        output->right;

    backend->frequency =
        output->frequency;

    backend->duration =
        output->duration;

    /*
     * Production:
     *
     *   HSO_Output
     *        |
     *        +--> left controller
     *        |
     *        +--> right controller
     *        |
     *        v
     *   OpenXR / Meta haptic backend
     */

    printf(
        "HAPTIC OS | "
        "L=%0.3f "
        "R=%0.3f "
        "F=%6.1fHz "
        "D=%0.3fs\n",

        output->left,
        output->right,
        output->frequency,
        output->duration
    );
}


/* ============================================================
 * DEMO
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "====================================================\n"
        "           HAPTIC SPATIAL OPERATING SYSTEM\n"
        "                    C11 / QUEST\n"
        "====================================================\n\n"
    );

    HSO_Runtime runtime;

    hso_init(
        &runtime
    );

    HSO_QuestBackend quest;

    hso_quest_init(
        &quest
    );


    /*
     * Different system channels can be
     * independently controlled.
     */

    runtime.channel_gain[
        HSO_CHANNEL_UI
    ] = 0.8f;

    runtime.channel_gain[
        HSO_CHANNEL_MATERIAL
    ] = 1.0f;

    runtime.channel_gain[
        HSO_CHANNEL_AUDIO
    ] = 0.7f;

    runtime.channel_gain[
        HSO_CHANNEL_ROBOTICS
    ] = 1.0f;


    /*
     * Demonstrate multiple simultaneous
     * haptic events.
     */

    hso_ui_click(
        &runtime
    );

    hso_material_contact(
        &runtime,
        0.45f,
        0.75f,
        -0.7f
    );

    hso_impact(
        &runtime,
        0.65f,
        0.35f
    );

    hso_robot_force(
        &runtime,
        0.55f,
        -0.4f
    );


    /*
     * System warning.
     */

    hso_warning(
        &runtime
    );


    /*
     * Simulated VR runtime.
     */

    const float dt =
        0.016f;

    for (
        int frame = 0;
        frame < 120;
        ++frame
    )
    {
        /*
         * Render central haptic state.
         */

        HSO_Output output =
            hso_render(
                &runtime,
                dt
            );

        /*
         * Submit to Quest backend.
         */

        hso_quest_submit(
            &quest,
            &output
        );
    }


    printf(
        "\n"
        "====================================================\n"
        "HAPTIC SPATIAL OS COMPLETE\n"
        "====================================================\n"
    );

    return 0;
}



