/*
 * Mook D UI for Move Everything
 *
 * Uses the shared sound generator UI base for standard bank/patch preset
 * browsing (L/R = patch step, Shift+L/R = bank step, jog = patch scrub,
 * Up/Down = octave). Mono voice, so polyphony is not shown.
 * Parameter editing via shadow UI hierarchy when in chain context.
 */

import { createSoundGeneratorUI } from '/data/UserData/schwung/shared/sound_generator_ui.mjs';

const ui = createSoundGeneratorUI({
    moduleName: 'Mook D',

    onPresetChange: () => {
        host_module_set_param('all_notes_off', '1');
    },

    showPolyphony: false,
    showOctave: true,
});

globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
