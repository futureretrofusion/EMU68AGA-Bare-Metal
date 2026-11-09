/* agaboot - the Chipset menu's Sound window and the mix settings (sound.c).
 * Keep this and sound.c in step, or spend the afternoon chasing silence.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_SOUND_H
#define AGABOOT_SOUND_H

struct sound_prefs {
    int hdmi;           /* sound on HDMI: 0 off, 1 on (ENVARC:AGA_HDMISOUND) */
    int filter;         /* 0 off, 1 A500, 2 A1200 (ENVARC:AGA_HDMIFILTER) */
    int vol;            /* HDMI volume: 0..4 = 100, 150, 200, 300, 400 % (ENVARC:AGA_HDMIVOL) */
    int width;          /* stereo, both outputs: 0..4 = Amiga, 75, 50, 25 %, mono (ENVARC:AGA_STEREO) */
    int swap;           /* left and right swapped, both outputs (ENVARC:AGA_SWAP) */
    int ahi16;          /* HiFi sound: AHI programs on HDMI (ENVARC:AGA_AHI16, ahi16.c) */
    int paula;          /* Paula on HDMI: 0 classic, 1 band-limited (ENVARC:AGA_HDMIPAULA) */
    int rate;           /* HDMI sound rate: 0 follow (EDID), 1 follow (any), 2 96 kHz, 3 192 kHz (ENVARC:AGA_AUDIORATE) */
};

void sound_load(struct sound_prefs *s);
/* filter, volume, stereo and swap into ENV: - and ENVARC: when save. HDMI
   on/off is agaboot.c's, with config.txt's hdmi_drive=2. */
void sound_store_mix(const struct sound_prefs *s, int save);
void sound_send_mix(const struct sound_prefs *s);   /* volume, stereo, swap to the kernel */
const char *sound_vol_name(int i);                  /* "200 %" */
const char *sound_width_name(int i);                /* "Amiga", "75 %", ..., "Mono" */
int  sound_parse_vol(const char *w);                /* "200" or "200%" -> index, -1 */
int  sound_parse_width(const char *w);              /* "AMIGA", "75", ..., "MONO" -> index, -1 */
int  sound_parse_rate(const char *w);               /* "SCREEN", "ANY", "96", "192" -> 0..3, -1 */
const char *sound_rate_name(int i);                 /* "Follow screen", ..., "192 kHz" */
/* Chipset menu - Sound. 0 Cancel (the old mix is sent back), 1 Use, 2 Save;
   *s holds the choice. Filter, volume, stereo and swap are heard at once. */
int  sound_window(struct sound_prefs *s);

/* HiFi volume (1.118): AHI programs' sound on HDMI, which the HDMI volume
   leaves alone - dB down, 0 to 62, and muted or not (ENVARC:AGA_HIFIVOL,
   "-12" or "-12 MUTE"; 0 dB when not set). The Sound monitor sets it. */
int  hifivol_pref(int *mute);
void hifivol_store(int db, int mute, int save);
void hifivol_send(int db, int mute);                /* $5180 + E6(d), 63 silence */
int  hifivol_parse(const char *w, int *db, int *mute);   /* "-12", "12", "0", "MUTE", "UNMUTE": 1 when understood */

/* agaboot.c */
int  hdmifilter_pref(void);
void hdmifilter_send(int m);

#endif
