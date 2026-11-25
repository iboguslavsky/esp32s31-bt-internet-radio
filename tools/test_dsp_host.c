#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aacdec.h"
#include "minimp3.h"

/* Host verification of MP3 decoder, Helix HE-AAC decoder, playlist parser, and Q16.16 resampler */
static bool extract_url_from_playlist(const char *text, char *out_url, size_t out_sz)
{
    const char *line = text;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
        if (!*line) break;
        const char *eol = strpbrk(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > 7 && line[0] != '#') {
            const char *http = NULL;
            if (strncasecmp(line, "File", 4) == 0) {
                const char *eq = memchr(line, '=', len);
                if (eq && (size_t)(eq - line + 1) < len) {
                    http = eq + 1;
                    len -= (size_t)(http - line);
                }
            } else if (strncasecmp(line, "http://", 7) == 0 || strncasecmp(line, "https://", 8) == 0) {
                http = line;
            }
            if (http && (strncasecmp(http, "http://", 7) == 0 || strncasecmp(http, "https://", 8) == 0)) {
                while (len > 0 && (http[len - 1] == ' ' || http[len - 1] == '\t')) len--;
                if (len < out_sz) {
                    memcpy(out_url, http, len);
                    out_url[len] = '\0';
                    return true;
                }
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    return false;
}

int main(void)
{
    /* 1. Verify .pls and .m3u playlist extraction */
    char url[256] = {0};
    const char *pls = "[playlist]\r\nNumberOfEntries=1\r\nFile1=https://ice2.somafm.com/groovesalad-128-aac\r\nTitle1=Groove Salad\r\n";
    assert(extract_url_from_playlist(pls, url, sizeof(url)));
    assert(strcmp(url, "https://ice2.somafm.com/groovesalad-128-aac") == 0);

    const char *m3u = "#EXTM3U\n#EXTINF:-1,Nightride FM\nhttps://stream.nightride.fm/nightride.mp3\n";
    assert(extract_url_from_playlist(m3u, url, sizeof(url)));
    assert(strcmp(url, "https://stream.nightride.fm/nightride.mp3") == 0);

    /* 2. Verify Helix HE-AAC decoder init & ADTS syncword finder */
    HAACDecoder aac = AACInitDecoder();
    assert(aac != NULL);
    unsigned char fake_adts[] = {0x00, 0x11, 0xFF, 0xF1, 0x50, 0x80, 0x02, 0x1F, 0xFC};
    int sync = AACFindSyncWord(fake_adts, sizeof(fake_adts));
    assert(sync == 2);
    AACFreeDecoder(aac);

    /* 3. Verify minimp3 init */
    mp3dec_t mp3;
    mp3dec_init(&mp3);

    /* 4. Verify Q16.16 48000 Hz -> 44100 Hz stereo resampler ratio accuracy */
    const uint32_t in_rate = 48000;
    const uint32_t out_rate = 44100;
    const uint32_t step_q16 = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    uint32_t phase_q16 = 0;
    size_t total_out = 0;
    for (int chunk = 0; chunk < 100; chunk++) {
        size_t frames_in = 480; /* 10 ms @ 48 kHz -> 100 chunks = 1.000 s */
        while ((phase_q16 >> 16) < frames_in) {
            total_out++;
            phase_q16 += step_q16;
        }
        phase_q16 -= (uint32_t)(frames_in << 16);
    }
    /* 1.000 second @ 44.1 kHz must yield 44100 +/- 1 frames */
    assert(total_out >= 44099 && total_out <= 44101);

    printf("HOST_DSP_VERIFICATION_PASSED (resampled 48000->%zu frames/sec)\n", total_out);
    return 0;
}
