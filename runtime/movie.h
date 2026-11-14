// Native movie backend (ffmpeg) replacing the game's Media Foundation player (WmvPlayer, used by AwWmvWin32Movie).
// The game owns the clock: it advances its own position in microseconds and asks for the frame at that time.
#pragma once
#include <stdint.h>

typedef struct Movie Movie;

Movie *movie_open(const char *path);            // NULL if the file can't be opened or has no video
void movie_size(const Movie *m, int *w, int *h);

// Writes the frame shown at pos_us (32-bit BGRA, top row first, alpha 0xFF) into dst, at most maxw x maxh pixels.
// Returns 1 if a new frame was written, 0 if the frame at pos_us is the one already written, -1 once past the end.
int movie_frame_at(Movie *m, int64_t pos_us, uint8_t *dst, int pitch, int maxw, int maxh);

// Pulls up to `frames` interleaved stereo 48 kHz float frames. Returns frames written; fewer than asked = end.
// May run on an audio thread, but never concurrently with movie_rewind or movie_close (the caller locks).
int movie_audio(Movie *m, float *out, int frames);

void movie_rewind(Movie *m);                    // back to 0 for looping
void movie_close(Movie *m);
