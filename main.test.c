#include <stdlib.h>

#include <SDL.h>
#include "common.h"
#include "pl_thread.h"
#include "libplacebo/config.h"
#include "libplacebo/log.h"
#include "libplacebo/options.h"
#include "libplacebo/renderer.h"
#include "libplacebo/utils/frame_queue.h"
#include "pl_clock.h"
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/cpu.h>
#include <libavutil/file.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
// #include <libavutil/internal.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libplacebo/utils/libav.h>
#include <libswscale/swscale.h>
#include "window.h"
#include <stdatomic.h>
#include "va/va.h"

#define MAX_FRAME_PASSES 256
#define MAX_BLEND_PASSES 8
#define MAX_BLEND_FRAMES 8

enum {
    ZOOM_PAD = 0,
    ZOOM_CROP,
    ZOOM_STRETCH,
    ZOOM_FIT,
    ZOOM_RAW,
    ZOOM_400,
    ZOOM_200,
    ZOOM_100,
    ZOOM_50,
    ZOOM_25,
    ZOOM_COUNT,
};

struct plplay_args {
    const struct pl_render_params *preset;
    enum pl_log_level verbosity;
    const char *window_impl;
    const char *filename;
    bool hwdec;
};

struct plplay {
    struct plplay_args args;
    struct window *win;
    struct ui *ui;
    char cache_file[512];

    // libplacebo
    pl_log log;
    pl_renderer renderer;
    pl_queue queue;
    pl_cache cache;

    uint64_t cache_sig;

    // libav*
    AVFormatContext *format;
    AVCodecContext *codec;
    const AVStream *stream; // points to first video stream of `format`
    AVFrame *src_raw_frame;
    AVFrame *hdr_raw_frame;
    struct SwsContext *ff_scaling_ctx_;
    pl_thread decoder_thread;
    bool decoder_thread_created;
    bool exit_thread;

    // settings / ui state
    pl_options opts;
    pl_rotation target_rot;
    int target_zoom;
    bool colorspace_hint;
    bool colorspace_hint_dynamic;
    bool ignore_dovi;
    bool toggle_fullscreen;
    bool advanced_scalers;

    bool target_override; // if false, fields below are ignored
    struct pl_color_repr force_repr;
    enum pl_color_primaries force_prim;
    enum pl_color_transfer force_trc;
    struct pl_hdr_metadata force_hdr;
    bool force_hdr_enable;
    bool fps_override;
    float fps;

    // ICC profile
    pl_icc_object icc;
    char *icc_name;
    bool use_icc_luma;
    bool force_bpc;

    // custom shaders
    const struct pl_hook **shader_hooks;
    char **shader_paths;
    size_t shader_num;
    size_t shader_size;

    // pass metadata
    struct pl_dispatch_info blend_info[MAX_BLEND_FRAMES][MAX_BLEND_PASSES];
    struct pl_dispatch_info frame_info[MAX_FRAME_PASSES];
    int num_frame_passes;
    int num_blend_passes[MAX_BLEND_FRAMES];

    // playback statistics
    struct {
        uint32_t decoded;
        uint32_t rendered;
        uint32_t mapped;
        uint32_t dropped;
        uint32_t missed;
        uint32_t stalled;
        double missed_ms;
        double stalled_ms;
        double current_pts;

        struct timing {
            double sum, sum2, peak;
            uint64_t count;
        } acquire, update, render, draw_ui, sleep, submit, swap, vsync_interval,
                pts_interval;
    } stats;
};

static struct plplay state;

void update_settings(struct plplay *p, const struct pl_frame *target) {}

static inline void log_time(struct timing *t, double ts) {
    t->sum += ts;
    t->sum2 += ts * ts;
    t->peak = fmax(t->peak, ts);
    t->count++;
}

static void info_callback(void *priv, const struct pl_render_info *info) {
    struct plplay *p = priv;
    switch (info->stage) {
        case PL_RENDER_STAGE_FRAME:
            if (info->index >= MAX_FRAME_PASSES)
                return;
            p->num_frame_passes = info->index + 1;
            pl_dispatch_info_move(&p->frame_info[info->index], info->pass);
            return;

        case PL_RENDER_STAGE_BLEND:
            if (info->index >= MAX_BLEND_PASSES || info->count >= MAX_BLEND_FRAMES)
                return;
            p->num_blend_passes[info->count] = info->index + 1;
            pl_dispatch_info_move(&p->blend_info[info->count][info->index], info->pass);
            return;

        case PL_RENDER_STAGE_COUNT:
            break;
    }

    abort();
}

static bool map_frame(pl_gpu gpu, pl_tex *tex,
                      const struct pl_source_frame *src,
                      struct pl_frame *out_frame) {
    AVFrame *frame = src->frame_data;
    struct plplay *p = frame->opaque;
    bool ok = pl_map_avframe_ex(gpu, out_frame,
                                pl_avframe_params(.frame = frame, .tex = tex,
                                                  .map_dovi = !p->ignore_dovi,));

    av_frame_free(&frame); // references are preserved by `out_frame`
    if (!ok) {
        fprintf(stderr, "Failed mapping AVFrame!\n");
        return false;
    }

    p->stats.mapped++;
    pl_frame_copy_stream_props(out_frame, p->stream);
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src) {
    pl_unmap_avframe(gpu, frame);
}

static void discard_frame(const struct pl_source_frame *src) {
    AVFrame *frame = src->frame_data;
    struct plplay *p = frame->opaque;
    p->stats.dropped++;
    av_frame_free(&frame);
    printf("Dropped frame with PTS %.3f\n", src->pts);
}

/*VAStatus vaUnmapBuffer(VADisplay dpy, VABufferID buf_id) {
}

VAStatus vaGetConfigAttributes(VADisplay dpy,
                               VAProfile profile,
                               VAEntrypoint entrypoint,
                               VAConfigAttrib *attrib_list,
                               int num_attribs
) {

}


VAStatus vaSyncBuffer(VADisplay dpy,
                      VABufferID buf_id,
                      uint64_t timeout_ns
) {

}

VAStatus vaDestroyContext(VADisplay dpy,
                          VAContextID context
) {

}

VAStatus vaBeginPicture(VADisplay dpy,
                        VAContextID context,
                        VASurfaceID render_target
) {

}

const char *vaErrorStr(VAStatus error_status) {

}

int vaMaxNumProfiles(VADisplay dpy) {

}
VAStatus vaCreateBuffer	(	VADisplay 	dpy,
                                VAContextID 	context,
                                VABufferType 	type,
                                unsigned int 	size,
                                unsigned int 	num_elements,
                                void * 	data,
                                VABufferID * 	buf_id
) {

}

VAStatus vaMapBuffer	(	VADisplay 	dpy,
                             VABufferID 	buf_id,
                             void ** 	pbuf
) {

}*/

static PL_THREAD_VOID decode_loop(void *arg) {
    int ret;
    struct plplay *p = arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!frame || !packet)
        goto done;

    float frame_duration = av_q2d(av_inv_q(p->stream->avg_frame_rate));
    double first_pts = 0.0, base_pts = 0.0, last_pts = 0.0;
    uint64_t num_frames = 0;

    while (!p->exit_thread) {
        ret = av_read_frame(p->format, packet);
        switch (ret) {
            case 0:
                if (packet->stream_index != p->stream->index) {
                    // Ignore unrelated packets
                    //av_packet_unref(packet);
                    continue;
                }
                //ret = avcodec_send_packet(p->codec, packet);
                //av_packet_unref(packet);
                break;
            case AVERROR_EOF:
                // Send empty input to flush decoder
                //ret = avcodec_send_packet(p->codec, NULL);
                break;
            default:
                fprintf(stderr, "libavformat: Failed reading packet: %s\n",
                        av_err2str(ret));
                goto done;
        }

        if (ret < 0) {
            fprintf(stderr, "libavcodec: Failed sending packet to decoder: %s\n",
                    av_err2str(ret));
            goto done;
        }
    }
    done:
    pl_queue_push(p->queue, NULL); // Signal EOF to flush queue
    av_packet_free(&packet);
    av_frame_free(&frame);
    PL_THREAD_RETURN();
}

int main(int argc, char *argv[]) {
}
