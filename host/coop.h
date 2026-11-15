/* (hookshot) Co-op: a host opens a session on a UDP port, friends join it by its address (and a key, when the host set one), the
   host starts it, and from then on nobody new can join. Once started, everyone's Link position is shared, the other players
   show over the picture (a name and a marker, coop_marks), and proximity chat carries the microphones: a voice is heard by
   those on the same map, loud close by and fading to nothing at COOP_VOICE_RANGE metres (coop_voice_mix).

   host/coop.c has the whole of it and touches no game memory: the game's side is a few calls from host/hos_main.c
   (coop_set_local, coop_set_camera, coop_note_fov), host/nn_audio.c (coop_voice_mix) and the renderer's menu (the
   coop_command, coop_status and coop_marks that host/rx_backend.c hands it, gpu/vk/vk_api.h version 23). docs/coop.md says
   how it works and what it does not do (the other players are drawn as markers; the traffic is not encrypted). */
#pragma once
#include <stdint.h>

#include "../gpu/vk/vk_api.h"   /* (MkwVkCoopStatus, MkwVkCoopMark and the command numbers) */

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_DEFAULT_PORT 7770
#define COOP_NAME_MAX 24   /* (bytes, with the end) */
#define COOP_MAP_MAX 24
#define COOP_KEY_MAX 48
#define COOP_VOICE_REF 3.0f      /* (metres: as loud as at the speaker's side up to here) */
#define COOP_VOICE_RANGE 30.0f   /* (metres: heard no further than this) */

/* The menu's (MkwVkHost.coop_command): command is MKWVK_COOP_*; 1 when taken, 0 when not (the status' message says why) */
int coop_command(int command, const char *text, int value);
void coop_status(MkwVkCoopStatus *out);
/* Where the other players are in the picture (MkwVkHost.coop_marks): aspect is the picture's width over its height; each
   mark's x and y are 0 to 1 across and down the picture, offscreen ones on its edge. Up to max, the count */
int coop_marks(MkwVkCoopMark *out, int max, float aspect);

/* The game's thread, every update of Link (a few times a second is enough): where he is. valid 0: there is no Link in a world
   now (a load, the title). event 1: a cutscene or a talk has him (nobody hears or sees him then) */
typedef struct {
    int valid, child, event;
    char map[COOP_MAP_MAX];
    float pos[3], yaw;
} CoopLocal;
void coop_set_local(const CoopLocal *local);
/* The gameplay camera, once a frame (eye and the point it looks at, the game's units: metres), and its vertical field of view
   in radians. coop_wants_fov: whether the angle is wanted now (a session has started): it costs the game a few reads */
void coop_set_camera(const double eye[3], const double target[3]);
int coop_wants_fov(void);
void coop_note_fov(float fovy);

/* The audio mixer's thread: the other players' voices added to the frame of the final mix, 48 kHz, 2 or 6 channels */
void coop_voice_mix(int16_t *pcm, int channels, int frames);

/* Where the player's choices (name, address, key on or off, voice mode; never the key) are kept, or nothing when not set;
   and the push-to-talk probe: whether the talk key is down and the game has the keyboard */
void coop_set_prefs_path(const char *path);
void coop_set_ptt_probe(int (*probe)(void));

/* A session running (hosting or joined) and started */
int coop_active(void);
void coop_shutdown(void);   /* (the process ends: leaves the session, joins the thread) */

/* ---- for tests/test_coop.py ---- */
float coop_voice_gain(float distance);   /* 1 up to COOP_VOICE_REF, then fading to 0 at COOP_VOICE_RANGE */

#ifdef __cplusplus
}
#endif
