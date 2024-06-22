#define __STDC_CONSTANT_MACROS
extern "C"
{
#include "common/common.h"
}
#include "vapoursynth_vf.h"
#include <mutex>
#include <string>
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <sstream>
#include <mutex>
#include <chrono>
#include <thread>
#include <queue>
#include <vector>
#include <condition_variable>
#include <unordered_map>
#include <atomic>

#define MODULE_PREFIX "[filter_vapoursynth]: "

#define PRINT(text) fprintf(stderr, (MODULE_PREFIX + std::string(text) + "\n").c_str())
#define PRINTF(text, ...) fprintf(stderr, (MODULE_PREFIX + std::string(text) + "\n").c_str(), __VA_ARGS__)

#define MAX_SKIP_MS 100

bool g_vapoursynth_monitor_fps = 0;

struct monitor_fps {
    int last_enc_frame_num;
    std::chrono::milliseconds last_time;
};

struct filter_vapoursynth_ctx {   
    obe_t *h;

    bool vs_loaded = false;

    const VSAPI *vsapi = nullptr;
    const VSSCRIPTAPI *vssapi = nullptr;
    VSScript *vsscript;

    VSVideoInfo vi;

    VSNode *source_node;
    VSNode *out_node;

    int start_frame_num;
    int frame_num;
    int out_frame_num;
    std::atomic<int> enc_frame_num;
    int last_fetch_frame_num;

    char error_msg[1024];

    std::string script_path;
    std::string py_script;

    std::unordered_map<int, obe_raw_frame_t *> in_images;
    std::unordered_map<int, obe_raw_frame_t *> out_images;

    std::mutex in_img_mtx;
    std::mutex out_img_mtx;
    std::mutex vsscript_mtx;

    std::deque<VSScript *> old_envs;

    monitor_fps monitor;
};

std::chrono::milliseconds get_chrono_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    );
}

void set_monitor_fps(filter_vapoursynth_ctx *ctx) {
    ctx->monitor.last_time = get_chrono_now();
    ctx->monitor.last_enc_frame_num = ctx->enc_frame_num;
}

void filter_vapoursynth_free(filter_vapoursynth_ctx *ctx) {
    if (!ctx)
    	return;

    delete ctx;
}

int filter_vapoursynth_alloc(filter_vapoursynth_ctx **p, obe_t *h) {
    filter_vapoursynth_ctx *ctx = new filter_vapoursynth_ctx();

    if (!ctx)
        return -1;

    *p = ctx;

    if (!ctx->vs_loaded) {
        std::string old_locale(setlocale(LC_ALL, NULL));

        ctx->vssapi = getVSScriptAPI(VSSCRIPT_API_VERSION);

        std::string new_locale(setlocale(LC_ALL, NULL));

        if (old_locale != new_locale)
            setlocale(LC_ALL, old_locale.c_str());

        if (!ctx->vssapi) {
            PRINT("Failed to get VapourSynth ScriptAPI. Make sure VapourSynth is installed correctly.");
            return -1;
        }

        ctx->vsapi = ctx->vssapi->getVSAPI(VAPOURSYNTH_API_VERSION);

        if (!ctx->vsapi)  {
            PRINT("Failed to get VapourSynth API.");
            return -1;
        }

        ctx->vs_loaded = true;
    }

    ctx->h = h;
    ctx->frame_num = ctx->last_fetch_frame_num = -1;
    ctx->script_path = std::string{h->vapoursynth_script_path};

    ctx->in_images.reserve(100);
    ctx->out_images.reserve(100);

    return 0;
}

std::string get_script_content(filter_vapoursynth_ctx *ctx) {
    std::ifstream file(ctx->script_path);

    if (!file.is_open()) {
        PRINTF("Specified file path \"%s\" could not be opened!", ctx->script_path.c_str());

        return "";
    }

    std::ostringstream py_script;

    std::string line;
    while (std::getline(file, line))
        py_script << line;

    if (ctx->py_script == py_script.str())
        return "";

    return py_script.str();
}

static const char *vs_msg_type_to_str(int msgType) {
    switch (msgType) {
        case mtDebug: return "Debug";
        case mtInformation: return "Information";
        case mtWarning: return "Warning";
        case mtCritical: return "Critical";
        case mtFatal: return "Fatal";
        default: return "";
    }
}


static void VS_CC vs_log_message_handler(int msgType, const char *msg, void *userData) {
    if (msgType >= mtInformation)
        PRINTF("%s: %s", vs_msg_type_to_str(msgType), msg);
}

template <bool to_vs, typename T, typename S>
void copy_frame(filter_vapoursynth_ctx *ctx, const T *src, S *dst) {
    ptrdiff_t src_stride, dst_stride;
    const uint8_t *src_ptr;
    uint8_t *dst_ptr;

    for(int i = 0; i < ctx->vi.format.numPlanes; i++ ) {
        if constexpr (to_vs) {
            src_stride = src->stride[i];
            dst_stride = ctx->vsapi->getStride(dst, i);

            src_ptr = src->plane[i];
            dst_ptr = ctx->vsapi->getWritePtr(dst, i);
        } else {
            src_stride = ctx->vsapi->getStride(src, i);
            dst_stride = dst->stride[i];

            src_ptr = ctx->vsapi->getReadPtr(src, i);
            dst_ptr = dst->plane[i];
        }

        int width = ctx->vi.width / (1 << (i ? ctx->vi.format.subSamplingW : 0));
        int height = ctx->vi.height / (1 << (i ? ctx->vi.format.subSamplingH : 0));

        vsh::bitblt(dst_ptr, dst_stride, src_ptr, src_stride, width, height);
    }
}

static const VSFrame *VS_CC vs_filter_get_frame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {  
    filter_vapoursynth_ctx *ctx = (filter_vapoursynth_ctx *)instanceData;

    if (activationReason != VSActivationReason::arError) {
        VSFrame *curr_frame = vsapi->newVideoFrame(&ctx->vi.format, ctx->vi.width, ctx->vi.height, nullptr, core);

        if (ctx->last_fetch_frame_num < 0)
            return curr_frame;

        std::lock_guard lock(ctx->in_img_mtx);

        auto in_img_it = ctx->in_images.find(n);
        bool in_img_found = in_img_it != ctx->in_images.end();

        if (!in_img_found) {
            // first load of script may fetch frame before we input from obe
            if (ctx->last_fetch_frame_num < 0)
                return curr_frame;

            // if a temporal filter is trying to access previous frames we already
            // disposed of after hot reload of script
            if (n < ctx->last_fetch_frame_num)
                return curr_frame;

            vsapi->freeFrame(curr_frame);

            if (n > ctx->frame_num)
                PRINTF(
                    "Script filterchain requested a frame (%d) that is over the buffer (%d frames) of current frame (%d)!",
                    n, ctx->frame_num - ctx->out_frame_num, ctx->out_frame_num
                );
            else
                PRINTF("Error in input OBE frame fetch! (%d)", n);

            return nullptr;
        }

        copy_frame<true>(ctx, &in_img_it->second->img, curr_frame);

        return curr_frame;
    }

    return nullptr;
}

static void VS_CC vs_filter_free(void *instanceData, VSCore *core, const VSAPI *vsapi) {}

void VS_CC vs_frame_done_callback(void *userData, const VSFrame *f, int n, VSNode *node, const char *errorMsg) {
    filter_vapoursynth_ctx *ctx = (filter_vapoursynth_ctx *)userData;

    std::lock_guard vsslock(ctx->vsscript_mtx);

    if (!f) {
        PRINTF("There was an error fetching the frame:\n\t%s", errorMsg);

        return;
    }

    if (!ctx->in_images.contains(n)) {
        PRINTF("There was an error getting the OBE image for frame (%d)!", n);

        ctx->vsapi->freeFrame(f);

        return;
    }

    ctx->in_img_mtx.lock();
    obe_raw_frame_t *raw_frame = ctx->in_images.extract(n).mapped();
    ctx->in_img_mtx.unlock();

    copy_frame<false>(ctx, f, &raw_frame->img);

    ctx->vsapi->freeFrame(f);

    // fastpath without reorder
    if (n == ctx->enc_frame_num) {
        add_to_encode_queue( ctx->h, raw_frame, 0 );
        ctx->enc_frame_num++;
        return;
    }

    std::lock_guard lock(ctx->out_img_mtx);

    ctx->out_images.insert_or_assign(n, raw_frame);

    for (int enc_n = ctx->enc_frame_num; enc_n <= ctx->out_frame_num; enc_n++) {
        if (!ctx->out_images.contains(enc_n))
            break;

        auto out_img_it = ctx->out_images.extract(enc_n);

        add_to_encode_queue( ctx->h, out_img_it.mapped(), 0 );

        ctx->enc_frame_num++;
    }
}

const char *create_filterchain(filter_vapoursynth_ctx *ctx, obe_raw_frame_t *raw_frame, std::string py_script) {
    std::lock_guard vsslock(ctx->vsscript_mtx);

    int last_fetch_frame_num = ctx->last_fetch_frame_num;

    VSCore *vscore = ctx->vsapi->createCore(0);
    
    ctx->vsapi->addLogHandler(vs_log_message_handler, nullptr, nullptr, vscore);

    ctx->vsapi->setCoreNodeTiming(vscore, true);

    VSScript *vsscript = ctx->vssapi->createScript(vscore);

    if (!vsscript || !vscore) {
        ctx->vssapi->freeScript(vsscript);

        return "Could not create core or vsscript!";
    }

    uint32_t vs_fmt_id;
    if (raw_frame->img.csp == AV_PIX_FMT_YUV420P)
        vs_fmt_id = VSPresetVideoFormat::pfYUV420P8;
    else if (raw_frame->img.csp == AV_PIX_FMT_YUV420P10)
        vs_fmt_id = VSPresetVideoFormat::pfYUV420P10;
    else if (raw_frame->img.csp == AV_PIX_FMT_YUV422P10)
        vs_fmt_id = VSPresetVideoFormat::pfYUV422P10;
    else
        return "Invalid image format!";

    ctx->vsapi->getVideoFormatByID(&ctx->vi.format, vs_fmt_id, vscore);
    ctx->vi.fpsNum = raw_frame->timebase_den;
    ctx->vi.fpsDen = raw_frame->timebase_num;
    ctx->vi.width = raw_frame->img.width;
    ctx->vi.height = raw_frame->img.height;
    ctx->vi.numFrames = INT_MAX;

    VSNode *source_node = ctx->vsapi->createVideoFilter2(
        "OBEInternalStream", &ctx->vi, vs_filter_get_frame, vs_filter_free, VSFilterMode::fmParallel, nullptr, 0, ctx, vscore
    );

#define SCRIPT_LOAD_ERROR(err) do { \
    ctx->vsapi->freeNode(source_node); \
    ctx->vssapi->freeScript(vsscript); \
    return err; \ 
} while (0);

    if (!source_node)
        SCRIPT_LOAD_ERROR("Could not create internal stream for VapourSynth!")

    ctx->vsapi->setCacheMode(source_node, VSCacheMode::cmForceEnable);
    ctx->vsapi->setCacheOptions(source_node, true, ctx->vi.fpsNum * 3, ctx->vi.fpsNum * 3);

    VSMap *vars_map = ctx->vsapi->createMap();

#define SCRIPT_LOAD_PMAP_ERROR(err) do { \
    ctx->last_fetch_frame_num = last_fetch_frame_num; \
    ctx->vsapi->freeMap(vars_map); \
    SCRIPT_LOAD_ERROR(err) \ 
} while (0);

    if (!vars_map)
        SCRIPT_LOAD_PMAP_ERROR("Couldn't create VSMap! Out of memory?")

    if(ctx->vsapi->mapSetNode(vars_map, "clip", source_node, VSMapAppendMode::maReplace))
        SCRIPT_LOAD_PMAP_ERROR("Couldn't set input node in the variables for the script!")

    if(ctx->vssapi->setVariables(vsscript, vars_map)){
        SCRIPT_LOAD_PMAP_ERROR("Couldn't set variables in the script!")}

    ctx->vsapi->freeMap(vars_map);

    ctx->last_fetch_frame_num = -1;

    if (ctx->vssapi->evaluateBuffer(vsscript, py_script.c_str(), "obe_rt")){
        const char *error_msg = ctx->vssapi->getError(vsscript);

        if (!error_msg)
            SCRIPT_LOAD_ERROR("No error specified.")

        char *out_msg = (char *) malloc(strlen(error_msg) + 1); 
        strcpy(out_msg, error_msg);

        SCRIPT_LOAD_ERROR(out_msg)
    }

    VSNode *out_node = ctx->vssapi->getOutputNode(vsscript, 0);

    const VSVideoInfo outVi = *ctx->vsapi->getVideoInfo(out_node);

#define SCRIPT_ERROR(err) do { \
    ctx->last_fetch_frame_num = last_fetch_frame_num; \
    ctx->vsapi->freeNode(source_node); \
    ctx->vsapi->freeNode(out_node); \
    ctx->vssapi->freeScript(vsscript); \
    return err; \ 
} while (0);

    if (!vsh::isSameVideoFormat(&ctx->vi.format, &outVi.format))
        SCRIPT_ERROR("The input and output video formats don't match!")

    if (ctx->vi.width != outVi.width || ctx->vi.height != outVi.height)
        SCRIPT_ERROR("The input and output video resolutions don't match!")

    if (ctx->vi.fpsNum != outVi.fpsNum || ctx->vi.fpsDen != outVi.fpsDen)
        SCRIPT_ERROR("The input and output video framerates don't match!")

    if (ctx->vi.numFrames != outVi.numFrames)
        SCRIPT_ERROR("The input and output video lengths don't match!")

#undef SCRIPT_ERROR

    if (!ctx->source_node) {
        ctx->start_frame_num = ctx->enc_frame_num = ctx->frame_num;
        ctx->out_frame_num = ctx->frame_num;
        set_monitor_fps(ctx);
    }

    ctx->vsapi->freeNode(ctx->source_node);    
    ctx->vsapi->freeNode(ctx->out_node);    
    ctx->old_envs.push_back(ctx->vsscript);

    ctx->source_node = source_node;
    ctx->out_node = out_node;
    ctx->py_script = py_script;
    ctx->vsscript = vsscript;
    return nullptr;
}

int filter_vapoursynth_process(filter_vapoursynth_ctx *ctx, obe_raw_frame_t *raw_frame) {
    if (!ctx || !raw_frame || !ctx->vs_loaded || !(
        raw_frame->img.csp == AV_PIX_FMT_YUV420P ||
        raw_frame->img.csp == AV_PIX_FMT_YUV420P10 ||
        raw_frame->img.csp == AV_PIX_FMT_YUV422P10
    ))
        return 1;

    if (!raw_frame->img.width)
        return 1;

    ctx->frame_num++;

    if (g_vapoursynth_monitor_fps && ctx->out_node) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());

        if ((now - ctx->monitor.last_time).count() >= 1000) {
            PRINTF("Current FPS: %dfps", ctx->enc_frame_num - ctx->monitor.last_enc_frame_num);
            set_monitor_fps(ctx);
        }
    }

    if (ctx->script_path.length() && ctx->frame_num % raw_frame->timebase_den == 0) {
        std::string py_script = get_script_content(ctx);

        if (ctx->old_envs.size()) {
            for (auto env : ctx->old_envs)
                ctx->vssapi->freeScript(env);
            ctx->old_envs.clear();
        }

        if (py_script.length()) {
            const char *error_msg = create_filterchain(ctx, raw_frame, py_script);

            if (error_msg)
                PRINTF("There was an error running the VapourSynth script!\n%s", error_msg);
        }
    }

    if (!ctx->source_node || !ctx->out_node)
        return 1;

    {
        std::lock_guard lock(ctx->in_img_mtx);
        ctx->in_images.insert_or_assign(ctx->frame_num, raw_frame);
    }

    // give 1s buffer
    if (ctx->frame_num - ctx->start_frame_num <= ctx->vi.fpsNum)
        return 0;

    if (ctx->last_fetch_frame_num != ctx->out_frame_num) {
        ctx->vsapi->getFrameAsync(ctx->out_frame_num, ctx->out_node, vs_frame_done_callback, ctx);
        ctx->last_fetch_frame_num = ctx->out_frame_num;
    }

    ctx->out_frame_num++;

    return 0;
}

#undef PRINTF
#undef PRINT