/*
 * audio.c — configure initial audio levels.
 *
 * On Ubuntu 26.04 (PipeWire + WirePlumber + sof-hda-dsp driver) audio
 * devices are auto-discovered by WirePlumber; no module loading is
 * needed. This module only applies the initial volume/mute state from
 * the configuration file.
 *
 * Volume values from cfg (0..100):
 *   audio_volumen_microfono — applied to @DEFAULT_SOURCE@. 0 = mute.
 *   audio_volumen_altavoces — applied to @DEFAULT_SINK@.   0 = mute.
 *
 * WirePlumber selects the best source/sink automatically (DMIC when
 * no external mic, headset when plugged in; HDMI output or headphones
 * similarly), so @DEFAULT_SOURCE@ / @DEFAULT_SINK@ are always correct.
 *
 * zbd-system.service starts before the audio stack; the first call at
 * boot will fail and the caller logs a warning without aborting.
 * zbd-tray retries at session start when PipeWire is up.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "comun.h"
#include "exec.h"

static int pactl_server_reachable(void)
{
    char *const args[] = { "pactl", "info", NULL };
    return exec_cmd_argv("pactl", args) == 0;
}

/* Apply volume + mute to a capture source or playback sink.
 * set_vol / set_mute are the pactl subcommands, e.g.:
 *   "set-source-volume" / "set-source-mute"
 *   "set-sink-volume"   / "set-sink-mute"
 */
static int apply_audio_level(const char *target, int level,
                             const char *set_vol, const char *set_mute)
{
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", level);

    char *const vol_args[] = {
        "pactl", (char *)set_vol, (char *)target, vol_str, NULL
    };
    if (exec_cmd_argv("pactl", vol_args) != 0)
    {
        fprintf(stderr, "configurar_audio: %s %s fallo\n", set_vol, target);
        return -1;
    }

    const char *mute_val = (level == 0) ? "true" : "false";
    char *const mute_args[] = {
        "pactl", (char *)set_mute, (char *)target, (char *)mute_val, NULL
    };
    exec_cmd_argv("pactl", mute_args);
    return 0;
}

int configurar_dmic_raw(void)
{
    if (!pactl_server_reachable())
    {
        fprintf(stderr,
                "configurar_audio: PipeWire no responde "
                "(XDG_RUNTIME_DIR=%s); saltando configuracion de audio.\n",
                getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        return EXIT_FAILURE;
    }

    int mic_vol = cfg ? cfg->audio_volumen_microfono : 70;
    int spk_vol = cfg ? cfg->audio_volumen_altavoces : 80;

    int ok = 1;
    if (apply_audio_level("@DEFAULT_SOURCE@", mic_vol,
                          "set-source-volume", "set-source-mute") != 0) ok = 0;
    if (apply_audio_level("@DEFAULT_SINK@",   spk_vol,
                          "set-sink-volume",   "set-sink-mute")   != 0) ok = 0;

    if (!ok)
    {
        fprintf(stderr, "configurar_audio: uno o mas ajustes de volumen fallaron\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
