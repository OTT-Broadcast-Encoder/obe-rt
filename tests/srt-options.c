/* cc -I. -I<prefix>/include tests/srt-options.c -L<prefix>/lib -lavutil -lm
 * Tests the actual option builder without networking or a libsrt dependency. */
#include <assert.h>
#include "input/lavf/srt_options.h"

int main(void)
{
    const char *blocked[] = {
        "srt://host:9000?passphrase=test-value",
        "srt://host:9000?mode=caller&%70assphrase=test-value",
        "srt://host:9000?passphrase%00ignored=test-value",
        "srt://host:9000?pbkeylen=16",
        "srt://host:9000?enforced_encryption=0",
        "srt://host:9000?enforcedencryption=0",
        "srt://host:9000?%65nforced_encryption=0",
        "srt://host:9000?streamid=a b&passphrase=test-value",
        "srt://host:9000?streamid=a\tb&passphrase=test-value",
        "srt://host:9000?streamid=a,b&passphrase=test-value",
    };
    for (unsigned i = 0; i < sizeof(blocked) / sizeof(*blocked); i++) {
        AVDictionary *options = NULL;
        assert(obe_srt_url_has_crypto_options(blocked[i]));
        assert(obe_srt_options(blocked[i], NULL, &options) == AVERROR(EINVAL));
        assert(options == NULL);
    }
    assert(!obe_srt_url_has_crypto_options("srt://host:9000?mode=caller&latency=200000"));
    assert(!obe_srt_url_has_crypto_options("srt://host:9000?streamid=passphrase"));
    assert(obe_srt_url_has_crypto_options("SRT://host:9000?Passphrase=test-value"));
    assert(!obe_srt_url_has_crypto_options("srt://host:9000 name=passphrase"));
    assert(!obe_srt_url_has_crypto_options("udp://host:9000?passphrase=other-protocol"));

    AVDictionary *options = NULL;
    assert(obe_srt_options("udp://host:9000", "short", &options) == 0);
    assert(options == NULL);
    assert(obe_srt_options("srt://host:9000", NULL, &options) == 0);
    assert(!strcmp(av_dict_get(options, "enforced_encryption", NULL, 0)->value, "1"));
    assert(!av_dict_get(options, "passphrase", NULL, 0));
    assert(!av_dict_get(options, "pbkeylen", NULL, 0));
    av_dict_free(&options);

    char key[81];
    memset(key, 'x', sizeof(key));
    for (int length = 0; length <= 80; length++) {
        key[length] = 0;
        int ret = obe_srt_options("srt://host:9000?mode=listener", key, &options);
        if (length < 10 || length > 79) {
            assert(ret == AVERROR(EINVAL) && options == NULL);
        } else {
            assert(ret == 0);
            assert(!strcmp(av_dict_get(options, "passphrase", NULL, 0)->value, key));
            assert(!strcmp(av_dict_get(options, "pbkeylen", NULL, 0)->value, "32"));
            assert(!strcmp(av_dict_get(options, "enforced_encryption", NULL, 0)->value, "1"));
        }
        av_dict_free(&options);
        key[length] = 'x';
    }
    return 0;
}
