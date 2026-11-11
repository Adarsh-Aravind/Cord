#include "wasapi-bridge.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h> /* os_set_thread_name */

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h> /* WAVEFORMATEXTENSIBLE */
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#define LOG(level, fmt, ...) \
	blog(level, "[cord/micsend] " fmt, ##__VA_ARGS__)

/* 10ms periods: short enough that the bridge is inaudible in a conversation. */
static constexpr int kPeriodMs = 10;
static constexpr int kBufferMs = 40;

/*
 * KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, written out rather than included: the only
 * place it is declared is ksmedia.h, which drags in the whole kernel-streaming
 * header set for one GUID.
 */
static const GUID kSubtypeIeeeFloat = {
	0x00000003, 0x0000, 0x0010,
	{0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

namespace {

std::string to_utf8(const wchar_t *w)
{
	if (!w)
		return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr,
				      nullptr);
	if (len <= 0)
		return {};
	std::string out((size_t)len - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr,
			    nullptr);
	return out;
}

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

/* Minimal COM pointer: avoids pulling in ATL for four interfaces. */
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
		/* RPC_E_CHANGED_MODE means someone else already initialised. */
		ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
	}
	~ComInit()
	{
		if (ok)
			CoUninitialize();
	}
};

bool list_devices(EDataFlow flow, struct cord_audio_device_list *out)
{
	memset(out, 0, sizeof(*out));

	ComInit com;
	ComPtr<IMMDeviceEnumerator> enumerator;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
				    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				    (void **)&enumerator)))
		return false;

	/* Remember the default so the UI can mark it. */
	std::string default_id;
	ComPtr<IMMDevice> def;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eCommunications,
							  &def))) {
		LPWSTR id = nullptr;
		if (SUCCEEDED(def->GetId(&id))) {
			default_id = to_utf8(id);
			CoTaskMemFree(id);
		}
	}

	ComPtr<IMMDeviceCollection> collection;
	if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
						  &collection)))
		return false;

	UINT count = 0;
	collection->GetCount(&count);

	out->items = (struct cord_audio_device *)bzalloc(
		sizeof(struct cord_audio_device) * (count + 1));

	for (UINT i = 0; i < count; i++) {
		ComPtr<IMMDevice> device;
		if (FAILED(collection->Item(i, &device)))
			continue;

		LPWSTR id = nullptr;
		if (FAILED(device->GetId(&id)))
			continue;
		std::string id_utf8 = to_utf8(id);
		CoTaskMemFree(id);

		std::string name = id_utf8;
		ComPtr<IPropertyStore> store;
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
			PROPVARIANT pv;
			PropVariantInit(&pv);
			if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName,
						      &pv)) &&
			    pv.vt == VT_LPWSTR)
				name = to_utf8(pv.pwszVal);
			PropVariantClear(&pv);
		}

		auto &slot = out->items[out->count++];
		snprintf(slot.id, sizeof(slot.id), "%s", id_utf8.c_str());
		snprintf(slot.name, sizeof(slot.name), "%s", name.c_str());
		slot.is_default = (id_utf8 == default_id);
	}

	return true;
}

/* Opens an endpoint by id, or the communications default when id is empty. */
bool open_device(IMMDeviceEnumerator *enumerator, EDataFlow flow,
		 const char *id, ComPtr<IMMDevice> &out)
{
	if (id && *id) {
		std::wstring wid = to_wide(id);
		if (SUCCEEDED(enumerator->GetDevice(wid.c_str(), &out)))
			return true;
		LOG(LOG_WARNING,
		    "configured device is gone, falling back to the default");
	}
	return SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
		flow, eCommunications, &out));
}

} // namespace

/*
 * Everything the worker thread needs. The thread owns both audio clients, so
 * COM apartment rules stay simple: created and released on the same thread.
 */
struct cord_micsend {
	std::string capture_id;
	std::string render_id;

	std::thread worker;
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};
	std::atomic<float> gain{1.0f};
	std::atomic<bool> muted{false};
	std::atomic<float> peak{0.0f};
	std::atomic<int> tone_frames_remaining{0};

	void run();
};

/*
 * Converts an arbitrary WASAPI mix format block to interleaved float stereo.
 * Shared-mode endpoints are nearly always 32-bit float already, so this is a
 * fallback path, not the hot one.
 */
static void to_float_stereo(const BYTE *src, UINT32 frames,
			    const WAVEFORMATEX *fmt, std::vector<float> &out)
{
	const int ch = fmt->nChannels;
	out.resize((size_t)frames * 2);

	const bool is_float =
		fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
		(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
		 IsEqualGUID(((const WAVEFORMATEXTENSIBLE *)fmt)->SubFormat,
			     kSubtypeIeeeFloat));

	for (UINT32 i = 0; i < frames; i++) {
		float l = 0.0f, r = 0.0f;

		if (is_float && fmt->wBitsPerSample == 32) {
			const float *s = (const float *)src + (size_t)i * ch;
			l = s[0];
			r = ch > 1 ? s[1] : s[0];
		} else if (fmt->wBitsPerSample == 16) {
			const int16_t *s = (const int16_t *)src + (size_t)i * ch;
			l = s[0] / 32768.0f;
			r = ch > 1 ? s[1] / 32768.0f : l;
		} else if (fmt->wBitsPerSample == 32) {
			const int32_t *s = (const int32_t *)src + (size_t)i * ch;
			l = (float)(s[0] / 2147483648.0);
			r = ch > 1 ? (float)(s[1] / 2147483648.0) : l;
		}

		out[(size_t)i * 2] = l;
		out[(size_t)i * 2 + 1] = r;
	}
}

static void from_float_stereo(const std::vector<float> &in, UINT32 frames,
			      const WAVEFORMATEX *fmt, BYTE *dst)
{
	const int ch = fmt->nChannels;

	const bool is_float =
		fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
		(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
		 IsEqualGUID(((const WAVEFORMATEXTENSIBLE *)fmt)->SubFormat,
			     kSubtypeIeeeFloat));

	for (UINT32 i = 0; i < frames; i++) {
		float l = in.size() > (size_t)i * 2 ? in[(size_t)i * 2] : 0.0f;
		float r = in.size() > (size_t)i * 2 + 1 ? in[(size_t)i * 2 + 1]
							: l;

		if (is_float && fmt->wBitsPerSample == 32) {
			float *d = (float *)dst + (size_t)i * ch;
			for (int c = 0; c < ch; c++)
				d[c] = (c == 0) ? l : (c == 1 ? r : 0.0f);
		} else if (fmt->wBitsPerSample == 16) {
			int16_t *d = (int16_t *)dst + (size_t)i * ch;
			for (int c = 0; c < ch; c++) {
				float v = (c == 0) ? l : (c == 1 ? r : 0.0f);
				v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
				d[c] = (int16_t)(v * 32767.0f);
			}
		}
	}
}

void cord_micsend::run()
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

	ComPtr<IMMDevice> cap_dev, ren_dev;
	if (!open_device(enumerator, eCapture, capture_id.c_str(), cap_dev) ||
	    !open_device(enumerator, eRender, render_id.c_str(), ren_dev)) {
		LOG(LOG_ERROR, "could not open the audio endpoints");
		return;
	}

	ComPtr<IAudioClient> cap_client, ren_client;
	if (FAILED(cap_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				     nullptr, (void **)&cap_client)) ||
	    FAILED(ren_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				     nullptr, (void **)&ren_client))) {
		LOG(LOG_ERROR, "could not activate the audio clients");
		return;
	}

	WAVEFORMATEX *cap_fmt = nullptr;
	WAVEFORMATEX *ren_fmt = nullptr;
	if (FAILED(cap_client->GetMixFormat(&cap_fmt)) ||
	    FAILED(ren_client->GetMixFormat(&ren_fmt))) {
		LOG(LOG_ERROR, "could not read the device mix formats");
		return;
	}

	const REFERENCE_TIME buffer_duration = kBufferMs * 10000LL;

	HRESULT hr = cap_client->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
			AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
		buffer_duration, 0, cap_fmt, nullptr);
	if (FAILED(hr)) {
		LOG(LOG_ERROR, "could not initialise capture (0x%08lx)", hr);
		CoTaskMemFree(cap_fmt);
		CoTaskMemFree(ren_fmt);
		return;
	}

	hr = ren_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
				    buffer_duration, 0, ren_fmt, nullptr);
	if (FAILED(hr)) {
		LOG(LOG_ERROR, "could not initialise render (0x%08lx)", hr);
		CoTaskMemFree(cap_fmt);
		CoTaskMemFree(ren_fmt);
		return;
	}

	HANDLE cap_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	cap_client->SetEventHandle(cap_event);

	ComPtr<IAudioCaptureClient> capture;
	ComPtr<IAudioRenderClient> render;
	if (FAILED(cap_client->GetService(__uuidof(IAudioCaptureClient),
					  (void **)&capture)) ||
	    FAILED(ren_client->GetService(__uuidof(IAudioRenderClient),
					  (void **)&render))) {
		LOG(LOG_ERROR, "could not get the audio services");
		CloseHandle(cap_event);
		CoTaskMemFree(cap_fmt);
		CoTaskMemFree(ren_fmt);
		return;
	}

	UINT32 ren_buffer_frames = 0;
	ren_client->GetBufferSize(&ren_buffer_frames);

	/*
	 * Pro Audio priority: without it, a busy game can starve this thread
	 * and the voice going to teammates breaks up.
	 */
	DWORD task_index = 0;
	HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

	cap_client->Start();
	ren_client->Start();
	running = true;

	LOG(LOG_INFO, "bridge running: capture %lu Hz %u ch -> render %lu Hz %u ch",
	    cap_fmt->nSamplesPerSec, cap_fmt->nChannels,
	    ren_fmt->nSamplesPerSec, ren_fmt->nChannels);

	const double rate_ratio =
		(double)ren_fmt->nSamplesPerSec / (double)cap_fmt->nSamplesPerSec;

	std::vector<float> block;     /* captured, float stereo */
	std::vector<float> resampled; /* at the render rate */
	double tone_phase = 0.0;

	while (!stop) {
		if (WaitForSingleObject(cap_event, 200) != WAIT_OBJECT_0)
			continue;

		UINT32 packet = 0;
		while (SUCCEEDED(capture->GetNextPacketSize(&packet)) &&
		       packet > 0) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;

			if (FAILED(capture->GetBuffer(&data, &frames, &flags,
						      nullptr, nullptr)))
				break;

			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				block.assign((size_t)frames * 2, 0.0f);
			} else {
				to_float_stereo(data, frames, cap_fmt, block);
			}
			capture->ReleaseBuffer(frames);

			/* gain, mute and peak */
			const float g = muted ? 0.0f : gain.load();
			float local_peak = 0.0f;
			for (auto &s : block) {
				s *= g;
				float a = std::fabs(s);
				if (a > local_peak)
					local_peak = a;
			}
			peak = local_peak;

			/* The wiring test tone overrides the mic entirely. */
			int tone_left = tone_frames_remaining.load();
			if (tone_left > 0) {
				const double step = 2.0 * 3.14159265358979 *
						    1000.0 /
						    (double)cap_fmt->nSamplesPerSec;
				for (UINT32 i = 0; i < frames; i++) {
					float v = (float)(0.25 *
							  std::sin(tone_phase));
					tone_phase += step;
					block[(size_t)i * 2] = v;
					block[(size_t)i * 2 + 1] = v;
				}
				tone_frames_remaining = tone_left -
							(int)frames;
			}

			/*
			 * Linear resample when the endpoints disagree on rate.
			 * Fine for voice; a proper polyphase filter would be
			 * overkill for a 48k->44.1k hop on a mic feed.
							 */
			const float *send = block.data();
			UINT32 send_frames = frames;

			if (std::fabs(rate_ratio - 1.0) > 1e-6) {
				send_frames =
					(UINT32)((double)frames * rate_ratio);
				resampled.assign((size_t)send_frames * 2, 0.0f);
				for (UINT32 i = 0; i < send_frames; i++) {
					double src_pos =
						(double)i / rate_ratio;
					size_t i0 = (size_t)src_pos;
					double frac = src_pos - (double)i0;
					size_t i1 = i0 + 1 < frames ? i0 + 1
								    : i0;
					for (int c = 0; c < 2; c++) {
						float a = block[i0 * 2 + c];
						float b = block[i1 * 2 + c];
						resampled[(size_t)i * 2 + c] =
							a + (float)frac *
								    (b - a);
					}
				}
				send = resampled.data();
			}

			/* Render, dropping the block if the device is behind. */
			UINT32 padding = 0;
			if (FAILED(ren_client->GetCurrentPadding(&padding)))
				break;

			const UINT32 available = ren_buffer_frames - padding;
			const UINT32 to_write = send_frames < available
							? send_frames
							: available;
			if (to_write == 0)
				continue;

			BYTE *out = nullptr;
			if (FAILED(render->GetBuffer(to_write, &out)))
				break;

			std::vector<float> tmp(send,
					       send + (size_t)to_write * 2);
			from_float_stereo(tmp, to_write, ren_fmt, out);
			render->ReleaseBuffer(to_write, 0);
		}
	}

	running = false;
	cap_client->Stop();
	ren_client->Stop();

	if (avrt)
		AvRevertMmThreadCharacteristics(avrt);

	CloseHandle(cap_event);
	CoTaskMemFree(cap_fmt);
	CoTaskMemFree(ren_fmt);

	LOG(LOG_INFO, "bridge stopped");
}

/* ------------------------------------------------------------------ */
/* C API                                                               */
/* ------------------------------------------------------------------ */

extern "C" {

bool cord_list_capture_devices(struct cord_audio_device_list *out)
{
	return list_devices(eCapture, out);
}

bool cord_list_render_devices(struct cord_audio_device_list *out)
{
	return list_devices(eRender, out);
}

void cord_audio_device_list_free(struct cord_audio_device_list *list)
{
	bfree(list->items);
	list->items = nullptr;
	list->count = 0;
}

struct cord_micsend *cord_micsend_create(const char *capture_id,
					 const char *render_id)
{
	auto *ms = new cord_micsend();
	ms->capture_id = capture_id ? capture_id : "";
	ms->render_id = render_id ? render_id : "";

	ms->worker = std::thread([ms] {
		os_set_thread_name("cord: mic send");
		ms->run();
	});

	return ms;
}

void cord_micsend_destroy(struct cord_micsend *ms)
{
	if (!ms)
		return;

	ms->stop = true;
	if (ms->worker.joinable())
		ms->worker.join();

	delete ms;
}

void cord_micsend_set_gain(struct cord_micsend *ms, float gain)
{
	if (ms)
		ms->gain = gain;
}

void cord_micsend_set_muted(struct cord_micsend *ms, bool muted)
{
	if (ms)
		ms->muted = muted;
}

float cord_micsend_get_peak(struct cord_micsend *ms)
{
	return ms ? ms->peak.load() : 0.0f;
}

bool cord_micsend_is_running(const struct cord_micsend *ms)
{
	return ms && ms->running.load();
}

void cord_micsend_test_tone(struct cord_micsend *ms, int ms_duration)
{
	if (!ms)
		return;
	/* Capture rate is unknown here; 48k is the near-universal default. */
	ms->tone_frames_remaining = ms_duration * 48;
}

} // extern "C"
