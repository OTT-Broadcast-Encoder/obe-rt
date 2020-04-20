#ifndef LTN_WS_H
#define LTN_WS_H

#define LTN_WS_BASE_DIR "mount-origin"
#define LTN_WS_LAST_FRAME_JPG_NAME "image.jpg"

#define LTN_WS_ENABLE 0

#ifdef __cplusplus
extern "C" {
#endif

extern void *g_ltn_ws_handle;

int  ltn_ws_alloc(void **p, void *userContext, int portNr);
void ltn_ws_free(void *ctx);

int  ltn_ws_set_property_current_fingerprint(void *ctx, const char *string);
int  ltn_ws_set_property_software_version(void *ctx, const char *string);
int  ltn_ws_set_property_hardware_version(void *ctx, const char *string);
int  ltn_ws_set_property_signal(void *ctx, int width, int height, int progressive, int framerateX100);
int  ltn_ws_set_thumbnail_jpg(void *ctx, const unsigned char *buf, int sizeBytes);

#ifdef __cplusplus
};
#endif

#endif /* LTN_WS_H */
