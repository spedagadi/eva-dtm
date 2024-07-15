#include <SDL.h>
// #include <string>
// #define PL_LIBAV_IMPLEMENTATION 1
// extern "C" {
#include "common.h"
#include "libplacebo/config.h"
#include "libplacebo/log.h"
#include "libplacebo/options.h"
#include "libplacebo/renderer.h"
#include "libplacebo/utils/frame_queue.h"
#include "pl_clock.h"
#include "pl_thread.h"

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
//}
#include "window.h"

#include <stdatomic.h>

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

#define MAX_AVFRAMES 100

typedef struct {
    AVFrame *items[MAX_AVFRAMES];
    int front;
    int rear;
} AVFrameQueue;

void init_avq(AVFrameQueue *q) {
    q->front = -1;
    q->rear = 0;
}

bool is_avq_empty(AVFrameQueue *q) { return (q->front == q->rear - 1); }

bool is_avq_full(AVFrameQueue *q) { return (q->rear == MAX_AVFRAMES); }

bool avq_enqueue(AVFrameQueue *q, AVFrame *value) {
    if (is_avq_full(q)) {
        printf("Queue is full\n");
        return false;
    }
    q->items[q->rear] = value;
    q->rear++;
    if (q->rear == MAX_AVFRAMES)
        q->rear = 0;
    return true;
}

void avq_dequeue(AVFrameQueue *q) {
    if (is_avq_empty(q)) {
        printf("Queue is empty\n");
        return;
    }
    q->front++;
    if (q->front == MAX_AVFRAMES)
        q->front = 0;
}

AVFrame *avq_peek(AVFrameQueue *q) {
    if (is_avq_empty(q)) {
        printf("Queue is empty\n");
        return NULL;
    }
    return q->items[q->front + 1];
}

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
    AVFrameQueue *decoder_queue;
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
    float fps_delta;
    float inv_fps_acc;
    float inv_fps_target;
    float frame_duration;
    bool is_live;

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
    float target_fps;
    int num_frames;

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

static PL_THREAD_VOID decode_loop(void *arg) {
    int ret;
    struct plplay *p = arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!frame || !packet)
        goto done;

    p->frame_duration = av_q2d(av_inv_q(p->stream->avg_frame_rate));
    double first_pts = 0.0, base_pts = 0.0, last_pts = 0.0;
    int queue_count = 0;
    // uint64_t num_frames = 0;

    while (!p->exit_thread) {
        switch ((ret = av_read_frame(p->format, packet))) {
            case 0:
                if (packet->stream_index != p->stream->index) {
                    // Ignore unrelated packets
                    av_packet_unref(packet);
                    continue;
                }
                ret = avcodec_send_packet(p->codec, packet);
                av_packet_unref(packet);
                break;
            case AVERROR_EOF:
                // Send empty input to flush decoder
                ret = avcodec_send_packet(p->codec, NULL);
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

        // Decode all frames from this packet
        while ((ret = avcodec_receive_frame(p->codec, frame)) == 0) {
            last_pts = frame->pts * av_q2d(p->stream->time_base);
            frame->opaque = p;

            (void) atomic_fetch_add(&p->stats.decoded, 1);
            if (p->inv_fps_acc >= p->inv_fps_target) {
                if (p->num_frames++ == 0) {
                    first_pts = last_pts;
                    pl_gpu_finish(p->win->gpu);
                }
                frame->pts = last_pts - first_pts + base_pts;
                frame->color_range = AVCOL_RANGE_MPEG;
                frame->color_primaries = AVCOL_PRI_BT2020;
                frame->color_trc = AVCOL_TRC_SMPTE2084;
                frame->colorspace = AVCOL_SPC_BT2020_NCL;
                frame->chroma_location = AVCHROMA_LOC_TOPLEFT;
                if (p->is_live) {
                    queue_count = pl_queue_num_frames(p->queue);
                    pl_queue_push(p->queue,
                                  &(struct pl_source_frame) {
                                          .pts = last_pts - first_pts + base_pts,
                                          .duration = p->frame_duration,
                                          .map = map_frame,
                                          .unmap = unmap_frame,
                                          .discard = discard_frame,
                                          .frame_data = frame,
                                          .first_field = p->opts->params.deinterlace_params
                                                         ? pl_field_from_avframe(frame)
                                                         : PL_FIELD_NONE,
                                  });
                    if (queue_count > 50)
                        fprintf(stderr,
                                "WARNING Render queue is filling up, queue size : %d\n",
                                queue_count);
                } else {
                    pl_queue_push_block(
                            p->queue, UINT64_MAX,
                            &(struct pl_source_frame) {
                                    .pts = last_pts - first_pts + base_pts,
                                    .duration = p->frame_duration,
                                    .map = map_frame,
                                    .unmap = unmap_frame,
                                    .discard = discard_frame,
                                    .frame_data = frame,
                                    .first_field = p->opts->params.deinterlace_params
                                                   ? pl_field_from_avframe(frame)
                                                   : PL_FIELD_NONE,
                            });
                }
                p->inv_fps_acc -= p->inv_fps_target;
            } else
                av_frame_free(&frame);
            p->inv_fps_acc += p->fps_delta;
            frame = av_frame_alloc();
        }

        switch (ret) {
            case AVERROR(EAGAIN):
                continue;
            case AVERROR_EOF:
                if (p->num_frames <= 1)
                    goto done; // still image or empty file
                // loop infinitely
                ret = av_seek_frame(p->format, p->stream->index, 0, AVSEEK_FLAG_BACKWARD);
                if (ret < 0) {
                    fprintf(stderr, "libavformat: Failed seeking in stream: %s\n",
                            av_err2str(ret));
                    goto done;
                }
                avcodec_flush_buffers(p->codec);
                base_pts += last_pts;
                p->num_frames = 0;
                continue;
            default:
                fprintf(stderr, "libavcodec: Failed decoding frame: %s\n",
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

static inline void apply_csp_overrides(struct plplay *p,
                                       struct pl_color_space *csp) {
    if (p->force_prim) {
        csp->primaries = p->force_prim;
        csp->hdr.prim = *pl_raw_primaries_get(csp->primaries);
    }
    if (p->force_trc)
        csp->transfer = p->force_trc;
    if (p->force_hdr_enable) {
        struct pl_hdr_metadata fix = p->force_hdr;
        fix.prim = csp->hdr.prim;
        csp->hdr = fix;
    } else if (p->colorspace_hint_dynamic) {
        pl_color_space_nominal_luma_ex(
                pl_nominal_luma_params(.color = csp, .metadata = PL_HDR_METADATA_ANY,
                                       .scaling = PL_HDR_NITS,
                                       .out_min = &csp->hdr.min_luma,
                                       .out_max = &csp->hdr.max_luma,));
    }
}

static void update_colorspace_hint(struct plplay *p,
                                   const struct pl_frame_mix *mix) {
    const struct pl_frame *frame = NULL;

    for (int i = 0; i < mix->num_frames; i++) {
        if (mix->timestamps[i] > 0.0)
            break;
        frame = mix->frames[i];
    }

    if (!frame)
        return;

    struct pl_color_space hint = {0};
    if (p->colorspace_hint)
        hint = frame->color;
    if (p->target_override)
        apply_csp_overrides(p, &hint);
    pl_swapchain_colorspace_hint(p->win->swapchain, &hint);
}

static bool render_frame(struct plplay *p,
                         const struct pl_swapchain_frame *frame,
                         const struct pl_frame_mix *mix) {
    struct pl_frame target;
    pl_options opts = p->opts;
    pl_frame_from_swapchain(&target, frame);
    update_settings(p, &target);

    if (p->target_override) {
        target.repr = p->force_repr;
        pl_color_repr_merge(&target.repr, &frame->color_repr);
        apply_csp_overrides(p, &target.color);

        // Update ICC profile parameters dynamically
        float target_luma = 50.0f;//0.0f;
        if (!p->use_icc_luma) {
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                                                   .metadata = PL_HDR_METADATA_HDR10, // use only static HDR nits
                                                   .scaling = PL_HDR_NITS, .color = &target.color,
                                                   .out_max = &target_luma,));
        }
        pl_icc_update(
                p->log, &p->icc, NULL,
                pl_icc_params(.max_luma = target_luma, .force_bpc = p->force_bpc,));
        target.icc = p->icc;
    }

    assert(mix->num_frames);
    pl_rect2df crop = mix->frames[0]->crop;
    if (p->stream->sample_aspect_ratio.num && p->target_zoom != ZOOM_RAW) {
        float sar = av_q2d(p->stream->sample_aspect_ratio);
        pl_rect2df_stretch(&crop, fmaxf(1.0f, sar), fmaxf(1.0f, 1.0 / sar));
    }

    // Apply target rotation and un-rotate crop relative to target
    target.rotation = p->target_rot;
    pl_rect2df_rotate(&crop, mix->frames[0]->rotation - target.rotation);

    switch (p->target_zoom) {
        case ZOOM_PAD:
            pl_rect2df_aspect_copy(&target.crop, &crop, 0.0);
            break;
        case ZOOM_CROP:
            pl_rect2df_aspect_copy(&target.crop, &crop, 1.0);
            break;
        case ZOOM_STRETCH:
            break; // target.crop already covers full image
        case ZOOM_FIT:
            pl_rect2df_aspect_fit(&target.crop, &crop, 0.0);
            break;
        case ZOOM_RAW:;
            // Ensure pixels are exactly aligned, to avoid fractional scaling
            int w = roundf(fabsf(pl_rect_w(crop)));
            int h = roundf(fabsf(pl_rect_h(crop)));
            target.crop.x0 = roundf((pl_rect_w(target.crop) - w) / 2.0f);
            target.crop.y0 = roundf((pl_rect_h(target.crop) - h) / 2.0f);
            target.crop.x1 = target.crop.x0 + w;
            target.crop.y1 = target.crop.y0 + h;
            break;
        case ZOOM_400:
        case ZOOM_200:
        case ZOOM_100:
        case ZOOM_50:
        case ZOOM_25:;
            const float z = powf(2.0f, (int) ZOOM_100 - p->target_zoom);
            const float sx = z * fabsf(pl_rect_w(crop)) / pl_rect_w(target.crop);
            const float sy = z * fabsf(pl_rect_h(crop)) / pl_rect_h(target.crop);
            pl_rect2df_stretch(&target.crop, sx, sy);
            break;
    }

    struct pl_color_map_params *cpars = &opts->color_map_params;
    if (cpars->visualize_lut) {
        cpars->visualize_rect = (pl_rect2df) {0, 0, 1, 1};
        float tar = pl_rect2df_aspect(&target.crop);
        pl_rect2df_aspect_set(&cpars->visualize_rect, 1.0f / tar, 0.0f);
    }

    pl_clock_t ts_pre = pl_clock_now();
    if (!pl_render_image_mix(p->renderer, mix, &target, &opts->params))
        return false;
    pl_clock_t ts_rendered = pl_clock_now();
    // if (!ui_draw(p->ui, frame))
    //     return false;
    pl_clock_t ts_ui_drawn = pl_clock_now();

    log_time(&p->stats.render, pl_clock_diff(ts_rendered, ts_pre));
    log_time(&p->stats.draw_ui, pl_clock_diff(ts_ui_drawn, ts_rendered));

    p->stats.rendered++;
    if (p->stats.rendered == 1)
        pl_gpu_finish(p->win->gpu);
    return true;
}

enum pl_queue_status get_decoded_frame(struct pl_source_frame *out_frame,
                                       const struct pl_queue_params *params) {
    AVFrame *new_frame = NULL;
    bool success = false;
    while (!success) {
        pl_thread_sleep(0.041);
        new_frame = avq_peek(state.decoder_queue);
        if (new_frame != NULL)
            success = true;
    }
    out_frame->pts = new_frame->pts, out_frame->duration = state.frame_duration,
    out_frame->map = map_frame, out_frame->unmap = unmap_frame,
    out_frame->discard = discard_frame, out_frame->frame_data = new_frame,
    out_frame->first_field = state.opts->params.deinterlace_params
                             ? pl_field_from_avframe(new_frame)
                             : PL_FIELD_NONE;
    avq_dequeue(state.decoder_queue);
    return PL_QUEUE_OK;
}

static bool render_loop(struct plplay *p) {
    pl_options opts = p->opts;

    struct pl_queue_params qparams =
            *pl_queue_params(.interpolation_threshold = 0.01,
                             .timeout = UINT64_MAX,);
    // qparams.get_frame = get_decoded_frame;

    // Initialize the frame queue, blocking indefinitely until done
    struct pl_frame_mix mix;
    switch (pl_queue_update(p->queue, &mix, &qparams)) {
        case PL_QUEUE_OK:
            break;
        case PL_QUEUE_EOF:
            return true;
        case PL_QUEUE_ERR:
            goto error;
        default:
            abort();
    }

    struct pl_swapchain_frame frame;
    update_colorspace_hint(p, &mix);
    if (!pl_swapchain_start_frame(p->win->swapchain, &frame))
        goto error;

    // Disable background transparency by default if the swapchain does not
    // appear to support alpha transaprency
    if (frame.color_repr.alpha == PL_ALPHA_NONE)
        opts->params.background_transparency = 0.0;

    if (!render_frame(p, &frame, &mix))
        goto error;
    if (!pl_swapchain_submit_frame(p->win->swapchain))
        goto error;

    // Wait until rendering is complete. Do this before measuring the time
    // start, to ensure we don't count initialization overhead as part of the
    // first vsync.
    pl_gpu_finish(p->win->gpu);
    p->stats.render = p->stats.draw_ui = (struct timing) {0};

    pl_clock_t ts_start = 0, ts_prev = 0;
    pl_swapchain_swap_buffers(p->win->swapchain);
    window_poll(p->win, false);

    double pts_target = 0.0, prev_pts = 0.0;

    while (!p->win->window_lost) {
        if (window_get_key(p->win, KEY_ESC))
            break;

        // if (p->toggle_fullscreen)
        // if (!window_is_fullscreen(p->win))
        //    window_toggle_fullscreen(p->win, true);
        // window_toggle_fullscreen(p->win, !window_is_fullscreen(p->win));

        update_colorspace_hint(p, &mix);
        pl_clock_t ts_acquire = pl_clock_now();
        if (!pl_swapchain_start_frame(p->win->swapchain, &frame)) {
            // Window stuck/invisible? Block for events and try again.
            window_poll(p->win, true);
            continue;
        }

        pl_clock_t ts_pre_update = pl_clock_now();
        log_time(&p->stats.acquire, pl_clock_diff(ts_pre_update, ts_acquire));
        if (!ts_start)
            ts_start = ts_pre_update;

        qparams.timeout = 0; // p->inv_fps_target * 1e6; // non-blocking update
        qparams.radius = pl_frame_mix_radius(&p->opts->params);
        qparams.pts = fmax(pts_target, pl_clock_diff(ts_pre_update, ts_start));
        p->stats.current_pts = qparams.pts;
        if (qparams.pts != prev_pts)
            log_time(&p->stats.pts_interval, qparams.pts - prev_pts);
        prev_pts = qparams.pts;

        retry:
        switch (pl_queue_update(p->queue, &mix, &qparams)) {
            case PL_QUEUE_ERR:
                goto error;
            case PL_QUEUE_EOF:
                printf("End of file reached\n");
                return true;
            case PL_QUEUE_OK:
                break;
            case PL_QUEUE_MORE:
                qparams.timeout = 0; // UINT64_MAX; // retry in blocking mode
                goto retry;
        }

        pl_clock_t ts_post_update = pl_clock_now();
        log_time(&p->stats.update, pl_clock_diff(ts_post_update, ts_pre_update));

        if (qparams.timeout) {
            double stuck_ms = 1e3 * pl_clock_diff(ts_post_update, ts_pre_update);
            fprintf(stderr, "Stalled for %.4f ms due to frame queue under run!\n",
                    stuck_ms);
            ts_start += ts_post_update - ts_pre_update; // subtract time spent waiting
            p->stats.stalled++;
            p->stats.stalled_ms += stuck_ms;
        }

        if (!render_frame(p, &frame, &mix))
            goto error;

        if (pts_target) {
            pl_gpu_flush(p->win->gpu);
            pl_clock_t ts_wait = pl_clock_now();
            double pts_now = pl_clock_diff(ts_wait, ts_start);
            if (pts_target >= pts_now) {
                log_time(&p->stats.sleep, pts_target - pts_now);
                pl_thread_sleep(pts_target - pts_now);
            } else {
                double missed_ms = 1e3 * (pts_now - pts_target);
                fprintf(stderr,
                        "Missed PTS target %.3f (%.3f ms in the past) Queued frames : "
                        "{%d}\n",
                        pts_target, missed_ms, pl_queue_num_frames(p->queue));
                p->stats.missed++;
                p->stats.missed_ms += missed_ms;
            }

            pts_target = 0.0;
        }

        pl_clock_t ts_pre_submit = pl_clock_now();
        if (!pl_swapchain_submit_frame(p->win->swapchain)) {
            fprintf(stderr, "libplacebo: failed presenting frame!\n");
            goto error;
        }
        pl_clock_t ts_post_submit = pl_clock_now();
        log_time(&p->stats.submit, pl_clock_diff(ts_post_submit, ts_pre_submit));

        if (ts_prev)
            log_time(&p->stats.vsync_interval,
                     pl_clock_diff(ts_post_submit, ts_prev));
        ts_prev = ts_post_submit;

        pl_swapchain_swap_buffers(p->win->swapchain);
        pl_clock_t ts_post_swap = pl_clock_now();
        log_time(&p->stats.swap, pl_clock_diff(ts_post_swap, ts_post_submit));

        window_poll(p->win, false);

        // In content-timed mode (frame mixing disabled), delay rendering
        // until the next frame should become visible
        if (!opts->params.frame_mixer) {
            struct pl_source_frame next;
            for (int i = 0;; i++) {
                if (!pl_queue_peek(p->queue, i, &next))
                    break;
                if (next.pts > qparams.pts) {
                    pts_target = next.pts;
                    break;
                }
            }
        }

        if (p->fps_override)
            pts_target = fmax(pts_target, qparams.pts + 1.0 / p->fps);
    }

    return true;

    error:
    fprintf(stderr, "Render loop failed, exiting early...\n");
    return false;
}

static void uninit(struct plplay *p) {
    if (p->decoder_thread_created) {
        p->exit_thread = true;
        pl_queue_push(p->queue, NULL); // Signal EOF to wake up thread
        pl_thread_join(p->decoder_thread);
    }

    pl_queue_destroy(&p->queue);
    pl_renderer_destroy(&p->renderer);
    pl_options_free(&p->opts);

    for (int i = 0; i < p->shader_num; i++) {
        pl_mpv_user_shader_destroy(&p->shader_hooks[i]);
        free(p->shader_paths[i]);
    }

    for (int i = 0; i < MAX_FRAME_PASSES; i++)
        pl_shader_info_deref(&p->frame_info[i].shader);
    for (int j = 0; j < MAX_BLEND_FRAMES; j++) {
        for (int i = 0; i < MAX_BLEND_PASSES; i++)
            pl_shader_info_deref(&p->blend_info[j][i].shader);
    }

    free(p->shader_hooks);
    free(p->shader_paths);
    free(p->icc_name);
    pl_icc_close(&p->icc);

    if (p->cache) {
        if (pl_cache_signature(p->cache) != p->cache_sig) {
            FILE *file = fopen(p->cache_file, "wb");
            if (file) {
                pl_cache_save_file(p->cache, file);
                fclose(file);
            }
        }
        pl_cache_destroy(&p->cache);
    }

    // Free this before destroying the window to release associated GPU buffers
    avcodec_free_context(&p->codec);
    avformat_free_context(p->format);

    // ui_destroy(&p->ui);
    window_destroy(&p->win);

    pl_log_destroy(&p->log);
    memset(p, 0, sizeof(*p));
}

static bool open_file(struct plplay *p, const char *filename) {
    static const int av_log_level[] = {
            [PL_LOG_NONE] = AV_LOG_QUIET, [PL_LOG_FATAL] = AV_LOG_PANIC,
            [PL_LOG_ERR] = AV_LOG_ERROR, [PL_LOG_WARN] = AV_LOG_WARNING,
            [PL_LOG_INFO] = AV_LOG_INFO, [PL_LOG_DEBUG] = AV_LOG_VERBOSE,
            [PL_LOG_TRACE] = AV_LOG_DEBUG,
    };

    av_log_set_level(av_log_level[p->args.verbosity]);

    printf("Opening file: '%s'\n", filename);
    if (avformat_open_input(&p->format, filename, NULL, NULL) != 0) {
        fprintf(stderr, "libavformat: Failed opening file!\n");
        return false;
    }

    printf("Format: %s\n", p->format->iformat->name);

    if (p->format->duration != AV_NOPTS_VALUE)
        printf("Duration: %.3f s\n", p->format->duration / 1e6);

    if (avformat_find_stream_info(p->format, NULL) < 0) {
        fprintf(stderr, "libavformat: Failed finding stream info!\n");
        return false;
    }

    // Find "best" video stream
    int stream_idx =
            av_find_best_stream(p->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if (stream_idx < 0) {
        fprintf(stderr, "plplay: File contains no video streams?\n");
        return false;
    }

    const AVStream *stream = p->format->streams[stream_idx];
    const AVCodecParameters *par = stream->codecpar;
    printf("Found video track (stream %d)\n", stream_idx);
    printf("Resolution: %d x %d\n", par->width, par->height);

    if (stream->avg_frame_rate.den && stream->avg_frame_rate.num)
        printf("FPS: %f\n", av_q2d(stream->avg_frame_rate));

    if (stream->r_frame_rate.den && stream->r_frame_rate.num)
        printf("TBR: %f\n", av_q2d(stream->r_frame_rate));

    if (stream->time_base.den && stream->time_base.num)
        printf("TBN: %f\n", av_q2d(stream->time_base));

    if (par->bit_rate)
        printf("Bitrate: %" PRIi64 " kbps\n", par->bit_rate / 1000);

    // printf("Format: %s\n", av_get_pix_fmt_name(par->format));
    p->fps_delta = 1.0f / av_q2d(stream->avg_frame_rate);
    p->is_live = false;
    p->stream = stream;
    return true;
}

static bool open_device(struct plplay *p, const char *device_name) {
    avdevice_register_all();
    av_log_set_level(AV_LOG_QUIET);

    printf("Opening device: '%s'\n", device_name);
    AVDictionary *in_options = NULL;
    av_dict_set(&in_options, "rtbufsize", "1610612736", 0);
    // av_dict_set(&in_options, "pixel_format", "BGR3", 0);
    const AVInputFormat *input_format = av_find_input_format("v4l2");
    if (avformat_open_input(&p->format, device_name, input_format, &in_options) !=
        0) {
        fprintf(stderr, "libavformat: Failed opening device!\n");
        return false;
    }

    printf("Format: %s\n", p->format->iformat->name);

    if (p->format->duration != AV_NOPTS_VALUE)
        printf("Duration: %.3f s\n", p->format->duration / 1e6);

    if (avformat_find_stream_info(p->format, NULL) < 0) {
        fprintf(stderr, "libavformat: Failed finding stream info!\n");
        return false;
    }

    // Find "best" video stream
    int stream_idx =
            av_find_best_stream(p->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if (stream_idx < 0) {
        fprintf(stderr, "plplay: File contains no video streams?\n");
        return false;
    }

    const AVStream *stream = p->format->streams[stream_idx];
    const AVCodecParameters *par = stream->codecpar;
    printf("Found video track (stream %d)\n", stream_idx);
    printf("Resolution: %d x %d\n", par->width, par->height);

    if (stream->avg_frame_rate.den && stream->avg_frame_rate.num)
        printf("FPS: %f\n", av_q2d(stream->avg_frame_rate));

    if (stream->r_frame_rate.den && stream->r_frame_rate.num)
        printf("TBR: %f\n", av_q2d(stream->r_frame_rate));

    if (stream->time_base.den && stream->time_base.num)
        printf("TBN: %f\n", av_q2d(stream->time_base));

    if (par->bit_rate)
        printf("Bitrate: %" PRIi64 " kbps\n", par->bit_rate / 1000);

    // printf("Format: %s\n", av_get_pix_fmt_name(par->format));
    p->fps_delta = 1.0f / av_q2d(stream->avg_frame_rate);
    p->is_live = true;
    p->stream = stream;
    return true;
}

static bool init_codec(struct plplay *p) {
    assert(p->stream);
    assert(p->win->gpu);

    const AVCodec *codec = avcodec_find_decoder(p->stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "libavcodec: Failed finding matching codec\n");
        return false;
    }

    p->codec = avcodec_alloc_context3(codec);
    if (!p->codec) {
        fprintf(stderr, "libavcodec: Failed allocating codec\n");
        return false;
    }

    if (avcodec_parameters_to_context(p->codec, p->stream->codecpar) < 0) {
        fprintf(stderr, "libavcodec: Failed copying codec parameters to codec\n");
        return false;
    }

    printf("Codec: %s (%s)\n", codec->name, codec->long_name);
    const AVCodecHWConfig *hwcfg = 0;
    if (p->args.hwdec) {
        for (int i = 0; (hwcfg = avcodec_get_hw_config(codec, i)); i++) {
            if (!pl_test_pixfmt(p->win->gpu, hwcfg->pix_fmt))
                continue;
            if (!(hwcfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                continue;

            int ret = av_hwdevice_ctx_create(&p->codec->hw_device_ctx,
                                             hwcfg->device_type, NULL, NULL, 0);
            if (ret < 0) {
                fprintf(stderr,
                        "libavcodec: Failed opening HW device context, skipping\n");
                continue;
            }

            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(hwcfg->pix_fmt);
            printf("Using hardware frame format: %s\n", desc->name);
            p->codec->extra_hw_frames = 4;
            break;
        }
    }

    if (!hwcfg)
        printf("Using software decoding\n");

    p->codec->thread_count = FFMIN(av_cpu_count() + 1, 16);
    p->codec->get_buffer2 = pl_get_buffer2;
    p->codec->opaque = &p->win->gpu;
#if LIBAVCODEC_VERSION_MAJOR < 60
    AV_NOWARN_DEPRECATED({ p->codec->thread_safe_callbacks = 1; });
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 113, 100)
    p->codec->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
#endif

    if (avcodec_open2(p->codec, codec, NULL) < 0) {
        fprintf(stderr, "libavcodec: Failed opening codec\n");
        return false;
    }

    return true;
}

int main(int argc, char *argv[]) {
    // printf("Hello, from eva-dtm!\n");
    state.target_override = true;
    state.use_icc_luma = false;
    state.fps = 23.976;
    state.fps_override = true;
    state.inv_fps_acc = 0;
    state.inv_fps_target = 1.0f / state.fps;
    state.args.hwdec = true;
    state.num_frames = 0;
    state.args.verbosity = PL_LOG_ALL;
    state.args.hwdec = true;
    state.toggle_fullscreen = false;
    struct pl_log_params log_params;
    log_params.log_cb = pl_log_color;
    log_params.log_level = state.args.verbosity;
    state.log = pl_log_create(PL_API_VER, NULL);
    pl_options opts = state.opts = pl_options_alloc(state.log);
    // state.args.preset = &pl_render_fast_params;
    // state.args.preset = &pl_render_high_quality_params;
    state.args.preset = &pl_render_default_params;
    pl_options_reset(opts, state.args.preset);
    opts->params.cone_params = &opts->cone_params;
    opts->params.dynamic_constants = false; // true;
    opts->params.info_callback = info_callback;
    opts->params.info_priv = &state;

    opts->params.upscaler = NULL;
    opts->params.downscaler = &pl_filter_hermite;
    opts->params.frame_mixer = &pl_filter_oversample;
    opts->params.antiringing_strength = 0.0f;
    opts->params.deband_params = NULL;
    opts->params.sigmoid_params = &pl_sigmoid_default_params;
    opts->params.color_adjustment = &pl_color_adjustment_neutral;
    opts->params.peak_detect_params = &pl_peak_detect_high_quality_params;
    opts->params.color_map_params = &pl_color_map_high_quality_params;
    opts->params.dither_params = &pl_dither_default_params;
    opts->params.cone_params = NULL;
    opts->params.blend_params = NULL;
    opts->params.deinterlace_params = NULL;
    opts->params.distort_params = NULL;

    //opts->peak_detect_params.allow_delayed = true;
    state.force_hdr_enable = true;
    state.force_prim = PL_COLOR_PRIM_BT_2020;//PL_COLOR_PRIM_BT_709;//PL_COLOR_PRIM_BT_2020;
    // #if defined(WIN32)
    //     state.args.window_impl = "glfw-d3d11";
    // #else
    //     state.args.window_impl = "glfw-sdl";
    // #endif
    struct plplay *p = &state;

    char *mp4_file = "D://Videos//HDRDemo//Exodus.mp4";
    // char *mp4_file = "D://Videos//HDRDemo//Bi-directional_smoothing.mp4";
    //if (!open_file(p, mp4_file))
    //  return -1;
    if (!open_device(p, "/dev/video0"))
        return -1;
    const AVCodecParameters *par = p->stream->codecpar;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(par->format);
    if (!desc)
        return -1;
    struct window_params params = {
            "plplay",
            p->stream->codecpar->width,
            p->stream->codecpar->height,
            state.args.window_impl,
    };
    if (desc->flags & AV_PIX_FMT_FLAG_ALPHA) {
        params.alpha = true;
        opts->params.background_transparency = 1.0;
    }
    p->win = window_create(p->log, &params);
    if (!pl_test_pixfmt(p->win->gpu, par->format)) {
        fprintf(stderr, "Unsupported AVPixelFormat: %s\n", desc->name);
        return -1;
    }
    window_toggle_fullscreen(p->win, true);
    if (!init_codec(p))
        return -1;
    p->decoder_queue = (AVFrameQueue *) malloc(sizeof(AVFrameQueue *));
    init_avq(p->decoder_queue);
    p->queue = pl_queue_create(p->win->gpu);
    int ret = pl_thread_create(&p->decoder_thread, decode_loop, p);
    if (ret != 0) {
        fprintf(stderr, "Failed creating decode thread: %s\n", strerror(errno));
        return -1;
    }

    p->decoder_thread_created = true;
    p->renderer = pl_renderer_create(p->log, p->win->gpu);
    if (!render_loop(p))
        return -1;

    printf("Exiting...\n");
    uninit(p);
    free(p->decoder_queue);
    return 0;
}