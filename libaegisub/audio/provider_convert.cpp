// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "libaegisub/audio/provider.h"

#include <libaegisub/endian.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

using namespace agi;

/// Anything integral -> 16 bit signed machine-endian audio converter
namespace {
int64_t AssembleSample(std::span<uint8_t> data) {
	const auto accumulate_bytes = [](auto view) {
		return std::accumulate(view.begin(), view.end(), int64_t{0},
			[](int64_t acc, uint8_t value) {
				return (acc << 8) + value;
			});
	};

	if (endian::IsBigEndian)
		return accumulate_bytes(data);

	return accumulate_bytes(data | std::views::reverse);
}

template<class Target>
class BitdepthConvertAudioProvider final : public AudioProviderWrapper {
	int src_bytes_per_sample;
	mutable std::vector<uint8_t> src_buf;

public:
	BitdepthConvertAudioProvider(std::unique_ptr<AudioProvider> src) : AudioProviderWrapper(std::move(src)) {
		if (bytes_per_sample > 8)
			throw AudioProviderError("Audio format converter: audio with bitdepths greater than 64 bits/sample is currently unsupported");

		src_bytes_per_sample = bytes_per_sample;
		bytes_per_sample = sizeof(Target);
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		src_buf.resize(count * src_bytes_per_sample * channels);
		source->GetAudio(src_buf.data(), start, count);

		auto dest = static_cast<int16_t*>(buf);

		for (int64_t i = 0; i < count * channels; ++i) {
			int64_t sample = 0;

			// 8 bits per sample is assumed to be unsigned with a bias of 127,
			// while everything else is assumed to be signed with zero bias
			if (src_bytes_per_sample == 1)
				sample = src_buf[i] - 128;
			else {
				sample = AssembleSample(std::span(src_buf.data() + i * src_bytes_per_sample, src_bytes_per_sample));
			}

			if (src_bytes_per_sample > static_cast<int>(sizeof(Target)))
				sample /= 1LL << (src_bytes_per_sample - sizeof(Target)) * 8;
			else if (src_bytes_per_sample < static_cast<int>(sizeof(Target)))
				sample *=  1LL << (sizeof(Target) - src_bytes_per_sample ) * 8;

			dest[i] = static_cast<Target>(sample);
		}
	}
};

/// Floating point -> 16 bit signed machine-endian audio converter
template<class Source, class Target>
class FloatConvertAudioProvider final : public AudioProviderWrapper {
	mutable std::vector<Source> src_buf;

public:
	FloatConvertAudioProvider(std::unique_ptr<AudioProvider> src) : AudioProviderWrapper(std::move(src)) {
		bytes_per_sample = sizeof(Target);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		src_buf.resize(count * channels);
		source->GetAudio(&src_buf[0], start, count);

		auto dest = static_cast<Target*>(buf);

		for (int i = 0; i < count * channels; ++i) {
			Source expanded;
			if (src_buf[i] < 0)
				expanded = static_cast<Target>(-src_buf[i] * std::numeric_limits<Target>::min() - 0.5);
			else
				expanded = static_cast<Target>(src_buf[i] * std::numeric_limits<Target>::max() + 0.5);

			dest[i] = expanded < std::numeric_limits<Target>::min() ? std::numeric_limits<Target>::min() :
			          expanded > std::numeric_limits<Target>::max() ? std::numeric_limits<Target>::max() :
			                                                          static_cast<Target>(expanded);
		}
	}
};

/// Multichannel 16-bit signed machine-endian -> stereo (or mono) 16-bit signed
/// machine endian converter
///
/// Layouts are assumed to follow the usual WAVE / FFmpeg channel order:
///   3: FL FR FC                       4: FL FR BL BR
///   5: FL FR FC BL BR                 6: FL FR FC LFE BL BR  (5.1)
///   7: FL FR FC LFE BC SL SR  (6.1)   8: FL FR FC LFE BL BR SL SR  (7.1)
/// Anything else is averaged into both output channels. For mono output the
/// stereo mix is averaged down to one channel.
///
/// The coefficients are the same ones libswresample uses for its default
/// downmix (center and surround at -3dB, LFE dropped), normalized so that the
/// loudest output channel can't clip, so the result is consistent with what the
/// FFmpegSource provider produces.
class DownmixAudioProvider final : public AudioProviderWrapper {
	int src_channels;
	std::vector<float> matrix; ///< One row of src_channels coefficients per output channel
	mutable std::vector<int16_t> src_buf;

	void BuildMatrix() {
		constexpr float mix = 0.70710678f; // -3dB
		constexpr float back_center = 0.5f; // -6dB
		matrix.assign(2 * src_channels, 0.f);
		auto set = [&](int ch, float left, float right) {
			matrix[ch] = left;
			matrix[src_channels + ch] = right;
		};

		switch (src_channels) {
		case 3: set(0, 1, 0); set(1, 0, 1); set(2, mix, mix); break;
		case 4: set(0, 1, 0); set(1, 0, 1); set(2, mix, 0); set(3, 0, mix); break;
		case 5: set(0, 1, 0); set(1, 0, 1); set(2, mix, mix); set(3, mix, 0); set(4, 0, mix); break;
		case 6: set(0, 1, 0); set(1, 0, 1); set(2, mix, mix); set(4, mix, 0); set(5, 0, mix); break;
		case 7:
			set(0, 1, 0); set(1, 0, 1); set(2, mix, mix);
			set(4, back_center, back_center); set(5, mix, 0); set(6, 0, mix);
			break;
		case 8:
			set(0, 1, 0); set(1, 0, 1); set(2, mix, mix);
			set(4, mix, 0); set(5, 0, mix); set(6, mix, 0); set(7, 0, mix);
			break;
		default:
			for (int c = 0; c < src_channels; ++c)
				set(c, 1.f, 1.f);
			break;
		}

		// Normalize so the largest possible output is full scale
		float max_sum = 0.f;
		for (int row = 0; row < 2; ++row) {
			float sum = 0.f;
			for (int c = 0; c < src_channels; ++c)
				sum += std::abs(matrix[row * src_channels + c]);
			max_sum = std::max(max_sum, sum);
		}
		if (max_sum > 1.f)
			for (auto& coef : matrix)
				coef /= max_sum;

		// Fold the stereo mix down to a single channel
		if (channels == 1) {
			for (int c = 0; c < src_channels; ++c)
				matrix[c] = (matrix[c] + matrix[src_channels + c]) * 0.5f;
			matrix.resize(src_channels);
		}
	}

public:
	DownmixAudioProvider(std::unique_ptr<AudioProvider> src, int out_channels) : AudioProviderWrapper(std::move(src)) {
		src_channels = channels;
		channels = out_channels;
		BuildMatrix();
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		src_buf.resize(count * src_channels);
		source->GetAudio(&src_buf[0], start, count);

		auto dst = static_cast<int16_t*>(buf);
		for (int64_t i = 0; i < count; ++i) {
			const int16_t *in = &src_buf[i * src_channels];
			for (int row = 0; row < channels; ++row) {
				const float *coefs = &matrix[row * src_channels];
				float sum = 0.f;
				for (int c = 0; c < src_channels; ++c)
					sum += in[c] * coefs[c];
				dst[i * channels + row] = static_cast<int16_t>(std::clamp(std::lround(sum), -32768L, 32767L));
			}
		}
	}
};

/// Sample doubler with linear interpolation for the samples provider
/// Requires 16-bit input (any number of interleaved channels)
class SampleDoublingAudioProvider final : public AudioProviderWrapper {
public:
	SampleDoublingAudioProvider(std::unique_ptr<AudioProvider> src) : AudioProviderWrapper(std::move(src)) {
		sample_rate *= 2;
		num_samples *= 2;
		decoded_samples = decoded_samples * 2;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		const int ch = channels;
		int16_t *src, *dst = static_cast<int16_t *>(buf);

		// We need to always get at least two frames to be able to interpolate
		std::vector<int16_t> srcbuf;
		if (count == 1) {
			srcbuf.resize(2 * ch);
			source->GetAudio(srcbuf.data(), start / 2, 2);
			src = srcbuf.data();
		}
		else {
			source->GetAudio(buf, start / 2, (start + count) / 2 - start / 2 + 1);
			src = dst;
		}

		// walking backwards so that the conversion can be done in place
		for (; count > 0; --count) {
			auto src_index = (start + count - 1) / 2 - start / 2;
			auto i = count - 1;
			if ((start + i) & 1) {
				for (int c = 0; c < ch; ++c)
					dst[i * ch + c] = (int16_t)(((int32_t)src[src_index * ch + c] + src[(src_index + 1) * ch + c]) / 2);
			}
			else {
				for (int c = 0; c < ch; ++c)
					dst[i * ch + c] = src[src_index * ch + c];
			}
		}
	}
};
}

namespace agi {
std::unique_ptr<AudioProvider> CreateConvertAudioProvider(std::unique_ptr<AudioProvider> provider, int max_channels) {
	max_channels = max_channels == 1 ? 1 : 2;

	// Ensure 16-bit audio with proper endianness
	if (provider->AreSamplesFloat()) {
		LOG_D("audio_provider") << "Converting float to S16";
		if (provider->GetBytesPerSample() == sizeof(float))
			provider = std::make_unique<FloatConvertAudioProvider<float, int16_t>>(std::move(provider));
		else
			provider = std::make_unique<FloatConvertAudioProvider<double, int16_t>>(std::move(provider));
	}
	if (provider->GetBytesPerSample() != 2) {
		LOG_D("audio_provider") << "Converting " << provider->GetBytesPerSample() << " bytes per sample or wrong endian to S16";
		provider = std::make_unique<BitdepthConvertAudioProvider<int16_t>>(std::move(provider));
	}

	// Anything with more channels than requested is downmixed
	if (provider->GetChannels() > max_channels) {
		LOG_D("audio_provider") << "Downmixing to " << max_channels << " channel(s) from " << provider->GetChannels() << " channels";
		provider = std::make_unique<DownmixAudioProvider>(std::move(provider), max_channels);
	}

	// Some players don't like low sample rate audio
	while (provider->GetSampleRate() < 32000) {
		LOG_D("audio_provider") << "Doubling sample rate";
		provider = std::make_unique<SampleDoublingAudioProvider>(std::move(provider));
	}

	return provider;
}
}
