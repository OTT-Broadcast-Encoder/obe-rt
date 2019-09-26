#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <math.h>
#include <assert.h>

#include <signal.h>
#define _GNU_SOURCE

#include "ltn_ws.h"

#if LTN_WS_ENABLE

#include <libwebsockets.h>
#include <json-c/json.h>

#define LOCAL_DEBUG 1

/* References:
 *   https://libwebsockets.org/
 *   https://javascript.info/websocket
 *   https://medium.com/@martin.sikora/libwebsockets-simple-http-server-2b34c13ab540
 *   https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_client_applications
 */
struct websockets_ctx
{
	void *userContext;
	int portNr;

	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;

	/* websockets */
	struct lws_context_creation_info info;
	struct lws_context *context;

	/* Stats */
	int width, height, progressive;
	int framerateX100;
	const char *software_version;
	const char *hardware_version;
	const char *fingerprint;
};

/*
 * Unlike ws, http is a stateless protocol.  This pss only exists for the
 * duration of a single http transaction.  With http/1.1 keep-alive and http/2,
 * that is unrelated to (shorter than) the lifetime of the network connection.
 */
struct pss {
	time_t established;
	char url[256];
#define URL_TARGET_UNDEFINED 0
#define URL_TARGET_ENCODERSTATUS 1
#define URL_TARGET_FINGERPRINT 2
	int urltype;
};

#define SECS_REPORT 5

struct lws *g_fp_wsi = NULL;

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len )
{
//	struct websockets_ctx *ctx = lws_context_user(lws_get_context(wsi));

//	printf("%s(reason 0x%x) ctx = %p\n", __func__, reason, ctx);

	char *requested_uri = (char *)in;
	switch(reason) {
		case LWS_CALLBACK_CLIENT_WRITEABLE:
			printf("connection established\n");
		case LWS_CALLBACK_HTTP:
			printf("requested URI: %s\n", requested_uri);
#if 0
			if (strcmp(requested_uri, "/") == 0) {
				printf("requested URI: %s (processing)\n", requested_uri);
				void *universal_response = "<html><body>Hello, World!</body></html>\x0d\x0a\x0d\x0a";
				lws_write(wsi, universal_response, strlen(universal_response), LWS_WRITE_HTTP);
				lws_callback_on_writable(wsi);
				return -1;
				//return -1;
			}
#endif

			return 0;

			break;
		default:
			break;
	}

	return 0;
	//return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static int callback_sse(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	struct websockets_ctx *ctx = lws_context_user(lws_get_context(wsi));

	struct pss *pss = (struct pss *)user;
	uint8_t buf[LWS_PRE + LWS_RECOMMENDED_MIN_HEADER_SPACE], *start = &buf[LWS_PRE],
		*p = start, *end = &buf[sizeof(buf) - 1];

	switch (reason) {
	case LWS_CALLBACK_HTTP:
		/*
		 * `in` contains the url part after our mountpoint /sse, if any
		 * you can use this to determine what data to return and store
		 * that in the pss
		 */
		lwsl_notice("%s: LWS_CALLBACK_HTTP: '%s'\n", __func__,
			    (const char *)in);

		pss->established = time(NULL);
		if (strncasecmp(in, "/encoderstatus", len) == 0) {
			pss->urltype = URL_TARGET_ENCODERSTATUS;
		} else
		if (strncasecmp(in, "/fp", len) == 0) {
			pss->urltype = URL_TARGET_FINGERPRINT;
			g_fp_wsi = wsi;
		} else {
			pss->urltype = URL_TARGET_UNDEFINED;
		}
		strncpy(pss->url, in, sizeof(pss->url));

		/* SSE requires a response with this content-type */

		if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
						"text/event-stream",
						LWS_ILLEGAL_HTTP_CONTENT_LEN,
						&p, end))
			return 1;

		if (lws_finalize_write_http_header(wsi, start, &p, end))
			return 1;

		/*
		 * This tells lws we are no longer a normal http stream,
		 * but are an "immortal" (plus or minus whatever timeout you
		 * set on it afterwards) SSE stream.  In http/2 case that also
		 * stops idle timeouts being applied to the network connection
		 * while this wsi is still open.
		 */
		lws_http_mark_sse(wsi);

		/* write the body separately */

		lws_callback_on_writable(wsi);

		return 0;

	case LWS_CALLBACK_HTTP_WRITEABLE:

		lwsl_notice("%s: LWS_CALLBACK_HTTP_WRITEABLE %s\n", __func__, pss->url);

		if (!pss)
			break;

		json_object *jresp = json_object_new_object();
		const char *resp_str = "";
		if (pss->urltype == URL_TARGET_ENCODERSTATUS) {
			{
				json_object *jrespint = json_object_new_string(ctx->software_version);
				json_object_object_add(jresp, "software_version", jrespint);
			}
			{
				json_object *jrespint = json_object_new_string(ctx->hardware_version);
				json_object_object_add(jresp, "hardware_version", jrespint);
			}
			{
				json_object *jrespint = json_object_new_string("SDI#A");
				json_object_object_add(jresp, "input", jrespint);
			}
			{
				json_object *jrespint = json_object_new_int(1);
				json_object_object_add(jresp, "locked", jrespint);
			}
			{
				json_object *jrespint = json_object_new_int(ctx->width);
				json_object_object_add(jresp, "width", jrespint);
			}
			{
				json_object *jrespint = json_object_new_int(ctx->height);
				json_object_object_add(jresp, "height", jrespint);
			}
			{
				json_object *jrespint = json_object_new_int(ctx->progressive);
				json_object_object_add(jresp, "progressive", jrespint);
			}
			{
				json_object *jrespint = json_object_new_int(ctx->framerateX100);
				json_object_object_add(jresp, "framerate", jrespint);
			}
			resp_str = strdup(json_object_to_json_string(jresp));
		} else
		if (pss->urltype == URL_TARGET_FINGERPRINT) {
			{
				json_object *jrespint = json_object_new_string(ctx->fingerprint);
				json_object_object_add(jresp, "fingerprint", jrespint);
			}
			resp_str = strdup(json_object_to_json_string(jresp));
		}

		p += lws_snprintf((char *)p, end - p, "data: %s\x0d\x0a\x0d\x0a", resp_str);

		if (lws_write(wsi, (uint8_t *)start, lws_ptr_diff(p, start),
			      LWS_WRITE_HTTP) != lws_ptr_diff(p, start))
			return 1;

		if (pss->urltype == URL_TARGET_ENCODERSTATUS) {
			lws_set_timer_usecs(wsi, SECS_REPORT * LWS_USEC_PER_SEC);
		}

		json_object_put(jresp); /* Free */

		return 0;

	case LWS_CALLBACK_TIMER:

		lwsl_notice("%s: LWS_CALLBACK_TIMER\n", __func__);
		lws_callback_on_writable(wsi);

		return 0;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static struct lws_protocols protocols[] = {
	//{ "http", lws_callback_http_dummy, 0, 0 },
	{ "http", callback_http, 0, 0 },
	{ "sse", callback_sse, sizeof(struct pss), 0 },
	{ NULL, NULL, 0, 0 } /* terminator */
};

/* override the default mount for /sse in the URL space */

static const struct lws_http_mount mount_sse = {
	/* .mount_next */		NULL,		/* linked-list "next" */
	/* .mountpoint */		"/sse",		/* mountpoint URL */
	/* .origin */			NULL,		/* protocol */
	/* .def */			NULL,
	/* .protocol */			"sse",
	/* .cgienv */			NULL,
	/* .extra_mimetypes */		NULL,
	/* .interpret */		NULL,
	/* .cgi_timeout */		0,
	/* .cache_max_age */		0,
	/* .auth_mask */		0,
	/* .cache_reusable */		0,
	/* .cache_revalidate */		0,
	/* .cache_intermediaries */	0,
	/* .origin_protocol */		LWSMPRO_CALLBACK, /* dynamic */
	/* .mountpoint_len */		4,		  /* char count */
	/* .basic_auth_login_file */	NULL,
};

/* default mount serves the URL space from ./mount-origin */

static const struct lws_http_mount mount = {
	/* .mount_next */		&mount_sse,	/* linked-list "next" */
	/* .mountpoint */		"/",		/* mountpoint URL */
	/* .origin */			"./mount-origin", /* serve from dir */
	/* .def */			"index.html",	/* default filename */
	/* .protocol */			NULL,
	/* .cgienv */			NULL,
	/* .extra_mimetypes */		NULL,
	/* .interpret */		NULL,
	/* .cgi_timeout */		0,
	/* .cache_max_age */		0,
	/* .auth_mask */		0,
	/* .cache_reusable */		0,
	/* .cache_revalidate */		0,
	/* .cache_intermediaries */	0,
	/* .origin_protocol */		LWSMPRO_FILE,	/* files in a dir */
	/* .mountpoint_len */		1,		/* char count */
	/* .basic_auth_login_file */	NULL,
};

static int _initialize(struct websockets_ctx *ctx)
{
	ctx->threadTerminated = 0;
	ctx->threadTerminate = 0;
	ctx->threadRunning = 1;

	//const char *p;
	int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE
			/* for LLL_ verbosity above NOTICE to be built into lws,
			 * lws must have been configured and built with
			 * -DCMAKE_BUILD_TYPE=DEBUG instead of =RELEASE */
			/* | LLL_INFO */ /* | LLL_PARSER */ /* | LLL_HEADER */
			/* | LLL_EXT */ /* | LLL_CLIENT */ /* | LLL_LATENCY */
			/* | LLL_DEBUG */;

	lws_set_log_level(logs, NULL);

	memset(&ctx->info, 0, sizeof ctx->info); /* otherwise uninitialized garbage */

	ctx->info.protocols = protocols;
	ctx->info.mounts = &mount;
	ctx->info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
	ctx->info.port = ctx->portNr;
	ctx->info.user = ctx;

#if 1 // SSL
	ctx->info.port = 8443;
	ctx->info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	ctx->info.ssl_cert_filepath = "localhost-100y.cert";
	ctx->info.ssl_private_key_filepath = "localhost-100y.key";
#endif

	lwsl_user("LWS minimal http Server-Side Events | visit http://localhost:%d\n", ctx->info.port);

	ctx->context = lws_create_context(&ctx->info);
	if (!ctx->context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	while (n >= 0 && !ctx->threadTerminate)
		n = lws_service(ctx->context, 0);

	lws_context_destroy(ctx->context);

	ctx->threadTerminated = 1;

	return 0;
}

static void *ltn_ws_threadfunc(void *p)
{
	struct websockets_ctx *ctx = (struct websockets_ctx *)p;

printf("%s() ctx = %p\n", __func__, ctx);
	_initialize(ctx);

	return NULL;
}

int ltn_ws_alloc(void **p, void *userContext, int portNr)
{
	struct websockets_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->userContext = userContext;
	ctx->portNr = portNr;
	ctx->software_version = strdup("n/a");
	ctx->hardware_version = strdup("n/a");
	ctx->fingerprint = strdup("n/a");

	pthread_create(&ctx->threadId, NULL, ltn_ws_threadfunc, ctx);

	printf("ws alloc ctx = %p\n", ctx);
	*p = ctx;
	return 0;
}

void ltn_ws_free(void *p)
{
	if (!p)
		return;

	struct websockets_ctx *ctx = (struct websockets_ctx *)p;


	lws_cancel_service(ctx->context);

	if (ctx->threadRunning) {
		ctx->threadTerminate = 1;
		while (!ctx->threadTerminated) {
			usleep(50 * 1000);
		}
	}

	free(ctx);
}

int ltn_ws_set_thumbnail_jpg(void *p, const unsigned char *buf, int sizeBytes)
{
//	struct websockets_ctx *ctx = (struct websockets_ctx *)p;

	FILE *fh = fopen(LTN_WS_BASE_DIR "/" LTN_WS_LAST_FRAME_JPG_NAME, "wb");
	if (!fh)
		return -1;

	fwrite(buf, 1, sizeBytes, fh);
	fclose(fh);
#if LOCAL_DEBUG
	printf("%s() wrote image %s size %d bytes\n", __func__,
		LTN_WS_BASE_DIR "/" LTN_WS_LAST_FRAME_JPG_NAME, sizeBytes);
#endif

	return 0;
}

int ltn_ws_set_property_signal(void *p, int width, int height, int progressive, int framerateX100)
{
	struct websockets_ctx *ctx = (struct websockets_ctx *)p;
	if (!ctx)
		return -1;

#if LOCAL_DEBUG
	//printf("%s(%d,%d)\n", __func__, width, height);
#endif
	ctx->width = width;
	ctx->height = height;
	ctx->progressive = progressive;
	ctx->framerateX100 = framerateX100;

	return 0;
}

int ltn_ws_set_property_software_version(void *p, const char *string)
{
	struct websockets_ctx *ctx = (struct websockets_ctx *)p;
	if (!ctx)
		return -1;

	if (ctx->software_version)
		free((char *)ctx->software_version);

	ctx->software_version = strdup(string);

	return 0;
}

int ltn_ws_set_property_hardware_version(void *p, const char *string)
{
	struct websockets_ctx *ctx = (struct websockets_ctx *)p;
	if (!ctx)
		return -1;

	if (ctx->hardware_version)
		free((char *)ctx->hardware_version);

	ctx->hardware_version = strdup(string);

	return 0;
}

int ltn_ws_set_property_current_fingerprint(void *p, const char *string)
{
	struct websockets_ctx *ctx = (struct websockets_ctx *)p;
	if (!ctx)
		return -1;

	if (ctx->fingerprint)
		free((char *)ctx->fingerprint);

	ctx->fingerprint = strdup(string);

	if (g_fp_wsi) {
		lws_callback_on_writable(g_fp_wsi);
	}

	return 0;
}

#endif /* LTN_WS_ENABLE */
