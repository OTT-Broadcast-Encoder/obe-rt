#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "common/common.h"
#include "common/bitstream.h"
#include "ltn_ws.h"

struct filter_compress_ctx
{
	time_t lastFrameTime;
};

void filter_compress_free(struct filter_compress_ctx *ctx)
{
	free(ctx);
}

int filter_compress_alloc(struct filter_compress_ctx **p)
{
	struct filter_compress_ctx *ctx = calloc(1, sizeof(*ctx));

	//av_register_all();

	*p = ctx;
	return 0;
}

int filter_compress_jpg(struct filter_compress_ctx *ctx, obe_raw_frame_t *rf)
{
	time_t now = time(0);
	if (ctx->lastFrameTime == now) {
		return -1;
	}
	printf("%s()\n", __func__);

	ctx->lastFrameTime = now;

	AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (!codec) {
		printf("Codec not found\n");
		exit(1);
	}

	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c) {
		printf("Could not allocate video codec context\n");
		exit(1);
	}

	c->bit_rate = 400000;
	c->width = rf->img.width;
	c->height = rf->img.height;
	c->time_base = (AVRational) { 1, 25 };
	c->pix_fmt = AV_PIX_FMT_YUVJ420P;

	if (avcodec_open2(c, codec, NULL) < 0) {
		printf("Could not open codec\n");
		exit(1);
	}

	AVFrame *frame = avcodec_alloc_frame();
	if (!frame) {
		printf("Could not allocate video frame\n");
		exit(1);
	}
	frame->format = c->pix_fmt;
	frame->width = c->width;
	frame->height = c->height;

	int ret = av_image_alloc(frame->data, frame->linesize, c->width,
		c->height, c->pix_fmt, 32);
	if (ret < 0) {
		printf("Could not allocate raw picture buffer\n");
		exit(1);
	}

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	memcpy(frame->data[0], rf->img.plane[0], c->height * c->width);
	memcpy(frame->data[1], rf->img.plane[1], (c->height * c->width) / 4);
	memcpy(frame->data[2], rf->img.plane[2], (c->height * c->width) / 4);

	frame->pts = 1;

	int got_output = 0;
	ret = avcodec_encode_video2(c, &pkt, frame, &got_output);
	if (ret < 0) {
		printf("Error encoding frame\n");
		exit(1);
	}

	if (got_output) {
#if LTN_WS_ENABLE
		ltn_ws_set_thumbnail_jpg(g_ltn_ws_handle, pkt.data, pkt.size);

#endif
#if 0
		printf("%s() frame output\n", __func__);
		FILE *f = fopen("/tmp/image.jpg", "wb");
		fwrite(pkt.data, 1, pkt.size, f);
		fclose(f);
#endif
		av_free_packet(&pkt);
	}

	avcodec_close(c);
	av_free(c);
	av_freep(&frame->data[0]);
	avcodec_free_frame(&frame);

	return 0;
}
