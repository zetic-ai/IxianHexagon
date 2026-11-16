// Ixian Hexagon server - media decoding helpers (see server-media.h).
//
// This translation unit also holds the implementations of the two vendored single-header libraries
// (stb_image for pictures, miniaudio for MP3 / FLAC / WAV decoding); nothing else in the program may define them.

#include "server-media.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// third-party implementations: keep their warnings out of our build regardless of the compiler flags
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC    // internal linkage: no clash with any other stb_image user linked into the process
#define STBI_NO_STDIO       // we only ever decode from memory
#define STBI_NO_HDR
#define STBI_NO_PIC
// enabled: PNG, JPEG, BMP, GIF, PNM, PSD, TGA
#include "stb_image.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING      // the WAV writer lives here (wav_header / encode_wav_s16)
// kept: decoding of WAV, FLAC and MP3
#include "miniaudio.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace ixsrv {

namespace {

//
// small helpers
//

// a decoded picture must not exceed this many pixels (a tiny PNG can claim a gigantic canvas); 64 Mpx = 192 MB of RGB
constexpr size_t k_max_image_pixels = (size_t) 64 * 1024 * 1024;

inline uint16_t rd_u16le(const uint8_t * p) { return (uint16_t) (p[0] | (p[1] << 8)); }
inline uint32_t rd_u32le(const uint8_t * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

inline void put_u16le(std::string & s, uint16_t v) {
    s.push_back((char) (v & 0xFF));
    s.push_back((char) ((v >> 8) & 0xFF));
}
inline void put_u32le(std::string & s, uint32_t v) {
    s.push_back((char) (v & 0xFF));
    s.push_back((char) ((v >> 8) & 0xFF));
    s.push_back((char) ((v >> 16) & 0xFF));
    s.push_back((char) ((v >> 24) & 0xFF));
}

inline bool starts_with_bytes(const uint8_t * data, size_t size, const char * magic, size_t n, size_t at = 0) {
    return size >= at + n && std::memcmp(data + at, magic, n) == 0;
}

inline bool is_riff_wave(const uint8_t * data, size_t size) {
    return starts_with_bytes(data, size, "RIFF", 4) && starts_with_bytes(data, size, "WAVE", 4, 8);
}

std::string to_lower_ascii(std::string s) {
    for (char & c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
    }
    return s;
}

std::string trim_ascii(const std::string & s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string hex4(unsigned v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X", v & 0xFFFFu);
    return buf;
}

//
// base64 tables
//

constexpr char k_b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

enum : int8_t {
    B64_INVALID = -1,
    B64_SKIP    = -2,  // whitespace
    B64_PAD     = -3,  // '='
};

struct B64Table {
    int8_t v[256];
    constexpr B64Table() : v() {
        for (int i = 0; i < 256; ++i) {
            v[i] = B64_INVALID;
        }
        for (int i = 0; i < 64; ++i) {
            v[(uint8_t) k_b64_alphabet[i]] = (int8_t) i;
        }
        v[(uint8_t) '-'] = 62;  // URL-safe alphabet
        v[(uint8_t) '_'] = 63;
        v[(uint8_t) ' ']  = B64_SKIP;
        v[(uint8_t) '\t'] = B64_SKIP;
        v[(uint8_t) '\r'] = B64_SKIP;
        v[(uint8_t) '\n'] = B64_SKIP;
        v[(uint8_t) '\f'] = B64_SKIP;
        v[(uint8_t) '\v'] = B64_SKIP;
        v[(uint8_t) '=']  = B64_PAD;
    }
};

constexpr B64Table k_b64_table{};

//
// image resampling
//

// the source samples that feed one destination sample along one axis: a contiguous run starting at `start`
struct AxisTaps {
    int start = 0;
    std::vector<float> weights;
};

// Shrinking: box (area) filter, each destination sample averages the source interval it covers with fractional
// coverage weights (exact for integer ratios). Enlarging: bilinear, centre-aligned. Same size: identity.
std::vector<AxisTaps> make_axis_taps(int src_n, int dst_n) {
    std::vector<AxisTaps> taps((size_t) dst_n);
    if (src_n == dst_n) {
        for (int i = 0; i < dst_n; ++i) {
            taps[i].start = i;
            taps[i].weights.assign(1, 1.0f);
        }
        return taps;
    }
    const double scale = (double) src_n / (double) dst_n;
    if (dst_n < src_n) {
        for (int i = 0; i < dst_n; ++i) {
            const double lo = i * scale;
            const double hi = std::min((i + 1) * scale, (double) src_n);
            int s0 = (int) std::floor(lo);
            int s1 = (int) std::ceil(hi) - 1;
            s0 = std::max(0, std::min(s0, src_n - 1));
            s1 = std::max(s0, std::min(s1, src_n - 1));
            AxisTaps & t = taps[i];
            t.start = s0;
            t.weights.resize((size_t) (s1 - s0 + 1));
            double sum = 0.0;
            for (int s = s0; s <= s1; ++s) {
                const double overlap = std::min(hi, (double) s + 1.0) - std::max(lo, (double) s);
                const double w = overlap > 0.0 ? overlap : 0.0;
                t.weights[(size_t) (s - s0)] = (float) w;
                sum += w;
            }
            if (sum <= 0.0) {  // cannot happen, but never divide by zero
                t.weights.assign(1, 1.0f);
                sum = 1.0;
            }
            for (float & w : t.weights) {
                w = (float) (w / sum);
            }
        }
    } else {
        for (int i = 0; i < dst_n; ++i) {
            double sx = (i + 0.5) * scale - 0.5;
            sx = std::max(0.0, std::min(sx, (double) (src_n - 1)));
            const int x0 = (int) std::floor(sx);
            const int x1 = std::min(x0 + 1, src_n - 1);
            const float t = (float) (sx - x0);
            AxisTaps & tp = taps[i];
            tp.start = x0;
            if (x1 == x0 || t <= 0.0f) {
                tp.weights.assign(1, 1.0f);
            } else {
                tp.weights = {1.0f - t, t};
            }
        }
    }
    return taps;
}

inline uint8_t to_u8(float v) {
    v = std::max(0.0f, std::min(255.0f, v));
    return (uint8_t) (int) (v + 0.5f);
}

// Separable two-pass resampler for packed RGB: horizontal pass to float rows, then vertical pass to bytes.
std::vector<uint8_t> resample_rgb(const uint8_t * src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> out((size_t) dw * dh * 3);
    if (sw == dw && sh == dh) {
        std::memcpy(out.data(), src, out.size());
        return out;
    }

    // horizontal pass: sh rows of dw pixels
    std::vector<float> tmp((size_t) sh * dw * 3);
    if (sw == dw) {
        for (size_t i = 0; i < tmp.size(); ++i) {
            tmp[i] = (float) src[i];
        }
    } else {
        const std::vector<AxisTaps> xt = make_axis_taps(sw, dw);
        for (int y = 0; y < sh; ++y) {
            const uint8_t * srow = src + (size_t) y * sw * 3;
            float * drow = tmp.data() + (size_t) y * dw * 3;
            for (int x = 0; x < dw; ++x) {
                const AxisTaps & t = xt[x];
                float r = 0.0f, g = 0.0f, b = 0.0f;
                const uint8_t * p = srow + (size_t) t.start * 3;
                for (float w : t.weights) {
                    r += w * p[0];
                    g += w * p[1];
                    b += w * p[2];
                    p += 3;
                }
                drow[x * 3 + 0] = r;
                drow[x * 3 + 1] = g;
                drow[x * 3 + 2] = b;
            }
        }
    }

    // vertical pass: dh rows
    if (sh == dh) {
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = to_u8(tmp[i]);
        }
    } else {
        const std::vector<AxisTaps> yt = make_axis_taps(sh, dh);
        const size_t row_floats = (size_t) dw * 3;
        std::vector<float> acc(row_floats);
        for (int y = 0; y < dh; ++y) {
            const AxisTaps & t = yt[y];
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const float w = t.weights[k];
                const float * srow = tmp.data() + (size_t) (t.start + (int) k) * row_floats;
                for (size_t i = 0; i < row_floats; ++i) {
                    acc[i] += w * srow[i];
                }
            }
            uint8_t * drow = out.data() + (size_t) y * row_floats;
            for (size_t i = 0; i < row_floats; ++i) {
                drow[i] = to_u8(acc[i]);
            }
        }
    }
    return out;
}

// copies the w x h window at (x0, y0) out of a packed RGB image
std::vector<uint8_t> crop_rgb(const uint8_t * src, int sw, int x0, int y0, int w, int h) {
    std::vector<uint8_t> out((size_t) w * h * 3);
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.data() + (size_t) y * w * 3, src + ((size_t) (y0 + y) * sw + x0) * 3, (size_t) w * 3);
    }
    return out;
}

void check_image(const DecodedImage & img) {
    if (img.width <= 0 || img.height <= 0 || img.rgb.size() != (size_t) img.width * (size_t) img.height * 3) {
        throw std::invalid_argument("invalid image buffer");
    }
}

//
// WAV sample conversion
//

inline float clamp_unit(float v) {
    if (!(v == v)) {  // NaN
        return 0.0f;
    }
    return std::max(-1.0f, std::min(1.0f, v));
}

//
// miniaudio
//

struct MaDecoderGuard {
    ma_decoder * dec;
    ~MaDecoderGuard() { ma_decoder_uninit(dec); }
};

DecodedAudio decode_audio_miniaudio(const uint8_t * data, size_t size) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);  // float, native channels and rate
    ma_decoder dec;
    ma_result r = ma_decoder_init_memory(data, size, &cfg, &dec);
    if (r != MA_SUCCESS) {
        std::string reason;
        if (r == MA_NO_BACKEND || r == MA_INVALID_FILE || r == MA_FORMAT_NOT_SUPPORTED) {
            reason = "unknown or unsupported audio format (expected WAV, MP3 or FLAC)";
        } else {
            reason = ma_result_description(r);
        }
        throw std::invalid_argument("failed to decode audio: " + reason);
    }
    MaDecoderGuard guard{&dec};

    ma_format fmt = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 rate = 0;
    r = ma_decoder_get_data_format(&dec, &fmt, &channels, &rate, nullptr, 0);
    if (r != MA_SUCCESS || fmt != ma_format_f32 || channels == 0 || rate == 0) {
        throw std::invalid_argument("failed to decode audio: cannot determine the sample format");
    }

    DecodedAudio out;
    out.channels    = (int) channels;
    out.sample_rate = (int) rate;

    const ma_uint64 chunk_frames = 4096;
    std::vector<float> buf((size_t) chunk_frames * channels);
    for (;;) {
        ma_uint64 got = 0;
        r = ma_decoder_read_pcm_frames(&dec, buf.data(), chunk_frames, &got);
        if (got > 0) {
            out.pcm.insert(out.pcm.end(), buf.begin(), buf.begin() + (ptrdiff_t) (got * channels));
        }
        if (r == MA_AT_END || got == 0) {
            break;
        }
        if (r != MA_SUCCESS) {
            if (out.pcm.empty()) {
                throw std::invalid_argument(std::string("failed to decode audio: ") + ma_result_description(r));
            }
            break;  // keep what was decoded before the error
        }
    }
    return out;
}

}  // namespace

//
// base64 and data URLs
//

std::string base64_encode(const uint8_t * data, size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8) | (uint32_t) data[i + 2];
        out.push_back(k_b64_alphabet[(v >> 18) & 63]);
        out.push_back(k_b64_alphabet[(v >> 12) & 63]);
        out.push_back(k_b64_alphabet[(v >> 6) & 63]);
        out.push_back(k_b64_alphabet[v & 63]);
    }
    if (i + 1 == n) {
        const uint32_t v = (uint32_t) data[i] << 16;
        out.push_back(k_b64_alphabet[(v >> 18) & 63]);
        out.push_back(k_b64_alphabet[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == n) {
        const uint32_t v = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8);
        out.push_back(k_b64_alphabet[(v >> 18) & 63]);
        out.push_back(k_b64_alphabet[(v >> 12) & 63]);
        out.push_back(k_b64_alphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

raw_buffer base64_decode(const std::string & s) {
    raw_buffer out;
    out.reserve((s.size() / 4) * 3 + 3);

    uint32_t acc  = 0;
    int      quad = 0;    // characters in the current group of four
    bool     pad  = false;

    for (unsigned char c : s) {
        const int8_t v = k_b64_table.v[c];
        if (v == B64_SKIP) {
            continue;
        }
        if (v == B64_PAD) {
            // padding closes the group: it needs at least two data characters, and only '=' / whitespace may follow
            if (quad < 2) {
                throw std::invalid_argument("invalid base64");
            }
            pad = true;
            continue;
        }
        if (v < 0 || pad) {
            throw std::invalid_argument("invalid base64");
        }
        acc = (acc << 6) | (uint32_t) v;
        if (++quad == 4) {
            out.push_back((uint8_t) ((acc >> 16) & 0xFF));
            out.push_back((uint8_t) ((acc >> 8) & 0xFF));
            out.push_back((uint8_t) (acc & 0xFF));
            acc  = 0;
            quad = 0;
        }
    }
    // a final group of two or three characters (missing or present padding) yields one or two bytes
    if (quad == 1) {
        throw std::invalid_argument("invalid base64");
    }
    if (quad == 2) {
        out.push_back((uint8_t) ((acc >> 4) & 0xFF));
    } else if (quad == 3) {
        out.push_back((uint8_t) ((acc >> 10) & 0xFF));
        out.push_back((uint8_t) ((acc >> 2) & 0xFF));
    }
    if (out.empty()) {
        throw std::invalid_argument("invalid base64");
    }
    return out;
}

bool parse_data_url(const std::string & url, std::string & mime, raw_buffer & out) {
    if (url.size() < 5 || to_lower_ascii(url.substr(0, 5)) != "data:") {
        return false;
    }
    const size_t comma = url.find(',');
    if (comma == std::string::npos) {
        throw std::invalid_argument("Invalid uri-encoded base64 value");
    }
    const std::string meta = url.substr(5, comma - 5);  // "<mime>[;param][;base64]"

    // the media type ends at the first ';'; the remaining ';'-separated parameters must include "base64"
    const size_t semi = meta.find(';');
    std::string type = to_lower_ascii(trim_ascii(semi == std::string::npos ? meta : meta.substr(0, semi)));
    bool is_base64 = false;
    size_t pos = semi;
    while (pos != std::string::npos) {
        const size_t next = meta.find(';', pos + 1);
        const std::string param = to_lower_ascii(trim_ascii(meta.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1)));
        if (param == "base64") {
            is_base64 = true;
        }
        pos = next;
    }
    if (!is_base64) {
        throw std::invalid_argument("uri must be base64 encoded");
    }
    out  = base64_decode(url.substr(comma + 1));  // throws "invalid base64" on a bad payload
    mime = std::move(type);
    return true;
}

std::string sniff_media_kind(const uint8_t * data, size_t size) {
    if (data == nullptr || size < 4) {
        return "";
    }
    // images
    if (starts_with_bytes(data, size, "\x89PNG", 4)) {
        return "image";
    }
    if (starts_with_bytes(data, size, "\xFF\xD8\xFF", 3)) {
        return "image";
    }
    if (starts_with_bytes(data, size, "GIF8", 4)) {
        return "image";
    }
    if (starts_with_bytes(data, size, "BM", 2)) {
        return "image";
    }
    if (data[0] == 'P' && (data[1] == '6' || data[1] == '5' || data[1] == '3' || data[1] == '2') &&
        (data[2] == ' ' || data[2] == '\n' || data[2] == '\r' || data[2] == '\t')) {
        return "image";
    }
    if (starts_with_bytes(data, size, "RIFF", 4) && size >= 12) {
        if (std::memcmp(data + 8, "WEBP", 4) == 0) {
            return "image";
        }
        if (std::memcmp(data + 8, "WAVE", 4) == 0) {
            return "audio";
        }
        if (std::memcmp(data + 8, "AVI ", 4) == 0) {
            return "video";
        }
    }
    // audio
    if (starts_with_bytes(data, size, "ID3", 3)) {
        return "audio";
    }
    if (starts_with_bytes(data, size, "fLaC", 4)) {
        return "audio";
    }
    if (starts_with_bytes(data, size, "OggS", 4)) {
        return "audio";
    }
    if (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        // MPEG audio frame header: version != reserved, layer != reserved, bitrate index != 15, sample rate index != 3
        const int version  = (data[1] >> 3) & 3;
        const int layer    = (data[1] >> 1) & 3;
        const int bitrate  = (data[2] >> 4) & 15;
        const int srate    = (data[2] >> 2) & 3;
        if (version != 1 && layer != 0 && bitrate != 15 && srate != 3) {
            return "audio";
        }
    }
    // video
    if (size >= 8 && std::memcmp(data + 4, "ftyp", 4) == 0) {
        return "video";
    }
    if (starts_with_bytes(data, size, "\x1A\x45\xDF\xA3", 4)) {
        return "video";
    }
    return "";
}

//
// images
//

DecodedImage decode_image(const uint8_t * data, size_t size) {
    if (data == nullptr || size == 0) {
        throw std::invalid_argument("failed to decode image: empty data");
    }
    if (size > (size_t) INT32_MAX) {
        throw std::invalid_argument("failed to decode image: data too large");
    }
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, (int) size, &w, &h, &comp)) {
        const char * reason = stbi_failure_reason();
        throw std::invalid_argument(std::string("failed to decode image: ") + (reason ? reason : "unknown image format"));
    }
    if (w <= 0 || h <= 0 || (size_t) w * (size_t) h > k_max_image_pixels) {
        throw std::invalid_argument("failed to decode image: image too large (" + std::to_string(w) + "x" + std::to_string(h) +
                                    ", at most " + std::to_string(k_max_image_pixels) + " pixels)");
    }
    unsigned char * pixels = stbi_load_from_memory(data, (int) size, &w, &h, &comp, 3);
    if (pixels == nullptr) {
        const char * reason = stbi_failure_reason();
        throw std::invalid_argument(std::string("failed to decode image: ") + (reason ? reason : "corrupt image"));
    }
    DecodedImage img;
    img.width  = w;
    img.height = h;
    img.rgb.assign(pixels, pixels + (size_t) w * (size_t) h * 3);
    stbi_image_free(pixels);
    return img;
}

const char * image_fit_name(ImageFit fit) {
    switch (fit) {
        case ImageFit::Stretch: return "stretch";
        case ImageFit::Crop:    return "crop";
        case ImageFit::Letterbox:
        default:                return "letterbox";
    }
}

ImageFit image_fit_from_name(const std::string & name) {
    const std::string n = to_lower_ascii(trim_ascii(name));
    if (n == "letterbox") {
        return ImageFit::Letterbox;
    }
    if (n == "stretch") {
        return ImageFit::Stretch;
    }
    if (n == "crop") {
        return ImageFit::Crop;
    }
    throw std::invalid_argument("unknown image fit '" + name.substr(0, 32) + "' (expected letterbox, stretch or crop)");
}

DecodedImage fit_image(const DecodedImage & src, int width, int height, ImageFit fit) {
    check_image(src);
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("invalid target image size");
    }
    DecodedImage out;
    out.width  = width;
    out.height = height;

    const int sw = src.width;
    const int sh = src.height;
    if (sw == width && sh == height) {
        out.rgb = src.rgb;
        return out;
    }

    switch (fit) {
        case ImageFit::Stretch: {
            out.rgb = resample_rgb(src.rgb.data(), sw, sh, width, height);
            break;
        }
        case ImageFit::Crop: {
            // scale so the target is covered, i.e. take the largest centred source window with the target's aspect
            // ratio, then resample that window to the target size
            const double scale = std::max((double) width / sw, (double) height / sh);
            int cw = (int) std::lround(width / scale);
            int ch = (int) std::lround(height / scale);
            cw = std::max(1, std::min(cw, sw));
            ch = std::max(1, std::min(ch, sh));
            const int x0 = (sw - cw) / 2;
            const int y0 = (sh - ch) / 2;
            if (cw == sw && ch == sh) {
                out.rgb = resample_rgb(src.rgb.data(), sw, sh, width, height);
            } else {
                const std::vector<uint8_t> window = crop_rgb(src.rgb.data(), sw, x0, y0, cw, ch);
                out.rgb = resample_rgb(window.data(), cw, ch, width, height);
            }
            break;
        }
        case ImageFit::Letterbox:
        default: {
            const double scale = std::min((double) width / sw, (double) height / sh);
            int nw = (int) std::lround(sw * scale);
            int nh = (int) std::lround(sh * scale);
            nw = std::max(1, std::min(nw, width));
            nh = std::max(1, std::min(nh, height));
            const std::vector<uint8_t> inner = resample_rgb(src.rgb.data(), sw, sh, nw, nh);
            out.rgb.assign((size_t) width * height * 3, 0);  // black bars
            const int x0 = (width - nw) / 2;
            const int y0 = (height - nh) / 2;
            for (int y = 0; y < nh; ++y) {
                std::memcpy(out.rgb.data() + ((size_t) (y0 + y) * width + x0) * 3, inner.data() + (size_t) y * nw * 3, (size_t) nw * 3);
            }
            break;
        }
    }
    return out;
}

std::string encode_ppm(const DecodedImage & img) {
    check_image(img);
    std::string out = "P6\n" + std::to_string(img.width) + " " + std::to_string(img.height) + "\n255\n";
    out.append((const char *) img.rgb.data(), img.rgb.size());
    return out;
}

//
// audio
//

bool parse_wav(const uint8_t * data, size_t size, DecodedAudio & out, std::string & err) {
    if (data == nullptr || !is_riff_wave(data, size)) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    bool     have_fmt   = false;
    bool     have_data  = false;
    uint16_t format_tag = 0;
    uint16_t channels   = 0;
    uint16_t bits       = 0;
    uint32_t rate       = 0;
    size_t   data_pos   = 0;
    size_t   data_size  = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t * id    = data + pos;
        const uint32_t  csize = rd_u32le(data + pos + 4);
        const size_t    body  = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (csize < 16 || body + 16 > size) {
                err = "truncated fmt chunk";
                return false;
            }
            format_tag = rd_u16le(data + body);
            channels   = rd_u16le(data + body + 2);
            rate       = rd_u32le(data + body + 4);
            bits       = rd_u16le(data + body + 14);
            if (format_tag == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes of the SubFormat GUID
                if (csize < 40 || body + 40 > size) {
                    err = "truncated WAVE_FORMAT_EXTENSIBLE fmt chunk";
                    return false;
                }
                format_tag = rd_u16le(data + body + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const bool open_ended = (csize == 0 || csize == 0xFFFFFFFFu);
            data_pos  = body;
            data_size = (open_ended || (uint64_t) body + csize > size) ? size - body : (size_t) csize;  // streamed / truncated: to the end
            have_data = true;
            if (open_ended || have_fmt) {
                break;  // an open-ended chunk cannot be skipped; otherwise keep walking only if fmt is still missing
            }
        }
        // other chunks (LIST, fact, ...) are skipped; chunk bodies are padded to an even size
        const uint64_t next = (uint64_t) body + csize + (csize & 1u);
        if (next > size) {
            break;
        }
        pos = (size_t) next;
    }

    if (!have_fmt) {
        err = "missing fmt chunk";
        return false;
    }
    if (!have_data) {
        err = "missing data chunk";
        return false;
    }
    if (channels == 0 || rate == 0) {
        err = "invalid fmt chunk (zero channels or sample rate)";
        return false;
    }

    enum class Kind { U8, S16, S24, S32, F32, F64 } kind;
    if (format_tag == 1) {
        switch (bits) {
            case 8:  kind = Kind::U8;  break;
            case 16: kind = Kind::S16; break;
            case 24: kind = Kind::S24; break;
            case 32: kind = Kind::S32; break;
            default:
                err = "unsupported WAV bit depth " + std::to_string(bits) + " for PCM (expected 8, 16, 24 or 32)";
                return false;
        }
    } else if (format_tag == 3) {
        switch (bits) {
            case 32: kind = Kind::F32; break;
            case 64: kind = Kind::F64; break;
            default:
                err = "unsupported WAV bit depth " + std::to_string(bits) + " for IEEE float (expected 32 or 64)";
                return false;
        }
    } else {
        err = "unsupported WAV format tag " + hex4(format_tag) + " (expected PCM or IEEE float)";
        return false;
    }

    const size_t bytes_per_sample = bits / 8;
    const size_t frame_bytes      = bytes_per_sample * channels;
    const size_t frames           = data_size / frame_bytes;
    const size_t samples          = frames * channels;

    out.sample_rate = (int) rate;
    out.channels    = channels;
    out.pcm.resize(samples);

    const uint8_t * p = data + data_pos;
    switch (kind) {
        case Kind::U8:
            for (size_t i = 0; i < samples; ++i) {
                out.pcm[i] = ((int) p[i] - 128) / 128.0f;
            }
            break;
        case Kind::S16:
            for (size_t i = 0; i < samples; ++i, p += 2) {
                out.pcm[i] = (int16_t) rd_u16le(p) / 32768.0f;
            }
            break;
        case Kind::S24:
            for (size_t i = 0; i < samples; ++i, p += 3) {
                int32_t v = (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16));
                if (v & 0x800000) {
                    v -= 0x1000000;  // sign-extend 24 -> 32 bits
                }
                out.pcm[i] = (float) (v / 8388608.0);
            }
            break;
        case Kind::S32:
            for (size_t i = 0; i < samples; ++i, p += 4) {
                out.pcm[i] = (float) ((int32_t) rd_u32le(p) / 2147483648.0);
            }
            break;
        case Kind::F32:
            for (size_t i = 0; i < samples; ++i, p += 4) {
                const uint32_t u = rd_u32le(p);
                float f;
                std::memcpy(&f, &u, 4);
                out.pcm[i] = clamp_unit(f);
            }
            break;
        case Kind::F64:
            for (size_t i = 0; i < samples; ++i, p += 8) {
                const uint64_t u = (uint64_t) rd_u32le(p) | ((uint64_t) rd_u32le(p + 4) << 32);
                double d;
                std::memcpy(&d, &u, 8);
                out.pcm[i] = clamp_unit((float) d);
            }
            break;
    }
    err.clear();
    return true;
}

DecodedAudio decode_audio(const uint8_t * data, size_t size) {
    if (data == nullptr || size == 0) {
        throw std::invalid_argument("failed to decode audio: empty data");
    }
    DecodedAudio out;
    if (is_riff_wave(data, size)) {
        std::string err;
        if (!parse_wav(data, size, out, err)) {
            throw std::invalid_argument("failed to decode audio: " + err);
        }
    } else {
        out = decode_audio_miniaudio(data, size);
    }
    if (out.pcm.empty()) {
        throw std::invalid_argument("failed to decode audio: no audio samples");
    }
    return out;
}

std::vector<float> audio_to_mono(const DecodedAudio & in) {
    if (in.channels <= 1) {
        return in.pcm;
    }
    const size_t ch     = (size_t) in.channels;
    const size_t frames = in.pcm.size() / ch;
    std::vector<float> mono(frames);
    const float inv = 1.0f / (float) ch;
    for (size_t f = 0; f < frames; ++f) {
        const float * p = in.pcm.data() + f * ch;
        float sum = 0.0f;
        for (size_t c = 0; c < ch; ++c) {
            sum += p[c];
        }
        mono[f] = sum * inv;
    }
    return mono;
}

std::vector<std::vector<float>> split_audio(const std::vector<float> & mono, int sample_rate, double max_seconds) {
    if (sample_rate <= 0 || !(max_seconds > 0.0)) {
        throw std::invalid_argument("invalid audio split parameters");
    }
    const double chunk_d = std::floor(max_seconds * sample_rate);
    if (chunk_d < 1.0) {
        throw std::invalid_argument("audio clip length is shorter than one sample");
    }
    const size_t chunk = chunk_d > 1e15 ? (size_t) 1e15 : (size_t) chunk_d;
    std::vector<std::vector<float>> clips;
    for (size_t pos = 0; pos < mono.size(); pos += chunk) {
        const size_t n = std::min(chunk, mono.size() - pos);
        clips.emplace_back(mono.begin() + (ptrdiff_t) pos, mono.begin() + (ptrdiff_t) (pos + n));
    }
    return clips;
}

std::string wav_header(uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample, uint32_t data_bytes) {
    const bool     stream      = (data_bytes == 0xFFFFFFFFu);
    const uint32_t riff_size   = stream ? 0xFFFFFFFFu : (data_bytes > 0xFFFFFFFFu - 36u ? 0xFFFFFFFFu : 36u + data_bytes);
    const uint16_t block_align = (uint16_t) (channels * (bits_per_sample / 8));
    const uint32_t byte_rate   = sample_rate * block_align;

    std::string h;
    h.reserve(44);
    h.append("RIFF", 4);
    put_u32le(h, riff_size);
    h.append("WAVE", 4);
    h.append("fmt ", 4);
    put_u32le(h, 16);                 // fmt chunk size
    put_u16le(h, 1);                  // WAVE_FORMAT_PCM
    put_u16le(h, channels);
    put_u32le(h, sample_rate);
    put_u32le(h, byte_rate);
    put_u16le(h, block_align);
    put_u16le(h, bits_per_sample);
    h.append("data", 4);
    put_u32le(h, data_bytes);
    return h;
}

std::string pcm_f32_to_s16(const float * samples, size_t n) {
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float v = clamp_unit(samples[i]);
        const int16_t s = (int16_t) std::lround(v * 32767.0f);
        out[2 * i]     = (char) ((uint16_t) s & 0xFF);
        out[2 * i + 1] = (char) (((uint16_t) s >> 8) & 0xFF);
    }
    return out;
}

std::string pcm_f32_bytes(const float * samples, size_t n) {
    std::string out;
    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        uint32_t u;
        std::memcpy(&u, &samples[i], 4);
        out[4 * i]     = (char) (u & 0xFF);
        out[4 * i + 1] = (char) ((u >> 8) & 0xFF);
        out[4 * i + 2] = (char) ((u >> 16) & 0xFF);
        out[4 * i + 3] = (char) ((u >> 24) & 0xFF);
    }
    return out;
}

std::string encode_wav_s16(const float * samples, size_t n, uint32_t sample_rate) {
    const uint64_t data_bytes = (uint64_t) n * 2;
    if (data_bytes > 0xFFFFFFFEu) {
        throw std::invalid_argument("audio too long for a WAV file");
    }
    std::string out = wav_header(sample_rate, 1, 16, (uint32_t) data_bytes);
    out += pcm_f32_to_s16(samples, n);
    return out;
}

}  // namespace ixsrv
