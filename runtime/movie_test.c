// Self-test: steps the clock through a movie 1 ms at a time and prints one MD5 per new video frame ("V <md5>"),
// then the MD5 of all audio as interleaved stereo float ("A <md5>"). movie_test.sh diffs this against the ffmpeg CLI.
#include "movie.h"
#include <libavutil/md5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex(const char *tag, const uint8_t d[16]) {
    printf("%s ", tag);
    for (int i = 0; i < 16; i++) printf("%02x", d[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    Movie *m = movie_open(argv[1]);
    if (!m) { fprintf(stderr, "open failed: %s\n", argv[1]); return 1; }
    int w, h;
    movie_size(m, &w, &h);
    uint8_t *px = malloc((size_t)w * h * 4), d[16], first[16];
    int frames = 0, r;
    for (int64_t t = 0; (r = movie_frame_at(m, t, px, w * 4, w, h)) >= 0; t += 1000) {
        if (r == 0) continue;
        av_md5_sum(d, px, w * h * 4);
        if (!frames++) memcpy(first, d, 16);
        hex("V", d);
    }
    struct AVMD5 *ctx = av_md5_alloc();
    float buf[2 * 1024];
    long samples = 0;
    av_md5_init(ctx);
    for (int n; (n = movie_audio(m, buf, 1024)) > 0; samples += n) av_md5_update(ctx, (uint8_t *)buf, n * 8);
    av_md5_final(ctx, d);
    hex("A", d);

    movie_rewind(m);                       // looping must restart at the very first frame
    movie_frame_at(m, 0, px, w * 4, w, h);
    av_md5_sum(d, px, w * h * 4);
    fprintf(stderr, "%dx%d, %d frames, %ld audio frames (%.2f s), rewind %s\n", w, h, frames, samples,
            samples / 48000.0, memcmp(d, first, 16) ? "BROKEN" : "ok");
    movie_close(m);
    return memcmp(d, first, 16) != 0;
}
