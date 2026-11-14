// ffmpeg movie backend. Video and audio each get their own demuxer, so the audio thread never shares state
// with the game thread (the file is just read twice; cutscenes are a few MB per minute).
#include "movie.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frm;
    int idx, eof;
    int64_t start;
} Stream;

struct Movie {
    Stream v, a;
    struct SwsContext *sws;
    SwrContext *swr;
    int w, h;
    AVFrame *cur, *next;                // frame on screen, and the one after it
    int have_cur, have_next, written, v_end;
    int64_t cur_pts, next_pts, frame_us;
    float *abuf;                        // converted audio not yet handed out
    int acap, afill, apos, a_end;
};

static int open_stream(Stream *s, const char *path, enum AVMediaType type) {
    const AVCodec *codec;
    memset(s, 0, sizeof *s);
    if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(s->fmt, NULL) < 0) return -1;
    if ((s->idx = av_find_best_stream(s->fmt, type, -1, -1, &codec, 0)) < 0) return -1;
    AVStream *st = s->fmt->streams[s->idx];
    if (!(s->dec = avcodec_alloc_context3(codec))) return -1;
    if (avcodec_parameters_to_context(s->dec, st->codecpar) < 0) return -1;
    if (avcodec_open2(s->dec, codec, NULL) < 0) return -1;
    s->pkt = av_packet_alloc();
    s->frm = av_frame_alloc();
    s->start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    return s->pkt && s->frm ? 0 : -1;
}

static void close_stream(Stream *s) {
    av_frame_free(&s->frm);
    av_packet_free(&s->pkt);
    avcodec_free_context(&s->dec);
    avformat_close_input(&s->fmt);
}

// Decodes the next frame of this stream into s->frm. 0 = got one, -1 = end of stream.
static int next_frame(Stream *s) {
    for (;;) {
        int r = avcodec_receive_frame(s->dec, s->frm);
        if (r == 0) return 0;
        if (r != AVERROR(EAGAIN)) return -1;   // AVERROR_EOF after the final flush, or a hard error
        if (av_read_frame(s->fmt, s->pkt) < 0) {
            if (s->eof) return -1;
            s->eof = 1;
            avcodec_send_packet(s->dec, NULL); // flush: drain the frames the decoder still holds
            continue;
        }
        if (s->pkt->stream_index == s->idx) avcodec_send_packet(s->dec, s->pkt);  // a corrupt packet only drops that frame
        av_packet_unref(s->pkt);
    }
}

static int64_t pts_us(const Stream *s, const AVFrame *f) {
    int64_t ts = f->best_effort_timestamp == AV_NOPTS_VALUE ? s->start : f->best_effort_timestamp;
    return av_rescale_q(ts - s->start, s->fmt->streams[s->idx]->time_base, AV_TIME_BASE_Q);
}

static int setup_swr(Movie *m) {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    swr_free(&m->swr);
    if (swr_alloc_set_opts2(&m->swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &m->a.dec->ch_layout,
                            m->a.dec->sample_fmt, m->a.dec->sample_rate, 0, NULL) < 0) return -1;
    return swr_init(m->swr);
}

Movie *movie_open(const char *path) {
    Movie *m = calloc(1, sizeof *m);
    if (!m || open_stream(&m->v, path, AVMEDIA_TYPE_VIDEO) < 0) { movie_close(m); return NULL; }
    if (open_stream(&m->a, path, AVMEDIA_TYPE_AUDIO) < 0 || setup_swr(m) < 0) {
        close_stream(&m->a);            // silent movie: video still plays
        m->a_end = 1;
    }
    m->w = m->v.dec->width;
    m->h = m->v.dec->height;
    // SWS_BICUBIC matches the ffmpeg command line's default, which the self-test compares against
    m->sws = sws_getContext(m->w, m->h, m->v.dec->pix_fmt, m->w, m->h, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
    m->cur = av_frame_alloc();
    m->next = av_frame_alloc();
    AVRational fr = m->v.fmt->streams[m->v.idx]->r_frame_rate;
    m->frame_us = fr.num ? av_rescale_q(1, av_inv_q(fr), AV_TIME_BASE_Q) : 33333;
    if (!m->sws || !m->cur || !m->next) { movie_close(m); return NULL; }
    return m;
}

void movie_size(const Movie *m, int *w, int *h) { *w = m->w; *h = m->h; }

int movie_frame_at(Movie *m, int64_t pos_us, uint8_t *dst, int pitch, int maxw, int maxh) {
    if (m->w > maxw || m->h > maxh) return -1;   // the game's texture is a power of two >= the video, so this never hits
    for (;;) {
        if (!m->have_next && !m->v_end) {
            if (next_frame(&m->v) == 0) {
                av_frame_unref(m->next);
                av_frame_move_ref(m->next, m->v.frm);
                m->next_pts = pts_us(&m->v, m->next);
                m->have_next = 1;
            } else {
                m->v_end = 1;
            }
        }
        if (!m->have_next || (m->have_cur && m->next_pts > pos_us)) break;
        AVFrame *t = m->cur; m->cur = m->next; m->next = t;
        m->cur_pts = m->next_pts;
        m->have_cur = 1;
        m->have_next = 0;
        m->written = 0;
    }
    if (!m->have_cur || (m->v_end && pos_us >= m->cur_pts + m->frame_us)) return -1;
    if (m->written) return 0;
    uint8_t *planes[4] = {dst};
    int pitches[4] = {pitch};
    sws_scale(m->sws, (const uint8_t *const *)m->cur->data, m->cur->linesize, 0, m->h, planes, pitches);
    m->written = 1;
    return 1;
}

int movie_audio(Movie *m, float *out, int frames) {
    int n = 0;
    while (n < frames) {
        if (m->apos < m->afill) {
            int k = m->afill - m->apos < frames - n ? m->afill - m->apos : frames - n;
            memcpy(out + 2 * n, m->abuf + 2 * m->apos, (size_t)k * 2 * sizeof(float));
            m->apos += k;
            n += k;
            continue;
        }
        if (m->a_end) break;
        int got = next_frame(&m->a) == 0;
        if (!got) m->a_end = 1;          // one last swr_convert below flushes the resampler's tail
        int cap = swr_get_out_samples(m->swr, got ? m->a.frm->nb_samples : 0);
        if (cap > m->acap) {
            float *b = realloc(m->abuf, (size_t)cap * 2 * sizeof(float));
            if (!b) { m->a_end = 1; break; }
            m->abuf = b;
            m->acap = cap;
        }
        uint8_t *o[1] = {(uint8_t *)m->abuf};
        int r = swr_convert(m->swr, o, m->acap, got ? (const uint8_t **)m->a.frm->extended_data : NULL,
                            got ? m->a.frm->nb_samples : 0);
        m->afill = r > 0 ? r : 0;
        m->apos = 0;
    }
    return n;
}

static void rewind_stream(Stream *s) {
    av_seek_frame(s->fmt, s->idx, s->start, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(s->dec);
    s->eof = 0;
}

void movie_rewind(Movie *m) {
    rewind_stream(&m->v);
    m->have_cur = m->have_next = m->written = m->v_end = 0;
    if (m->a.fmt) {
        rewind_stream(&m->a);
        setup_swr(m);                   // drop samples buffered inside the resampler
        m->afill = m->apos = m->a_end = 0;
    }
}

void movie_close(Movie *m) {
    if (!m) return;
    close_stream(&m->v);
    close_stream(&m->a);
    sws_freeContext(m->sws);
    swr_free(&m->swr);
    av_frame_free(&m->cur);
    av_frame_free(&m->next);
    free(m->abuf);
    free(m);
}
