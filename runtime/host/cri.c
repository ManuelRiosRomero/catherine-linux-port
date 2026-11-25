// CRI sound output. CRI's library asks COM for XAudio2, which isn't there, and falls back to its "dummy" output
// (object built at 0x55d946): a worker thread (0x7a4b10) that every 10 ms has CRI's mixer render the samples that came
// due (6 planar float channels, 5.1, 48 kHz, into 6 buffers of 4096 at +0x21c) and then drops them. This native worker
// makes the same mixer call and plays the result through SDL3, which downmixes 5.1 to the device.
#include "host.h"
#include <SDL3/SDL.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#define O_STATE 0x4        // 1 = playing
#define O_MIX 0x208        // cdecl mix(user, channels, float **buffers, samples)
#define O_USER 0x20c
#define O_RATE 0x214       // 48000
#define O_BUFS 0x21c       // 6 x 0x4000 bytes
#define O_CS 0x2021c
#define O_EVENT 0x20238    // set by a 10 ms timeSetEvent
#define O_STOP 0x20240
#define BLOCK 128          // CRI mixes whole 128-sample blocks, at most 4096 per call

// CRI dummy output worker thread (stdcall, param = output object)
NATIVE(0x7a4b10, cri_output_worker) {
    uint32_t o = ARG(c, 0), rate = U32(o + O_RATE), ptrs = gmalloc(6 * 4);
    // The output's device count (vtable slot +0x18) gates every "is a voice still playing" check of the game (message
    // window 0x5ced80: lines wait for their voice; 0x792250 and ~8 more). XAudio2's answers GetDeviceCount (>= 1 on a
    // PC); the dummy's slot (0xba361c + 0x18) points at a shared return-0 stub, so lines advanced when typing ended and
    // cut the voice. We do have a device: point the slot at the exe's own "mov eax,1; ret" (0x571bc0).
    U32(0xba3634) = 0x571bc0;
    for (int k = 0; k < 6; k++) U32(ptrs + 4 * k) = o + O_BUFS + 0x4000 * k;
    SDL_AudioStream *s = NULL;
    // CRI's 6 channels are FL FR BL BR FC LFE (measured: during music 2+3 carry a matched ambience pair and 4+5 are
    // silent; voices sat in 4, which SDL's FL FR FC LFE BL BR order played as back-left = left speaker only)
    static const int sdl_from_cri[6] = {0, 1, 4, 5, 2, 3};
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 6, (int)rate};
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) && (s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)))
        SDL_ResumeAudioStreamDevice(s);
    else
        fprintf(stderr, "[cri] no audio output: %s\n", SDL_GetError());
    // keep ~40 ms queued: the device's own clock paces the mixer (no drift against a timer)
    uint32_t target = rate / 25, last = SDL_GetTicks(), dropouts = 0, report = last + 10000, primed = 0;
    static float out[4096 * 6];
    static double rms[6], rms_n;
    while (!U32(o + O_STOP)) {
        wait_handle(U32(o + O_EVENT), 0xFFFFFFFF);
        guest_cs(c, o + O_CS, 1);
        uint32_t queued = s ? (uint32_t)SDL_GetAudioStreamQueued(s) / 24 : 0, now = SDL_GetTicks();
        // without a device, keep CRI's own pace: the samples that came due on the wall clock
        uint32_t n = s ? (queued < target ? target - queued : 0) : (now - last) * rate / 1000;
        // the device ran dry since the last mix: an audible gap (crackle/stutter); counted for the self-test log
        if (s && primed && queued == 0) dropouts++;
        if (now >= report) {
            if (dropouts) fprintf(stderr, "[cri] %u dropouts in the last 10 s\n", dropouts);
            if (rms_n) {   // which channel carries what (voices, music): the speaker order check
                fprintf(stderr, "[cri] channel RMS dB (FL FR FC LFE BL BR):");
                for (int k = 0; k < 6; k++) fprintf(stderr, " %.1f", rms[k] > 0 ? 10 * log10(rms[k] / rms_n) : -99.0);
                fputc('\n', stderr);
            }
            memset(rms, 0, sizeof rms), rms_n = 0;
            dropouts = 0;
            report = now + 10000;
        }
        if (n > 4096) n = 4096;
        n &= ~(BLOCK - 1);
        if (n) last = now;
        if (n && U32(o + O_MIX) && U32(o + O_STATE) == 1) {
            // One block per call: the mix holds CRI's locks, which the game's main thread takes every frame; a 10 ms
            // mix in translated x87 code held them ~3 ms (the bar's frame waited on it). Between blocks the locks are
            // free and a short pause lets a woken waiter take them before this loop does again (mutexes aren't fair).
            for (uint32_t done = 0; done < n; done += BLOCK) {
                if (done) {
                    guest_cs(c, o + O_CS, 0); usleep(20); guest_cs(c, o + O_CS, 1);
                    if (!U32(o + O_MIX) || U32(o + O_STATE) != 1) { n = done; break; }   // stopped meanwhile
                }
                for (int k = 0; k < 6; k++) U32(ptrs + 4 * k) = o + O_BUFS + 0x4000 * k + 4 * done;
                guest_call(c, U32(o + O_MIX), 0, 4, U32(o + O_USER), 6, ptrs, BLOCK);
            }
            if (s) {
                for (uint32_t i = 0; i < n; i++)
                    for (int k = 0; k < 6; k++) {
                        float v = V(GM(o + O_BUFS + 0x4000 * sdl_from_cri[k] + 4 * i), float);
                        out[6 * i + k] = v;
                        rms[k] += (double)v * v;
                    }
                rms_n += n;
                SDL_PutAudioStreamData(s, out, (int)(n * 24));
                primed = 1;
            }
        }
        guest_cs(c, o + O_CS, 0);
    }
    if (s) SDL_DestroyAudioStream(s);
    gfree(ptrs);
    ret_std(c, 0, 1);
}
