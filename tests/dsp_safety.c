#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/mookd.c"

static void assert_finite_state(const synth_t *s)
{
    assert(finitef_strict(s->o1.phase));
    assert(finitef_strict(s->o2.phase));
    assert(finitef_strict(s->o3.phase));
    assert(finitef_strict(s->fenv.level));
    assert(finitef_strict(s->aenv.level));
    assert(finitef_strict(s->lpf.z));
    for (int i = 0; i < 4; ++i)
        assert(finitef_strict(s->lpf.s[i]));
}

static void render_blocks(plugin_api_v2_t *api, synth_t *s, int count)
{
    int16_t block[MOVE_FRAMES_PER_BLOCK * 2];
    for (int i = 0; i < count; ++i) {
        api->render_block(s, block, MOVE_FRAMES_PER_BLOCK);
        assert_finite_state(s);
    }
}

static void assert_param(plugin_api_v2_t *api, synth_t *s,
                         const char *key, const char *expected)
{
    char value[32];
    assert(api->get_param(s, key, value, sizeof(value)) > 0);
    if (strcmp(value, expected) != 0)
        fprintf(stderr, "%s: expected %s, got %s\n", key, expected, value);
    assert(strcmp(value, expected) == 0);
}

static uint32_t fuzz_state = 0x31415926u;
static uint32_t fuzz_u32(void)
{
    fuzz_state = fuzz_state * 1664525u + 1013904223u;
    return fuzz_state;
}

int main(void)
{
    host_api_v1_t host = {0};
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    synth_t *s = api->create_instance(".", NULL);
    assert(s != NULL);

    /* Constructor defaults and the Init preset are one contract. */
    const char *all_keys[] = {
        "o1_range", "o1_wave", "o2_range", "o2_wave", "o2_tune",
        "o3_range", "o3_wave", "o3_tune", "o3_kbd", "mix_o1",
        "mix_o2", "mix_o3", "mix_noise", "noise_color", "cutoff",
        "emphasis", "contour", "filt_a", "filt_d", "filt_s", "kbd_track",
        "loud_a", "loud_d", "loud_s", "glide", "master_tune", "mod_mix",
        "mod_osc", "mod_filter", "decay_sw", "lfo_rate", "lfo_shape",
        "lfo_pitch", "lfo_filter", "lfo_sync", "lfo_div", "osc_sync",
        "filt_fm", "volume"
    };
    synth_t *init_preset = api->create_instance(".", NULL);
    api->set_param(init_preset, "preset", "0");
    for (unsigned int i = 0; i < sizeof(all_keys) / sizeof(all_keys[0]); ++i) {
        char constructor_value[32], preset_value[32];
        assert(api->get_param(s, all_keys[i], constructor_value,
                              sizeof(constructor_value)) > 0);
        assert(api->get_param(init_preset, all_keys[i], preset_value,
                              sizeof(preset_value)) > 0);
        assert(strcmp(constructor_value, preset_value) == 0);
    }
    api->destroy_instance(init_preset);

    /* Factory values must stay inside the same safe ranges exposed to knobs. */
    for (int i = 0; i < NPRESETS; ++i) {
        assert(PRESETS[i].cut >= 0.0f && PRESETS[i].cut <= MAX_CUTOFF_CTRL);
        assert(PRESETS[i].emph >= 0.0f && PRESETS[i].emph <= MAX_EMPHASIS);
        assert(PRESETS[i].cont >= 0.0f && PRESETS[i].cont <= MAX_CONTOUR);
        assert(PRESETS[i].ffm >= 0.0f && PRESETS[i].ffm <= MAX_FILTER_FM);
        assert(PRESETS[i].lf >= 0.0f && PRESETS[i].lf <= MAX_LFO_FILTER);
    }

    /* Truncated MIDI is hostile but must never read beyond the packet. */
    uint8_t one_byte[1] = {0x90};
    uint8_t two_bytes[2] = {0x90, 60};
    api->on_midi(s, one_byte, 1, MOVE_MIDI_SOURCE_INTERNAL);
    api->on_midi(s, two_bytes, 2, MOVE_MIDI_SOURCE_INTERNAL);

    /* String parameters are an ABI boundary: reject non-finite/extreme input. */
    api->set_param(s, "cutoff", "nan");
    api->set_param(s, "emphasis", "inf");
    api->set_param(s, "contour", "-1000000");
    api->set_param(s, "filt_fm", "1000000");
    api->set_param(s, "o1_range", "999");
    api->set_param(s, "o1_wave", "-999");

    uint8_t note_on[3] = {0x90, 127, 127};
    api->on_midi(s, note_on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    render_blocks(api, s, 100);
    assert_param(api, s, "cutoff", "0.3800");       /* NaN retained current */
    assert_param(api, s, "emphasis", "0.2500");    /* Inf retained current */
    assert_param(api, s, "contour", "0.0000");
    assert_param(api, s, "filt_fm", "0.4000");
    assert_param(api, s, "o1_range", "5");
    assert_param(api, s, "o1_wave", "0");

    /* Every factory patch must survive the playable MIDI range and extreme
     * public filter controls without contaminating internal state. */
    const int notes[] = {0, 36, 60, 84, 108, 127};
    for (int preset = 0; preset < NPRESETS; ++preset) {
        char preset_value[16];
        snprintf(preset_value, sizeof(preset_value), "%d", preset);
        api->set_param(s, "preset", preset_value);
        api->set_param(s, "cutoff", "999");
        api->set_param(s, "emphasis", "999");
        api->set_param(s, "contour", "999");
        api->set_param(s, "filt_fm", "999");
        api->set_param(s, "lfo_filter", "999");
        for (unsigned int i = 0; i < sizeof(notes) / sizeof(notes[0]); ++i) {
            uint8_t on[3] = {0x90, (uint8_t)notes[i], 127};
            uint8_t off[3] = {0x80, (uint8_t)notes[i], 0};
            api->on_midi(s, on, 3, MOVE_MIDI_SOURCE_INTERNAL);
            render_blocks(api, s, 4);
            api->on_midi(s, off, 3, MOVE_MIDI_SOURCE_INTERNAL);
        }
    }

    /* Deterministic combined-control fuzzing catches interactions that an
     * isolated min/max sweep misses. Values deliberately exceed the ABI's
     * documented ranges; set_param must contain them. */
    const char *float_keys[] = {
        "o2_tune", "o3_tune", "mix_o1", "mix_o2", "mix_o3", "mix_noise",
        "cutoff", "emphasis", "contour", "filt_fm", "filt_a", "filt_d",
        "filt_s", "loud_a", "loud_d", "loud_s", "glide", "master_tune",
        "mod_mix", "lfo_rate", "lfo_pitch", "lfo_filter", "volume"
    };
    const char *enum_keys[] = {
        "o1_range", "o1_wave", "o2_range", "o2_wave", "o3_range",
        "o3_wave", "o3_kbd", "noise_color", "kbd_track", "mod_osc",
        "mod_filter", "decay_sw", "lfo_shape", "lfo_sync", "lfo_div",
        "osc_sync"
    };
    char fuzz_value[32];
    int16_t fuzz_block[MOVE_FRAMES_PER_BLOCK * 2];
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const char *float_key = float_keys[fuzz_u32() %
            (sizeof(float_keys) / sizeof(float_keys[0]))];
        float hostile = ((int32_t)fuzz_u32() / 2147483648.0f) * 100.0f;
        snprintf(fuzz_value, sizeof(fuzz_value), "%.6f", hostile);
        api->set_param(s, float_key, fuzz_value);

        const char *enum_key = enum_keys[fuzz_u32() %
            (sizeof(enum_keys) / sizeof(enum_keys[0]))];
        snprintf(fuzz_value, sizeof(fuzz_value), "%d", (int)(fuzz_u32() % 40) - 10);
        api->set_param(s, enum_key, fuzz_value);

        if ((iteration % 17) == 0) {
            uint8_t random_note[3] = {0x90, (uint8_t)(fuzz_u32() & 0x7f), 127};
            api->on_midi(s, random_note, 3, MOVE_MIDI_SOURCE_INTERNAL);
        }
        api->render_block(s, fuzz_block, MOVE_FRAMES_PER_BLOCK);
        assert_finite_state(s);
    }

    /* A fundamental above the anti-alias ceiling must fade out, never be
     * converted into the old half-sample-rate alternating tone. */
    api->destroy_instance(s);
    s = api->create_instance(".", NULL);
    api->set_param(s, "o1_range", "5");
    api->set_param(s, "mix_o1", "1");
    api->set_param(s, "mix_o2", "0");
    api->set_param(s, "mix_o3", "0");
    api->set_param(s, "mix_noise", "0");
    uint8_t ultrasonic_note[3] = {0x90, 127, 127};
    api->on_midi(s, ultrasonic_note, 3, MOVE_MIDI_SOURCE_INTERNAL);
    render_blocks(api, s, 20);
    assert(fabsf(osc_render(&s->o1, 50000.0f, NULL)) < 1.0e-7f);

    api->destroy_instance(s);
    puts("DSP safety checks passed");
    return 0;
}
