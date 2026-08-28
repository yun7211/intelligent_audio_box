/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include <inttypes.h>
#include "media_lib_os.h"
#include "data_queue.h"
#include "msg_q.h"
#include "av_render.h"
#include "audio_decoder.h"
#include "video_decoder.h"
#include "audio_render.h"
#include "video_render.h"
#include "audio_resample.h"
#include "esp_timer.h"
#include "color_convert.h"
#include "esp_log.h"

#define TAG "AV_RENDER"

/*
 * av_render 是“推模式”音视频播放管线。调用方不断 add_*_data()，内部按配置最多拆成四个
 * 独立阶段：音频解码、音频渲染、视频解码、视频渲染。阶段之间用 data_queue 传媒体数据，
 * 用 msg_q 传 pause/resume/flush/close 控制消息。
 *
 * 若未给某阶段配置 FIFO，该阶段不创建线程，而是在调用链中同步执行，以节省 RAM 和调度
 * 开销。同步模式决定谁是时钟：NONE 各自播放；AUDIO 以音频 PTS 为基准丢弃/等待视频；
 * TIME 则以系统单调时钟推进。所有公共 API 由 api_lock 串行保护生命周期变化。
 */

#define ADEC_CLOSED_BITS     (1 << 0)
#define VDEC_CLOSED_BITS     (1 << 1)
#define A_RENDER_CLOSED_BITS (1 << 2)
#define V_RENDER_CLOSED_BITS (1 << 3)

#define FLUSH_SHIFT_BITS (4)

#define _SET_BITS(group, bit) media_lib_event_group_set_bits((media_lib_event_grp_handle_t)group, bit)
// 事件位等待后不会自动清除，必须由宏显式清理，避免下一次等待误判为已完成。
#define _WAIT_BITS(group, bit)                                                                          \
    media_lib_event_group_wait_bits((media_lib_event_grp_handle_t)group, bit, MEDIA_LIB_MAX_LOCK_TIME); \
    media_lib_event_group_clr_bits(group, bit)

#define VIDEO_ERR_FRAME_TOLERANCE (5)
#define AUDIO_ERR_FRAME_TOLERANCE (10)

typedef enum {
    AV_RENDER_MSG_NONE,
    AV_RENDER_MSG_PAUSE,
    AV_RENDER_MSG_RESUME,
    AV_RENDER_MSG_FLUSH,
    AV_RENDER_MSG_DATA,
    AV_RENDER_MSG_CLOSE,
} av_render_msg_type_t;

typedef struct {
    av_render_msg_type_t type;
    uint32_t             data;
} av_render_msg_t;

typedef struct _render_thread_res_t {
    // 四种工作线程共用的运行时资源：控制队列、数据 FIFO、状态和具体处理函数。
    media_lib_thread_handle_t thread;
    msg_q_handle_t            msg_q;
    data_queue_t             *data_q;
    bool                      use_pool;
    const char               *name;
    uint8_t                   wait_bits;
    uint8_t                   flushing;
    struct _av_render        *render;
    bool                      paused;
    int (*render_body)(struct _render_thread_res_t *res, bool drop);
} av_render_thread_res_t;

typedef struct {
    av_render_thread_res_t thread_res;
    adec_handle_t          adec;
    int                    audio_err_cnt;
} av_render_adec_res_t;

typedef struct {
    av_render_thread_res_t       thread_res;
    vdec_handle_t                vdec;
    int                          video_err_cnt;
    av_render_video_frame_t     *fb_frame;
    av_render_video_frame_type_t dec_out_fmt;
    av_render_video_frame_type_t out_fmt;
    color_convert_table_t       *vid_convert;
    uint8_t                     *vid_convert_out;
    int                          vid_convert_out_size;
} av_render_vdec_res_t;

struct _av_render;

typedef struct {
    av_render_thread_res_t       thread_res;
    bool                         audio_packet_reached;
    bool                         audio_rendered;
    av_render_audio_frame_info_t audio_frame_info;
    av_render_audio_frame_info_t out_frame_info;
    audio_resample_handle_t      resample_handle;
    bool                         need_resample;
    uint32_t                     audio_send_pts;
    bool                         decode_in_sync;
    bool                         audio_is_pcm;
    bool                         a_render_in_sync;
} av_render_audio_res_t;

typedef struct {
    av_render_thread_res_t       thread_res;
    bool                         video_packet_reached;
    bool                         video_rendered;
    av_render_video_frame_info_t video_frame_info;
    bool                         decode_in_sync;
    bool                         use_fb;
    uint32_t                     video_send_pts;
    bool                         v_render_in_sync;
    bool                         video_is_raw;
    uint32_t                     sent_frame_num;
    uint32_t                     video_start_time;
    uint32_t                     video_start_pts;
    uint32_t                     video_last_pts;
    int                          sync_tolerance;
} av_render_video_res_t;

typedef struct _av_render {
    // 总上下文持有四阶段资源及用户提供的底层 audio/video render 实现。
    av_render_cfg_t cfg;

    av_render_adec_res_t        *adec_res;
    av_render_vdec_res_t        *vdec_res;
    av_render_audio_res_t       *a_render_res;
    av_render_video_res_t       *v_render_res;
    av_render_audio_frame_info_t aud_fix_info;

    media_lib_event_grp_handle_t event_group;
    media_lib_mutex_handle_t     api_lock;
    uint32_t                     audio_threshold;
    av_render_event_cb           event_cb;
    void                        *event_ctx;
    av_render_pool_data_free     pool_free;
    void                        *pool;
} av_render_t;

typedef enum {
    AV_RENDER_DUMP_ADEC_DATA,
    AV_RENDER_DUMP_ARENDER_DATA,
    AV_RENDER_DUMP_VDEC_DATA,
    AV_RENDER_DUMP_VRENDER_DATA,
    AV_RENDER_DUMP_STOP_INDEX,
} av_render_dump_type_t;

#define BREAK_ON_FAIL(ret) if (ret != 0) {   \
    break;                                   \
}
#define RETURN_ON_FAIL(ret) if (ret != 0) {   \
    return ret;                               \
}

#define RETURN_ON_NULL(ptr, ret) if (ptr == NULL) {   \
    return ret;                                       \
}

static uint8_t render_dump_mask;

static int av_render_get_audio_pts(av_render_t *render, uint32_t *out_pts);

static int av_render_get_video_pts(av_render_t *render, uint32_t *out_pts);

static void render_thread(void *arg);

static uint32_t get_cur_time()
{
    return esp_timer_get_time() / 1000;
}

static int put_to_adec(data_queue_t *q, av_render_audio_data_t *data, bool use_pool)
{
    // 入解码队列时先写固定头；不用外部内存池时再把负载复制到同一队列块中。
    int head_size = sizeof(av_render_audio_data_t);
    int size = head_size + (use_pool ? 0 : data->size);
    uint8_t *b = (uint8_t *)data_queue_get_buffer(q, size);
    if (b == NULL) {
        ESP_LOGE(TAG, "Drop for no enough %d", size);
        return -1;
    }
    memcpy(b, data, head_size);
    if (use_pool == false && data->size) {
        memcpy(b + head_size, data->data, data->size);
    }
    return data_queue_send_buffer(q, size);
}

static int put_to_vdec(data_queue_t *q, av_render_video_data_t *data, bool use_pool)
{
    // 视频包与音频包采用相同布局，use_pool=true 时队列只保存外部数据指针。
    int head_size = sizeof(av_render_video_data_t);
    int size = head_size + (use_pool ? 0 : data->size);
    uint8_t *b = (uint8_t *)data_queue_get_buffer(q, size);
    if (b == NULL) {
        return -1;
    }
    memcpy(b, data, head_size);
    if (use_pool == false && data->size) {
        memcpy(b + head_size, data->data, data->size);
    }
    return data_queue_send_buffer(q, size);
}

static int put_to_a_render(data_queue_t *q, av_render_audio_frame_t *data)
{
    // 解码后的 PCM 连同帧信息写入音频渲染 FIFO。
    int head_size = sizeof(av_render_audio_frame_t);
    int size = head_size + data->size;
    uint8_t *b = (uint8_t *)data_queue_get_buffer(q, size);
    if (b == NULL) {
        return -1;
    }
    memcpy(b, data, head_size);
    if (data->size) {
        memcpy(b + head_size, data->data, data->size);
    }
    return data_queue_send_buffer(q, size);
}

static int put_to_v_render(data_queue_t *q, av_render_video_frame_t *data)
{
    // 视频帧可能由解码器 frame buffer 持有，队列保存帧描述而非盲目复制整张图。
    int head_size = sizeof(av_render_video_frame_t);
    int size = head_size + data->size;
    uint8_t *b = (uint8_t *)data_queue_get_buffer(q, size);
    if (b == NULL) {
        return -1;
    }
    memcpy(b, data, head_size);
    if (data->size) {
        memcpy(b + head_size, data->data, data->size);
    }
    return data_queue_send_buffer(q, size);
}

static int read_for_adec(data_queue_t *q, av_render_audio_data_t *data, bool use_pool)
{
    // read_lock 返回的内存在 read_unlock 前有效；使用外部池时负载仍由池管理。
    uint8_t *b;
    int size;
    int ret = data_queue_read_lock(q, (void **)&b, &size);
    RETURN_ON_FAIL(ret);
    av_render_audio_data_t *r = (av_render_audio_data_t *)b;
    if (use_pool) {
        *data = *r;
        return ret;
    }
    int head_size = sizeof(av_render_audio_data_t);
    if (r->size + head_size != size) {
        ret = -1;
    } else {
        *data = *r;
        data->data = b + head_size;
    }
    return ret;
}

static int read_for_vdec(data_queue_t *q, av_render_video_data_t *data, bool use_pool)
{
    uint8_t *b;
    int size;
    int ret = data_queue_read_lock(q, (void **)&b, &size);
    RETURN_ON_FAIL(ret);
    av_render_video_data_t *r = (av_render_video_data_t *)b;
    if (use_pool) {
        *data = *r;
        return ret;
    }
    int head_size = sizeof(av_render_video_data_t);
    if (r->size + head_size != size) {
        ret = -1;
    } else {
        *data = *r;
        data->data = b + head_size;
    }
    return ret;
}

static int read_for_a_render(data_queue_t *q, av_render_audio_frame_t *data)
{
    uint8_t *b = NULL;
    int size;
    int ret = data_queue_read_lock(q, (void **)&b, &size);
    RETURN_ON_FAIL(ret);
    RETURN_ON_NULL(b, ESP_MEDIA_ERR_FAIL);
    av_render_audio_frame_t *r = (av_render_audio_frame_t *)b;
    int head_size = sizeof(av_render_audio_frame_t);
    if (r->size + head_size != size) {
        ret = -1;
    } else {
        *data = *r;
        data->data = b + head_size;
    }
    return ret;
}

static int read_for_v_render(data_queue_t *q, av_render_video_frame_t *data)
{
    uint8_t *b;
    int size;
    int ret = data_queue_read_lock(q, (void **)&b, &size);
    RETURN_ON_FAIL(ret);
    av_render_video_frame_t *r = (av_render_video_frame_t *)b;
    if (r->data > b && r->data + r->size == b + size) {
        *data = *r;
        return ret;
    }
    int head_size = sizeof(av_render_video_frame_t);
    if (r->size + head_size != size) {
        ret = -1;
    } else {
        *data = *r;
        data->data = b + head_size;
    }
    return ret;
}

static void dump_data(av_render_dump_type_t type, uint8_t *data, int size)
{
    static FILE *fp[AV_RENDER_DUMP_STOP_INDEX];
    static uint8_t dump_count = 0;
    if (type == AV_RENDER_DUMP_STOP_INDEX) {
        for (int i = 0; i < AV_RENDER_DUMP_STOP_INDEX; i++) {
            if (fp[i]) {
                fclose(fp[i]);
                fp[i] = NULL;
            }
        }
        dump_count++;
        if (dump_count > 9) {
            dump_count = 0;
        }
        return;
    }
    if ((render_dump_mask & (1 << type)) == 0) {
        return;
    }
    if (size == 0) {
        return;
    }
    if (fp[type] == NULL) {
        char *pre_name[] = { "ad", "ar", "vd", "vr" };
        char file_name[20];
        snprintf(file_name, sizeof(file_name), "/sdcard/%s%d.bin", pre_name[type], dump_count);
        fp[type] = fopen(file_name, "wb");
        if (fp[type]) {
            ESP_LOGI(TAG, "dump to %s", file_name);
        }
    }
    if (fp[type]) {
        fwrite(data, size, 1, fp[type]);
    }
}

static int decode_audio(av_render_adec_res_t *adec_res, av_render_audio_data_t *data)
{
    // 单帧解码后通过 frame_reached 回调继续送往音频渲染阶段。
    int ret = 0;
    if (data->size || data->eos) {
        dump_data(AV_RENDER_DUMP_ADEC_DATA, data->data, data->size);
        ret = adec_decode(adec_res->adec, data);
        if (ret != 0) {
            av_render_t *render = adec_res->thread_res.render;
            adec_res->audio_err_cnt++;
            if (adec_res->audio_err_cnt == AUDIO_ERR_FRAME_TOLERANCE) {
                adec_res->audio_err_cnt++;
                if (render->event_cb) {
                    render->event_cb(AV_RENDER_EVENT_AUDIO_DECODE_ERR, render->event_ctx);
                }
            }
            ESP_LOGE(TAG, "Fail to decode audio data ret %d size:%" PRIu32, ret, data->size);
        } else {
            adec_res->audio_err_cnt = 0;
        }
    }
    return ret;
}

static int adec_body(av_render_thread_res_t *res, bool drop)
{
    av_render_audio_data_t data;
    av_render_adec_res_t *adec_res = (av_render_adec_res_t *)res;
    int ret = read_for_adec(res->data_q, &data, res->use_pool);
    RETURN_ON_FAIL(ret);
    // EOS 包允许没有数据，只用于推动下游结束当前流。
    if (drop == false) {
        ret = decode_audio(adec_res, &data);
    }
    if (data.data && res->use_pool) {
        res->render->pool_free(data.data, res->render->pool);
    }
    data_queue_read_unlock(res->data_q);
    if (data.eos) {
        ESP_LOGI(TAG, "Decode finished");
        if (res->render->cfg.quit_when_eos == false) {
            res->paused = true;
        } else {
            return 1;
        }
    }
    return 0;
}

static int get_video_sync_time(av_render_t *render, av_render_video_res_t *v_render, uint32_t *cur_time,
                               uint32_t *ref_time)
{
    int ret = 0;
    *ref_time = get_cur_time() - v_render->video_start_time + v_render->video_start_pts;
    if (render->cfg.sync_mode == AV_RENDER_SYNC_FOLLOW_AUDIO) {
        ret = av_render_get_audio_pts(render, cur_time);
    } else {
        *cur_time = *ref_time;
    }
    return ret;
}

static int video_sync_control_before_decode(av_render_t *render, uint32_t video_pts, int q_num, bool *skip)
{
    // 解码前只做“是否丢弃”的粗粒度同步，避免为已经严重落后的视频帧浪费解码算力。
    av_render_video_res_t *v_render = render->v_render_res;
    if (render->cfg.allow_drop_data == false) {
        return 0;
    }
    // 未启用同步时不做等待或丢帧判断。
    if (render->cfg.sync_mode == AV_RENDER_SYNC_NONE) {
        if (q_num >= 2) {
            *skip = true;
        }
        return 0;
    }
    uint32_t now, ref_time;
    int ret = get_video_sync_time(render, v_render, &now, &ref_time);
    RETURN_ON_FAIL(ret);
    if (v_render->sent_frame_num) {
        // 首帧解码成功前不丢数据；之后允许 100 ms 容差，超出则跳过落后帧。
        if (video_pts + 100 < now) {
            printf("D %d < %d\n", (int)video_pts, (int)now);
            *skip = true;
        }
    }
    return 0;
}

static int audio_drop_before_render(av_render_t *render, int size, bool *skip)
{
    // 音频一般作为同步基准，仅在无同步且输出可计算时长时根据积压量做保护性丢弃。
    if (render->cfg.allow_drop_data == false) {
        return 0;
    }
    av_render_audio_frame_info_t *info = &render->a_render_res->out_frame_info;
    // No need sync, return directly
    if (render->cfg.sync_mode == AV_RENDER_SYNC_NONE && info->sample_rate) {
        int sample_size = info->channel * info->bits_per_sample >> 3;
        int latency = size / sample_size * 1000 / info->sample_rate;
        if (latency >= 200) {
            *skip = true;
        }
    }
    return 0;
}

static int decode_video(av_render_vdec_res_t *vdec_res, av_render_video_data_t *data)
{
    // 无独立渲染线程时直接向 renderer 申请 frame buffer，可减少一次大图像拷贝。
    av_render_t *render = vdec_res->thread_res.render;
    if (render->v_render_res->thread_res.thread == NULL) {
        // frame buffer 直出模式不能跨线程持有，因此只用于同步渲染路径。
        av_render_frame_buffer_t frame_buffer = { 0 };
        int ret = video_render_get_frame_buffer(render->cfg.video_render, &frame_buffer);
        if (ret == 0) {
            vdec_set_frame_buffer(vdec_res->vdec, &frame_buffer);
        }
    }
    int ret = 0;
    if (data->size || data->eos) {
        dump_data(AV_RENDER_DUMP_VDEC_DATA, data->data, data->size);
        int ret = vdec_decode(vdec_res->vdec, data);
        if (ret != 0) {
            vdec_res->video_err_cnt++;
            if (vdec_res->video_err_cnt == AUDIO_ERR_FRAME_TOLERANCE) {
                vdec_res->video_err_cnt++;
                if (render->event_cb) {
                    render->event_cb(AV_RENDER_EVENT_VIDEO_DECODE_ERR, render->event_ctx);
                }
            }
            ESP_LOGE(TAG, "Fail to decode video data ret %d size:%" PRIu32, ret, data->size);
        } else {
            vdec_res->video_err_cnt = 0;
        }
    }
    return ret;
}

static int vdec_body(av_render_thread_res_t *res, bool drop)
{
    av_render_video_data_t data;
    av_render_vdec_res_t *vdec_res = (av_render_vdec_res_t *)res;
    int ret = read_for_vdec(res->data_q, &data, vdec_res->thread_res.render->pool_free != NULL);
    RETURN_ON_FAIL(ret);
    int q_num = 0, q_size = 0;
    data_queue_query(res->data_q, &q_num, &q_size);
    // EOS 包可为空，用于结束/冲刷解码器。
    if (data.size || data.eos) {
        bool skip = false;
        video_sync_control_before_decode(res->render, data.pts, q_num, &skip);
        if (drop == false && (skip == false || data.eos)) {
            decode_video(vdec_res, &data);
        }
    }
    if (data.data && vdec_res->thread_res.use_pool) {
        vdec_res->thread_res.render->pool_free(data.data, vdec_res->thread_res.render->pool);
    }
    data_queue_read_unlock(res->data_q);
    if (data.eos) {
        ESP_LOGI(TAG, "Decode finished");
        if (res->render->cfg.quit_when_eos == false) {
            res->paused = true;
        } else {
            return 1;
        }
    }
    return 0;
}

static int video_sync_control_before_render(av_render_t *render, uint32_t video_pts, bool *skip)
{
    // 渲染前执行精确 A/V 同步：视频早到则等待，晚到且允许丢帧时跳过，时钟跳变则重同步。
    av_render_video_res_t *v_render = render->v_render_res;
    // No need sync, return directly
    if (render->cfg.sync_mode == AV_RENDER_SYNC_NONE) {
        return 0;
    }
    // 若解码已在同步线程中完成预控制，这里仍以实际渲染时钟做最终校正。
    uint32_t now, ref_time, audio_pts;
    int ret = get_video_sync_time(render, v_render, &audio_pts, &ref_time);
    RETURN_ON_FAIL(ret);
    now = audio_pts;
    int fps = v_render->video_frame_info.fps;
    if (fps == 0) {
        fps = 20;
    }
    if (v_render->sent_frame_num) {
        uint32_t video_cur_pts = 0;
        av_render_get_video_pts(render, &video_cur_pts);
        bool in_sync = false;
        for (int i = 0; i < 2; i++) {
            if (video_cur_pts < now + v_render->sync_tolerance && video_cur_pts + v_render->sync_tolerance > now) {
                in_sync = true;
                break;
            }
            if (now == ref_time) {
                break;
            }
            now = ref_time;
            continue;
        }
        if (in_sync) {
            if (video_cur_pts > now) {
                uint32_t sleep_time = video_cur_pts - now;
                uint32_t max_frame_time = 2 * 1000 / fps;
                if (sleep_time > max_frame_time) {
                    sleep_time = max_frame_time;
                }
                media_lib_thread_sleep(sleep_time);
            } else {
                // TODO need more accurate to control drop threshold
                uint32_t frame_pts = 1000 / fps * 4;
                // Only do drop if not drop data before decode
                if (video_pts + frame_pts <= now && render->cfg.allow_drop_data == false) {
                    *skip = true;
                }
            }
            if ((v_render->sent_frame_num % fps) == 0) {
                ESP_LOGI(TAG, "Video pts:%d now:%d ref:%d", (int)video_cur_pts, (int)audio_pts, (int)ref_time);
            }
        } else {
            // 时间戳发生大跳变时从当前帧重新建立基准，避免永久追赶旧时间线。
            v_render->sent_frame_num = 0;
            ESP_LOGE(TAG, "Video lost sync now:%d audio:%d ref:%d", (int)video_cur_pts, (int)audio_pts, (int)ref_time);
        }
    }
    if (v_render->sent_frame_num == 0) {
        ESP_LOGI(TAG, "Video start pts set to %" PRIu32, video_pts);
        v_render->video_start_time = get_cur_time();
        v_render->video_start_pts = video_pts;
    }
    v_render->sent_frame_num++;
    return 0;
}

static int _render_write_audio(av_render_thread_res_t *res, av_render_audio_frame_t *audio_frame)
{
    // 音频写入成功后更新主时钟 PTS；视频同步模式会读取这个值。
    if (res->render->a_render_res->audio_rendered == false) {
        if (res->render->event_cb) {
            res->render->event_cb(AV_RENDER_EVENT_AUDIO_RENDERED, res->render->event_ctx);
        }
        if (res->render->cfg.pause_on_first_frame) {
            res->paused = true;
        }
        ESP_LOGI(TAG, "%s Auto pause audio first frame at %" PRIu32, res->name, audio_frame->pts);
        res->render->a_render_res->audio_rendered = true;
    }
    if (res->paused) {
        return 0;
    }
    // 记录已经实际交给硬件的音频 PTS，而不是仅仅解码完成的 PTS。
    res->render->a_render_res->audio_send_pts = audio_frame->pts;
    int ret = 0;
    if (res->flushing == false) {
        ret = audio_render_write(res->render->cfg.audio_render, audio_frame);
        if (ret != 0) {
            ESP_LOGE(TAG, "Fail to render audio ret %d", ret);
            return ret;
        }
    }
    if (audio_frame->eos) {
        if (res->render->event_cb) {
            res->render->event_cb(AV_RENDER_EVENT_AUDIO_EOS, res->render->event_ctx);
        }
    }
    return ret;
}

static void _video_check_first_frame_render_notify(av_render_thread_res_t *res, av_render_video_frame_t *video_frame)
{
    if (res->render->v_render_res->video_rendered == false) {
        ESP_LOGI(TAG, "%s Auto pause video first frame at %" PRIu32, res->name, video_frame->pts);
        if (res->render->event_cb) {
            res->render->event_cb(AV_RENDER_EVENT_VIDEO_RENDERED, res->render->event_ctx);
        }
        if (res->render->cfg.pause_on_first_frame) {
            res->paused = true;
            if (res->render->v_render_res->v_render_in_sync && res->render->vdec_res) {
                // Auto pause decoder if render in sync
                res->render->vdec_res->thread_res.paused = true;
            }
        }
        res->render->v_render_res->video_rendered = true;
        // Do not return when pause on first frame, so that it can show the picture after seek
    }
}

static int _render_write_video(av_render_thread_res_t *res, av_render_video_frame_t *video_frame)
{
    // start_pts 之前的帧不显示；flush 期间也只消费数据，不把画面送到屏幕。
    int ret = 0;
    // Only render when PTS exceed start PTS
    res->render->v_render_res->video_send_pts = video_frame->pts;
    if (video_frame->pts >= res->render->v_render_res->video_start_pts) {
        _video_check_first_frame_render_notify(res, video_frame);
        // flush 只排空管线，不显示途中残留画面。
        if (res->flushing == false) {
            bool skip = false;
            if (res->paused && res->render->v_render_res->v_render_in_sync && res->render->vdec_res && res->render->vdec_res->thread_res.paused == false) {
                res->paused = false;
            }
            if (res->paused == false) {
                // 真正写屏前根据选定时钟执行最后一次同步判断。
                video_sync_control_before_render(res->render, video_frame->pts, &skip);
            }
            if (1 || skip == false) {
                ret = video_render_write(res->render->cfg.video_render, video_frame);
            }
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to render video ret %d", ret);
            }
        }
    }
    if (video_frame->eos) {
        _video_check_first_frame_render_notify(res, video_frame);
        if (res->render->event_cb) {
            res->render->event_cb(AV_RENDER_EVENT_VIDEO_EOS, res->render->event_ctx);
        }
    }
    return ret;
}

static int a_render_body(av_render_thread_res_t *res, bool drop)
{
    av_render_audio_frame_t data;
    int ret = read_for_a_render(res->data_q, &data);
    RETURN_ON_FAIL(ret);
    bool skip = false;
    if (data.size) {
        int q_num = 0, q_size = 0;
        data_queue_query(res->data_q, &q_num, &q_size);
        if (res->paused == false && res->render->audio_threshold) {
            if (res->render->a_render_res->audio_rendered == false) {
                if (q_size < res->render->audio_threshold) {
                    data_queue_peek_unlock(res->data_q);
                    // Wait for data reach threshold
                    media_lib_thread_sleep(10);
                    return 0;
                }
            } else if (q_num < 3) {
                res->render->a_render_res->audio_rendered = false;
                data_queue_peek_unlock(res->data_q);
                media_lib_thread_sleep(10);
                return 0;
            }
        }
        audio_drop_before_render(res->render, q_size, &skip);
    }
    if (drop == false && skip == false && (data.size || data.eos)) {
        ret = _render_write_audio(res, &data);
        if (ret != 0) {
            ESP_LOGE(TAG, "Fail to render audio");
        }
    }
    if (res->paused) {
        data_queue_peek_unlock(res->data_q);
    } else {
        data_queue_read_unlock(res->data_q);
    }
    return 0;
}

static int v_render_body(av_render_thread_res_t *res, bool drop)
{
    av_render_video_frame_t data;
    int ret = read_for_v_render(res->data_q, &data);
    RETURN_ON_FAIL(ret);
    if (drop == false && (data.size || data.eos)) {
        av_render_vdec_res_t *vdec_res = res->render->vdec_res;
        if (vdec_res && vdec_res->vid_convert) {
            // Do color convert firstly
            ret = convert_color(vdec_res->vid_convert,
                 data.data, data.size,
                 vdec_res->vid_convert_out, vdec_res->vid_convert_out_size);
            data.data = vdec_res->vid_convert_out;
            data.size = vdec_res->vid_convert_out_size;
        }
        ret = _render_write_video(res, &data);
        if (ret != 0) {
            ESP_LOGE(TAG, "Fail to render video");
        }
    }
    if (res->paused) {
        data_queue_peek_unlock(res->data_q);
    } else {
        data_queue_read_unlock(res->data_q);
    }
    return 0;
}

static int create_thread_res(av_render_thread_res_t *res, const char *name,
                             int (*body)(av_render_thread_res_t *res, bool drop),
                             int buffer_size, int wait_bits)
{
    // 只有配置了 FIFO 才创建线程；控制消息队列固定很小，媒体数据使用独立字节环形队列。
    do {
        if (res->msg_q == NULL) {
            res->msg_q = msg_q_create(4, sizeof(av_render_msg_t));
        }
        if (res->msg_q == NULL) {
            break;
        }
        res->name = name;
        if (res->data_q == NULL) {
            res->data_q = data_queue_init(buffer_size);
        }
        if (res->data_q == NULL) {
            break;
        }
        res->wait_bits = wait_bits;
        res->render_body = body;
        int ret = media_lib_thread_create_from_scheduler(&res->thread, name, render_thread, res);
        BREAK_ON_FAIL(ret);
        return 0;
    } while (0);
    return -1;
}

static void destroy_thread_res(av_render_thread_res_t *res)
{
    // 先唤醒所有可能阻塞的数据读写，再销毁消息队列和 FIFO，线程会自行退出。
    if (res->msg_q) {
        msg_q_destroy(res->msg_q);
        res->msg_q = NULL;
    }
    if (res->data_q) {
        data_queue_deinit(res->data_q);
        res->data_q = NULL;
    }
    // 工作线程收到 CLOSE 后自行退出，此处只回收它依赖的同步资源。
}

static int send_msg_to_thread(av_render_thread_res_t *res, int head_size, av_render_msg_t *msg)
{
    // 发送控制消息前尽量唤醒因 FIFO 满/空而等待的线程，避免控制命令被数据等待卡住。
    int ret = -1;
    if (res->msg_q) {
        ret = msg_q_send(res->msg_q, msg, sizeof(av_render_msg_t));
    }
    // 唤醒 FIFO 等待者，让线程有机会先处理 pause/flush/close。
    if (res->data_q) {
        int q_num = 0, q_size = 0;
        data_queue_query(res->data_q, &q_num, &q_size);
        if (q_num == 0) {
            uint8_t *b = (uint8_t *)data_queue_get_buffer(res->data_q, head_size);
            if (b) {
                memset(b, 0, head_size);
                data_queue_send_buffer(res->data_q, head_size);
            }
        }
    }
    return ret;
}

static const char *msg_to_str(av_render_msg_type_t msg)
{
    switch (msg) {
        case AV_RENDER_MSG_CLOSE:
            return "Close";
        case AV_RENDER_MSG_PAUSE:
            return "Pause";
        case AV_RENDER_MSG_RESUME:
            return "Resume";
        case AV_RENDER_MSG_FLUSH:
            return "Flush";
        default:
            return "";
    }
}

static void render_consume_all(av_render_thread_res_t *res)
{
    // 线程退出时丢弃并释放残留数据；外部池模式还要调用用户提供的 pool_free。
    if (res->use_pool == false) {
        data_queue_consume_all(res->data_q);
    } else {
        while (data_queue_have_data(res->data_q)) {
            res->render_body(res, true);
        }
    }
}

static void render_thread(void *arg)
{
    // 四类线程共用同一循环：优先处理控制消息，再从 FIFO 取一帧调用各自 render_body。
    // flush 会持续消费但不输出，pause 保留队列，close 则退出并通知等待方。
    av_render_thread_res_t *res = (av_render_thread_res_t *)arg;
    while (1) {
        av_render_msg_t msg = { 0 };
        msg_q_recv(res->msg_q, &msg, sizeof(av_render_msg_t), !res->paused);
        if (msg.type != AV_RENDER_MSG_NONE) {
            ESP_LOGI(TAG, "%s got msg:%s", res->name, msg_to_str(msg.type));
        }
        if (msg.type == AV_RENDER_MSG_CLOSE) {
            break;
        }
        if (msg.type == AV_RENDER_MSG_FLUSH) {
            render_consume_all(res);
            _SET_BITS(res->render->event_group, res->wait_bits << FLUSH_SHIFT_BITS);
            res->flushing = false;
        }
        if (msg.type == AV_RENDER_MSG_PAUSE) {
            res->paused = true;
            continue;
        }
        if (msg.type == AV_RENDER_MSG_RESUME) {
            ESP_LOGI(TAG, "%s resumed", res->name);
            res->paused = false;
        }
        if (res->paused) {
            continue;
        }
        if (res->render_body) {
            int ret = res->render_body(res, false);
            if (ret != 0) {
                ESP_LOGE(TAG, "Thread %s process fail %d", res->name, ret);
                break;
            }
        }
    }
    res->thread = NULL;
    ESP_LOGI(TAG, "Thread %s exited", res->name);
    // 退出前唤醒等待本线程关闭的 API，并消费所有残留数据。
    render_consume_all(res);
    _SET_BITS(res->render->event_group, res->wait_bits);
    media_lib_thread_destroy(NULL);
}

static bool audio_need_decode_in_sync(av_render_t *render, av_render_audio_info_t *audio_info)
{
    if (audio_info->codec == AV_RENDER_AUDIO_CODEC_PCM || render->cfg.audio_raw_fifo_size == 0) {
        return true;
    }
    return false;
}

static bool video_is_raw(av_render_video_info_t *video_info)
{
    if (video_info->codec == AV_RENDER_VIDEO_CODEC_YUV420 || video_info->codec == AV_RENDER_VIDEO_CODEC_YUV422 || video_info->codec == AV_RENDER_VIDEO_CODEC_RGB565) {
        return true;
    }
    return false;
}

static bool video_need_decode_in_sync(av_render_t *render, av_render_video_info_t *video_info)
{
    if (video_is_raw(video_info) || render->cfg.video_raw_fifo_size == 0) {
        return true;
    }
    return false;
}

static bool audio_need_render_in_sync(av_render_t *render)
{
    if (render->cfg.audio_render_fifo_size == 0) {
        return true;
    }
    return false;
}

static bool audio_need_resample(av_render_audio_res_t *a_render)
{
    if (0 != memcmp(&a_render->out_frame_info, &a_render->audio_frame_info, sizeof(av_render_audio_frame_info_t))) {
        return true;
    }
    return false;
}

static bool video_need_render_in_sync(av_render_t *render)
{
    if (render->cfg.video_render_fifo_size == 0) {
        return true;
    }
    return false;
}

int audio_render_frame_reached(av_render_audio_frame_t *frame, void *ctx)
{
    // 解码器输出回调：有音频渲染线程则入队，没有则在当前线程直接写硬件。
    av_render_audio_res_t *a_render = (av_render_audio_res_t *)ctx;
    int ret = -1;
    dump_data(AV_RENDER_DUMP_ARENDER_DATA, frame->data, frame->size);
    if (a_render->audio_packet_reached) {
        // 是否跨线程由音频渲染 FIFO 配置决定。
        if (a_render->thread_res.thread) {
            ret = put_to_a_render(a_render->thread_res.data_q, frame);
        } else {
            ret = _render_write_audio(&a_render->thread_res, frame);
        }
    }
    return ret;
}

static int av_render_audio_frame_reached(av_render_audio_frame_t *frame, void *ctx)
{
    // 首帧可修正实际采样参数并重开底层 renderer；后续帧可选先经过重采样器。
    av_render_t *render = (av_render_t *)ctx;
    av_render_audio_res_t *a_render = render->a_render_res;
    if (a_render == NULL) {
        return 0;
    }
    av_render_adec_res_t *adec_res = render->adec_res;
    int ret = 0;
    // 首帧到达时才打开硬件，解码器此时才能提供可信的输出格式。
    if (a_render->audio_packet_reached == false) {
        if (a_render->audio_is_pcm == false) {
            ret = -1;
            // 优先采用解码器报告的真实输出参数修正初始流信息。
            if (adec_res && adec_res->adec) {
                ret = adec_get_frame_info(adec_res->adec, &a_render->audio_frame_info);
            }
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to get audio frame information");
                return ret;
            }
        }
        // 用修正后的帧信息重新打开音频 renderer。
        audio_render_close(render->cfg.audio_render);
        ESP_LOGI(TAG, "Get need resample %d in:%d out:%d", a_render->need_resample,
                 (int)a_render->audio_frame_info.sample_rate, (int)a_render->out_frame_info.sample_rate);
        if (a_render->need_resample && audio_need_resample(a_render)) {
            ret = audio_render_open(render->cfg.audio_render, &a_render->out_frame_info);
            audio_resample_cfg_t resample_cfg = {
                .input_info = a_render->audio_frame_info,
                .output_info = a_render->out_frame_info,
                .resample_cb = audio_render_frame_reached,
                .ctx = a_render,
            };
            a_render->resample_handle = audio_resample_open(&resample_cfg);
            if (a_render->resample_handle == NULL) {
                ESP_LOGE(TAG, "Fail to create audio resample");
                ret = -1;
            }
        } else {
            if (a_render->resample_handle) {
                audio_resample_close(a_render->resample_handle);
                a_render->resample_handle = NULL;
            }
            ret = audio_render_open(render->cfg.audio_render, &a_render->audio_frame_info);
        }
        if (ret != 0) {
            ESP_LOGE(TAG, "Fail to create audio render");
            return ret;
        }
        a_render->audio_packet_reached = true;
        a_render->a_render_in_sync = true;
        a_render->thread_res.render = render;
        if (audio_need_render_in_sync(render) == false && a_render->thread_res.thread == NULL) {
            ret = create_thread_res(&a_render->thread_res, "ARender", a_render_body, render->cfg.audio_render_fifo_size,
                                    A_RENDER_CLOSED_BITS);
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to create audio render thread resource");
            } else {
                a_render->a_render_in_sync = true;
            }
        }
    }
    if (a_render->resample_handle) {
        // 硬件输出参数不匹配时，先重采样再进入 renderer。
        ret = audio_resample_write(a_render->resample_handle, frame);
    } else {
        ret = audio_render_frame_reached(frame, a_render);
    }
    return ret;
}

static void convert_to_audio_frame(av_render_audio_info_t *audio_info, av_render_audio_frame_info_t *frame_info)
{
    frame_info->bits_per_sample = audio_info->bits_per_sample;
    frame_info->channel = audio_info->channel;
    frame_info->sample_rate = audio_info->sample_rate;
}

static uint8_t *av_render_fetch_vid_fb(int align, int size, void *ctx)
{
    av_render_t *render = (av_render_t *)ctx;
    av_render_video_res_t *v_render = render->v_render_res;
    if (v_render == NULL || size == 0) {
        return NULL;
    }
    av_render_vdec_res_t *vdec_res = render->vdec_res;
    size = sizeof(av_render_video_frame_t) + size + align;
    uint8_t *b = (uint8_t *)data_queue_get_buffer(v_render->thread_res.data_q, size);
    if (b == NULL) {
        return NULL;
    }
    vdec_res->fb_frame = (av_render_video_frame_t *)b;
    b += sizeof(av_render_video_frame_t);
    align -= 1;
    vdec_res->fb_frame->data = (uint8_t *)(((uint32_t)b + align) & (~align));
    vdec_res->fb_frame->size = 0;
    return vdec_res->fb_frame->data;
}

static int av_render_release_vid_fb(uint8_t *addr, bool drop, void *ctx)
{
    av_render_t *render = (av_render_t *)ctx;
    av_render_video_res_t *v_render = render->v_render_res;
    if (v_render == NULL) {
        return 0;
    }
    av_render_vdec_res_t *vdec_res = render->vdec_res;
    if (vdec_res->fb_frame == NULL || addr != vdec_res->fb_frame->data) {
        ESP_LOGE(TAG, "Release wrong data");
    }
    uint32_t size = 0;
    if (drop == false) {
        size = vdec_res->fb_frame->size + (uint32_t)(addr - (uint8_t *)vdec_res->fb_frame);
    }
    return data_queue_send_buffer(v_render->thread_res.data_q, size);
}

static int av_render_video_frame_reached(av_render_video_frame_t *frame, void *ctx)
{
    // 视频解码输出入口：首帧确定实际格式/尺寸，必要时创建颜色转换，再选择入队或直接渲染。
    av_render_t *render = (av_render_t *)ctx;
    av_render_video_res_t *v_render = render->v_render_res;
    if (v_render == NULL) {
        return 0;
    }
    av_render_vdec_res_t *vdec_res = render->vdec_res;
    int ret = 0;
    dump_data(AV_RENDER_DUMP_VRENDER_DATA, frame->data, frame->size);
    // 与音频相同，等首帧拿到可信参数后才打开底层视频 renderer。
    if (v_render->video_packet_reached == false) {
        if (v_render->video_is_raw == false) {
            ret = -1;
            // 使用解码器实际输出格式、尺寸和帧率修正协商值。
            int old_fps = v_render->video_frame_info.fps;
            if (vdec_res && vdec_res->vdec) {
                ret = vdec_get_frame_info(vdec_res->vdec, &v_render->video_frame_info);
            }
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to get video frame information");
                return ret;
            }
            if (vdec_res->dec_out_fmt != vdec_res->out_fmt) {
                color_convert_cfg_t convert_cfg = {
                    .from = vdec_res->dec_out_fmt,
                    .to = vdec_res->out_fmt,
                    .width = v_render->video_frame_info.width,
                    .height = v_render->video_frame_info.height,
                };
                vdec_res->vid_convert = init_convert_table(&convert_cfg);
                if (vdec_res->vid_convert == NULL) {
                    ESP_LOGE(TAG, "Fail to init video convert");
                    return ESP_MEDIA_ERR_NO_MEM;
                }
            }
            if (vdec_res && vdec_res->vid_convert) {
                // 首帧确定输出尺寸后再分配颜色转换缓冲，避免按错误尺寸预分配。
                int image_size = convert_table_get_image_size(vdec_res->out_fmt,
                        v_render->video_frame_info.width,
                        v_render->video_frame_info.height);
                uint8_t* vid_cvt_out = media_lib_realloc(vdec_res->vid_convert_out, image_size);
                if (vid_cvt_out == NULL) {
                    ESP_LOGE(TAG, "Fail to allocate video convert output");
                    return ESP_MEDIA_ERR_NO_MEM;
                }
                vdec_res->vid_convert_out = vid_cvt_out;
                vdec_res->vid_convert_out_size = image_size;
                v_render->video_frame_info.type = vdec_res->out_fmt;
            }
            if (v_render->video_frame_info.fps == 0) {
                v_render->video_frame_info.fps = old_fps;
            }
        }
        // 用最终帧格式重新打开视频 renderer。
        video_render_close(render->cfg.video_render);
        ret = video_render_open(render->cfg.video_render, &v_render->video_frame_info);
        if (ret != 0) {
            ESP_LOGE(TAG, "Fail to create video render");
            return ret;
        }
        v_render->video_packet_reached = true;
        v_render->v_render_in_sync = true;
        // Tolerance for 3 frame later
        v_render->sync_tolerance = 600;
        v_render->thread_res.render = render;
        if (v_render->use_fb == false && video_need_render_in_sync(render) == false && v_render->thread_res.thread == NULL) {
            ret = create_thread_res(&v_render->thread_res, "VRender", v_render_body, render->cfg.video_render_fifo_size,
                                    V_RENDER_CLOSED_BITS);
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to create video render thread resource");
            } else {
                v_render->v_render_in_sync = false;
            }
        }
        v_render->thread_res.name = "VRender";
    }
    if (v_render->video_packet_reached) {
        // 有渲染 FIFO 时跨线程入队，否则在解码回调线程直接渲染。
        if (v_render->thread_res.thread) {
            if (v_render->use_fb) {
                // Update frame information only
                if (vdec_res->fb_frame) {
                    uint8_t *frame_data = vdec_res->fb_frame->data;
                    memcpy(vdec_res->fb_frame, frame, sizeof(av_render_video_frame_t));
                    vdec_res->fb_frame->data = frame_data;
                }
            } else {
                ret = put_to_v_render(v_render->thread_res.data_q, frame);
            }
        } else {
            av_render_vdec_res_t *vdec_res = render->vdec_res;
            if (vdec_res && vdec_res->vid_convert) {
                // Do color convert firstly
                ret = convert_color(vdec_res->vid_convert,
                    frame->data, frame->size,
                    vdec_res->vid_convert_out, vdec_res->vid_convert_out_size);
                frame->data = vdec_res->vid_convert_out;
                frame->size = vdec_res->vid_convert_out_size;
            }
            ret = _render_write_video(&v_render->thread_res, frame);
        }
    }
    return ret;
}

static void convert_to_video_frame(av_render_video_info_t *video_info, av_render_video_frame_info_t *frame_info)
{
    memset(frame_info, 0, sizeof(av_render_video_frame_info_t));
    switch (video_info->codec) {
        case AV_RENDER_VIDEO_CODEC_YUV420:
            frame_info->type = AV_RENDER_VIDEO_RAW_TYPE_YUV420;
            break;
        case AV_RENDER_VIDEO_CODEC_YUV422:
            frame_info->type = AV_RENDER_VIDEO_RAW_TYPE_YUV422;
            break;
        case AV_RENDER_VIDEO_CODEC_RGB565:
            frame_info->type = AV_RENDER_VIDEO_RAW_TYPE_RGB565;
            break;
        default:
            break;
    }
    frame_info->width = video_info->width;
    frame_info->height = video_info->height;
    frame_info->fps = video_info->fps;
}

static void correct_video_fps(av_render_video_res_t *v_render, uint32_t pts)
{
    // Considering minimum fps is 10 for file
    if (v_render->video_last_pts && pts > v_render->video_last_pts && pts <= v_render->video_last_pts + 100) {
        v_render->video_frame_info.fps = 1000 / (pts - v_render->video_last_pts);
        ESP_LOGI(TAG, "Correct video fps to %d", v_render->video_frame_info.fps);
    } else {
        v_render->video_last_pts = pts;
    }
}

av_render_handle_t av_render_open(av_render_cfg_t *cfg)
{
    // open 只创建总上下文、锁和事件组；具体解码器/renderer 等 add_*_stream() 后再按需创建。
    if (cfg == NULL || (cfg->audio_render == NULL && cfg->video_render == NULL)) {
        ESP_LOGE(TAG, "Arg wrong %p %p\n", cfg, cfg ? cfg->audio_render : NULL);
        return NULL;
    }
    av_render_t *render = (av_render_t *)calloc(1, sizeof(av_render_t));
    if (render == NULL) {
        return NULL;
    }
    // 保存用户提供的 renderer、同步模式和丢帧策略；底层 renderer 所有权仍归调用方。
    render->cfg = *cfg;
    do {
        int ret = media_lib_mutex_create(&render->api_lock);
        BREAK_ON_FAIL(ret);
        ret = media_lib_event_group_create(&render->event_group);
        BREAK_ON_FAIL(ret);
        return render;
    } while (0);
    av_render_close(render);
    return NULL;
}

int av_render_config_audio_fifo(av_render_handle_t h, av_render_fifo_cfg_t *fifo_cfg)
{
    // 必须在添加音频流前配置；FIFO 大于 0 才会把对应阶段拆到独立线程。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || fifo_cfg == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    render->cfg.audio_raw_fifo_size = fifo_cfg->raw_fifo_size;
    render->cfg.audio_render_fifo_size = fifo_cfg->render_fifo_size;
    media_lib_mutex_unlock(render->api_lock);
    return ESP_MEDIA_ERR_OK;
}

int av_render_config_video_fifo(av_render_handle_t h, av_render_fifo_cfg_t *fifo_cfg)
{
    // 分别控制视频解码 FIFO 和渲染 FIFO，决定内存、吞吐与端到端延迟的取舍。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || fifo_cfg == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    render->cfg.video_raw_fifo_size = fifo_cfg->raw_fifo_size;
    render->cfg.video_render_fifo_size = fifo_cfg->render_fifo_size;
    media_lib_mutex_unlock(render->api_lock);
    return ESP_MEDIA_ERR_OK;
}

int av_render_add_audio_stream(av_render_handle_t h, av_render_audio_info_t *audio_info)
{
    // 新音频流会关闭旧解码器、清空首帧状态，再按 codec 和 FIFO 配置重建解码/渲染资源。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || audio_info == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = 0;
    do {
        if (render->cfg.audio_render == NULL) {
            ESP_LOGE(TAG, "Audio render not set, stream is skipped");
            ret = ESP_MEDIA_ERR_NOT_SUPPORT;
            break;
        }
        // 音频渲染资源只创建一次，切流时复用容器并更新内部状态。
        if (render->a_render_res == NULL) {
            render->a_render_res = (av_render_audio_res_t *)media_lib_calloc(1, sizeof(av_render_audio_res_t));
            if (render->a_render_res == NULL) {
                ret = ESP_MEDIA_ERR_NO_MEM;
                break;
            }
        }
        if (render->aud_fix_info.sample_rate) {
            memcpy(&render->a_render_res->out_frame_info, &render->aud_fix_info, sizeof(av_render_audio_frame_info_t));
            render->a_render_res->need_resample = true;
        }
        av_render_audio_res_t *a_render = render->a_render_res;
        // 切流前关闭旧解码器，防止旧 codec 缓存污染新流。
        if (render->adec_res) {
            av_render_adec_res_t *adec_res = render->adec_res;
            if (adec_res->thread_res.thread) {
                av_render_msg_t msg = {
                    .type = AV_RENDER_MSG_CLOSE,
                };
                ret = send_msg_to_thread(&adec_res->thread_res, sizeof(av_render_audio_data_t), &msg);
                _WAIT_BITS(render->event_group, adec_res->thread_res.wait_bits);
            }
            adec_close(adec_res->adec);
            adec_res->adec = NULL;
        }
        // Clear frame number
        a_render->audio_packet_reached = false;
        a_render->audio_rendered = false;
        // PCM 可直接渲染；压缩格式才创建解码器及可选解码线程。
        a_render->audio_is_pcm = (audio_info->codec == AV_RENDER_AUDIO_CODEC_PCM);

        a_render->decode_in_sync = true;

        if (a_render->audio_is_pcm == false) {
            if (render->adec_res == NULL) {
                // 首次压缩音频流才分配解码资源容器。
                render->adec_res = (av_render_adec_res_t *)media_lib_calloc(1, sizeof(av_render_adec_res_t));
                if (render->adec_res == NULL) {
                    ret = ESP_MEDIA_ERR_NO_MEM;
                    break;
                }
            }
            av_render_adec_res_t *adec_res = render->adec_res;
            adec_cfg_t cfg = {
                .audio_info = *audio_info,
                .frame_cb = av_render_audio_frame_reached,
                .ctx = render,
            };
            adec_res->adec = adec_open(&cfg);
            if (adec_res->adec == NULL) {
                ESP_LOGE(TAG, "Fail to create audio decoder");
                ret = ESP_MEDIA_ERR_FAIL;
                break;
            }
            adec_res->thread_res.render = render;
            adec_res->thread_res.use_pool = (render->pool_free != NULL);
            // 配置了解码 FIFO 时创建异步解码线程，否则 add_audio_data() 同步解码。
            if (audio_need_decode_in_sync(render, audio_info) == false) {
                ret = create_thread_res(&adec_res->thread_res, "Adec", adec_body, render->cfg.audio_raw_fifo_size,
                                        ADEC_CLOSED_BITS);
                if (ret != 0) {
                    ESP_LOGE(TAG, "Fail to create thread for ADec");
                    ret = ESP_MEDIA_ERR_FAIL;
                    break;
                }
                // Clear decode in sync flag
                a_render->decode_in_sync = false;
            }
        } else {
            // Save to stream info
            convert_to_audio_frame(audio_info, &a_render->audio_frame_info);
            ESP_LOGI(TAG, "Save pcm frame information");
        }
    } while (0);
    media_lib_mutex_unlock(render->api_lock);
    if (ret != ESP_MEDIA_ERR_OK) {
        ESP_LOGE(TAG, "Fail to add video stream %d", ret);
    }
    return ret;
}

int av_render_set_event_cb(av_render_handle_t h, av_render_event_cb cb, void *ctx)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || cb == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = ESP_MEDIA_ERR_WRONG_STATE;
    render->event_ctx = ctx;
    render->event_cb = cb;
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

int av_render_set_fixed_frame_info(av_render_handle_t h, av_render_audio_frame_info_t *frame_info)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || frame_info == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = ESP_MEDIA_ERR_WRONG_STATE;
    render->aud_fix_info = *frame_info;
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

static bool get_support_output_format(av_render_t *render, av_render_video_info_t *video_info, vdec_cfg_t *cfg)
{
    av_render_video_frame_type_t out_type;
    av_render_video_frame_type_t out_fmts[6];
    av_render_vdec_res_t *vdec_res = render->vdec_res;
    uint8_t num = sizeof(out_fmts)/sizeof(out_fmts[0]);
    vdec_get_output_formats(video_info->codec, out_fmts, &num);
    // Try to match decoder supported formats
    for (int i = 0; i < num; i++) {
        if (video_render_format_supported(render->cfg.video_render, out_fmts[i])) {
            ESP_LOGI(TAG, "Set video decoder prefer output format %d", out_fmts[i]);
            cfg->out_type = out_fmts[i];
            vdec_res->out_fmt = out_fmts[i];
            vdec_res->dec_out_fmt = out_fmts[i];
            return true;
        }
    }
    for (out_type = AV_RENDER_VIDEO_RAW_TYPE_NONE + 1; out_type < AV_RENDER_VIDEO_RAW_TYPE_MAX; out_type++) {
        if (video_render_format_supported(render->cfg.video_render, out_type) == false) {
            continue;
        }
        ESP_LOGI(TAG, "Set video render output format %d", out_type);
        // Let decoder do color convert
        if (render->cfg.video_cvt_in_render == false) {
            cfg->out_type = out_type;
            vdec_res->out_fmt = out_type;
            vdec_res->dec_out_fmt = out_type;
            return true;
        }
        // User codec prefer one
        cfg->out_type = out_fmts[0];
        vdec_res->out_fmt = out_type;
        vdec_res->dec_out_fmt = out_fmts[0];
        return true;
    }
    return false;
}

int av_render_add_video_stream(av_render_handle_t h, av_render_video_info_t *video_info)
{
    // 视频切流与音频类似，但还需协商解码器输出格式、颜色转换和 frame buffer 模式。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || video_info == NULL) {
        return -ESP_MEDIA_ERR_INVALID_ARG;
    }
    int ret = 0;
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    do {
        if (render->cfg.video_render == NULL) {
            ESP_LOGE(TAG, "Video render not set, stream is skipped");
            ret = ESP_MEDIA_ERR_NOT_SUPPORT;
            break;
        }
        // 视频渲染资源按需创建，后续切流复用外壳并重置状态。
        if (render->v_render_res == NULL) {
            render->v_render_res = (av_render_video_res_t *)media_lib_calloc(1, sizeof(av_render_video_res_t));
            if (render->v_render_res == NULL) {
                ret = ESP_MEDIA_ERR_NO_MEM;
                break;
            }
        }
        av_render_video_res_t *v_render = render->v_render_res;
        // TODO here force to use fetch decode output data
        v_render->use_fb = true;
        // 关闭旧解码器和颜色转换资源，避免跨分辨率/格式复用错误缓冲区。
        if (render->vdec_res) {
            av_render_vdec_res_t *vdec_res = render->vdec_res;
            if (vdec_res->thread_res.thread) {
                av_render_msg_t msg = {
                    .type = AV_RENDER_MSG_CLOSE,
                };
                ret = send_msg_to_thread(&vdec_res->thread_res, sizeof(av_render_video_data_t), &msg);
                if (ret == 0) {
                    _WAIT_BITS(render->event_group, vdec_res->thread_res.wait_bits);
                }
            }
            vdec_close(vdec_res->vdec);
            vdec_res->vdec = NULL;
            if (vdec_res->vid_convert) {
                deinit_convert_table(vdec_res->vid_convert);
                vdec_res->vid_convert = NULL;
            }
        }
        // Clear frame number
        v_render->video_packet_reached = false;
        v_render->video_rendered = false;
        // 原始图像可直接渲染，H264/MJPEG 等压缩流需要解码器。
        v_render->video_is_raw = video_is_raw(video_info);

        // Set initial decoder in sync flag
        v_render->decode_in_sync = true;

        if (v_render->video_is_raw == false) {
            if (render->vdec_res == NULL) {
                // 首次压缩视频流才分配视频解码资源容器。
                render->vdec_res = (av_render_vdec_res_t *)media_lib_calloc(1, sizeof(av_render_vdec_res_t));
                if (render->vdec_res == NULL) {
                    ret = ESP_MEDIA_ERR_NO_MEM;
                    break;
                }
            }
            av_render_vdec_res_t *vdec_res = render->vdec_res;
            vdec_cfg_t cfg = {
                .video_info = *video_info,
                .frame_cb = av_render_video_frame_reached,
                .ctx = render,
            };
            if (get_support_output_format(render, video_info, &cfg) == false) {
                break;
            }
            vdec_res->vdec = vdec_open(&cfg);
            if (vdec_res->vdec == NULL) {
                ESP_LOGE(TAG, "Fail to create video decoder");
                ret = ESP_MEDIA_ERR_FAIL;
                break;
            }
            vdec_res->thread_res.render = render;
            vdec_res->thread_res.use_pool = (render->pool_free != NULL);
            v_render->thread_res.render = render;
            // When use FB pre create render resource
            if (v_render->use_fb && video_need_render_in_sync(render) == false && v_render->thread_res.thread == NULL) {
                ret = create_thread_res(&v_render->thread_res, "VRender", v_render_body, render->cfg.video_render_fifo_size,
                                        V_RENDER_CLOSED_BITS);
                if (ret != 0) {
                    ESP_LOGE(TAG, "Fail to create video render thread resource");
                } else {
                    v_render->v_render_in_sync = false;
                    vdec_fb_cb_cfg_t vdec_cfg = {
                        .fb_fetch = av_render_fetch_vid_fb,
                        .fb_return = av_render_release_vid_fb,
                        .ctx = render,
                    };
                    vdec_set_fb_cb(vdec_res->vdec, &vdec_cfg);
                }
            } else {
                v_render->use_fb = false;
            }
            // 配置视频解码 FIFO 时创建异步线程，否则在 add_video_data() 中同步解码。
            if (video_need_decode_in_sync(render, video_info) == false) {
                ret = create_thread_res(&vdec_res->thread_res, "Vdec", vdec_body, render->cfg.video_raw_fifo_size,
                                        VDEC_CLOSED_BITS);
                if (ret != 0) {
                    ESP_LOGE(TAG, "Fail to create thread for VDec");
                    break;
                }
                // Clear decode in sync flag
                v_render->decode_in_sync = false;
            }
        } else {
            // Convert to frame info
            convert_to_video_frame(video_info, &v_render->video_frame_info);
        }
    } while (0);
    media_lib_mutex_unlock(render->api_lock);
    if (ret != ESP_MEDIA_ERR_OK) {
        ESP_LOGE(TAG, "Fail to add video stream %d", ret);
    }
    return ret;
}

int av_render_use_data_pool(av_render_handle_t h, av_render_pool_data_free free, void *pool)
{
    // 外部池模式避免复制大媒体包；队列消费完成后通过 free 回调归还数据。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || free == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    render->pool_free = free;
    render->pool = pool;
    return 0;
}

int av_render_add_audio_data(av_render_handle_t h, av_render_audio_data_t *audio_data)
{
    // 数据入口根据“是否 PCM、是否有解码线程”选择直接渲染、同步解码或写入解码 FIFO。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || audio_data == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = 0;
    do {
        av_render_audio_res_t *a_render = render->a_render_res;
        if (a_render == NULL) {
            ret = ESP_MEDIA_ERR_WRONG_STATE;
            break;
        }
        // PCM 不经过解码器，直接进入音频帧到达回调。
        if (a_render->audio_is_pcm) {
            av_render_audio_frame_t audio_frame = {
                .pts = audio_data->pts,
                .data = audio_data->data,
                .size = audio_data->size,
                .eos = audio_data->eos,
            };
            ret = av_render_audio_frame_reached(&audio_frame, render);
            break;
        }
        if (render->adec_res->adec == NULL) {
            ret = ESP_MEDIA_ERR_WRONG_STATE;
            break;
        }
        av_render_adec_res_t *adec = render->adec_res;
        // 有独立解码线程时只入队，尽快返回给网络接收线程。
        if (adec->thread_res.thread) {
            media_lib_mutex_unlock(render->api_lock);
            ret = put_to_adec(adec->thread_res.data_q, audio_data, adec->thread_res.use_pool);
            if (ret != 0) {
                if (render->pool_free && audio_data->data) {
                    render->pool_free(audio_data->data, render->pool);
                }
            }
            return ret;
        } else {
            ret = decode_audio(adec, audio_data);
        }
    } while (0);
    if (render->pool_free && audio_data->data) {
        render->pool_free(audio_data->data, render->pool);
    }
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

int av_render_set_audio_threshold(av_render_handle_t h, uint32_t audio_threshold)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    if (render->cfg.audio_render_fifo_size == 0) {
        ESP_LOGW(TAG, "Not support set audio threshold without render fifo");
    } else {
        uint32_t top_limit = render->cfg.audio_render_fifo_size / 2;
        if (audio_threshold > top_limit) {
            render->audio_threshold = top_limit;
        } else {
            render->audio_threshold = audio_threshold;
        }
        ESP_LOGE(TAG, "Set audio threshold %d", (int)render->audio_threshold);
    }
    media_lib_mutex_unlock(render->api_lock);
    return 0;
}

int av_render_add_video_data(av_render_handle_t h, av_render_video_data_t *video_data)
{
    // 视频入口同样在直接渲染、同步解码和异步解码三条路径间选择。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || video_data == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    av_render_video_res_t *v_render = render->v_render_res;
    int ret = 0;
    do {
        if (v_render == NULL) {
            ret = ESP_MEDIA_ERR_WRONG_STATE;
            break;
        }
        if (v_render->video_frame_info.fps == 0) {
            correct_video_fps(v_render, video_data->pts);
        }
        // 原始视频帧跳过解码器。
        if (v_render->video_is_raw) {
            av_render_video_frame_t video_frame = {
                .pts = video_data->pts,
                .data = video_data->data,
                .size = video_data->size,
                .eos = video_data->eos,
            };
            ret = av_render_video_frame_reached(&video_frame, render);
            break;
        }
        if (render->vdec_res->vdec == NULL) {
            ret = ESP_MEDIA_ERR_WRONG_STATE;
            break;
        }
        av_render_vdec_res_t *vdec = render->vdec_res;
        // 异步模式写入视频解码 FIFO，由 vdec 线程继续处理。
        if (vdec->thread_res.thread) {
            media_lib_mutex_unlock(render->api_lock);
            ret = put_to_vdec(vdec->thread_res.data_q, video_data, vdec->thread_res.use_pool);
            if (ret != 0) {
                if (render->pool_free && video_data->data) {
                    render->pool_free(video_data->data, render->pool);
                }
            }
            return ret;
        } else {
            ret = decode_video(vdec, video_data);
        }
    } while (0);
    if (render->pool_free && video_data->data) {
        render->pool_free(video_data->data, render->pool);
    }
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

bool av_render_audio_fifo_enough(av_render_handle_t h, av_render_audio_data_t *audio_data)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || audio_data == NULL) {
        return false;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    av_render_audio_res_t *a_render = render->a_render_res;
    int need_size = sizeof(av_render_audio_data_t) + audio_data->size;
    bool enough = false;
    do {
        if (a_render == NULL) {
            break;
        }
        if (render->adec_res && render->adec_res->thread_res.data_q) {
            if (render->adec_res->thread_res.use_pool) {
                need_size -= audio_data->size;
            }
            int avail = data_queue_get_available(render->adec_res->thread_res.data_q);
            enough = (need_size <= avail);
            break;
        }
        if (a_render->thread_res.data_q) {
            int avail = data_queue_get_available(a_render->thread_res.data_q);
            enough = (need_size <= avail);
            break;
        }
        // Data directly write to render
        enough = true;
    } while (0);
    media_lib_mutex_unlock(render->api_lock);
    return enough;
}

bool av_render_video_fifo_enough(av_render_handle_t h, av_render_video_data_t *video_data)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || video_data == NULL) {
        return false;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    av_render_video_res_t *v_render = render->v_render_res;
    int need_size = sizeof(av_render_video_data_t) + video_data->size;
    bool enough = false;
    do {
        if (v_render == NULL) {
            break;
        }
        if (render->vdec_res && render->vdec_res->thread_res.data_q) {
            if (render->vdec_res->thread_res.use_pool) {
                need_size -= video_data->size;
            }
            int avail = data_queue_get_available(render->vdec_res->thread_res.data_q);
            enough = (need_size <= avail);
            break;
        }
        if (v_render->thread_res.data_q) {
            int avail = data_queue_get_available(v_render->thread_res.data_q);
            enough = (need_size <= avail);
            break;
        }
        enough = true;
    } while (0);
    media_lib_mutex_unlock(render->api_lock);
    return enough;
}

int av_render_get_audio_fifo_level(av_render_handle_t h, av_render_fifo_stat_t *fifo_stat)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    memset(fifo_stat, 0, sizeof(av_render_fifo_stat_t));
    av_render_audio_res_t *a_render = render->a_render_res;
    if (a_render) {
        if (render->adec_res && render->adec_res->thread_res.data_q) {
            data_queue_query(render->adec_res->thread_res.data_q, &fifo_stat->q_num, &fifo_stat->data_size);
        }
        if (a_render->thread_res.data_q) {
            data_queue_query(a_render->thread_res.data_q, &fifo_stat->render_q_num, &fifo_stat->render_data_size);
        }
    }
    media_lib_mutex_unlock(render->api_lock);
    return 0;
}

int av_render_get_video_fifo_level(av_render_handle_t h, av_render_fifo_stat_t *fifo_stat)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || fifo_stat == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    memset(fifo_stat, 0, sizeof(av_render_fifo_stat_t));
    av_render_video_res_t *v_render = render->v_render_res;
    if (v_render) {
        if (render->vdec_res && render->vdec_res->thread_res.data_q) {
            data_queue_query(render->vdec_res->thread_res.data_q, &fifo_stat->q_num, &fifo_stat->data_size);
        } else if (v_render->thread_res.data_q) {
            data_queue_query(v_render->thread_res.data_q, &fifo_stat->q_num, &fifo_stat->data_size);
        }
    }
    media_lib_mutex_unlock(render->api_lock);
    return 0;
}

int render_pause(av_render_t *render, bool pause)
{
    // 通过控制消息同步暂停/恢复所有已创建线程；无独立线程的输出由底层 renderer 控制。
    av_render_msg_t msg = {
        .type = pause ? AV_RENDER_MSG_PAUSE : AV_RENDER_MSG_RESUME,
    };
    if (pause) {
        // pause_render_only=true 时尽量只停输出；没有渲染线程则必须连解码一起暂停。
        if (render->cfg.pause_render_only == false || (render->a_render_res && render->a_render_res->thread_res.thread == NULL)) {
            if (render->adec_res && render->adec_res->thread_res.thread) {
                send_msg_to_thread(&render->adec_res->thread_res, sizeof(av_render_audio_data_t), &msg);
            }
        }
        if (render->cfg.pause_render_only == false || (render->v_render_res && render->v_render_res->thread_res.thread == NULL)) {
            if (render->vdec_res && render->vdec_res->thread_res.thread) {
                send_msg_to_thread(&render->vdec_res->thread_res, sizeof(av_render_video_data_t), &msg);
            }
        }
        if (render->a_render_res && render->a_render_res->thread_res.thread) {
            send_msg_to_thread(&render->a_render_res->thread_res, sizeof(av_render_audio_frame_t), &msg);
        }
        if (render->v_render_res && render->v_render_res->thread_res.thread) {
            send_msg_to_thread(&render->v_render_res->thread_res, sizeof(av_render_video_frame_t), &msg);
        }
    } else {
        if (render->a_render_res && render->a_render_res->thread_res.thread) {
            send_msg_to_thread(&render->a_render_res->thread_res, sizeof(av_render_audio_frame_t), &msg);
        }
        if (render->v_render_res && render->v_render_res->thread_res.thread) {
            send_msg_to_thread(&render->v_render_res->thread_res, sizeof(av_render_video_frame_t), &msg);
        }
        if (render->adec_res && render->adec_res->thread_res.thread) {
            send_msg_to_thread(&render->adec_res->thread_res, sizeof(av_render_audio_data_t), &msg);
        }
        if (render->vdec_res && render->vdec_res->thread_res.thread) {
            send_msg_to_thread(&render->vdec_res->thread_res, sizeof(av_render_video_data_t), &msg);
        }
    }
    printf("Pause set to %d\n", pause);
    return 0;
}

static int render_flush(av_render_t *render)
{
    // flush 排空各级 FIFO 和 codec 缓存但保留流配置，完成后仍可继续送入数据。
    av_render_msg_t msg = {
        .type = AV_RENDER_MSG_FLUSH,
    };
    int wait_bits;
    // 先消费渲染队列，为解码器 flush 产生的尾帧腾出空间。
    if (render->adec_res && render->adec_res->thread_res.thread) {
        render->adec_res->thread_res.flushing = true;
        if (render->a_render_res && render->a_render_res->thread_res.data_q) {
            render->a_render_res->thread_res.flushing = true;
            render_consume_all(&render->a_render_res->thread_res);
        }
        send_msg_to_thread(&render->adec_res->thread_res, sizeof(av_render_audio_data_t), &msg);
        wait_bits = render->adec_res->thread_res.wait_bits << FLUSH_SHIFT_BITS;
        _WAIT_BITS(render->event_group, wait_bits);
        printf("Wait for %x finished\n", wait_bits);
    }
    if (render->vdec_res && render->vdec_res->thread_res.thread) {
        render->vdec_res->thread_res.flushing = true;
        if (render->v_render_res && render->v_render_res->thread_res.data_q) {
            render->v_render_res->thread_res.flushing = true;
            render_consume_all(&render->v_render_res->thread_res);
        }
        send_msg_to_thread(&render->vdec_res->thread_res, sizeof(av_render_video_data_t), &msg);
        wait_bits = render->vdec_res->thread_res.wait_bits << FLUSH_SHIFT_BITS;
        _WAIT_BITS(render->event_group, wait_bits);
        printf("Wait for %x finished\n", wait_bits);
    }
    wait_bits = 0;
    if (render->a_render_res && render->a_render_res->thread_res.thread) {
        render->a_render_res->thread_res.flushing = true;
        send_msg_to_thread(&render->a_render_res->thread_res, sizeof(av_render_audio_frame_t), &msg);
        wait_bits = render->a_render_res->thread_res.wait_bits << FLUSH_SHIFT_BITS;
    }
    if (render->v_render_res && render->v_render_res->thread_res.thread) {
        render->v_render_res->thread_res.flushing = true;
        send_msg_to_thread(&render->v_render_res->thread_res, sizeof(av_render_video_frame_t), &msg);
        wait_bits = render->v_render_res->thread_res.wait_bits << FLUSH_SHIFT_BITS;
    }
    _WAIT_BITS(render->event_group, wait_bits);
    // flush 后音视频都重新从首帧建立同步基准。
    if (render->v_render_res) {
        render->v_render_res->video_rendered = false;
        render->v_render_res->sent_frame_num = 0;
    }
    if (render->a_render_res) {
        render->a_render_res->audio_rendered = false;
    }
    return 0;
}

int av_render_set_speed(av_render_handle_t h, float speed)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return -1;
    }
    if (render->cfg.audio_render == NULL) {
        return 0;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = audio_render_set_speed(render->cfg.audio_render, speed);
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

int av_render_pause(av_render_handle_t h, bool pause)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = render_pause(render, pause);
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

int av_render_flush(av_render_handle_t h)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    int ret = render_flush(render);
    media_lib_mutex_unlock(render->api_lock);
    return ret;
}

int av_render_set_video_start_pts(av_render_handle_t h, uint32_t video_pts)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return -1;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    if (render && render->v_render_res) {
        render->v_render_res->video_start_pts = video_pts;
    }
    media_lib_mutex_unlock(render->api_lock);
    return 0;
}

static int av_render_get_audio_pts(av_render_t *render, uint32_t *out_pts)
{
    // 从最后提交 PTS 中扣除硬件缓存延迟，近似得到用户当前真正听到的位置。
    av_render_audio_res_t *a_render = render->a_render_res;
    if (a_render && a_render->audio_packet_reached) {
        uint32_t audio_pts = a_render->audio_send_pts;
        uint32_t latency = 0;
        int ret = audio_render_get_latency(render->cfg.audio_render, &latency);
        if (ret == 0) {
            if (audio_pts >= latency) {
                audio_pts -= latency;
            }
        }
        *out_pts = audio_pts;
        return 0;
    }
    return -1;
}

static int av_render_get_video_pts(av_render_t *render, uint32_t *out_pts)
{
    // 从已提交 PTS 扣除 renderer 延迟和待显示队列时长，估算当前实际画面位置。
    av_render_video_res_t *v_render = render->v_render_res;

    if (v_render && v_render->video_packet_reached) {
        uint32_t video_pts = v_render->video_send_pts;
        uint32_t latency = 0;
        int ret = video_render_get_latency(render->cfg.video_render, &latency);
        if (ret == 0) {
            video_pts -= latency;
        }
        if (v_render->thread_res.data_q) {
            int q_num = 0, q_size = 0;
            data_queue_query(v_render->thread_res.data_q, &q_num, &q_size);
            if (q_num) {
                int fps = v_render->video_frame_info.fps;
                if (fps == 0) {
                    fps = 20;
                }
                int q_pts = q_num * 1000 / fps;
                video_pts -= q_pts;
            }
        }
        *out_pts = video_pts;
        return 0;
    }
    return -1;
}

int av_render_query(av_render_handle_t h)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    if (render->adec_res) {
        data_queue_t *q = render->adec_res->thread_res.data_q;
        int q_num = 0, q_size = 0;
        if (q) {
            data_queue_query(q, &q_num, &q_size);
        }
        ESP_LOGI(TAG, "Audio decoder fifo size %d items %d err:%d", q_size, q_num, render->adec_res->audio_err_cnt);
        ESP_LOGI(TAG, "Audio decoder status flashing: %d paused: %d",
                 render->adec_res->thread_res.flushing, render->adec_res->thread_res.paused);
    }
    if (render->a_render_res) {
        data_queue_t *q = render->a_render_res->thread_res.data_q;
        int q_num = 0, q_size = 0;
        if (q) {
            data_queue_query(q, &q_num, &q_size);
        }
        ESP_LOGI(TAG, "Audio render fifo size %d items %d", q_size, q_num);
        ESP_LOGI(TAG, "Audio render status flashing: %d paused: %d",
                 render->a_render_res->thread_res.flushing, render->a_render_res->thread_res.paused);
        ESP_LOGI(TAG, "Audio render pts %" PRIu32 " use resample %d",
                 render->a_render_res->audio_send_pts, render->a_render_res->need_resample);
    }
    if (render->vdec_res) {
        data_queue_t *q = render->vdec_res->thread_res.data_q;
        int q_num = 0, q_size = 0;
        if (q) {
            data_queue_query(q, &q_num, &q_size);
        }
        ESP_LOGI(TAG, "Video decoder fifo size %d items %d err:%d", q_size, q_num, render->vdec_res->video_err_cnt);
        ESP_LOGI(TAG, "Video decoder status flashing: %d paused: %d",
                 render->vdec_res->thread_res.flushing, render->vdec_res->thread_res.paused);
    }
    if (render->v_render_res) {
        data_queue_t *q = render->v_render_res->thread_res.data_q;
        int q_num = 0, q_size = 0;
        if (q) {
            data_queue_query(q, &q_num, &q_size);
        }
        ESP_LOGI(TAG, "Video render fifo size %d items %d", q_size, q_num);
        ESP_LOGI(TAG, "Video render status flashing: %d paused: %d",
                 render->v_render_res->thread_res.flushing, render->v_render_res->thread_res.paused);
        ESP_LOGI(TAG, "Video render pts %" PRIu32, render->v_render_res->video_send_pts);
    }
    media_lib_mutex_unlock(render->api_lock);
    return 0;
}

void av_render_dump(av_render_handle_t h, uint8_t mask)
{
    render_dump_mask = mask;
    ESP_LOGI(TAG, "Dump mask set to %x", mask);
    if (mask == 0) {
        dump_data(AV_RENDER_DUMP_STOP_INDEX, NULL, 0);
    }
}

int av_render_get_render_pts(av_render_handle_t h, uint32_t *out_pts)
{
    av_render_t *render = (av_render_t *)h;
    if (render == NULL || out_pts == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    uint32_t pts = 0xFFFFFFFF;
    int ret = 0;
    av_render_audio_res_t *a_render = render->a_render_res;
    av_render_video_res_t *v_render = render->v_render_res;

    if (a_render && a_render->audio_packet_reached) {
        uint32_t audio_pts = 0;
        ret = av_render_get_audio_pts(render, &audio_pts);
        if (ret == 0) {
            pts = audio_pts;
        }
    }
    if (v_render && v_render->video_packet_reached) {
        uint32_t video_pts;
        ret = av_render_get_video_pts(render, &video_pts);
        if (ret == 0 && video_pts < pts) {
            pts = video_pts;
        }
    }
    media_lib_mutex_unlock(render->api_lock);
    *out_pts = pts;
    return 0;
}

int av_render_reset(av_render_handle_t h)
{
    // reset 停止当前流但保留 av_render 对象：先关线程，再释放 codec 和渲染阶段资源。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    media_lib_mutex_lock(render->api_lock, MEDIA_LIB_MAX_LOCK_TIME);
    // 向所有存在的线程发送 CLOSE，并等待事件位确认退出。
    av_render_msg_t msg = {
        .type = AV_RENDER_MSG_CLOSE,
    };
    int recv_bits = 0;
    // 等待两轮，覆盖关闭过程中解码回调刚好创建渲染线程的竞态窗口。
    for (int i = 0; i < 2; i++) {
        int wait_bits = 0;
        if (render->adec_res && render->adec_res->thread_res.thread) {
            if ((recv_bits & render->adec_res->thread_res.wait_bits) == 0) {
                wait_bits |= render->adec_res->thread_res.wait_bits;
                send_msg_to_thread(&render->adec_res->thread_res, sizeof(av_render_audio_data_t), &msg);
            }
        }
        if (render->vdec_res && render->vdec_res->thread_res.thread) {
            if ((recv_bits & render->vdec_res->thread_res.wait_bits) == 0) {
                wait_bits |= render->vdec_res->thread_res.wait_bits;
                send_msg_to_thread(&render->vdec_res->thread_res, sizeof(av_render_video_data_t), &msg);
            }
        }
        if (render->a_render_res && render->a_render_res->thread_res.thread) {
            if ((recv_bits & render->a_render_res->thread_res.wait_bits) == 0) {
                wait_bits |= render->a_render_res->thread_res.wait_bits;
                send_msg_to_thread(&render->a_render_res->thread_res, sizeof(av_render_audio_frame_t), &msg);
            }
        }
        if (render->v_render_res && render->v_render_res->thread_res.thread) {
            if ((recv_bits & render->v_render_res->thread_res.wait_bits) == 0) {
                wait_bits |= render->v_render_res->thread_res.wait_bits;
                send_msg_to_thread(&render->v_render_res->thread_res, sizeof(av_render_video_frame_t), &msg);
            }
        }
        if (wait_bits == 0) {
            break;
        }
        media_lib_event_group_wait_bits(render->event_group, wait_bits, MEDIA_LIB_MAX_LOCK_TIME);
        media_lib_event_group_clr_bits(render->event_group, wait_bits);
        recv_bits |= wait_bits;
    }
    ESP_LOGI(TAG, "Close done");
    dump_data(AV_RENDER_DUMP_STOP_INDEX, NULL, 0);
    // 线程全部退出后再关闭解码器，避免工作线程继续调用已释放的 codec。
    if (render->adec_res) {
        if (render->adec_res->adec) {
            adec_close(render->adec_res->adec);
            render->adec_res->adec = NULL;
        }
        destroy_thread_res(&render->adec_res->thread_res);
        media_lib_free(render->adec_res);
        render->adec_res = NULL;
    }
    if (render->vdec_res) {
        av_render_vdec_res_t *vdec_res = render->vdec_res;
        if (vdec_res->vdec) {
            vdec_close(vdec_res->vdec);
            vdec_res->vdec = NULL;
        }
        if (vdec_res->vid_convert) {
            deinit_convert_table(vdec_res->vid_convert);
            vdec_res->vid_convert = NULL;
        }
        if (vdec_res->vid_convert_out) {
            media_lib_free(vdec_res->vid_convert_out);
            vdec_res->vid_convert_out = NULL;
        }
        destroy_thread_res(&render->vdec_res->thread_res);
        media_lib_free(render->vdec_res);
        render->vdec_res = NULL;
    }
    // 释放渲染阶段容器以及重采样、颜色转换等附属资源。
    if (render->a_render_res) {
        destroy_thread_res(&render->a_render_res->thread_res);
        if (render->a_render_res->resample_handle) {
            audio_resample_close(render->a_render_res->resample_handle);
            render->a_render_res->resample_handle = NULL;
        }
        media_lib_free(render->a_render_res);
        render->a_render_res = NULL;
    }
    if (render->v_render_res) {
        destroy_thread_res(&render->v_render_res->thread_res);
        media_lib_free(render->v_render_res);
        render->v_render_res = NULL;
    }
    // 最后关闭用户提供的底层 audio/video renderer。
    if (render->cfg.audio_render) {
        audio_render_close(render->cfg.audio_render);
    }
    if (render->cfg.video_render) {
        video_render_close(render->cfg.video_render);
    }
    media_lib_mutex_unlock(render->api_lock);
    return 0;
}

int av_render_close(av_render_handle_t h)
{
    // close 复用 reset 的完整停流流程，再销毁总事件组、API 锁和上下文本身。
    av_render_t *render = (av_render_t *)h;
    if (render == NULL) {
        return ESP_MEDIA_ERR_INVALID_ARG;
    }
    av_render_reset(h);
    if (render->event_group) {
        media_lib_event_group_destroy(render->event_group);
    }
    if (render->api_lock) {
        media_lib_mutex_destroy(render->api_lock);
    }
    media_lib_free(render);
    return ESP_MEDIA_ERR_OK;
}
