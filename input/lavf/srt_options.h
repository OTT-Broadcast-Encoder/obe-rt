/* SRT encryption policy shared by the URL input and CLI history guard. */
#ifndef OBE_SRT_OPTIONS_H
#define OBE_SRT_OPTIONS_H

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

static inline int obe_srt_hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int obe_srt_url_has_crypto_options(const char *location)
{
    if (strncasecmp(location, "srt://", 6)) return 0;
    const char *end = location + strlen(location);
    const char *query = memchr(location, '?', end - location);
    if (!query) return 0;
    query++;
    while (query < end) {
        char name[128];
        size_t len = 0;
        while (query < end && *query != '=' && *query != '&') {
            unsigned char c = *query++;
            if (c == '%' && end - query >= 2) {
                int hi = obe_srt_hex_digit(query[0]);
                int lo = obe_srt_hex_digit(query[1]);
                if (hi >= 0 && lo >= 0) {
                    c = (hi << 4) | lo;
                    query += 2;
                }
            }
            if (len < sizeof(name) - 1) name[len++] = c;
        }
        name[len] = 0;
        if (!strcasecmp(name, "passphrase") || !strcasecmp(name, "pbkeylen") ||
            !strcasecmp(name, "enforced_encryption") || !strcasecmp(name, "enforcedencryption"))
            return 1;
        while (query < end && *query != '&') query++;
        if (query < end) query++;
    }
    return 0;
}

/* Never put a key in the URL: FFmpeg logs URLs on connection failure, and
 * query options override dictionary options. A configured key requires AES-256
 * and matching encryption on the peer. NULL means explicitly unencrypted. */
static inline int obe_srt_options(const char *location, const char *passphrase,
                                  AVDictionary **options)
{
    if (strncmp(location, "srt://", 6)) return 0;
    if (obe_srt_url_has_crypto_options(location)) return AVERROR(EINVAL);
    if (passphrase && (strlen(passphrase) < 10 || strlen(passphrase) > 79))
        return AVERROR(EINVAL);
    int ret = av_dict_set(options, "enforced_encryption", "1", 0);
    if (ret < 0 || !passphrase) return ret;
    if ((ret = av_dict_set(options, "pbkeylen", "32", 0)) < 0) return ret;
    return av_dict_set(options, "passphrase", passphrase, 0);
}

#endif
