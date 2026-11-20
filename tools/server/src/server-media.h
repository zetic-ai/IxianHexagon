// Ixian Hexagon server - media decoding helpers (no SDK dependency).
//
// Everything the HTTP layer needs to turn request payloads (base64 / data URLs / multipart uploads) into the
// pixel and sample buffers the SDK takes (Image::fromRgb, Audio::fromPcm), plus the encoders for the audio we
// send back (WAV / PCM16). The SDK itself does no resizing (images must be exactly 512 x 512 RGB), so the
// resize policy lives here.
//
// All functions are thread-safe (no shared mutable state). Failures throw std::invalid_argument with a
// message that is safe to show to an API client.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ixsrv {

using raw_buffer = std::vector<uint8_t>;

//
// base64 and data URLs
//

std::string base64_encode(const uint8_t * data, size_t n);
inline std::string base64_encode(const std::string & s) { return base64_encode((const uint8_t *) s.data(), s.size()); }
inline std::string base64_encode(const raw_buffer & b) { return base64_encode(b.data(), b.size()); }

// Tolerant decoder: standard and URL-safe alphabets, ignores whitespace, accepts missing padding.
// Throws std::invalid_argument on any other character or on an empty result.
raw_buffer base64_decode(const std::string & s);

// "data:<mime>[;charset=...][;base64],<payload>". Returns false when `url` is not a data URL at all.
// Throws std::invalid_argument when it is a data URL but malformed (no comma, not base64, bad payload).
// `mime` is lower-cased, e.g. "image/png", "audio/wav"; it may be empty.
bool parse_data_url(const std::string & url, std::string & mime, raw_buffer & out);

// Looks at the first bytes: "image" (PNG, JPEG, GIF, BMP, PPM/PGM, WEBP), "audio" (RIFF/WAVE, MP3 frame or
// ID3, FLAC, OGG), "video" (MP4/ISO BMFF, Matroska/WebM, AVI) or "" when unknown.
std::string sniff_media_kind(const uint8_t * data, size_t size);

//
// images
//

struct DecodedImage {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3, row-major, no padding
};

// Decodes PNG / JPEG / BMP / GIF (first frame) / PNM / PSD / TGA via stb_image to 8-bit RGB (alpha dropped,
// grey expanded). Throws std::invalid_argument("failed to decode image: ...") on failure.
DecodedImage decode_image(const uint8_t * data, size_t size);

enum class ImageFit {
    Letterbox,  // keep the whole image and its aspect ratio, pad with black (default)
    Stretch,    // resample to the target size, aspect ratio not kept
    Crop,       // keep the aspect ratio, scale so the target is covered, center-crop the rest
};

const char * image_fit_name(ImageFit fit);
// "letterbox" | "stretch" | "crop" (case-insensitive); throws std::invalid_argument otherwise
ImageFit image_fit_from_name(const std::string & name);

// Resamples `src` to exactly width x height with the given policy. Area averaging when shrinking, bilinear
// when enlarging. An image that already has the target size is returned unchanged (copied).
DecodedImage fit_image(const DecodedImage & src, int width, int height, ImageFit fit);

// Encodes a DecodedImage as a binary PPM (P6); handy for debugging and tests.
std::string encode_ppm(const DecodedImage & img);

//
// audio
//

struct DecodedAudio {
    std::vector<float> pcm;  // interleaved, frames * channels, in [-1, 1]
    int sample_rate = 0;
    int channels    = 0;
    size_t frames() const { return channels > 0 ? pcm.size() / (size_t) channels : 0; }
    double seconds() const { return sample_rate > 0 ? (double) frames() / sample_rate : 0.0; }
};

// RIFF/WAVE reader: PCM 8 / 16 / 24 / 32-bit, IEEE float 32 / 64, WAVE_FORMAT_EXTENSIBLE, any channel count,
// tolerant of odd chunk padding and a data chunk size of 0 / 0xFFFFFFFF (streamed WAVs: reads to the end).
// Returns false with `err` set when `data` is not a WAV file or is corrupt.
bool parse_wav(const uint8_t * data, size_t size, DecodedAudio & out, std::string & err);

// Decodes WAV (own reader), MP3 and FLAC (miniaudio) to float PCM, channels and rate as in the file.
// Throws std::invalid_argument("failed to decode audio: ...") when the format is unknown or the data is corrupt.
DecodedAudio decode_audio(const uint8_t * data, size_t size);

// Mixes down to mono (average of the channels). Mono input is returned as is.
std::vector<float> audio_to_mono(const DecodedAudio & in);

// Splits mono samples into clips of at most `max_seconds` each (the last one may be shorter).
// Used for inputs longer than the model's 30 s limit per audio part.
std::vector<std::vector<float>> split_audio(const std::vector<float> & mono, int sample_rate, double max_seconds);

// 44-byte canonical WAV header for PCM data. `data_bytes` = 0xFFFFFFFF marks a stream of unknown length
// (RIFF size is then also 0xFFFFFFFF), which is what llama-server writes for streamed speech.
std::string wav_header(uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample, uint32_t data_bytes);

// float samples to little-endian signed 16-bit PCM bytes, clamped to [-1, 1] (round to nearest)
std::string pcm_f32_to_s16(const float * samples, size_t n);
// float samples as raw little-endian float32 bytes
std::string pcm_f32_bytes(const float * samples, size_t n);

// Complete 16-bit mono WAV file from float samples.
std::string encode_wav_s16(const float * samples, size_t n, uint32_t sample_rate);

}  // namespace ixsrv
