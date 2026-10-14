// dm_prefix.h - force-included in every dmusic source the kit compiles (dx/CMakeLists.txt).
// Not upstream's: see ../README.md.
//
// dmusic is linked into the same executable as the kit's own TinySoundFont
// (third_party/tsf, host/audio/midi_synth.cpp), and its vendor/TinySoundFont is
// a modified copy with different structures and signatures behind the same
// tsf_* names. Its C11-threads shim and a few helpers are global names too.
// Every external symbol the library defines and does not export as Dm* is
// renamed here, so neither copy can bind to the other's definitions.
//
// Regenerate the list after an upstream update: compile the library and list
// `nm -g -U` over its objects, minus the Dm* names.
#pragma once

#define SFGeneratorList_add dm_SFGeneratorList_add
#define SFGeneratorList_free dm_SFGeneratorList_free
#define SFGeneratorList_get dm_SFGeneratorList_get
#define SFGeneratorList_init dm_SFGeneratorList_init
#define SFModulatorList_add dm_SFModulatorList_add
#define SFModulatorList_free dm_SFModulatorList_free
#define SFModulatorList_get dm_SFModulatorList_get
#define SFModulatorList_init dm_SFModulatorList_init
#define call_once dm_call_once
#define clamp_s32 dm_clamp_s32
#define cnd_broadcast dm_cnd_broadcast
#define cnd_destroy dm_cnd_destroy
#define cnd_init dm_cnd_init
#define cnd_signal dm_cnd_signal
#define cnd_timedwait dm_cnd_timedwait
#define cnd_wait dm_cnd_wait
#define lerp dm_lerp
#define max_s32 dm_max_s32
#define max_usize dm_max_usize
#define min_u8 dm_min_u8
#define mtx_destroy dm_mtx_destroy
#define mtx_init dm_mtx_init
#define mtx_lock dm_mtx_lock
#define mtx_timedlock dm_mtx_timedlock
#define mtx_trylock dm_mtx_trylock
#define mtx_unlock dm_mtx_unlock
#define thrd_create dm_thrd_create
#define thrd_current dm_thrd_current
#define thrd_detach dm_thrd_detach
#define thrd_equal dm_thrd_equal
#define thrd_exit dm_thrd_exit
#define thrd_join dm_thrd_join
#define thrd_sleep dm_thrd_sleep
#define thrd_yield dm_thrd_yield
#define tsf_active_voice_count dm_tsf_active_voice_count
#define tsf_bank_get_presetname dm_tsf_bank_get_presetname
#define tsf_bank_note_off dm_tsf_bank_note_off
#define tsf_bank_note_on dm_tsf_bank_note_on
#define tsf_channel_get_pan dm_tsf_channel_get_pan
#define tsf_channel_get_pitchrange dm_tsf_channel_get_pitchrange
#define tsf_channel_get_pitchwheel dm_tsf_channel_get_pitchwheel
#define tsf_channel_get_preset_bank dm_tsf_channel_get_preset_bank
#define tsf_channel_get_preset_index dm_tsf_channel_get_preset_index
#define tsf_channel_get_preset_number dm_tsf_channel_get_preset_number
#define tsf_channel_get_tuning dm_tsf_channel_get_tuning
#define tsf_channel_get_volume dm_tsf_channel_get_volume
#define tsf_channel_midi_control dm_tsf_channel_midi_control
#define tsf_channel_note_off dm_tsf_channel_note_off
#define tsf_channel_note_off_all dm_tsf_channel_note_off_all
#define tsf_channel_note_on dm_tsf_channel_note_on
#define tsf_channel_set_bank dm_tsf_channel_set_bank
#define tsf_channel_set_bank_preset dm_tsf_channel_set_bank_preset
#define tsf_channel_set_pan dm_tsf_channel_set_pan
#define tsf_channel_set_pitchrange dm_tsf_channel_set_pitchrange
#define tsf_channel_set_pitchwheel dm_tsf_channel_set_pitchwheel
#define tsf_channel_set_presetindex dm_tsf_channel_set_presetindex
#define tsf_channel_set_presetnumber dm_tsf_channel_set_presetnumber
#define tsf_channel_set_tuning dm_tsf_channel_set_tuning
#define tsf_channel_set_volume dm_tsf_channel_set_volume
#define tsf_channel_sounds_off_all dm_tsf_channel_sounds_off_all
#define tsf_close dm_tsf_close
#define tsf_copy dm_tsf_copy
#define tsf_get_presetcount dm_tsf_get_presetcount
#define tsf_get_presetindex dm_tsf_get_presetindex
#define tsf_get_presetname dm_tsf_get_presetname
#define tsf_load dm_tsf_load
#define tsf_load_filename dm_tsf_load_filename
#define tsf_load_memory dm_tsf_load_memory
#define tsf_load_presets dm_tsf_load_presets
#define tsf_note_off dm_tsf_note_off
#define tsf_note_off_all dm_tsf_note_off_all
#define tsf_note_on dm_tsf_note_on
#define tsf_render_float dm_tsf_render_float
#define tsf_render_short dm_tsf_render_short
#define tsf_reset dm_tsf_reset
#define tsf_set_max_voices dm_tsf_set_max_voices
#define tsf_set_output dm_tsf_set_output
#define tsf_set_volume dm_tsf_set_volume
#define tss_create dm_tss_create
#define tss_delete dm_tss_delete
#define tss_get dm_tss_get
#define tss_set dm_tss_set
