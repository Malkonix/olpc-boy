/* coretest: dlopen a libretro core, run N frames with dummy callbacks, print fps.
 * usage: coretest core.so rom N */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libretro.h"

static int fmt = -1;
static unsigned vframes;
static size_t samples;

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt = *(int *)data; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "/tmp"; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: return false;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    default: return false;
    }
}
static void video_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; vframes++; }
static void audio_cb(int16_t l, int16_t r) { (void)l;(void)r; samples++; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; samples += n; return n; }
static void poll_cb(void) {}
static int16_t input_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p;(void)d;(void)i;(void)id; return 0; }

#define SYM(n) do { n = dlsym(h, "retro_" #n); if (!n) { fprintf(stderr, "missing retro_" #n "\n"); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s core.so rom frames\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void (*set_environment)(retro_environment_t), (*set_video_refresh)(retro_video_refresh_t),
         (*set_audio_sample)(retro_audio_sample_t), (*set_audio_sample_batch)(retro_audio_sample_batch_t),
         (*set_input_poll)(retro_input_poll_t), (*set_input_state)(retro_input_state_t),
         (*init)(void), (*run)(void), (*unload_game)(void), (*deinit)(void),
         (*get_system_info)(struct retro_system_info *), (*get_system_av_info)(struct retro_system_av_info *);
    bool (*load_game)(const struct retro_game_info *);
    SYM(set_environment); SYM(set_video_refresh); SYM(set_audio_sample); SYM(set_audio_sample_batch);
    SYM(set_input_poll); SYM(set_input_state); SYM(init); SYM(run); SYM(unload_game); SYM(deinit);
    SYM(get_system_info); SYM(get_system_av_info); SYM(load_game);

    set_environment(env_cb); init();
    set_video_refresh(video_cb); set_audio_sample(audio_cb); set_audio_sample_batch(audio_batch_cb);
    set_input_poll(poll_cb); set_input_state(input_cb);

    struct retro_system_info si; memset(&si, 0, sizeof si); get_system_info(&si);
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    void *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    struct retro_game_info gi = { argv[2], buf, (size_t)n, NULL };
    if (!load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 1; }

    struct retro_system_av_info av; memset(&av, 0, sizeof av); get_system_av_info(&av);
    int frames = atoi(argv[3]);
    struct timespec t0, t1;
    for (int i = 0; i < 60; i++) run();          /* warm-up */
    vframes = 0; samples = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < frames; i++) run();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    const char *fn = fmt == 0 ? "0RGB1555" : fmt == 1 ? "XRGB8888" : fmt == 2 ? "RGB565" : "unset(default 0RGB1555)";
    printf("%s %s: %dx%d (max %ux%u) core_fps=%.3f rate=%.0f Hz fmt=%s\n  ran %d frames in %.3fs = %.0f fps host (%.1fx realtime), video_cb=%u audio_samples=%zu\n",
           si.library_name, si.library_version, av.geometry.base_width, av.geometry.base_height,
           av.geometry.max_width, av.geometry.max_height, av.timing.fps, av.timing.sample_rate, fn,
           frames, s, frames / s, frames / s / av.timing.fps, vframes, samples);
    unload_game(); deinit();
    return 0;
}
