#include "sft/protocol.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void assert_case(int cond, const char *name)
{
    if (!cond) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}

static void put16(unsigned char *b, int off, unsigned short v)
{
    b[off] = (unsigned char)v;
    b[off + 1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *b, int off, unsigned int v)
{
    for (int i = 0; i < 4; ++i) b[off + i] = (unsigned char)(v >> (8*i));
}

static sft_packet_header hdr(unsigned short flags, unsigned int w,
                             unsigned int h, unsigned int len)
{
    unsigned char raw[SFT_HEADER_SIZE] = {0};
    sft_packet_header out = {0};
    put32(raw, 0, SFT_MAGIC);
    put16(raw, 4, SFT_PROTOCOL_VERSION);
    put16(raw, 6, flags);
    put32(raw, 8, w); put32(raw, 12, h); put32(raw, 20, len);
    put32(raw, 28, 123);
    assert_case(sft_parse_header(raw, &out) == 0, "parse header");
    return out;
}

int main(void)
{
    sft_packet_header h;
    h = hdr(SFT_FLAG_VIDEO | SFT_FLAG_KEY, 1280, 720, 2000);
    assert_case(sft_validate_header(&h) == 0 && sft_packet_is_video(&h), "legacy IDR");

    h = hdr(SFT_FLAG_VIDEO | SFT_FLAG_KEY | SFT_FLAG_INPUT, 1280, 720, 2000);
    assert_case(h.raw_flags & SFT_FLAG_INPUT, "v0.7.20 capability remembered");
    assert_case(!(h.flags & SFT_FLAG_INPUT), "capability stripped from media flags");
    assert_case(sft_validate_header(&h) == 0, "v0.7.20 video supported");

    h = hdr(SFT_FLAG_AUDIO | SFT_FLAG_INPUT, 48000, 2, 4096);
    assert_case(sft_validate_header(&h) == 0 && sft_packet_is_audio(&h), "audio with informational INPUT");

    h = hdr(SFT_FLAG_AUDIO | SFT_FLAG_STATUS | SFT_FLAG_INPUT, 0, 0, 192);
    assert_case(sft_validate_header(&h) == 0 && sft_packet_is_status(&h), "status with INPUT");

    h = hdr(SFT_FLAG_VIDEO | SFT_FLAG_CROP | SFT_FLAG_CROP_MID, 1280, 720, 2000);
    assert_case(sft_validate_header(&h) == 0, "crop video accepted");

    h = hdr(SFT_FLAG_AUDIO | SFT_FLAG_CROP, 48000, 2, 4096);
    assert_case(sft_validate_header(&h) != 0, "crop audio rejected");

    h = hdr(SFT_FLAG_VIDEO | 0x100u, 1280, 720, 2000);
    assert_case(sft_validate_header(&h) != 0, "unknown future flag rejected");

    h = hdr(SFT_FLAG_VIDEO | SFT_FLAG_INPUT, 1920, 1080, 3000);
    assert_case(sft_validate_header(&h) == 0, "1080p video accepted");

    h = hdr(SFT_FLAG_VIDEO, 1920, 1200, 3000);
    assert_case(sft_validate_header(&h) != 0, "video dimension guard");

    printf("SFTR v2 compatibility: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
