#include "wasapi-player.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/monitor] " fmt, ##__VA_ARGS__)

namespace {

/* From ksmedia.h, inlined to avoid pulling the whole header in. */
const GUID kSubtypeIeeeFloat = {0x00000003,
				0x0000,
				0x0010,
				{0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b,
				 0x71}};
const GUID kSubtypePcm = {0x00000001,
			  0x0000,
			  0x0010,
			  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/*
 * Not every SDK that can build this plugin declares these. They are stable
 * values, and the conversion they ask for is the whole point of the primary
 * path, so define them rather than fall back unnecessarily.
 */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

std::wstring to_wide(const char *s)
{
	if (!s || !*s)
		return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	if (len <= 0)
		return {};
	std::wstring out((size_t)len - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
	return out;
}

template<typename T> struct ComPtr {
	T *p = nullptr;
	ComPtr() = default;
	~ComPtr() { reset(); }
	ComPtr(const ComPtr &) = delete;
	ComPtr &operator=(const ComPtr &) = delete;
	T **operator&() { return &p; }
	T *operator->() const { return p; }
	operator T *() const { return p; }
	void reset()
	{
		if (p) {
			p->Release();
			p = nullptr;
		}
	}
};

struct ComInit {
	bool ok = false;
	ComInit()
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
	}
	~ComInit()
	{
		if (ok)
			CoUninitialize();
	}
};

/* How the endpoint wants its samples written. */
enum class OutFmt { Unsupported, Float32, Int16, Int24, Int32 };

OutFmt classify(const WAVEFORMATEX *fmt)
{
	bool is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
	bool is_pcm = fmt->wFormatTag == WAVE_FORMAT_PCM;
	uint16_t valid_bits = fmt->wBitsPerSample;

	if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
	    fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
		const auto *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
		is_float = IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat) != 0;
		is_pcm = IsEqualGUID(ext->SubFormat, kSubtypePcm) != 0;
		if (ext->Samples.wValidBitsPerSample)
			valid_bits = ext->Samples.wValidBitsPerSample;
	}

	if (is_float && fmt->wBitsPerSample == 32)
		return OutFmt::Float32;
	if (!is_pcm)
		return OutFmt::Unsupported;

	switch (fmt->wBitsPerSample) {
	case 16:
		return OutFmt::Int16;
	case 24:
		return OutFmt::Int24;
	case 32:
		/*
		 * 24-bit carried in a 32-bit container is common on USB DACs.
		 * Writing it as 32-bit integer just leaves the unused low bits
		 * at zero, which is exactly right.
		 */
		(void)valid_bits;
		return OutFmt::Int32;
	default:
		return OutFmt::Unsupported;
	}
}

const char *fmt_name(OutFmt f)
{
	switch (f) {
	case OutFmt::Float32:
		return "float32";
	case OutFmt::Int16:
		return "int16";
	case OutFmt::Int24:
		return "int24";
	case OutFmt::Int32:
		return "int32";
	default:
		return "unsupported";
	}
}

inline float clampf(float v)
{
	return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

/*
 * A fixed ring of interleaved float samples at the source rate. Preallocated
 * once: the decode thread only ever memcpys into it and the render thread only
 * ever memcpys out, so neither allocates or shifts the backlog while audio is
 * flowing. The old implementation used vector insert/erase, which memmoved the
 * whole backlog on the Pro Audio thread every period.
 */
class Ring {
public:
	void init(size_t capacity_samples)
	{
		buf.assign(capacity_samples, 0.0f);
		head = tail = count = 0;
	}

	size_t size() const { return count; }
	size_t capacity() const { return buf.size(); }

	void write(const float *src, size_t n)
	{
		if (n > buf.size())
			return;
		/* Never let a write run over samples not yet played. */
		if (count + n > buf.size())
			drop(count + n - buf.size());
		const size_t first = std::min(n, buf.size() - tail);
		memcpy(buf.data() + tail, src, first * sizeof(float));
		if (n > first)
			memcpy(buf.data(), src + first,
			       (n - first) * sizeof(float));
		tail = (tail + n) % buf.size();
		count += n;
	}

	/* Copies `n` samples out without consuming them. */
	void peek(float *dst, size_t n) const
	{
		const size_t first = std::min(n, buf.size() - head);
		memcpy(dst, buf.data() + head, first * sizeof(float));
		if (n > first)
			memcpy(dst + first, buf.data(),
			       (n - first) * sizeof(float));
	}

	void drop(size_t n)
	{
		n = std::min(n, count);
		head = (head + n) % buf.size();
		count -= n;
	}

private:
	std::vector<float> buf;
	size_t head = 0;
	size_t tail = 0;
	size_t count = 0;
};

} // namespace

struct cord_audio_player {
	std::string device_id;
	uint32_t src_rate = 48000;
	uint32_t src_channels = 2;

	std::mutex mutex;
	Ring ring;
	/* All in source frames; see the latency budget in the header. */
	size_t fill_frames = 0;
	size_t cushion_frames = 0;
	size_t prime_frames = 0;
	size_t max_frames = 0;

	std::atomic<uint64_t> dropped{0};
	std::atomic<uint64_t> underruns{0};

	std::thread worker;
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};
	std::atomic<float> volume{1.0f};

	void run();
	bool render(IAudioRenderClient *render, UINT32 frames,
		    const float *chunk, size_t chunk_frames, double step,
		    OutFmt out_fmt, uint32_t out_ch);

	/* Resampler state, render thread only. */
	double phase = 0.0;
	float prev_l = 0.0f;
	float prev_r = 0.0f;
	float cur_gain = 1.0f;
};

/*
 * Converts `frames` output frames out of `chunk` and hands them to WASAPI.
 *
 * `chunk` holds the source frames starting one past `prev_*`, so source
 * position 0 is the previous frame and position j is chunk[j-1]. Carrying that
 * one frame plus a fractional phase across calls is what keeps the
 * interpolation continuous; restarting it at every period, as this used to,
 * puts a small discontinuity into the signal 100 times a second.
 */
bool cord_audio_player::render(IAudioRenderClient *render_client, UINT32 frames,
			       const float *chunk, size_t chunk_frames,
			       double step, OutFmt out_fmt, uint32_t out_ch)
{
	BYTE *out = nullptr;
	if (FAILED(render_client->GetBuffer(frames, &out)))
		return false;

	const float target_gain = volume.load();
	/* Ramp over the block so a level change cannot click. */
	const float gain_step =
		frames ? (target_gain - cur_gain) / (float)frames : 0.0f;
	float gain = cur_gain;

	const size_t stride = src_channels;

	for (UINT32 i = 0; i < frames; i++, gain += gain_step) {
		const double pos = phase + (double)i * step;
		size_t i0 = (size_t)pos;
		const float frac = (float)(pos - (double)i0);

		/* Position 0 is the carried frame; j maps to chunk[j - 1]. */
		float l0, r0, l1, r1;
		if (i0 == 0) {
			l0 = prev_l;
			r0 = prev_r;
		} else {
			const size_t j = std::min(i0 - 1, chunk_frames - 1);
			l0 = chunk[j * stride];
			r0 = stride > 1 ? chunk[j * stride + 1] : l0;
		}
		{
			const size_t j = std::min(i0, chunk_frames - 1);
			l1 = chunk[j * stride];
			r1 = stride > 1 ? chunk[j * stride + 1] : l1;
		}

		const float l = clampf((l0 + frac * (l1 - l0)) * gain);
		const float r = clampf((r0 + frac * (r1 - r0)) * gain);

		switch (out_fmt) {
		case OutFmt::Float32: {
			float *d = (float *)out + (size_t)i * out_ch;
			if (out_ch == 1) {
				d[0] = (l + r) * 0.5f;
			} else {
				d[0] = l;
				d[1] = r;
				for (uint32_t c = 2; c < out_ch; c++)
					d[c] = 0.0f;
			}
			break;
		}
		case OutFmt::Int16: {
			int16_t *d = (int16_t *)out + (size_t)i * out_ch;
			if (out_ch == 1) {
				d[0] = (int16_t)((l + r) * 0.5f * 32767.0f);
			} else {
				d[0] = (int16_t)(l * 32767.0f);
				d[1] = (int16_t)(r * 32767.0f);
				for (uint32_t c = 2; c < out_ch; c++)
					d[c] = 0;
			}
			break;
		}
		case OutFmt::Int32: {
			int32_t *d = (int32_t *)out + (size_t)i * out_ch;
			if (out_ch == 1) {
				d[0] = (int32_t)((l + r) * 0.5f * 2147483520.0f);
			} else {
				d[0] = (int32_t)(l * 2147483520.0f);
				d[1] = (int32_t)(r * 2147483520.0f);
				for (uint32_t c = 2; c < out_ch; c++)
					d[c] = 0;
			}
			break;
		}
		case OutFmt::Int24: {
			BYTE *d = out + ((size_t)i * out_ch) * 3;
			const float mono = (l + r) * 0.5f;
			for (uint32_t c = 0; c < out_ch; c++) {
				float v = 0.0f;
				if (out_ch == 1)
					v = mono;
				else if (c == 0)
					v = l;
				else if (c == 1)
					v = r;
				const int32_t s =
					(int32_t)(v * 8388607.0f);
				d[c * 3 + 0] = (BYTE)(s & 0xff);
				d[c * 3 + 1] = (BYTE)((s >> 8) & 0xff);
				d[c * 3 + 2] = (BYTE)((s >> 16) & 0xff);
			}
			break;
		}
		default:
			break;
		}
	}

	cur_gain = target_gain;

	/* Advance the resampler past exactly what these frames consumed. */
	const double end = phase + (double)frames * step;
	const size_t advance = (size_t)end;
	if (advance >= 1) {
		const size_t j = std::min(advance - 1, chunk_frames - 1);
		prev_l = chunk[j * stride];
		prev_r = stride > 1 ? chunk[j * stride + 1] : prev_l;
	}
	phase = end - (double)advance;

	render_client->ReleaseBuffer(frames, 0);

	std::lock_guard<std::mutex> lock(mutex);
	ring.drop(advance * src_channels);
	return true;
}

void cord_audio_player::run()
{
	ComInit com;
	if (!com.ok) {
		LOG(LOG_ERROR, "could not initialise COM");
		return;
	}

	ComPtr<IMMDeviceEnumerator> enumerator;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
				    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				    (void **)&enumerator))) {
		LOG(LOG_ERROR, "could not create the device enumerator");
		return;
	}

	ComPtr<IMMDevice> device;
	bool opened = false;
	if (!device_id.empty()) {
		std::wstring wid = to_wide(device_id.c_str());
		opened = SUCCEEDED(enumerator->GetDevice(wid.c_str(), &device));
		if (!opened)
			LOG(LOG_WARNING,
			    "monitor device is gone, using the default");
	}
	if (!opened) {
		/*
		 * eConsole, not eCommunications: this is game audio for the
		 * person playing, not a call.
		 */
		if (FAILED(enumerator->GetDefaultAudioEndpoint(
			    eRender, eConsole, &device))) {
			LOG(LOG_ERROR, "no playback device available");
			return;
		}
	}

	/*
	 * Ask for the device buffer with headroom over the fill target, so a
	 * late period can be caught up with rather than truncated.
	 */
	const REFERENCE_TIME duration =
		(REFERENCE_TIME)(CORD_MONITOR_FILL_MS * 2) * 10000;

	ComPtr<IAudioClient> client;
	WAVEFORMATEXTENSIBLE want{};
	want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	want.Format.nChannels = (WORD)src_channels;
	want.Format.nSamplesPerSec = src_rate;
	want.Format.wBitsPerSample = 32;
	want.Format.nBlockAlign =
		(WORD)(src_channels * sizeof(float));
	want.Format.nAvgBytesPerSec =
		src_rate * want.Format.nBlockAlign;
	want.Format.cbSize =
		sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	want.Samples.wValidBitsPerSample = 32;
	want.dwChannelMask = src_channels == 1 ? SPEAKER_FRONT_CENTER
					       : (SPEAKER_FRONT_LEFT |
						  SPEAKER_FRONT_RIGHT);
	want.SubFormat = kSubtypeIeeeFloat;

	WAVEFORMATEX *stream_fmt = nullptr; /* CoTaskMemFree'd if from mix */
	bool stream_fmt_owned = false;
	bool converted_by_wasapi = false;

	/*
	 * Primary path: hand WASAPI our own format and let the audio engine
	 * resample and remap channels. Its resampler is better than anything
	 * worth hand-rolling here, and it makes the common case - a 48 kHz
	 * endpoint - a straight copy with no conversion at all.
	 */
	if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				       nullptr, (void **)&client))) {
		HRESULT hr = client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
				AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			duration, 0, (WAVEFORMATEX *)&want, nullptr);
		if (SUCCEEDED(hr)) {
			stream_fmt = (WAVEFORMATEX *)&want;
			converted_by_wasapi = true;
		} else {
			LOG(LOG_INFO,
			    "endpoint would not take 48 kHz float directly "
			    "(0x%08lx), converting in process",
			    hr);
			/* A failed Initialize cannot be retried on a client. */
			client.reset();
		}
	}

	/* Fallback: take the endpoint's mix format and convert ourselves. */
	if (!converted_by_wasapi) {
		if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
					    nullptr, (void **)&client))) {
			LOG(LOG_ERROR,
			    "could not activate the playback device");
			return;
		}
		if (FAILED(client->GetMixFormat(&stream_fmt))) {
			LOG(LOG_ERROR, "could not read the device mix format");
			return;
		}
		stream_fmt_owned = true;

		HRESULT hr = client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0,
			stream_fmt, nullptr);
		if (FAILED(hr)) {
			LOG(LOG_ERROR,
			    "could not initialise playback (0x%08lx)", hr);
			CoTaskMemFree(stream_fmt);
			return;
		}
	}

	const OutFmt out_fmt = classify(stream_fmt);
	if (out_fmt == OutFmt::Unsupported) {
		/*
		 * Better to say so than to write nothing into a buffer the
		 * endpoint then plays as noise, which is what used to happen
		 * for anything that was neither float nor 16-bit.
		 */
		LOG(LOG_ERROR,
		    "endpoint uses a sample format this cannot write "
		    "(tag %u, %u bits) - monitoring is off",
		    stream_fmt->wFormatTag, stream_fmt->wBitsPerSample);
		if (stream_fmt_owned)
			CoTaskMemFree(stream_fmt);
		return;
	}

	const uint32_t out_rate = stream_fmt->nSamplesPerSec;
	const uint32_t out_ch = stream_fmt->nChannels;
	/* Source frames consumed per output frame. */
	const double step = (double)src_rate / (double)out_rate;

	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	client->SetEventHandle(event);

	ComPtr<IAudioRenderClient> render_client;
	if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
				      (void **)&render_client))) {
		LOG(LOG_ERROR, "could not get the render service");
		CloseHandle(event);
		if (stream_fmt_owned)
			CoTaskMemFree(stream_fmt);
		return;
	}

	UINT32 buffer_frames = 0;
	client->GetBufferSize(&buffer_frames);

	/* Keep the endpoint topped up to the fill target, not to the brim. */
	UINT32 fill_target = (UINT32)((uint64_t)out_rate *
				      CORD_MONITOR_FILL_MS / 1000);
	if (fill_target > buffer_frames)
		fill_target = buffer_frames;

	DWORD task_index = 0;
	HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

	cur_gain = volume.load();

	client->Start();
	running = true;

	LOG(LOG_INFO,
	    "monitoring to %u Hz %u ch %s, %s, ~%u ms latency "
	    "(%u ms in the endpoint + %u ms jitter buffer) - independent of "
	    "OBS monitoring",
	    out_rate, out_ch, fmt_name(out_fmt),
	    converted_by_wasapi ? "converted by the audio engine"
				: "converted in process",
	    (unsigned)(CORD_MONITOR_FILL_MS + CORD_MONITOR_CUSHION_MS),
	    (unsigned)CORD_MONITOR_FILL_MS,
	    (unsigned)CORD_MONITOR_CUSHION_MS);

	std::vector<float> chunk(
		(size_t)(src_rate * src_channels)); /* 1 s, never reallocated */

	/*
	 * The jitter buffer has to fill before anything is played, and refill
	 * if the stream ever dries up. Playing early is what made this chop:
	 * the queue was drained every period and the shortfall written as
	 * silence, so a 20 ms packet rhythm came out as audio-gap-audio-gap.
	 */
	bool primed = false;
	bool first_prime = true;
	uint64_t timeouts = 0;

	while (!stop) {
		if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) {
			if (++timeouts == 10)
				LOG(LOG_WARNING,
				    "playback device has not asked for audio "
				    "in 2 s");
			continue;
		}
		timeouts = 0;

		UINT32 padding = 0;
		HRESULT hr = client->GetCurrentPadding(&padding);
		if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
			LOG(LOG_WARNING, "playback device was removed");
			break;
		}
		if (FAILED(hr))
			break;

		if (padding >= fill_target)
			continue;
		UINT32 want_frames = fill_target - padding;

		size_t have = 0;
		{
			std::lock_guard<std::mutex> lock(mutex);
			have = ring.size() / src_channels;
		}

		if (!primed) {
			if (have < prime_frames)
				continue; /* silence: the buffer starts zeroed */
			primed = true;
			phase = 0.0;
			prev_l = prev_r = 0.0f;
			if (!first_prime)
				underruns++;
			first_prime = false;
		}

		if (have == 0) {
			/* Dry. Re-prime once rather than dribble a silent gap
			 * into every period from here on. */
			primed = false;
			continue;
		}

		/*
		 * How many output frames the queued audio can actually cover.
		 * Writing short is fine - the endpoint keeps playing what it
		 * already has and asks again next period. Writing silence to
		 * make up the difference is not: that silence is spliced into
		 * the signal for good.
		 */
		size_t n = (size_t)(((double)have - phase) / step);
		while (n > 0 && (size_t)(phase + (double)n * step) + 1 > have)
			n--;
		if (n == 0)
			continue;
		if (n > want_frames)
			n = want_frames;

		/* Source frames this many output frames needs, plus the one
		 * past the end that the interpolation reads. */
		size_t need = (size_t)(phase + (double)n * step) + 1;
		if (need > have)
			need = have;
		if (need * src_channels > chunk.size())
			need = chunk.size() / src_channels;

		{
			std::lock_guard<std::mutex> lock(mutex);
			ring.peek(chunk.data(), need * src_channels);
		}

		if (!render(render_client, (UINT32)n, chunk.data(), need, step,
			    out_fmt, out_ch))
			break;
	}

	running = false;
	client->Stop();

	if (avrt)
		AvRevertMmThreadCharacteristics(avrt);
	CloseHandle(event);
	if (stream_fmt_owned)
		CoTaskMemFree(stream_fmt);

	const uint64_t lost = dropped.load();
	const uint64_t dry = underruns.load();
	if (lost || dry)
		LOG(LOG_INFO,
		    "monitor stopped (%llu underruns, %llu frames dropped)",
		    (unsigned long long)dry, (unsigned long long)lost);
	else
		LOG(LOG_INFO, "monitor stopped cleanly");
}

extern "C" {

struct cord_audio_player *cord_audio_player_create(const char *device_id,
						   uint32_t sample_rate,
						   uint32_t channels)
{
	auto *p = new cord_audio_player();
	p->device_id = device_id ? device_id : "";
	p->src_rate = sample_rate ? sample_rate : 48000;
	p->src_channels = channels ? channels : 2;

	const size_t per_ms = (size_t)p->src_rate / 1000;
	p->fill_frames = per_ms * CORD_MONITOR_FILL_MS;
	p->cushion_frames = per_ms * CORD_MONITOR_CUSHION_MS;
	p->prime_frames = p->fill_frames + p->cushion_frames;
	/*
	 * The phone's clock and the sound card's clock are not the same clock,
	 * so the queue drifts. Let it run 40 ms long before trimming back to
	 * the target: often enough to bound the delay, rarely enough that the
	 * trim is not what anyone hears.
	 */
	p->max_frames = p->prime_frames + per_ms * 40;

	p->ring.init((p->max_frames * 2 + per_ms * 100) * p->src_channels);

	p->worker = std::thread([p] {
		os_set_thread_name("cord: monitor");
		p->run();
	});

	return p;
}

void cord_audio_player_destroy(struct cord_audio_player *p)
{
	if (!p)
		return;
	p->stop = true;
	if (p->worker.joinable())
		p->worker.join();
	delete p;
}

void cord_audio_player_push(struct cord_audio_player *p,
			    const float *interleaved, uint32_t frames)
{
	if (!p || !interleaved || !frames)
		return;

	const size_t samples = (size_t)frames * p->src_channels;

	std::lock_guard<std::mutex> lock(p->mutex);
	if (samples > p->ring.capacity())
		return;

	p->ring.write(interleaved, samples);

	const size_t max_samples = p->max_frames * p->src_channels;
	if (p->ring.size() > max_samples) {
		const size_t excess =
			p->ring.size() - p->prime_frames * p->src_channels;
		p->ring.drop(excess);
		p->dropped += excess / p->src_channels;
	}
}

void cord_audio_player_set_volume(struct cord_audio_player *p, float volume)
{
	if (p)
		p->volume = volume;
}

bool cord_audio_player_is_running(const struct cord_audio_player *p)
{
	return p && p->running.load();
}

} // extern "C"
