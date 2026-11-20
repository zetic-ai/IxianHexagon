// Self-test for the media helpers (server-media.cpp): base64 / data URLs, image decoding and fitting, WAV / MP3 /
// FLAC decoding, the WAV writer and the audio splitter.
//
// Usage: media_selftest [samples_dir]      (default: the repository's samples/ directory)
// Exit code 0 when every check passed, 1 otherwise. Checks that need an optional tool (ffmpeg) print SKIP when
// the tool is missing instead of failing.

#include "server-media.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>  // getpid

using namespace ixsrv;

static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failures;                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                                    \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                 \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failures;                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);            \
            std::fprintf(stderr, __VA_ARGS__);                                               \
            std::fprintf(stderr, "\n");                                                      \
        }                                                                                    \
    } while (0)

// expects std::invalid_argument
#define CHECK_THROWS(expr)                                                                   \
    do {                                                                                     \
        ++g_checks;                                                                          \
        bool thrown_ = false;                                                                \
        try {                                                                                \
            (void) (expr);                                                                   \
        } catch (const std::invalid_argument &) {                                            \
            thrown_ = true;                                                                  \
        }                                                                                    \
        if (!thrown_) {                                                                      \
            ++g_failures;                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: expected std::invalid_argument from %s\n",     \
                         __FILE__, __LINE__, #expr);                                         \
        }                                                                                    \
    } while (0)

//
// helpers
//

static std::vector<uint8_t> read_file(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string bytes_to_string(const std::vector<uint8_t> & v) {
    return std::string(v.begin(), v.end());
}

static uint32_t rd_u32(const std::string & s, size_t at) {
    return (uint32_t) (uint8_t) s[at] | ((uint32_t) (uint8_t) s[at + 1] << 8) | ((uint32_t) (uint8_t) s[at + 2] << 16) |
           ((uint32_t) (uint8_t) s[at + 3] << 24);
}

static uint16_t rd_u16(const std::string & s, size_t at) {
    return (uint16_t) ((uint8_t) s[at] | ((uint8_t) s[at + 1] << 8));
}

static void put_u16(std::vector<uint8_t> & v, uint16_t x) {
    v.push_back((uint8_t) (x & 0xFF));
    v.push_back((uint8_t) (x >> 8));
}

static void put_u32(std::vector<uint8_t> & v, uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v.push_back((uint8_t) ((x >> (8 * i)) & 0xFF));
    }
}

static void put_tag(std::vector<uint8_t> & v, const char * tag) {
    v.insert(v.end(), tag, tag + 4);
}

// deterministic pseudo-random bytes
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint8_t next() {
        s = s * 1664525u + 1013904223u;
        return (uint8_t) (s >> 24);
    }
};

static const uint8_t * px(const DecodedImage & img, int x, int y) {
    return img.rgb.data() + ((size_t) y * img.width + x) * 3;
}

static bool is_rgb(const DecodedImage & img, int x, int y, int r, int g, int b, int tol = 2) {
    const uint8_t * p = px(img, x, y);
    return std::abs((int) p[0] - r) <= tol && std::abs((int) p[1] - g) <= tol && std::abs((int) p[2] - b) <= tol;
}

static bool is_black(const DecodedImage & img, int x, int y) {
    return is_rgb(img, x, y, 0, 0, 0, 0);
}

static bool row_is_black(const DecodedImage & img, int y) {
    for (int x = 0; x < img.width; ++x) {
        if (!is_black(img, x, y)) {
            return false;
        }
    }
    return true;
}

static bool col_is_black(const DecodedImage & img, int x) {
    for (int y = 0; y < img.height; ++y) {
        if (!is_black(img, x, y)) {
            return false;
        }
    }
    return true;
}

static int count_dark_pixels(const DecodedImage & img) {
    int n = 0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        if (img.rgb[i] < 100 && img.rgb[i + 1] < 100 && img.rgb[i + 2] < 100) {
            ++n;
        }
    }
    return n;
}

// left half red, right half blue
static DecodedImage make_red_blue(int w, int h) {
    DecodedImage img;
    img.width  = w;
    img.height = h;
    img.rgb.assign((size_t) w * h * 3, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t * p = img.rgb.data() + ((size_t) y * w + x) * 3;
            if (x < w / 2) {
                p[0] = 255;
            } else {
                p[2] = 255;
            }
        }
    }
    return img;
}

// synthetic RIFF/WAVE: `format_tag` 1 (PCM) or 3 (float); `extensible` wraps it in WAVE_FORMAT_EXTENSIBLE;
// `data_size_field` overrides the data chunk's size field; `odd_chunk` inserts a LIST chunk with an odd body size
static std::vector<uint8_t> make_wav(uint16_t format_tag, uint16_t channels, uint32_t rate, uint16_t bits,
                                     const std::vector<uint8_t> & pcm_bytes, bool extensible,
                                     std::optional<uint32_t> data_size_field = std::nullopt, bool odd_chunk = false,
                                     bool fmt_after_data = false) {
    std::vector<uint8_t> fmt;
    const uint16_t block_align = (uint16_t) (channels * bits / 8);
    put_u16(fmt, extensible ? 0xFFFE : format_tag);
    put_u16(fmt, channels);
    put_u32(fmt, rate);
    put_u32(fmt, rate * block_align);
    put_u16(fmt, block_align);
    put_u16(fmt, bits);
    if (extensible) {
        put_u16(fmt, 22);        // cbSize
        put_u16(fmt, bits);      // valid bits per sample
        put_u32(fmt, 3);         // channel mask (FL | FR)
        put_u16(fmt, format_tag);  // SubFormat GUID: <format_tag> 00 00 00 00 10 00 80 00 00 AA 00 38 9B 71
        const uint8_t guid_rest[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.insert(fmt.end(), guid_rest, guid_rest + 14);
    }

    std::vector<uint8_t> body;  // everything after "WAVE"
    auto add_fmt = [&]() {
        put_tag(body, "fmt ");
        put_u32(body, (uint32_t) fmt.size());
        body.insert(body.end(), fmt.begin(), fmt.end());
    };
    auto add_data = [&]() {
        put_tag(body, "data");
        put_u32(body, data_size_field ? *data_size_field : (uint32_t) pcm_bytes.size());
        body.insert(body.end(), pcm_bytes.begin(), pcm_bytes.end());
        if (pcm_bytes.size() & 1) {
            body.push_back(0);
        }
    };
    if (!fmt_after_data) {
        add_fmt();
    }
    if (odd_chunk) {
        put_tag(body, "LIST");
        put_u32(body, 3);
        body.push_back('I');
        body.push_back('N');
        body.push_back('F');
        body.push_back(0);  // pad byte
        put_tag(body, "fact");
        put_u32(body, 4);
        put_u32(body, 1234);
    }
    add_data();
    if (fmt_after_data) {
        add_fmt();
    }

    std::vector<uint8_t> out;
    put_tag(out, "RIFF");
    put_u32(out, (uint32_t) (4 + body.size()));
    put_tag(out, "WAVE");
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static void put_s24(std::vector<uint8_t> & v, float x) {
    const long s = std::lround((double) x * 8388607.0);
    const uint32_t u = (uint32_t) (int32_t) s;
    v.push_back((uint8_t) (u & 0xFF));
    v.push_back((uint8_t) ((u >> 8) & 0xFF));
    v.push_back((uint8_t) ((u >> 16) & 0xFF));
}

static void put_f32(std::vector<uint8_t> & v, float x) {
    uint32_t u;
    std::memcpy(&u, &x, 4);
    put_u32(v, u);
}

static void put_f64(std::vector<uint8_t> & v, double x) {
    uint64_t u;
    std::memcpy(&u, &x, 8);
    put_u32(v, (uint32_t) (u & 0xFFFFFFFFu));
    put_u32(v, (uint32_t) (u >> 32));
}

static float max_abs_diff(const std::vector<float> & a, const std::vector<float> & b) {
    if (a.size() != b.size()) {
        return 1e30f;
    }
    float m = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, std::fabs(a[i] - b[i]));
    }
    return m;
}

static float max_abs(const std::vector<float> & a) {
    float m = 0.0f;
    for (float v : a) {
        m = std::max(m, std::fabs(v));
    }
    return m;
}

static bool all_in_unit_range(const std::vector<float> & a) {
    for (float v : a) {
        if (!(v >= -1.0f && v <= 1.0f)) {
            return false;
        }
    }
    return true;
}

static std::string shell_quote(const std::string & s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

static bool tool_on_path(const char * name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

static std::string run_capture(const std::string & cmd) {
    std::string out;
    FILE * p = popen(cmd.c_str(), "r");
    if (!p) {
        return out;
    }
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p)) {
        out += buf;
    }
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

//
// tests
//

static void test_base64() {
    std::puts("[base64]");
    CHECK(base64_encode(std::string("")) == "");
    CHECK(base64_encode(std::string("M")) == "TQ==");
    CHECK(base64_encode(std::string("Ma")) == "TWE=");
    CHECK(base64_encode(std::string("Man")) == "TWFu");
    CHECK(base64_encode(std::string("Man ")) == "TWFuIA==");

    // round trips of every length 1..64 with pseudo-random bytes
    Lcg rng(42);
    for (size_t n = 1; n <= 64; ++n) {
        raw_buffer data(n);
        for (auto & b : data) {
            b = rng.next();
        }
        const std::string enc = base64_encode(data);
        CHECK(enc.size() == ((n + 2) / 3) * 4);
        CHECK_MSG(base64_decode(enc) == data, "n=%zu", n);
        // without padding
        std::string stripped = enc;
        while (!stripped.empty() && stripped.back() == '=') {
            stripped.pop_back();
        }
        CHECK_MSG(base64_decode(stripped) == data, "unpadded n=%zu", n);
        // URL-safe alphabet
        std::string urlsafe = stripped;
        for (char & c : urlsafe) {
            if (c == '+') {
                c = '-';
            } else if (c == '/') {
                c = '_';
            }
        }
        CHECK_MSG(base64_decode(urlsafe) == data, "url-safe n=%zu", n);
        // whitespace sprinkled in (MIME line breaks and such)
        std::string spaced;
        for (size_t i = 0; i < enc.size(); ++i) {
            spaced.push_back(enc[i]);
            if (i % 5 == 4) {
                spaced += "\r\n";
            }
            if (i % 7 == 6) {
                spaced.push_back(' ');
            }
        }
        spaced = "\n " + spaced + "\n";
        CHECK_MSG(base64_decode(spaced) == data, "whitespace n=%zu", n);
    }

    // specific decodes
    CHECK(bytes_to_string(base64_decode("TWE")) == "Ma");
    CHECK(bytes_to_string(base64_decode("TQ")) == "M");
    CHECK(bytes_to_string(base64_decode("QUJD")) == "ABC");
    CHECK(bytes_to_string(base64_decode("TWFu\nIA==\n")) == "Man ");
    {
        const raw_buffer hi = {0xFB, 0xFF, 0xBF};
        CHECK(base64_encode(hi) == "+/+/");
        CHECK(base64_decode("-_-_") == hi);
        CHECK(base64_decode("+/+/") == hi);
    }

    // rejections
    CHECK_THROWS(base64_decode(""));
    CHECK_THROWS(base64_decode("   \n"));
    CHECK_THROWS(base64_decode("A"));
    CHECK_THROWS(base64_decode("====") );
    CHECK_THROWS(base64_decode("TQ=x"));
    CHECK_THROWS(base64_decode("T!Fu"));
    CHECK_THROWS(base64_decode("TWFu*"));
    CHECK_THROWS(base64_decode("TWFu TWF\x80"));
    CHECK_THROWS(base64_decode("A==="));
    try {
        base64_decode("!!");
        CHECK(false);
    } catch (const std::invalid_argument & e) {
        CHECK(std::string(e.what()) == "invalid base64");
    }
}

static void test_data_url() {
    std::puts("[data url]");
    std::string mime;
    raw_buffer  out;

    const std::string png_magic("\x89PNG\r\n\x1a\n", 8);
    CHECK(parse_data_url("data:image/png;base64," + base64_encode(png_magic), mime, out));
    CHECK(mime == "image/png");
    CHECK(bytes_to_string(out) == png_magic);

    CHECK(parse_data_url("DATA:Image/PNG;charset=utf-8;BASE64,QUJD", mime, out));
    CHECK(mime == "image/png");
    CHECK(bytes_to_string(out) == "ABC");

    CHECK(parse_data_url("data:audio/wav;base64,UklGRg==", mime, out));
    CHECK(mime == "audio/wav");
    CHECK(bytes_to_string(out) == "RIFF");

    CHECK(parse_data_url("data:;base64,QUJD", mime, out));
    CHECK(mime.empty());
    CHECK(bytes_to_string(out) == "ABC");

    // payload with line breaks (as some clients send)
    CHECK(parse_data_url("data:text/plain;base64,TWFu\nIA==\n", mime, out));
    CHECK(mime == "text/plain");
    CHECK(bytes_to_string(out) == "Man ");

    // not data URLs
    CHECK(!parse_data_url("http://example.com/cats.png", mime, out));
    CHECK(!parse_data_url("", mime, out));
    CHECK(!parse_data_url("dat", mime, out));
    CHECK(!parse_data_url("data", mime, out));
    CHECK(!parse_data_url("datum:image/png;base64,QUJD", mime, out));

    // malformed
    CHECK_THROWS(parse_data_url("data:image/png;base64", mime, out));          // no comma
    CHECK_THROWS(parse_data_url("data:image/png,QUJD", mime, out));            // not base64
    CHECK_THROWS(parse_data_url("data:image/png;charset=x,QUJD", mime, out));  // not base64
    CHECK_THROWS(parse_data_url("data:image/png;base64,!!", mime, out));       // bad payload
    CHECK_THROWS(parse_data_url("data:image/png;base64,", mime, out));         // empty payload
    try {
        parse_data_url("data:image/png;base64", mime, out);
    } catch (const std::invalid_argument & e) {
        CHECK(std::string(e.what()) == "Invalid uri-encoded base64 value");
    }
    try {
        parse_data_url("data:image/png,QUJD", mime, out);
    } catch (const std::invalid_argument & e) {
        CHECK(std::string(e.what()) == "uri must be base64 encoded");
    }
}

static void test_sniff() {
    std::puts("[sniff]");
    auto kind = [](std::string s) {
        s.resize(std::max<size_t>(s.size(), 16), '\0');
        return sniff_media_kind((const uint8_t *) s.data(), s.size());
    };
    // images
    CHECK(kind(std::string("\x89PNG\r\n\x1a\n", 8)) == "image");
    CHECK(kind(std::string("\xFF\xD8\xFF\xE0\x00\x10JFIF", 10)) == "image");
    CHECK(kind("GIF89a") == "image");
    CHECK(kind("GIF87a") == "image");
    CHECK(kind(std::string("BM\x36\x00\x0c\x00", 6)) == "image");
    CHECK(kind("P6\n512 512\n255\n") == "image");
    CHECK(kind("P5 4 4 255 ") == "image");
    CHECK(kind("P3\n1 1\n255\n") == "image");
    CHECK(kind("P2\t1 1\n255\n") == "image");
    CHECK(kind("P7\n") == "");
    CHECK(kind("P6x") == "");
    CHECK(kind(std::string("RIFF\x00\x00\x00\x00WEBPVP8 ", 16)) == "image");
    // audio
    CHECK(kind(std::string("RIFF\x24\x00\x00\x00WAVEfmt ", 16)) == "audio");
    CHECK(kind(std::string("RIFF\xFF\xFF\xFF\xFFWAVEfmt ", 16)) == "audio");
    CHECK(kind(std::string("ID3\x04\x00\x00\x00\x00\x00\x00", 10)) == "audio");
    CHECK(kind(std::string("\xFF\xFB\x90\x00", 4)) == "audio");  // MPEG-1 layer III, 128 kbps, 44.1 kHz
    CHECK(kind(std::string("\xFF\xF3\x80\x00", 4)) == "audio");  // MPEG-2 layer III
    CHECK(kind(std::string("\xFF\xE3\x90\x00", 4)) == "audio");  // MPEG-2.5 layer III
    CHECK(kind(std::string("\xFF\xFD\x90\x00", 4)) == "audio");  // layer II
    CHECK(kind(std::string("\xFF\xFF\x90\x00", 4)) == "audio");  // layer I
    CHECK(kind(std::string("\xFF\xF9\x90\x00", 4)) == "");       // reserved layer
    CHECK(kind(std::string("\xFF\xEB\x90\x00", 4)) == "");       // reserved version
    CHECK(kind(std::string("\xFF\xFB\xF0\x00", 4)) == "");       // bad bitrate index
    CHECK(kind(std::string("\xFF\xFB\x9C\x00", 4)) == "");       // bad sample rate index
    CHECK(kind(std::string("\xFF\xD8\xFF\xFB", 4)) == "image");  // JPEG wins
    CHECK(kind("fLaC") == "audio");
    CHECK(kind("OggS") == "audio");
    // video
    CHECK(kind(std::string("\x00\x00\x00\x18" "ftypisom", 12)) == "video");
    CHECK(kind(std::string("\x00\x00\x00\x14" "ftypqt  ", 12)) == "video");
    CHECK(kind(std::string("\x1A\x45\xDF\xA3\x01\x00\x00\x00", 8)) == "video");
    CHECK(kind(std::string("RIFF\x00\x00\x00\x00" "AVI LIST", 16)) == "video");
    // unknown
    CHECK(kind("hello world!") == "");
    CHECK(kind(std::string("RIFF\x00\x00\x00\x00XXXX", 12)) == "");
    CHECK(kind("{\"json\": 1}") == "");
    CHECK(sniff_media_kind(nullptr, 0) == "");
    CHECK(sniff_media_kind((const uint8_t *) "BM", 2) == "");  // too short to tell
}

static void test_images(const std::string & samples) {
    std::puts("[images]");
    const std::vector<uint8_t> png = read_file(samples + "/cats.png");
    const std::vector<uint8_t> ppm = read_file(samples + "/cats.ppm");
    CHECK_MSG(!png.empty(), "cannot read %s/cats.png", samples.c_str());
    CHECK_MSG(!ppm.empty(), "cannot read %s/cats.ppm", samples.c_str());
    if (png.empty() || ppm.empty()) {
        return;
    }
    CHECK(sniff_media_kind(png.data(), png.size()) == "image");
    CHECK(sniff_media_kind(ppm.data(), ppm.size()) == "image");

    DecodedImage a;
    DecodedImage b;
    try {
        a = decode_image(png.data(), png.size());
        b = decode_image(ppm.data(), ppm.size());
    } catch (const std::exception & e) {
        CHECK_MSG(false, "decode threw: %s", e.what());
        return;
    }
    CHECK(a.width == 512 && a.height == 512);
    CHECK(a.rgb.size() == 512 * 512 * 3);
    CHECK(b.width == 512 && b.height == 512);
    CHECK(b.rgb.size() == 512 * 512 * 3);
    if (a.rgb.size() == b.rgb.size()) {
        int max_diff = 0;
        size_t n_diff = 0;
        for (size_t i = 0; i < a.rgb.size(); ++i) {
            const int d = std::abs((int) a.rgb[i] - (int) b.rgb[i]);
            max_diff = std::max(max_diff, d);
            n_diff += d != 0;
        }
        std::printf("  cats.png vs cats.ppm: max abs diff %d, %zu of %zu bytes differ\n", max_diff, n_diff, a.rgb.size());
        CHECK_MSG(max_diff <= 2, "max_diff=%d", max_diff);
    }
    // the picture is not degenerate
    CHECK(count_dark_pixels(a) < 512 * 512 / 2);

    // encode_ppm -> decode_image round trip is exact
    const DecodedImage synth = make_red_blue(640, 480);
    const std::string  ppm_s = encode_ppm(synth);
    const std::string ppm_hdr = "P6\n640 480\n255\n";  // 15 bytes
    CHECK(ppm_s.compare(0, ppm_hdr.size(), ppm_hdr) == 0);
    CHECK(ppm_s.size() == ppm_hdr.size() + synth.rgb.size());
    const DecodedImage back = decode_image((const uint8_t *) ppm_s.data(), ppm_s.size());
    CHECK(back.width == 640 && back.height == 480);
    CHECK(back.rgb == synth.rgb);

    // other decoders stb_image provides: a tiny grey PGM is expanded to RGB
    {
        const std::string pgm = std::string("P5\n2 1\n255\n") + std::string("\x10\xF0", 2);
        const DecodedImage g = decode_image((const uint8_t *) pgm.data(), pgm.size());
        CHECK(g.width == 2 && g.height == 1);
        CHECK(g.rgb == (std::vector<uint8_t>{0x10, 0x10, 0x10, 0xF0, 0xF0, 0xF0}));
    }

    // failures
    CHECK_THROWS(decode_image((const uint8_t *) "not an image at all", 19));
    CHECK_THROWS(decode_image(png.data(), 0));
    CHECK_THROWS(decode_image(png.data(), 40));  // truncated PNG
    try {
        decode_image((const uint8_t *) "garbage", 7);
    } catch (const std::invalid_argument & e) {
        CHECK_MSG(std::string(e.what()).rfind("failed to decode image: ", 0) == 0, "message: %s", e.what());
    }
    {
        // a PNG header claiming a gigantic canvas is refused before anything is allocated
        std::string huge("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR", 16);
        huge += std::string("\x00\x01\x00\x00\x00\x01\x00\x00\x08\x02\x00\x00\x00", 13);  // 65536 x 65536 RGB
        huge += std::string(8, '\0');
        CHECK_THROWS(decode_image((const uint8_t *) huge.data(), huge.size()));
    }
}

static void test_fit() {
    std::puts("[fit_image]");
    const DecodedImage src = make_red_blue(640, 480);

    // letterbox 640x480 -> 512x512: the picture becomes 512x384, centred -> 64 black rows top and bottom
    {
        const DecodedImage lb = fit_image(src, 512, 512, ImageFit::Letterbox);
        CHECK(lb.width == 512 && lb.height == 512 && lb.rgb.size() == 512 * 512 * 3);
        bool top_black = true, bottom_black = true, band_colored = true, boundary_ok = true;
        for (int y = 0; y < 64; ++y) {
            top_black = top_black && row_is_black(lb, y);
        }
        for (int y = 448; y < 512; ++y) {
            bottom_black = bottom_black && row_is_black(lb, y);
        }
        CHECK(top_black);
        CHECK(bottom_black);
        CHECK(!row_is_black(lb, 64));
        CHECK(!row_is_black(lb, 447));
        for (int y = 64; y < 448; ++y) {
            for (int x = 0; x < 512; ++x) {
                const uint8_t * p = px(lb, x, y);
                if (p[0] + p[1] + p[2] < 200) {
                    band_colored = false;
                }
                if (x <= 253 && !is_rgb(lb, x, y, 255, 0, 0)) {
                    boundary_ok = false;
                }
                if (x >= 258 && !is_rgb(lb, x, y, 0, 0, 255)) {
                    boundary_ok = false;
                }
            }
        }
        CHECK(band_colored);
        CHECK(boundary_ok);
        // the source boundary at x=320 lands exactly on x=256 (scale 0.8 with box filtering)
        CHECK(is_rgb(lb, 255, 256, 255, 0, 0));
        CHECK(is_rgb(lb, 256, 256, 0, 0, 255));
    }

    // stretch: no black anywhere, left edge red, right edge blue
    {
        const DecodedImage st = fit_image(src, 512, 512, ImageFit::Stretch);
        CHECK(st.width == 512 && st.height == 512 && st.rgb.size() == 512 * 512 * 3);
        CHECK(count_dark_pixels(st) == 0);
        CHECK(is_rgb(st, 0, 0, 255, 0, 0));
        CHECK(is_rgb(st, 0, 511, 255, 0, 0));
        CHECK(is_rgb(st, 511, 0, 0, 0, 255));
        CHECK(is_rgb(st, 511, 511, 0, 0, 255));
        CHECK(is_rgb(st, 255, 300, 255, 0, 0));
        CHECK(is_rgb(st, 256, 300, 0, 0, 255));
    }

    // crop: the centre 480x480 window is enlarged to 512x512; no black, boundary still in the middle
    {
        const DecodedImage cr = fit_image(src, 512, 512, ImageFit::Crop);
        CHECK(cr.width == 512 && cr.height == 512 && cr.rgb.size() == 512 * 512 * 3);
        CHECK(count_dark_pixels(cr) == 0);
        bool boundary_ok = true;
        for (int y = 0; y < 512; y += 7) {
            for (int x = 0; x < 512; ++x) {
                if (x <= 250 && !is_rgb(cr, x, y, 255, 0, 0)) {
                    boundary_ok = false;
                }
                if (x >= 262 && !is_rgb(cr, x, y, 0, 0, 255)) {
                    boundary_ok = false;
                }
            }
        }
        CHECK(boundary_ok);
        CHECK(is_rgb(cr, 0, 0, 255, 0, 0));
        CHECK(is_rgb(cr, 511, 511, 0, 0, 255));
    }

    // a tall source gets side bars instead
    {
        const DecodedImage tall = make_red_blue(480, 640);
        const DecodedImage lb   = fit_image(tall, 512, 512, ImageFit::Letterbox);
        bool left_black = true, right_black = true;
        for (int x = 0; x < 64; ++x) {
            left_black = left_black && col_is_black(lb, x);
        }
        for (int x = 448; x < 512; ++x) {
            right_black = right_black && col_is_black(lb, x);
        }
        CHECK(left_black);
        CHECK(right_black);
        CHECK(!col_is_black(lb, 64));
        CHECK(!col_is_black(lb, 447));
        CHECK(is_rgb(lb, 100, 256, 255, 0, 0));
        CHECK(is_rgb(lb, 400, 256, 0, 0, 255));
        // non-square target: 640x480 -> 100x200 gives a 100x75 picture at rows 62..136
        const DecodedImage small = fit_image(src, 100, 200, ImageFit::Letterbox);
        CHECK(small.width == 100 && small.height == 200);
        CHECK(row_is_black(small, 61) && row_is_black(small, 137));
        CHECK(!row_is_black(small, 62) && !row_is_black(small, 136));
        CHECK(is_rgb(small, 10, 100, 255, 0, 0));
        CHECK(is_rgb(small, 90, 100, 0, 0, 255));
    }

    // upscale 100x100 -> 512x512 (bilinear): quadrant colours survive, corners exact
    {
        DecodedImage q;
        q.width  = 100;
        q.height = 100;
        q.rgb.assign(100 * 100 * 3, 0);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 100; ++x) {
                uint8_t * p = q.rgb.data() + ((size_t) y * 100 + x) * 3;
                if (y < 50) {
                    if (x < 50) {
                        p[0] = 255;             // top-left red
                    } else {
                        p[1] = 255;             // top-right green
                    }
                } else {
                    if (x < 50) {
                        p[2] = 255;             // bottom-left blue
                    } else {
                        p[0] = p[1] = p[2] = 255;  // bottom-right white
                    }
                }
            }
        }
        const DecodedImage up = fit_image(q, 512, 512, ImageFit::Stretch);
        CHECK(up.width == 512 && up.height == 512 && up.rgb.size() == 512 * 512 * 3);
        CHECK(is_rgb(up, 0, 0, 255, 0, 0, 0));
        CHECK(is_rgb(up, 511, 0, 0, 255, 0, 0));
        CHECK(is_rgb(up, 0, 511, 0, 0, 255, 0));
        CHECK(is_rgb(up, 511, 511, 255, 255, 255, 0));
        CHECK(is_rgb(up, 128, 128, 255, 0, 0, 0));
        CHECK(is_rgb(up, 384, 128, 0, 255, 0, 0));
        CHECK(is_rgb(up, 128, 384, 0, 0, 255, 0));
        CHECK(is_rgb(up, 384, 384, 255, 255, 255, 0));
        // the transition in the middle is a blend, not a jump to black
        const uint8_t * mid = px(up, 256, 128);
        CHECK(mid[0] + mid[1] > 200);
        // letterbox / crop of a square into a square == stretch
        CHECK(fit_image(q, 512, 512, ImageFit::Letterbox).rgb == up.rgb);
        CHECK(fit_image(q, 512, 512, ImageFit::Crop).rgb == up.rgb);
    }

    // integer-ratio shrink is an exact box average
    {
        DecodedImage g;
        g.width  = 4;
        g.height = 4;
        g.rgb.resize(4 * 4 * 3);
        const uint8_t block[4] = {0, 100, 200, 255};
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const uint8_t v = block[(y & 1) * 2 + (x & 1)];
                for (int c = 0; c < 3; ++c) {
                    g.rgb[((size_t) y * 4 + x) * 3 + c] = v;
                }
            }
        }
        const DecodedImage half = fit_image(g, 2, 2, ImageFit::Stretch);
        CHECK(half.rgb.size() == 2 * 2 * 3);
        bool all_139 = true;
        for (uint8_t v : half.rgb) {
            all_139 = all_139 && (v == 139);  // (0 + 100 + 200 + 255) / 4 = 138.75
        }
        CHECK(all_139);
        // 1x1: the whole picture averaged
        const DecodedImage one = fit_image(g, 1, 1, ImageFit::Letterbox);
        CHECK(one.rgb.size() == 3 && one.rgb[0] == 139);
    }

    // already the right size: returned byte-equal for every policy
    {
        const DecodedImage same = make_red_blue(512, 512);
        CHECK(fit_image(same, 512, 512, ImageFit::Letterbox).rgb == same.rgb);
        CHECK(fit_image(same, 512, 512, ImageFit::Stretch).rgb == same.rgb);
        CHECK(fit_image(same, 512, 512, ImageFit::Crop).rgb == same.rgb);
    }

    // bad input
    CHECK_THROWS(fit_image(src, 0, 512, ImageFit::Letterbox));
    CHECK_THROWS(fit_image(src, 512, -1, ImageFit::Letterbox));
    {
        DecodedImage broken = src;
        broken.rgb.pop_back();
        CHECK_THROWS(fit_image(broken, 512, 512, ImageFit::Letterbox));
        CHECK_THROWS(encode_ppm(broken));
    }

    // names
    CHECK(image_fit_from_name("letterbox") == ImageFit::Letterbox);
    CHECK(image_fit_from_name("LetterBox") == ImageFit::Letterbox);
    CHECK(image_fit_from_name("STRETCH") == ImageFit::Stretch);
    CHECK(image_fit_from_name(" crop ") == ImageFit::Crop);
    CHECK_THROWS(image_fit_from_name("pad"));
    CHECK_THROWS(image_fit_from_name(""));
    CHECK(std::string(image_fit_name(ImageFit::Letterbox)) == "letterbox");
    CHECK(std::string(image_fit_name(ImageFit::Stretch)) == "stretch");
    CHECK(std::string(image_fit_name(ImageFit::Crop)) == "crop");
    for (ImageFit f : {ImageFit::Letterbox, ImageFit::Stretch, ImageFit::Crop}) {
        CHECK(image_fit_from_name(image_fit_name(f)) == f);
    }
}

static void test_wav_samples(const std::string & samples) {
    std::puts("[wav samples]");
    const std::vector<uint8_t> jfk = read_file(samples + "/jfk.wav");
    CHECK_MSG(!jfk.empty(), "cannot read %s/jfk.wav", samples.c_str());
    if (jfk.empty()) {
        return;
    }
    CHECK(sniff_media_kind(jfk.data(), jfk.size()) == "audio");

    DecodedAudio a;
    std::string  err;
    const bool ok = parse_wav(jfk.data(), jfk.size(), a, err);
    CHECK_MSG(ok, "parse_wav: %s", err.c_str());
    if (!ok) {
        return;
    }
    std::printf("  jfk.wav: %d Hz, %d ch, %zu frames (%.3f s)\n", a.sample_rate, a.channels, a.frames(), a.seconds());
    CHECK(a.sample_rate == 16000);
    CHECK(a.channels == 1);
    CHECK(a.frames() >= 175000 && a.frames() <= 177000);
    CHECK(a.pcm.size() == a.frames());
    CHECK(all_in_unit_range(a.pcm));
    CHECK(max_abs(a.pcm) > 0.05f);
    CHECK(std::fabs(a.seconds() - 11.0) < 0.1);

    const DecodedAudio d = decode_audio(jfk.data(), jfk.size());
    CHECK(d.sample_rate == a.sample_rate && d.channels == a.channels);
    CHECK(d.frames() == a.frames());
    CHECK(d.pcm == a.pcm);

    // the mono mixdown of a mono file is the file
    CHECK(audio_to_mono(a) == a.pcm);

    const std::vector<uint8_t> q = read_file(samples + "/question.wav");
    if (!q.empty()) {
        DecodedAudio qa;
        CHECK_MSG(parse_wav(q.data(), q.size(), qa, err), "question.wav: %s", err.c_str());
        CHECK(qa.sample_rate == 16000 && qa.channels == 1);
        CHECK(std::fabs(qa.seconds() - 2.7) < 0.3);
    } else {
        std::puts("  SKIP question.wav (not found)");
    }
}

static void test_wav_synth() {
    std::puts("[wav synthetic]");
    std::string err;
    const int n = 1000;
    std::vector<float> left(n), right(n);
    for (int i = 0; i < n; ++i) {
        left[i]  = 0.9f * std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
        right[i] = -0.5f * left[i];
    }

    // 24-bit stereo, WAVE_FORMAT_EXTENSIBLE, with an odd-sized LIST chunk before the data
    {
        std::vector<uint8_t> pcm;
        for (int i = 0; i < n; ++i) {
            put_s24(pcm, left[i]);
            put_s24(pcm, right[i]);
        }
        const std::vector<uint8_t> wav = make_wav(1, 2, 48000, 24, pcm, true, std::nullopt, true);
        DecodedAudio a;
        CHECK_MSG(parse_wav(wav.data(), wav.size(), a, err), "24-bit extensible: %s", err.c_str());
        CHECK(a.sample_rate == 48000 && a.channels == 2);
        CHECK(a.frames() == (size_t) n);
        float worst = 0.0f;
        for (int i = 0; i < n && (size_t) (2 * i + 1) < a.pcm.size(); ++i) {
            worst = std::max(worst, std::fabs(a.pcm[2 * i] - left[i]));
            worst = std::max(worst, std::fabs(a.pcm[2 * i + 1] - right[i]));
        }
        CHECK_MSG(worst < 1e-3f, "worst=%g", worst);
        const std::vector<float> mono = audio_to_mono(a);
        CHECK(mono.size() == (size_t) n);
        float worst_mono = 0.0f;
        for (int i = 0; i < n && (size_t) i < mono.size(); ++i) {
            worst_mono = std::max(worst_mono, std::fabs(mono[i] - 0.5f * (left[i] + right[i])));
        }
        CHECK_MSG(worst_mono < 1e-3f, "worst_mono=%g", worst_mono);
        // the same data through decode_audio
        const DecodedAudio d = decode_audio(wav.data(), wav.size());
        CHECK(d.pcm == a.pcm && d.channels == 2 && d.sample_rate == 48000);
        // plain (non-extensible) 24-bit as well
        const std::vector<uint8_t> wav2 = make_wav(1, 2, 48000, 24, pcm, false);
        DecodedAudio b;
        CHECK_MSG(parse_wav(wav2.data(), wav2.size(), b, err), "24-bit: %s", err.c_str());
        CHECK(b.pcm == a.pcm);
    }

    // float32 mono (format 3), odd chunk after fmt
    {
        std::vector<uint8_t> pcm;
        for (int i = 0; i < n; ++i) {
            put_f32(pcm, left[i]);
        }
        const std::vector<uint8_t> wav = make_wav(3, 1, 22050, 32, pcm, false, std::nullopt, true);
        DecodedAudio a;
        CHECK_MSG(parse_wav(wav.data(), wav.size(), a, err), "float32: %s", err.c_str());
        CHECK(a.sample_rate == 22050 && a.channels == 1);
        CHECK(a.frames() == (size_t) n);
        CHECK(max_abs_diff(a.pcm, left) < 1e-6f);
        // extensible float
        const std::vector<uint8_t> wav2 = make_wav(3, 1, 22050, 32, pcm, true);
        DecodedAudio b;
        CHECK_MSG(parse_wav(wav2.data(), wav2.size(), b, err), "float32 extensible: %s", err.c_str());
        CHECK(b.pcm == a.pcm);
        // out-of-range float samples are clamped to [-1, 1]
        std::vector<uint8_t> loud;
        put_f32(loud, 3.0f);
        put_f32(loud, -7.5f);
        put_f32(loud, 0.25f);
        const std::vector<uint8_t> wav3 = make_wav(3, 1, 8000, 32, loud, false);
        DecodedAudio c;
        CHECK(parse_wav(wav3.data(), wav3.size(), c, err));
        CHECK(c.pcm == (std::vector<float>{1.0f, -1.0f, 0.25f}));
    }

    // float64 mono
    {
        std::vector<uint8_t> pcm;
        for (int i = 0; i < n; ++i) {
            put_f64(pcm, (double) left[i]);
        }
        const std::vector<uint8_t> wav = make_wav(3, 1, 16000, 64, pcm, false);
        DecodedAudio a;
        CHECK_MSG(parse_wav(wav.data(), wav.size(), a, err), "float64: %s", err.c_str());
        CHECK(a.frames() == (size_t) n);
        CHECK(max_abs_diff(a.pcm, left) < 1e-6f);
    }

    // 8-bit unsigned and 32-bit signed PCM
    {
        const std::vector<uint8_t> pcm8 = {128, 255, 0, 192};
        const std::vector<uint8_t> wav8 = make_wav(1, 1, 8000, 8, pcm8, false);
        DecodedAudio a;
        CHECK_MSG(parse_wav(wav8.data(), wav8.size(), a, err), "8-bit: %s", err.c_str());
        CHECK(a.frames() == 4);
        CHECK(a.pcm.size() == 4 && std::fabs(a.pcm[0]) < 1e-6f && std::fabs(a.pcm[1] - 127.0f / 128.0f) < 1e-6f &&
              std::fabs(a.pcm[2] + 1.0f) < 1e-6f && std::fabs(a.pcm[3] - 0.5f) < 1e-6f);

        std::vector<uint8_t> pcm32;
        put_u32(pcm32, 0x40000000u);           // 0.5
        put_u32(pcm32, 0x80000000u);           // -1.0
        put_u32(pcm32, 0x7FFFFFFFu);           // ~1.0
        put_u32(pcm32, 0u);                    // 0
        const std::vector<uint8_t> wav32 = make_wav(1, 1, 8000, 32, pcm32, false);
        DecodedAudio b;
        CHECK_MSG(parse_wav(wav32.data(), wav32.size(), b, err), "32-bit: %s", err.c_str());
        CHECK(b.frames() == 4);
        CHECK(b.pcm.size() == 4 && std::fabs(b.pcm[0] - 0.5f) < 1e-6f && std::fabs(b.pcm[1] + 1.0f) < 1e-6f &&
              std::fabs(b.pcm[2] - 1.0f) < 1e-6f && b.pcm[3] == 0.0f);
    }

    // 16-bit via the writer, streamed sizes: data size 0xFFFFFFFF (and RIFF size 0xFFFFFFFF) and data size 0
    {
        std::vector<uint8_t> pcm;
        for (int i = 0; i < n; ++i) {
            const int16_t s = (int16_t) std::lround(left[i] * 32767.0f);
            put_u16(pcm, (uint16_t) s);
        }
        const std::string streamed = wav_header(16000, 1, 16, 0xFFFFFFFFu) + bytes_to_string(pcm);
        DecodedAudio a;
        CHECK_MSG(parse_wav((const uint8_t *) streamed.data(), streamed.size(), a, err), "streamed: %s", err.c_str());
        CHECK(a.frames() == (size_t) n);
        CHECK(a.sample_rate == 16000 && a.channels == 1);
        // s16 quantisation: |round(x * 32767) / 32768 - x| <= (0.5 + |x|) / 32768, here |x| <= 0.9
        CHECK(max_abs_diff(a.pcm, left) <= 1.4f / 32768.0f + 1e-6f);

        const std::vector<uint8_t> zero_size = make_wav(1, 1, 16000, 16, pcm, false, 0u);
        DecodedAudio b;
        CHECK_MSG(parse_wav(zero_size.data(), zero_size.size(), b, err), "data size 0: %s", err.c_str());
        CHECK(b.frames() == (size_t) n);
        CHECK(b.pcm == a.pcm);

        // a truncated file (size field larger than what is there) decodes what is there
        std::vector<uint8_t> truncated = make_wav(1, 1, 16000, 16, pcm, false);
        truncated.resize(truncated.size() - 1000);  // drop 500 frames
        DecodedAudio c;
        CHECK_MSG(parse_wav(truncated.data(), truncated.size(), c, err), "truncated: %s", err.c_str());
        CHECK(c.frames() == (size_t) n - 500);

        // fmt chunk after the data chunk still works
        const std::vector<uint8_t> swapped = make_wav(1, 1, 16000, 16, pcm, false, std::nullopt, false, true);
        DecodedAudio d;
        CHECK_MSG(parse_wav(swapped.data(), swapped.size(), d, err), "fmt after data: %s", err.c_str());
        CHECK(d.frames() == (size_t) n && d.pcm == a.pcm);

        // an odd data size is handled (last partial frame ignored)
        std::vector<uint8_t> odd = pcm;
        odd.push_back(7);
        const std::vector<uint8_t> oddwav = make_wav(1, 1, 16000, 16, odd, false);
        DecodedAudio e;
        CHECK(parse_wav(oddwav.data(), oddwav.size(), e, err));
        CHECK(e.frames() == (size_t) n);
    }

    // failures
    {
        DecodedAudio a;
        const std::vector<uint8_t> pcm(32, 0);
        CHECK(!parse_wav((const uint8_t *) "RIFF\x04\x00\x00\x00WAVE", 12, a, err));
        CHECK(!err.empty());
        CHECK(!parse_wav((const uint8_t *) "not a wav file at all!", 22, a, err));
        CHECK(err.find("RIFF") != std::string::npos);
        CHECK(!parse_wav(nullptr, 0, a, err));

        // fmt only
        std::vector<uint8_t> fmt_only;
        put_tag(fmt_only, "RIFF");
        put_u32(fmt_only, 28);
        put_tag(fmt_only, "WAVE");
        put_tag(fmt_only, "fmt ");
        put_u32(fmt_only, 16);
        put_u16(fmt_only, 1);
        put_u16(fmt_only, 1);
        put_u32(fmt_only, 16000);
        put_u32(fmt_only, 32000);
        put_u16(fmt_only, 2);
        put_u16(fmt_only, 16);
        CHECK(!parse_wav(fmt_only.data(), fmt_only.size(), a, err));
        CHECK(err.find("data") != std::string::npos);

        // data only
        std::vector<uint8_t> data_only;
        put_tag(data_only, "RIFF");
        put_u32(data_only, 4 + 8 + 32);
        put_tag(data_only, "WAVE");
        put_tag(data_only, "data");
        put_u32(data_only, 32);
        data_only.insert(data_only.end(), pcm.begin(), pcm.end());
        CHECK(!parse_wav(data_only.data(), data_only.size(), a, err));
        CHECK(err.find("fmt") != std::string::npos);

        // unsupported format tags / depths
        const std::vector<uint8_t> mp3_in_wav = make_wav(0x55, 1, 16000, 16, pcm, false);
        CHECK(!parse_wav(mp3_in_wav.data(), mp3_in_wav.size(), a, err));
        CHECK(err.find("unsupported") != std::string::npos);
        const std::vector<uint8_t> alaw = make_wav(6, 1, 8000, 8, pcm, false);
        CHECK(!parse_wav(alaw.data(), alaw.size(), a, err));
        const std::vector<uint8_t> bits12 = make_wav(1, 1, 16000, 12, pcm, false);
        CHECK(!parse_wav(bits12.data(), bits12.size(), a, err));
        CHECK(err.find("bit depth") != std::string::npos);
        const std::vector<uint8_t> f16 = make_wav(3, 1, 16000, 16, pcm, false);
        CHECK(!parse_wav(f16.data(), f16.size(), a, err));
        const std::vector<uint8_t> ext_alaw = make_wav(6, 1, 8000, 8, pcm, true);
        CHECK(!parse_wav(ext_alaw.data(), ext_alaw.size(), a, err));
        const std::vector<uint8_t> no_channels = make_wav(1, 0, 16000, 16, pcm, false);
        CHECK(!parse_wav(no_channels.data(), no_channels.size(), a, err));

        // decode_audio reports the reader's message
        try {
            decode_audio(mp3_in_wav.data(), mp3_in_wav.size());
            CHECK(false);
        } catch (const std::invalid_argument & e) {
            CHECK_MSG(std::string(e.what()).rfind("failed to decode audio: ", 0) == 0, "message: %s", e.what());
        }
        // an empty data chunk and nothing after it: valid WAV, but no samples for decode_audio
        const std::vector<uint8_t> empty = make_wav(1, 1, 16000, 16, {}, false);
        CHECK(parse_wav(empty.data(), empty.size(), a, err) && a.frames() == 0);
        CHECK_THROWS(decode_audio(empty.data(), empty.size()));
        CHECK_THROWS(decode_audio(nullptr, 0));
    }
}

static void test_miniaudio(const std::string & samples) {
    std::puts("[miniaudio]");
    // garbage is refused with the documented message
    {
        const std::string junk = "this is definitely not audio data, just a string of letters ....................";
        CHECK_THROWS(decode_audio((const uint8_t *) junk.data(), junk.size()));
        try {
            decode_audio((const uint8_t *) junk.data(), junk.size());
        } catch (const std::invalid_argument & e) {
            CHECK_MSG(std::string(e.what()).rfind("failed to decode audio: ", 0) == 0, "message: %s", e.what());
            std::printf("  junk -> %s\n", e.what());
        }
    }

    const std::vector<uint8_t> jfk = read_file(samples + "/jfk.wav");
    if (jfk.empty()) {
        std::puts("  SKIP mp3/flac (jfk.wav not found)");
        return;
    }
    if (!tool_on_path("ffmpeg")) {
        std::puts("  SKIP mp3/flac (ffmpeg not on PATH)");
        return;
    }
    DecodedAudio ref;
    std::string  err;
    if (!parse_wav(jfk.data(), jfk.size(), ref, err)) {
        CHECK_MSG(false, "jfk.wav: %s", err.c_str());
        return;
    }

    const char * tmpdir_env = std::getenv("TMPDIR");
    const std::string tmpdir = (tmpdir_env && *tmpdir_env) ? tmpdir_env : "/tmp";
    const std::string base = tmpdir + "/ixsrv_media_selftest_" + std::to_string((long) getpid());
    const std::string wav_path = samples + "/jfk.wav";

    // MP3 (lossy): rate and channels as written, duration within 0.1 s
    {
        const std::string mp3_path = base + ".mp3";
        const std::string cmd = "ffmpeg -y -loglevel error -i " + shell_quote(wav_path) +
                                " -codec:a libmp3lame -q:a 4 " + shell_quote(mp3_path) + " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        const std::vector<uint8_t> mp3 = rc == 0 ? read_file(mp3_path) : std::vector<uint8_t>();
        if (mp3.empty()) {
            std::puts("  SKIP mp3 (ffmpeg could not encode; libmp3lame missing?)");
        } else {
            int expected_rate = 16000;
            if (tool_on_path("ffprobe")) {
                const std::string probed = run_capture("ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of default=nw=1:nk=1 " +
                                                       shell_quote(mp3_path) + " 2>/dev/null");
                if (!probed.empty()) {
                    expected_rate = std::atoi(probed.c_str());
                }
            }
            CHECK(sniff_media_kind(mp3.data(), mp3.size()) == "audio");
            try {
                const DecodedAudio a = decode_audio(mp3.data(), mp3.size());
                std::printf("  jfk.mp3: %zu bytes -> %d Hz, %d ch, %zu frames (%.3f s)\n", mp3.size(), a.sample_rate,
                            a.channels, a.frames(), a.seconds());
                CHECK_MSG(a.sample_rate == expected_rate, "rate %d, expected %d", a.sample_rate, expected_rate);
                CHECK(a.channels == 1);
                CHECK_MSG(std::fabs(a.seconds() - ref.seconds()) < 0.1, "mp3 %.3f s vs wav %.3f s", a.seconds(), ref.seconds());
                CHECK(all_in_unit_range(a.pcm));
                CHECK(max_abs(a.pcm) > 0.05f);
                // the ID3 tag ffmpeg writes does not confuse the decoder, and the content resembles the original
                // (same loudness, give or take)
                float e_ref = 0.0f, e_mp3 = 0.0f;
                for (float v : ref.pcm) {
                    e_ref += v * v;
                }
                for (float v : a.pcm) {
                    e_mp3 += v * v;
                }
                CHECK_MSG(e_mp3 > 0.5f * e_ref && e_mp3 < 2.0f * e_ref, "energy mp3 %g vs wav %g", e_mp3, e_ref);
            } catch (const std::exception & e) {
                CHECK_MSG(false, "mp3 decode threw: %s", e.what());
            }
        }
        std::remove(mp3_path.c_str());
    }

    // FLAC (lossless): bit-exact with the WAV
    {
        const std::string flac_path = base + ".flac";
        const std::string cmd = "ffmpeg -y -loglevel error -i " + shell_quote(wav_path) + " -codec:a flac " +
                                shell_quote(flac_path) + " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        const std::vector<uint8_t> flac = rc == 0 ? read_file(flac_path) : std::vector<uint8_t>();
        if (flac.empty()) {
            std::puts("  SKIP flac (ffmpeg could not encode)");
        } else {
            CHECK(sniff_media_kind(flac.data(), flac.size()) == "audio");
            try {
                const DecodedAudio a = decode_audio(flac.data(), flac.size());
                std::printf("  jfk.flac: %zu bytes -> %d Hz, %d ch, %zu frames (%.3f s)\n", flac.size(), a.sample_rate,
                            a.channels, a.frames(), a.seconds());
                CHECK(a.sample_rate == 16000);
                CHECK(a.channels == 1);
                CHECK_MSG(a.frames() == ref.frames(), "flac %zu frames vs wav %zu", a.frames(), ref.frames());
                const float d = max_abs_diff(a.pcm, ref.pcm);
                CHECK_MSG(d < 1e-4f, "flac vs wav max abs diff %g", d);
            } catch (const std::exception & e) {
                CHECK_MSG(false, "flac decode threw: %s", e.what());
            }
        }
        std::remove(flac_path.c_str());
    }
}

static void test_wav_writer() {
    std::puts("[wav writer]");
    const int rate = 16000;
    const int n    = rate;  // 1 s
    std::vector<float> sine(n);
    for (int i = 0; i < n; ++i) {
        sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / rate);
    }
    const std::string wav = encode_wav_s16(sine.data(), sine.size(), rate);
    CHECK(wav.size() == 44 + (size_t) n * 2);
    CHECK(wav.compare(0, 4, "RIFF") == 0 && wav.compare(8, 4, "WAVE") == 0);
    CHECK(rd_u32(wav, 4) == 36 + (uint32_t) n * 2);
    CHECK(rd_u32(wav, 40) == (uint32_t) n * 2);
    CHECK(sniff_media_kind((const uint8_t *) wav.data(), wav.size()) == "audio");

    DecodedAudio a;
    std::string  err;
    CHECK_MSG(parse_wav((const uint8_t *) wav.data(), wav.size(), a, err), "%s", err.c_str());
    CHECK(a.sample_rate == rate && a.channels == 1);
    CHECK(a.frames() == (size_t) n);
    const float d = max_abs_diff(a.pcm, sine);
    CHECK_MSG(d <= 1.0f / 32767.0f + 1e-7f, "round trip max abs diff %g", d);
    // and through decode_audio
    CHECK(decode_audio((const uint8_t *) wav.data(), wav.size()).pcm == a.pcm);

    // streamed header: both size fields 0xFFFFFFFF, bits per sample at offset 34 == 16
    {
        const std::string h = wav_header(16000, 1, 16, 0xFFFFFFFFu);
        CHECK(h.size() == 44);
        CHECK(h.compare(0, 4, "RIFF") == 0);
        CHECK(rd_u32(h, 4) == 0xFFFFFFFFu);
        CHECK(h.compare(8, 4, "WAVE") == 0);
        CHECK(h.compare(12, 4, "fmt ") == 0);
        CHECK(rd_u32(h, 16) == 16);
        CHECK(rd_u16(h, 20) == 1);       // PCM
        CHECK(rd_u16(h, 22) == 1);       // channels
        CHECK(rd_u32(h, 24) == 16000);   // sample rate
        CHECK(rd_u32(h, 28) == 32000);   // byte rate
        CHECK(rd_u16(h, 32) == 2);       // block align
        CHECK(rd_u16(h, 34) == 16);      // bits per sample
        CHECK(h.compare(36, 4, "data") == 0);
        CHECK(rd_u32(h, 40) == 0xFFFFFFFFu);
    }
    {
        const std::string h = wav_header(24000, 2, 16, 1000);
        CHECK(rd_u32(h, 4) == 1036);
        CHECK(rd_u16(h, 22) == 2);
        CHECK(rd_u32(h, 24) == 24000);
        CHECK(rd_u32(h, 28) == 96000);
        CHECK(rd_u16(h, 32) == 4);
        CHECK(rd_u16(h, 34) == 16);
        CHECK(rd_u32(h, 40) == 1000);
        const std::string h32 = wav_header(44100, 1, 32, 400);
        CHECK(rd_u32(h32, 28) == 176400 && rd_u16(h32, 32) == 4 && rd_u16(h32, 34) == 32);
    }

    // sample conversion
    {
        const float in[] = {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f, -0.5f, 1.0f / 32767.0f, std::nanf("")};
        const std::string s16 = pcm_f32_to_s16(in, 9);
        CHECK(s16.size() == 18);
        const int16_t expected[] = {0, 32767, -32767, 32767, -32767, 16384, -16384, 1, 0};
        for (int i = 0; i < 9; ++i) {
            const int16_t got = (int16_t) rd_u16(s16, (size_t) i * 2);
            CHECK_MSG(got == expected[i], "sample %d: got %d, expected %d", i, got, expected[i]);
        }
        const float one = 1.0f;
        const std::string f = pcm_f32_bytes(&one, 1);
        CHECK(f == std::string("\x00\x00\x80\x3F", 4));
        const float pair[] = {-2.0f, 1.5f};
        CHECK(pcm_f32_bytes(pair, 2) == std::string("\x00\x00\x00\xC0\x00\x00\xC0\x3F", 8));
        CHECK(pcm_f32_to_s16(nullptr, 0).empty());
        CHECK(pcm_f32_bytes(nullptr, 0).empty());
        CHECK(encode_wav_s16(nullptr, 0, 16000).size() == 44);
    }
}

static void test_split() {
    std::puts("[split_audio]");
    const int rate = 16000;
    const size_t n = (size_t) 95 * rate;
    std::vector<float> mono(n);
    for (size_t i = 0; i < n; ++i) {
        mono[i] = (float) (i % 7) / 7.0f;
    }
    const auto clips = split_audio(mono, rate, 29.0);
    CHECK(clips.size() == 4);
    if (clips.size() == 4) {
        CHECK(clips[0].size() == (size_t) 29 * rate);
        CHECK(clips[1].size() == (size_t) 29 * rate);
        CHECK(clips[2].size() == (size_t) 29 * rate);
        CHECK(clips[3].size() == (size_t) 8 * rate);
        // contiguous, nothing lost
        CHECK(clips[1][0] == mono[(size_t) 29 * rate]);
        CHECK(clips[3].back() == mono.back());
        size_t total = 0;
        for (const auto & c : clips) {
            total += c.size();
        }
        CHECK(total == n);
    }
    CHECK(split_audio({}, rate, 29.0).empty());
    CHECK(split_audio(std::vector<float>((size_t) 29 * rate, 0.0f), rate, 29.0).size() == 1);
    {
        const auto two = split_audio(std::vector<float>((size_t) 29 * rate + 1, 0.0f), rate, 29.0);
        CHECK(two.size() == 2 && two[1].size() == 1);
    }
    CHECK(split_audio(mono, rate, 1000.0).size() == 1);
    // fractional lengths: floor(0.5 * 16000) = 8000
    CHECK(split_audio(std::vector<float>(16001, 0.0f), rate, 0.5).size() == 3);
    CHECK_THROWS(split_audio(mono, rate, 0.0));
    CHECK_THROWS(split_audio(mono, 0, 29.0));
}

static void test_misc() {
    std::puts("[misc]");
    DecodedAudio st;
    st.channels    = 2;
    st.sample_rate = 8000;
    st.pcm         = {1.0f, 0.0f, -1.0f, 1.0f, 0.5f, 0.5f};
    const std::vector<float> mono = audio_to_mono(st);
    CHECK(mono.size() == 3);
    CHECK(mono.size() == 3 && std::fabs(mono[0] - 0.5f) < 1e-6f && std::fabs(mono[1]) < 1e-6f && std::fabs(mono[2] - 0.5f) < 1e-6f);
    CHECK(st.frames() == 3);
    CHECK(std::fabs(st.seconds() - 3.0 / 8000.0) < 1e-9);

    DecodedAudio quad;
    quad.channels = 4;
    quad.pcm      = {1.0f, 1.0f, 1.0f, -1.0f};
    CHECK(audio_to_mono(quad) == (std::vector<float>{0.5f}));

    DecodedAudio none;
    CHECK(none.frames() == 0 && none.seconds() == 0.0);

    DecodedImage tiny;
    tiny.width  = 2;
    tiny.height = 1;
    tiny.rgb    = {1, 2, 3, 4, 5, 6};
    CHECK(encode_ppm(tiny) == std::string("P6\n2 1\n255\n\x01\x02\x03\x04\x05\x06", 17));
}

int main(int argc, char ** argv) {
    const std::string samples = argc > 1 ? argv[1] : "/home/yeonseok/workspace/exp/ixian/IxianHexagon/samples";
    std::printf("media_selftest: samples dir %s\n", samples.c_str());

    try {
        test_base64();
        test_data_url();
        test_sniff();
        test_images(samples);
        test_fit();
        test_wav_samples(samples);
        test_wav_synth();
        test_miniaudio(samples);
        test_wav_writer();
        test_split();
        test_misc();
    } catch (const std::exception & e) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++g_failures;
    }

    std::printf("%s: %d checks, %d failed\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
