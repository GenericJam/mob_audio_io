/*
 * mai_decode.c (Android): decode/2 with AMediaExtractor + AMediaCodec (WAV,
 * MP3, AAC/M4A, FLAC, Ogg: whatever the phone's decoders take). Included by
 * mob_audio_io_nif.c after ../common/mai_nif.c; ported from Humdinger's
 * and Magpie's decode_file.
 *
 * libmediandk.so is opened with dlopen, as AAudio is: the app library
 * needs no extra link flag. The headers give only the types; nothing here
 * references a libmediandk symbol directly (not even the AMEDIAFORMAT_KEY_*
 * strings, which are variables in the library).
 */
#include <errno.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>

/* AudioFormat.ENCODING_PCM_*: what a decoder's "pcm-encoding" says. */
#define ENC_PCM_16BIT 2
#define ENC_PCM_8BIT 3
#define ENC_PCM_FLOAT 4
#define ENC_PCM_24BIT_PACKED 21
#define ENC_PCM_32BIT 22

#define DEQUEUE_US 5000
/* This many empty dequeues in a row (about 10 s) and the decoder is stuck. */
#define STALL_TRIES 2000

static struct {
    bool ok;
    __typeof__(AMediaExtractor_new) *exNew;
    __typeof__(AMediaExtractor_delete) *exDelete;
    __typeof__(AMediaExtractor_setDataSourceFd) *exSetDataSourceFd;
    __typeof__(AMediaExtractor_getTrackCount) *exGetTrackCount;
    __typeof__(AMediaExtractor_getTrackFormat) *exGetTrackFormat;
    __typeof__(AMediaExtractor_selectTrack) *exSelectTrack;
    __typeof__(AMediaExtractor_readSampleData) *exReadSampleData;
    __typeof__(AMediaExtractor_getSampleTime) *exGetSampleTime;
    __typeof__(AMediaExtractor_advance) *exAdvance;
    __typeof__(AMediaFormat_getString) *fmtGetString;
    __typeof__(AMediaFormat_getInt32) *fmtGetInt32;
    __typeof__(AMediaFormat_delete) *fmtDelete;
    __typeof__(AMediaCodec_createDecoderByType) *codecCreateDecoderByType;
    __typeof__(AMediaCodec_configure) *codecConfigure;
    __typeof__(AMediaCodec_start) *codecStart;
    __typeof__(AMediaCodec_stop) *codecStop;
    __typeof__(AMediaCodec_delete) *codecDelete;
    __typeof__(AMediaCodec_dequeueInputBuffer) *codecDequeueInputBuffer;
    __typeof__(AMediaCodec_getInputBuffer) *codecGetInputBuffer;
    __typeof__(AMediaCodec_queueInputBuffer) *codecQueueInputBuffer;
    __typeof__(AMediaCodec_dequeueOutputBuffer) *codecDequeueOutputBuffer;
    __typeof__(AMediaCodec_getOutputBuffer) *codecGetOutputBuffer;
    __typeof__(AMediaCodec_releaseOutputBuffer) *codecReleaseOutputBuffer;
    __typeof__(AMediaCodec_getOutputFormat) *codecGetOutputFormat;
} M;

static pthread_once_t g_media_once = PTHREAD_ONCE_INIT;

static void media_load(void) {
    void *h = dlopen("libmediandk.so", RTLD_NOW);
    if (!h) return;
#define S(f, n) \
    if (!(*(void **)&M.f = dlsym(h, n))) return
    S(exNew, "AMediaExtractor_new");
    S(exDelete, "AMediaExtractor_delete");
    S(exSetDataSourceFd, "AMediaExtractor_setDataSourceFd");
    S(exGetTrackCount, "AMediaExtractor_getTrackCount");
    S(exGetTrackFormat, "AMediaExtractor_getTrackFormat");
    S(exSelectTrack, "AMediaExtractor_selectTrack");
    S(exReadSampleData, "AMediaExtractor_readSampleData");
    S(exGetSampleTime, "AMediaExtractor_getSampleTime");
    S(exAdvance, "AMediaExtractor_advance");
    S(fmtGetString, "AMediaFormat_getString");
    S(fmtGetInt32, "AMediaFormat_getInt32");
    S(fmtDelete, "AMediaFormat_delete");
    S(codecCreateDecoderByType, "AMediaCodec_createDecoderByType");
    S(codecConfigure, "AMediaCodec_configure");
    S(codecStart, "AMediaCodec_start");
    S(codecStop, "AMediaCodec_stop");
    S(codecDelete, "AMediaCodec_delete");
    S(codecDequeueInputBuffer, "AMediaCodec_dequeueInputBuffer");
    S(codecGetInputBuffer, "AMediaCodec_getInputBuffer");
    S(codecQueueInputBuffer, "AMediaCodec_queueInputBuffer");
    S(codecDequeueOutputBuffer, "AMediaCodec_dequeueOutputBuffer");
    S(codecGetOutputBuffer, "AMediaCodec_getOutputBuffer");
    S(codecReleaseOutputBuffer, "AMediaCodec_releaseOutputBuffer");
    S(codecGetOutputFormat, "AMediaCodec_getOutputFormat");
#undef S
    M.ok = true;
}

/* A MAI_PCM_* result as decode's error, with its detail. */
static const char *pcm_failure(int r, char *detail, size_t detail_n) {
    switch (r) {
    case MAI_PCM_TOO_LARGE: return "too_large";
    case MAI_PCM_NO_MEMORY: return "no_memory";
    default:
        snprintf(detail, detail_n, "the channel count changed mid-file");
        return "decode_error";
    }
}

/* Little-endian 8-bit unsigned, 24-bit packed or 32-bit integer frames into
 * `pcm`, through a stack buffer of floats. */
static int push_converted(mai_pcm *pcm, const uint8_t *x, size_t frames, int32_t ch, int32_t enc) {
    float tmp[4096];
    size_t per = sizeof tmp / sizeof tmp[0] / (size_t)ch;
    if (per == 0) return MAI_PCM_CHANNELS;
    size_t bps = enc == ENC_PCM_8BIT ? 1 : enc == ENC_PCM_24BIT_PACKED ? 3 : 4;
    for (size_t done = 0; done < frames;) {
        size_t n = frames - done < per ? frames - done : per;
        size_t samples = n * (size_t)ch;
        for (size_t i = 0; i < samples; i++, x += bps) {
            if (enc == ENC_PCM_8BIT) {
                tmp[i] = ((int)x[0] - 128) / 128.0f;
            } else if (enc == ENC_PCM_24BIT_PACKED) {
                int32_t v = (int32_t)((uint32_t)x[0] << 8 | (uint32_t)x[1] << 16 | (uint32_t)x[2] << 24);
                tmp[i] = (float)(v / 2147483648.0);
            } else {
                int32_t v;
                memcpy(&v, x, sizeof v);
                tmp[i] = (float)(v / 2147483648.0);
            }
        }
        int r = mai__pcm_push_f32(pcm, tmp, n, ch);
        if (r != MAI_PCM_OK) return r;
        done += n;
    }
    return MAI_PCM_OK;
}

static int push_block(mai_pcm *pcm, const uint8_t *x, size_t bytes, int32_t ch, int32_t enc) {
    size_t bps = enc == ENC_PCM_16BIT ? 2 : enc == ENC_PCM_8BIT ? 1 : enc == ENC_PCM_24BIT_PACKED ? 3 : 4;
    size_t frames = bytes / (bps * (size_t)ch);
    if (frames == 0) return MAI_PCM_OK;
    if (enc == ENC_PCM_16BIT) return mai__pcm_push_s16(pcm, (const int16_t *)(const void *)x, frames, ch);
    if (enc == ENC_PCM_FLOAT) return mai__pcm_push_f32(pcm, (const float *)(const void *)x, frames, ch);
    return push_converted(pcm, x, frames, ch, enc);
}

static bool encoding_known(int32_t enc) {
    return enc == ENC_PCM_16BIT || enc == ENC_PCM_8BIT || enc == ENC_PCM_FLOAT || enc == ENC_PCM_24BIT_PACKED ||
           enc == ENC_PCM_32BIT;
}

/* (no lock; a dirty IO scheduler) */
static const char *be_decode(const char *path, mai_pcm *pcm, int32_t *rate, char *detail, size_t detail_n) {
    pthread_once(&g_media_once, media_load);
    if (!M.ok) {
        snprintf(detail, detail_n, "libmediandk.so could not be loaded");
        return "unsupported";
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(detail, detail_n, "%s", strerror(errno));
        return "not_found";
    }
    /* fstat64: a 32-bit off_t can't hold a file over 2 GiB (EOVERFLOW);
     * such a file decodes until the cap (too_large) like any other. */
    struct stat64 st;
    int stat_rc = fstat64(fd, &st);
    if (stat_rc != 0 || !S_ISREG(st.st_mode)) {
        snprintf(detail, detail_n, "%s", stat_rc != 0 ? strerror(errno) : "not a regular file");
        close(fd);
        return "not_found";
    }
    AMediaExtractor *ex = M.exNew();
    if (!ex) {
        close(fd);
        return "no_memory";
    }
    media_status_t ms = M.exSetDataSourceFd(ex, fd, 0, (off64_t)st.st_size);
    if (ms != AMEDIA_OK) {
        snprintf(detail, detail_n, "AMediaExtractor_setDataSourceFd: %d", (int)ms);
        M.exDelete(ex);
        close(fd);
        return "unsupported";
    }

    AMediaCodec *codec = NULL;
    int32_t track_rate = 0, channels = 0;
    char mime[64] = "";
    const char *failure = NULL;
    size_t tracks = M.exGetTrackCount(ex);
    for (size_t i = 0; i < tracks && !mime[0]; i++) {
        AMediaFormat *f = M.exGetTrackFormat(ex, i);
        const char *m = NULL;
        if (f && M.fmtGetString(f, "mime", &m) && m && strncmp(m, "audio/", 6) == 0) {
            snprintf(mime, sizeof mime, "%s", m);
            M.fmtGetInt32(f, "sample-rate", &track_rate);
            M.fmtGetInt32(f, "channel-count", &channels);
            M.exSelectTrack(ex, i);
            codec = M.codecCreateDecoderByType(mime);
            if (!codec) {
                snprintf(detail, detail_n, "no decoder for %s", mime);
                failure = "unsupported";
            } else if ((ms = M.codecConfigure(codec, f, NULL, NULL, 0)) != AMEDIA_OK) {
                snprintf(detail, detail_n, "AMediaCodec_configure (%s): %d", mime, (int)ms);
                failure = "unsupported";
            } else if ((ms = M.codecStart(codec)) != AMEDIA_OK) {
                snprintf(detail, detail_n, "AMediaCodec_start (%s): %d", mime, (int)ms);
                failure = "decode_error";
            }
        }
        if (f) M.fmtDelete(f);
    }
    if (!mime[0]) {
        snprintf(detail, detail_n, "no audio track among %zu", tracks);
        failure = "no_audio";
    }
    if (failure) {
        if (codec) M.codecDelete(codec);
        M.exDelete(ex);
        close(fd);
        return failure;
    }

    int32_t out_channels = channels, out_rate = track_rate, enc = ENC_PCM_16BIT;
    bool input_done = false, eos = false;
    int idle = 0;
    while (!failure && !eos) {
        bool progress = false;
        if (!input_done) {
            ssize_t in = M.codecDequeueInputBuffer(codec, DEQUEUE_US);
            if (in >= 0) {
                size_t size = 0;
                uint8_t *buf = M.codecGetInputBuffer(codec, (size_t)in, &size);
                ssize_t n = buf ? M.exReadSampleData(ex, buf, size) : -1;
                if (n < 0) { /* the end of the track */
                    ms = M.codecQueueInputBuffer(codec, (size_t)in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    input_done = true;
                } else {
                    int64_t t = M.exGetSampleTime(ex);
                    ms = M.codecQueueInputBuffer(codec, (size_t)in, 0, (size_t)n, (uint64_t)(t > 0 ? t : 0), 0);
                    M.exAdvance(ex);
                }
                if (ms != AMEDIA_OK) {
                    snprintf(detail, detail_n, "AMediaCodec_queueInputBuffer: %d", (int)ms);
                    failure = "decode_error";
                    break;
                }
                progress = true;
            } else if (in != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                snprintf(detail, detail_n, "AMediaCodec_dequeueInputBuffer: %zd", in);
                failure = "decode_error";
                break;
            }
        }

        AMediaCodecBufferInfo info;
        ssize_t out = M.codecDequeueOutputBuffer(codec, &info, DEQUEUE_US);
        if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = M.codecGetOutputFormat(codec);
            int32_t v = 0;
            if (f && M.fmtGetInt32(f, "channel-count", &v) && v > 0) out_channels = v;
            /* (HE-AAC decodes at twice the container's rate) */
            if (f && pcm->frames == 0 && M.fmtGetInt32(f, "sample-rate", &v) && v > 0) out_rate = v;
            enc = f && M.fmtGetInt32(f, "pcm-encoding", &v) ? v : ENC_PCM_16BIT;
            if (f) M.fmtDelete(f);
            if (!encoding_known(enc)) {
                snprintf(detail, detail_n, "decoder output encoding %d", (int)enc);
                failure = "unsupported";
            }
            idle = 0;
            continue;
        }
        if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER || out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            if (!progress && ++idle > STALL_TRIES) {
                snprintf(detail, detail_n, "the %s decoder stalled", mime);
                failure = "decode_error";
            }
            continue;
        }
        if (out < 0) {
            snprintf(detail, detail_n, "AMediaCodec_dequeueOutputBuffer: %zd", out);
            failure = "decode_error";
            break;
        }
        idle = 0;
        size_t size = 0;
        uint8_t *buf = M.codecGetOutputBuffer(codec, (size_t)out, &size);
        if (!buf || out_channels <= 0 || info.offset < 0 || info.size < 0 ||
            (size_t)info.offset + (size_t)info.size > size) {
            M.codecReleaseOutputBuffer(codec, (size_t)out, false);
            snprintf(detail, detail_n, "bad output buffer (%d channels)", (int)out_channels);
            failure = "decode_error";
            break;
        }
        int r = push_block(pcm, buf + info.offset, (size_t)info.size, out_channels, enc);
        eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        M.codecReleaseOutputBuffer(codec, (size_t)out, false);
        if (r != MAI_PCM_OK) failure = pcm_failure(r, detail, detail_n);
    }

    M.codecStop(codec);
    M.codecDelete(codec);
    M.exDelete(ex);
    close(fd);
    *rate = out_rate;
    if (!failure && out_rate <= 0) {
        snprintf(detail, detail_n, "no sample rate");
        failure = "decode_error";
    }
    return failure;
}
