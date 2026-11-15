/* host/coop.c as a process: "host" or "client", driven by tests/test_coop.py over loopback UDP. The microphone is a fake
   (a loud tone), the game's side is set by hand. Lines starting with "OUT " are what the test reads. */
#define _GNU_SOURCE
#include "../host/coop.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

void hos_log(const char *fmt, ...) { (void)fmt; }
static int tone;
void *hos_sdl_mic_open(uint32_t id, int freq, int ch, uint16_t fmt) { (void)id, (void)freq, (void)ch, (void)fmt; return &tone; }
int hos_sdl_mic_available(void *m) { (void)m; return 1920; }
int hos_sdl_mic_read(void *m, void *dst, int bytes) {
    (void)m;
    int16_t *p = dst;
    for (int i = 0; i < bytes / 2; i++) p[i] = (int16_t)(8000 * sin(tone++ * 0.1));
    return bytes;
}
void hos_sdl_mic_close(void *m) { (void)m; }

static void sleep_ms(int ms) { usleep(ms * 1000); }
static void show(const char *what) {
    MkwVkCoopStatus s;
    coop_status(&s);
    printf("OUT %s mode=%d started=%d count=%d speaking=%d msg=[%s]", what, s.mode, s.started, s.count, s.speaking, s.message);
    for (int i = 0; i < s.count; i++) printf(" p%d=%s%s", i, s.players[i].name, s.players[i].self ? "*" : "");
    printf("\n");
    fflush(stdout);
}
static void place(float x, const char *map) {
    CoopLocal l = {1, 0, 0, {0}, {x, 0, 0}, 0};
    snprintf(l.map, sizeof l.map, "%s", map);
    coop_set_local(&l);
}
int main(int argc, char **argv) {
    const char *role = argc > 1 ? argv[1] : "host";
    const char *name = argc > 2 ? argv[2] : "Tester";
    const char *key = argc > 3 ? argv[3] : "";
    const int secs = argc > 4 ? atoi(argv[4]) : 6;
    setenv("HOS_COOP_PORT", "47770", 1);
    coop_command(MKWVK_COOP_NAME, name, 0);
    coop_command(MKWVK_COOP_VOICE, NULL, MKWVK_COOP_VOICE_ACTIVITY);
    if (*key) coop_command(MKWVK_COOP_KEY, key, 0), coop_command(MKWVK_COOP_KEY_ON, NULL, 1);
    else coop_command(MKWVK_COOP_KEY_ON, NULL, 0);
    if (!strcmp(role, "host")) {
        printf("OUT host-command=%d\n", coop_command(MKWVK_COOP_HOST, NULL, 0));
        place(0, "Kokiri");
    } else {
        coop_command(MKWVK_COOP_ADDRESS, "127.0.0.1:47770", 0);
        printf("OUT join-command=%d\n", coop_command(MKWVK_COOP_JOIN, NULL, 0));
        place(5, "Kokiri");
    }
    fflush(stdout);
    for (int t = 0; t < secs * 10; t++) {
        sleep_ms(100);
        if (t % 5 == 0) place(!strcmp(role, "host") ? 0 : 5, "Kokiri");
        if (!strcmp(role, "host") && t == 20) printf("OUT start=%d\n", coop_command(MKWVK_COOP_START, NULL, 0)), fflush(stdout);
        if (t % 10 == 9) show("tick");
        if (t > 25 && t % 5 == 0) {   /* the voice of the other, if heard: mixed into a frame */
            int16_t pcm[240 * 2] = {0};
            coop_voice_mix(pcm, 2, 240);
            int peak = 0;
            for (int i = 0; i < 480; i++) if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
            if (peak) { printf("OUT voice-heard peak=%d\n", peak); fflush(stdout); }
        }
    }
    show("end");
    coop_command(MKWVK_COOP_LEAVE, NULL, 0);
    show("left");
    printf("OUT gain %.3f %.3f %.3f\n", coop_voice_gain(1), coop_voice_gain(16.5f), coop_voice_gain(31));
    return 0;
}
