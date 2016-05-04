/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "ShineEncoderPlugin.hxx"
#include "config.h"
#include "../EncoderAPI.hxx"
#include "AudioFormat.hxx"
#include "config/ConfigError.hxx"
#include "util/DynamicFifoBuffer.hxx"
#include "util/Error.hxx"

extern "C"
{
#include <shine/layer3.h>
}

static constexpr size_t BUFFER_INIT_SIZE = 8192;
static constexpr unsigned CHANNELS = 2;

class ShineEncoder final : public Encoder {
	const AudioFormat audio_format;

	const shine_t shine;

	const size_t frame_size;

	/* workaround for bug:
	   https://github.com/savonet/shine/issues/11 */
	size_t input_pos = SHINE_MAX_SAMPLES + 1;

	int16_t *stereo[CHANNELS];

	DynamicFifoBuffer<uint8_t> output_buffer;

public:
	ShineEncoder(AudioFormat _audio_format, shine_t _shine)
		:Encoder(false),
		 audio_format(_audio_format), shine(_shine),
		 frame_size(shine_samples_per_pass(shine)),
		 stereo{new int16_t[frame_size], new int16_t[frame_size]},
		 output_buffer(BUFFER_INIT_SIZE) {}

	~ShineEncoder() override {
		if (input_pos > SHINE_MAX_SAMPLES) {
			/* write zero chunk */
			input_pos = 0;
			WriteChunk(true);
		}

		shine_close(shine);
		delete[] stereo[0];
		delete[] stereo[1];
	}

	bool WriteChunk(bool flush);

	/* virtual methods from class Encoder */
	bool End(Error &error) override {
		return Flush(error);
	}

	bool Flush(Error &) override;

	bool Write(const void *data, size_t length, Error &) override;

	size_t Read(void *dest, size_t length) override {
		return output_buffer.Read((uint8_t *)dest, length);
	}
};

class PreparedShineEncoder final : public PreparedEncoder {
	shine_config_t config;

public:
	bool Configure(const ConfigBlock &block, Error &error);

	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &audio_format, Error &) override;

	const char *GetMimeType() const override {
		return  "audio/mpeg";
	}
};

inline bool
PreparedShineEncoder::Configure(const ConfigBlock &block, Error &)
{
	shine_set_config_mpeg_defaults(&config.mpeg);
	config.mpeg.bitr = block.GetBlockValue("bitrate", 128);

	return true;
}

static PreparedEncoder *
shine_encoder_init(const ConfigBlock &block, Error &error)
{
	auto *encoder = new PreparedShineEncoder();

	/* load configuration from "block" */
	if (!encoder->Configure(block, error)) {
		/* configuration has failed, roll back and return error */
		delete encoder;
		return nullptr;
	}

	return encoder;
}

static shine_t
SetupShine(shine_config_t config, AudioFormat &audio_format,
	   Error &error)
{
	audio_format.format = SampleFormat::S16;
	audio_format.channels = CHANNELS;

	config.mpeg.mode = audio_format.channels == 2 ? STEREO : MONO;
	config.wave.samplerate = audio_format.sample_rate;
	config.wave.channels =
		audio_format.channels == 2 ? PCM_STEREO : PCM_MONO;

	if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0) {
		error.Format(config_domain,
			     "error configuring shine. "
			     "samplerate %d and bitrate %d configuration"
			     " not supported.",
			     config.wave.samplerate,
			     config.mpeg.bitr);

		return nullptr;
	}

	auto shine = shine_initialise(&config);
	if (!shine)
		error.Format(config_domain,
			     "error initializing shine.");

	return shine;
}

Encoder *
PreparedShineEncoder::Open(AudioFormat &audio_format, Error &error)
{
	auto shine = SetupShine(config, audio_format, error);
	if (!shine)
		return nullptr;

	return new ShineEncoder(audio_format, shine);
}

bool
ShineEncoder::WriteChunk(bool flush)
{
	if (flush || input_pos == frame_size) {
		if (flush) {
			/* fill remaining with 0s */
			for (; input_pos < frame_size; input_pos++) {
				stereo[0][input_pos] = stereo[1][input_pos] = 0;
			}
		}

		int written;
		const uint8_t *out =
			shine_encode_buffer(shine, stereo, &written);

		if (written > 0)
			output_buffer.Append(out, written);

		input_pos = 0;
	}

	return true;
}

bool
ShineEncoder::Write(const void *_data, size_t length, gcc_unused Error &error)
{
	const int16_t *data = (const int16_t*)_data;
	length /= sizeof(*data) * audio_format.channels;
	size_t written = 0;

	if (input_pos > SHINE_MAX_SAMPLES)
		input_pos = 0;

	/* write all data to de-interleaved buffers */
	while (written < length) {
		for (;
		     written < length && input_pos < frame_size;
		     written++, input_pos++) {
			const size_t base =
				written * audio_format.channels;
			stereo[0][input_pos] = data[base];
			stereo[1][input_pos] = data[base + 1];
		}
		/* write if chunk is filled */
		WriteChunk(false);
	}

	return true;
}

bool
ShineEncoder::Flush(gcc_unused Error &error)
{
	/* flush buffers and flush shine */
	WriteChunk(true);

	int written;
	const uint8_t *data = shine_flush(shine, &written);

	if (written > 0)
		output_buffer.Append(data, written);

	return true;
}

const EncoderPlugin shine_encoder_plugin = {
	"shine",
	shine_encoder_init,
};
