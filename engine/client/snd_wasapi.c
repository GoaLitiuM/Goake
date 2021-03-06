#include "quakedef.h"

#if defined(AVAIL_WASAPI) && !defined(SERVERONLY)
//wasapi is nice in that you can use it to bypass the windows audio mixer. hurrah for exclusive audio.
//this should give slightly lower latency audio.
//its otherwise not that interesting.

//side note: wasapi does provide proper notifications for when a sound device is enabled/disabled, which is useful even if you're using directsound instead.
//this means that we can finally restart the audio if someone plugs in a headset.
#include "winquake.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#define AUDIODRIVERNAME "WASAPI"

static qboolean wasapi_release_pending = qfalse; // ugh, needed to avoid restarting the device multiple times after device changes
static LPWSTR wasapi_output_device_name; // currently active output device, used to filter device changes

#define REFTIMES_PER_SEC  10000000

#define FORCE_DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        const GUID DECLSPEC_SELECTANY name \
                = { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }
#define FORCE_DEFINE_PROPERTYKEY(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8, pid) const PROPERTYKEY name = { { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }, pid }

FORCE_DEFINE_GUID(CLSID_MMDeviceEnumerator,			0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
FORCE_DEFINE_GUID(IID_IMMDeviceEnumerator,			0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
FORCE_DEFINE_GUID(IID_IAudioClient,					0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
FORCE_DEFINE_GUID(IID_IAudioClient3,				0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
FORCE_DEFINE_GUID(IID_IAudioRenderClient,			0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

FORCE_DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM,			0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
FORCE_DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,	0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

static cvar_t wasapi_forcerate		= CVARD("wasapi_forcerate", "0", "Attempts to force snd_khz instead of using the system's default channel count.\nFor this to work, you will need to set wasapi_exclusive 1");
static cvar_t wasapi_forcechannels	= CVARD("wasapi_forcechannels", "1", "Attempts to force snd_numchannels instead of using the system's default channel count.");
static cvar_t wasapi_exclusive		= CVARD("wasapi_exclusive", "0", "When set, attempts to take exclusive control of the output device, to the detriment of other programs (causing errors or even crashes in them).\nExclusive mode leaves the game free to change the hardware's playback details, instead of being required to use a single system-wide 'mixer' rate.\nIt should also reduce latency a little.");
static cvar_t wasapi_buffersize		= CVARD("wasapi_buffersize", "0.01", "Internal buffer size in seconds used for WASAPI output.\nFor this to work, you will need to set wasapi_exclusive 1");
extern cvar_t _snd_mixahead;
static void QDECL WASAPI_RegisterCvars(void)
{
	Cvar_Register(&wasapi_forcerate, "WASAPI audio output");
	Cvar_Register(&wasapi_forcechannels, "WASAPI audio output");
	Cvar_Register(&wasapi_exclusive, "WASAPI audio output");
	Cvar_Register(&wasapi_buffersize, "WASAPI audio output");
}

static void *WASAPI_Lock(soundcardinfo_t *sc, unsigned int *startoffset)
{
	*startoffset = 0;
	return sc->sn.buffer;
}
static void WASAPI_Unlock(soundcardinfo_t *sc, void *buffer)
{
	//no need to do anything
}
static void WASAPI_Submit(soundcardinfo_t *sc, int start, int end)
{
	//submit happens outside the mixer
}
static unsigned int WASAPI_GetDMAPos(soundcardinfo_t *sc)
{
	sc->sn.samplepos = sc->snd_sent;
	return sc->sn.samplepos;
}
static void WASAPI_Shutdown(soundcardinfo_t *sc)
{
	sc->Shutdown = NULL;
	Sys_WaitOnThread(sc->thread);
	sc->thread = NULL;
}

static qboolean WASAPI_AcceptableFormat(soundcardinfo_t *sc, IAudioClient *dev, WAVEFORMATEX *pwfx, qboolean isexclusive)
{
	if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && !memcmp(&((WAVEFORMATEXTENSIBLE*)pwfx)->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(GUID)) && pwfx->wBitsPerSample == 32)
	{	//oo, floating point audio. I guess this means we can have fun with clamping, right?
		sc->sn.samplebytes = 4;
		sc->sn.sampleformat = QSF_F32;
	}
	else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && memcmp(&((WAVEFORMATEXTENSIBLE*)pwfx)->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM, sizeof(GUID)))
	{
		Con_Printf("WASAPI: unsupported sample type\n");
		return false;	//we only support pcm / floats
	}
	else if (pwfx->wBitsPerSample == 8)
	{
		sc->sn.samplebytes = 1;
		sc->sn.sampleformat = QSF_U8;
	}
	else if (pwfx->wBitsPerSample == 16)
	{
		sc->sn.samplebytes = 2;
		sc->sn.sampleformat = QSF_S16;
	}
	else
	{
		Con_Printf("WASAPI: unsupported sample size\n");
		return false;	//unsupported bit formats
	}

	if (pwfx->nChannels > MAXSOUNDCHANNELS)
	{
		Con_Printf("WASAPI: too many channels\n");
		return false;
	}

	sc->sn.numchannels = pwfx->nChannels;
	sc->sn.speed = pwfx->nSamplesPerSec;

	Con_Printf("WASAPI: %i channel %ibit %ukhz%s\n", sc->sn.numchannels, pwfx->wBitsPerSample, (unsigned int)pwfx->nSamplesPerSec, isexclusive?" exclusive":" non-exclusive");
	return true;
}
static qboolean WASAPI_DetermineFormat(soundcardinfo_t *sc, IAudioClient *dev, qboolean exclusive, WAVEFORMATEX **ret)
{
	WAVEFORMATEX *pwfx;

	if (!SUCCEEDED(dev->lpVtbl->GetMixFormat(dev, &pwfx)))
		return false;

	if (snd_speed || wasapi_forcerate.ival)
	{	//if some other driver has already committed us to a set rate, we need to drive wasapi at that rate too.
		//this may cause failures later in this function.
		Con_Printf("WASAPI: overriding sampler rate\n");
		pwfx->nSamplesPerSec = snd_speed;
		pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
	}

	if (wasapi_forcechannels.ival)
	{
		Con_DPrintf("WASAPI: overriding channels\n");

		if (sc->sn.numchannels >= 8)
		{
			pwfx->nChannels = 8;
			if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				((WAVEFORMATEXTENSIBLE*)pwfx)->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER;
		}
		if (sc->sn.numchannels >= 6)
		{
			pwfx->nChannels = 6;
			if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				((WAVEFORMATEXTENSIBLE*)pwfx)->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT;
		}
		else if (sc->sn.numchannels >= 4)
		{
			pwfx->nChannels = 4;
			if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				((WAVEFORMATEXTENSIBLE*)pwfx)->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT;
		}
		else if (sc->sn.numchannels >= 2)
		{
			pwfx->nChannels = 2;
			if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				((WAVEFORMATEXTENSIBLE*)pwfx)->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
		}
		else
		{
			pwfx->nChannels = 1;
			if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				((WAVEFORMATEXTENSIBLE*)pwfx)->dwChannelMask = SPEAKER_FRONT_CENTER;
		}
		pwfx->nBlockAlign = pwfx->wBitsPerSample/8 * pwfx->nChannels;
		pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
	}

	if (!exclusive)
	{
		WAVEFORMATEX *pwfx2;
		if (SUCCEEDED(dev->lpVtbl->IsFormatSupported(dev, exclusive?AUDCLNT_SHAREMODE_EXCLUSIVE:AUDCLNT_SHAREMODE_SHARED, pwfx, &pwfx2)))
		{
			if (pwfx2)
			{
				CoTaskMemFree(pwfx);
				pwfx = pwfx2;
			}
		}
		if (!WASAPI_AcceptableFormat(sc, dev, pwfx, false))
			return false;
	}
	else
	{
		if (FAILED(dev->lpVtbl->IsFormatSupported(dev, exclusive?AUDCLNT_SHAREMODE_EXCLUSIVE:AUDCLNT_SHAREMODE_SHARED, pwfx, NULL)))
		{
			//try to switch over to 16bit pcm
			pwfx->wBitsPerSample = 16;
			pwfx->nBlockAlign = pwfx->wBitsPerSample/8 * pwfx->nChannels;
			pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
			if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				((WAVEFORMATEXTENSIBLE*)pwfx)->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
				((WAVEFORMATEXTENSIBLE*)pwfx)->Samples.wValidBitsPerSample = pwfx->wBitsPerSample;
			}

			if (FAILED(dev->lpVtbl->IsFormatSupported(dev, exclusive?AUDCLNT_SHAREMODE_EXCLUSIVE:AUDCLNT_SHAREMODE_SHARED, pwfx, NULL)))
			{
				//fixme: try float audio...

				//try to switch over to 24bit pcm (although with no more 16bit audio)
/*				pwfx->wBitsPerSample = 24;
				pwfx->nBlockAlign = pwfx->wBitsPerSample/8 * pwfx->nChannels;
				pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;
				if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
					((WAVEFORMATEXTENSIBLE*)pwfx)->Samples.wValidBitsPerSample = 16;
				if (FAILED(dev->lpVtbl->IsFormatSupported(dev, exclusive?AUDCLNT_SHAREMODE_EXCLUSIVE:AUDCLNT_SHAREMODE_SHARED, pwfx, NULL)))
*/
				{
					Con_Printf("WASAPI: IsFormatSupported failed\n");
					return false;
				}
			}
		}
		if (!WASAPI_AcceptableFormat(sc, dev, pwfx, true))
			return false;
	}

	*ret = pwfx;
	return true;
}

static IMMDevice *WASAPI_GetDevice(soundcardinfo_t *sc)
{
	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDevice *pDevice = NULL;
	CoInitialize(NULL);	//sigh.
	if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&pEnumerator)))
	{
		if (*sc->name)
		{
			WCHAR wname[256];
			pEnumerator->lpVtbl->GetDevice(pEnumerator, widen(wname, sizeof(wname), sc->name), &pDevice);
		}
		else
			pEnumerator->lpVtbl->GetDefaultAudioEndpoint(pEnumerator, eRender, eConsole, &pDevice);
		pEnumerator->lpVtbl->Release(pEnumerator);
	}
	return pDevice;
}

/*I HATE C++ APIS! THEY'RE ANNOYING AS HELL*/
static void WASAPI_DeviceChanged(void *ctx, void *data, size_t a, size_t b)
{
	S_EnumerateDevices();
	if (data)
	{
		char *msg = data;
		Con_Printf("%s", msg);
	}
	Cbuf_AddText("\nsnd_restart\n", RESTRICT_LOCAL);
}
static HRESULT	STDMETHODCALLTYPE WASAPI_Notifications_QueryInterface(IMMNotificationClient * This,REFIID riid,void **ppvObject)	{*ppvObject = NULL;return E_NOINTERFACE;}
static ULONG	STDMETHODCALLTYPE WASAPI_Notifications_AddRef(IMMNotificationClient * This)											{return 1;}
static ULONG	STDMETHODCALLTYPE WASAPI_Notifications_Release(IMMNotificationClient * This)										{return 1;}
static HRESULT	STDMETHODCALLTYPE WASAPI_Notifications_OnDeviceStateChanged(IMMNotificationClient * This,LPCWSTR pwstrDeviceId,DWORD dwNewState)
{
	if (wcscmp(pwstrDeviceId, wasapi_output_device_name) != 0)
		return S_OK;
		
	if (!wasapi_release_pending)
	{
		COM_AddWork(WG_MAIN, WASAPI_DeviceChanged, NULL, NULL, 0, 0);
		wasapi_release_pending = qtrue;
	}
	return S_OK;
}
static HRESULT	STDMETHODCALLTYPE WASAPI_Notifications_OnDeviceAdded(IMMNotificationClient * This,LPCWSTR pwstrDeviceId)
{
	if (wcscmp(pwstrDeviceId, wasapi_output_device_name) != 0)
		return S_OK;
		
	if (!wasapi_release_pending)
	{
		COM_AddWork(WG_MAIN, WASAPI_DeviceChanged, NULL, NULL, 0, 0);
		wasapi_release_pending = qtrue;
	}
	return S_OK;
}
static HRESULT	STDMETHODCALLTYPE WASAPI_Notifications_OnDeviceRemoved(IMMNotificationClient * This,LPCWSTR pwstrDeviceId)
{
	if (wcscmp(pwstrDeviceId, wasapi_output_device_name) != 0)
		return S_OK;
	
	if (!wasapi_release_pending)
	{
		COM_AddWork(WG_MAIN, WASAPI_DeviceChanged, NULL, NULL, 0, 0);
		wasapi_release_pending = qtrue;
	}
	return S_OK;
}
static HRESULT	STDMETHODCALLTYPE WASAPI_Notifications_OnDefaultDeviceChanged(IMMNotificationClient * This,EDataFlow flow,ERole role,LPCWSTR pwstrDefaultDeviceId)
{
	if (flow == eRender && role == eCommunications)
		return S_OK; // we don't use the communication device at this point, so ignore this change
		
	if (!wasapi_release_pending)
	{
		COM_AddWork(WG_MAIN, WASAPI_DeviceChanged, NULL, "Default audio device changed. Restarting audio.\n", 0, 0);
		wasapi_release_pending = qtrue;
	}
	return S_OK;
}
static HRESULT	STDMETHODCALLTYPE WASAPI_Notifications_OnPropertyValueChanged(IMMNotificationClient * This,LPCWSTR pwstrDeviceId,const PROPERTYKEY key)
{
	if (wcscmp(pwstrDeviceId, wasapi_output_device_name) != 0)
		return S_OK;
	
	if (!wasapi_release_pending)
	{
		COM_AddWork(WG_MAIN, WASAPI_DeviceChanged, NULL, NULL, 0, 0);
		wasapi_release_pending = qtrue;
	}
	return S_OK;
}
static CONST_VTBL IMMNotificationClientVtbl WASAPI_NotificationsVtbl =
{
	WASAPI_Notifications_QueryInterface,
	WASAPI_Notifications_AddRef,
	WASAPI_Notifications_Release,
	WASAPI_Notifications_OnDeviceStateChanged,
	WASAPI_Notifications_OnDeviceAdded,
	WASAPI_Notifications_OnDeviceRemoved,
	WASAPI_Notifications_OnDefaultDeviceChanged,
	WASAPI_Notifications_OnPropertyValueChanged
};
static IMMNotificationClient WASAPI_Notifications =
{
	&WASAPI_NotificationsVtbl
};

static int WASAPI_Thread(void *arg)
{
	soundcardinfo_t *sc = arg;
	qboolean inited = false;

	IAudioClient *pAudioClient = NULL;
#if _WIN32_WINNT >= 0x0A00 // Windows 10 (_WIN32_WINNT_WIN10)
	IAudioClient3 *pAudioClient3 = NULL;
#endif
	IAudioRenderClient *pRenderClient = NULL;
	IMMDeviceEnumerator *pEnumerator = NULL;
	UINT32 bufferFrameCount = 0;
	HANDLE hEvent = NULL;
	WAVEFORMATEX *pwfx = NULL;

	qboolean exclusive = wasapi_exclusive.ival;

	void *cond = sc->handle;

	//main thread will wait for us to finish initing, so lets do that...
	IMMDevice *pDevice = WASAPI_GetDevice(sc);
	if (pDevice)
	{
		pDevice->lpVtbl->GetId(pDevice, &wasapi_output_device_name);
		
		UINT32 periodicity_in_frames = 0;
		HRESULT hr;
#if _WIN32_WINNT >= 0x0A00
		if (!exclusive && SUCCEEDED(pDevice->lpVtbl->Activate(pDevice, &IID_IAudioClient3, CLSCTX_ALL, NULL, (void**)&pAudioClient3)))
			hr = pAudioClient3->lpVtbl->QueryInterface(pAudioClient3, &IID_IAudioClient, (void**)&pAudioClient);
		else
#endif
		{
			hr = pDevice->lpVtbl->Activate(pDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
		}

		if (FAILED(hr))
		{
			Con_Printf("WASAPI: failed to activate audio client\n");
		}
		else if (!WASAPI_DetermineFormat(sc, pAudioClient, exclusive, &pwfx))
		{
			Con_Printf("WASAPI: unable to determine mutually supported audio format\n");
		}
		else if (sc->sn.samplebytes && (!snd_speed || sc->sn.speed == snd_speed))
		{
#if _WIN32_WINNT >= 0x0A00
			if (pAudioClient3)
			{
				UINT32 default_period;
		        UINT32 fundamental_period;
		        UINT32 min_period;
		        UINT32 max_period;

		        AudioClientProperties properties = { 0 };
		        properties.cbSize = sizeof(AudioClientProperties);
		        properties.Options = AUDCLNT_STREAMOPTIONS_NONE;

		        if (SUCCEEDED(IAudioClient2_SetClientProperties(pAudioClient3, &properties)) &&
		            SUCCEEDED(IAudioClient3_GetSharedModeEnginePeriod(pAudioClient3, pwfx,
		                &default_period, &fundamental_period, &min_period, &max_period)))
		        {
		            periodicity_in_frames = fundamental_period * (UINT32)(pwfx->nSamplesPerSec *
		                _snd_mixahead.value / 2 / fundamental_period);

		            if (periodicity_in_frames > max_period)
					{
		               	pAudioClient3->lpVtbl->Release(pAudioClient3);
		                pAudioClient3 = NULL;
		            }
					else if (periodicity_in_frames < min_period)
						periodicity_in_frames = min_period;

					Con_DPrintf("WASAPI: requested periodicity: %d, default: %d, min: %d, max: %d, step: %d\n", periodicity_in_frames, default_period, min_period, max_period, fundamental_period);
		        }
				else
				{
		            pAudioClient3->lpVtbl->Release(pAudioClient3);
		            pAudioClient3 = NULL;
				}
			}

			if (pAudioClient3)
			{
				hr = IAudioClient3_InitializeSharedAudioStream(pAudioClient3,
	            	AUDCLNT_STREAMFLAGS_EVENTCALLBACK, periodicity_in_frames, pwfx, NULL);
				if (hr == AUDCLNT_E_ENGINE_PERIODICITY_LOCKED)
				{
					IAudioClient3_GetCurrentSharedModeEnginePeriod(pAudioClient3, &pwfx, &periodicity_in_frames);
					Con_Printf("WASAPI: Periodicity locked to %d\n", periodicity_in_frames);
					hr = IAudioClient3_InitializeSharedAudioStream(pAudioClient3,
	            		AUDCLNT_STREAMFLAGS_EVENTCALLBACK, periodicity_in_frames, pwfx, NULL);
				}

				if (SUCCEEDED(hr))
		        {
					Con_Printf("WASAPI: Low latency mode enabled\n");
		        }
				else
				{
					pAudioClient3->lpVtbl->Release(pAudioClient3);
	            	pAudioClient3 = NULL;
				}
			}

			if (!pAudioClient3 && pAudioClient)
#endif
			{
				REFERENCE_TIME buffersize, minBuffersize, defaultBuffersize;

				// minimum buffer size is used with exclusive mode, default is the recommended (minimum) buffer size for shared mode
				pAudioClient->lpVtbl->GetDevicePeriod(pAudioClient, &defaultBuffersize, &minBuffersize);

				if (exclusive)
				{
					buffersize = REFTIMES_PER_SEC * wasapi_buffersize.value;
					if (buffersize < minBuffersize)
						buffersize = minBuffersize;
				}
				else
				{
					buffersize = REFTIMES_PER_SEC * _snd_mixahead.value;
					if (buffersize < defaultBuffersize)
						buffersize = defaultBuffersize;
				}

				hr = pAudioClient->lpVtbl->Initialize(pAudioClient, exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffersize, (exclusive?buffersize:0), pwfx, NULL);

				if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
				{	//this is stupid, but does what the documentation says should be done.
					if (SUCCEEDED(pAudioClient->lpVtbl->GetBufferSize(pAudioClient, &bufferFrameCount)))
					{
						if (pAudioClient)
							pAudioClient->lpVtbl->Release(pAudioClient);
						pAudioClient = NULL;
						if (SUCCEEDED(pDevice->lpVtbl->Activate(pDevice, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient)))
						{
							buffersize = (REFERENCE_TIME)((10000.0 * 1000 / pwfx->nSamplesPerSec * bufferFrameCount) + 0.5);
							hr = pAudioClient->lpVtbl->Initialize(pAudioClient, exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffersize, (exclusive?buffersize:0), pwfx, NULL);
						}
					}
				}
			}

			if (SUCCEEDED(hr))
			{
				hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
				if (hEvent)
				{
					pAudioClient->lpVtbl->SetEventHandle(pAudioClient, hEvent);
					if (SUCCEEDED(pAudioClient->lpVtbl->GetBufferSize(pAudioClient, &bufferFrameCount)) &&
						SUCCEEDED(pAudioClient->lpVtbl->GetService(pAudioClient, &IID_IAudioRenderClient, (void**)&pRenderClient)))
					{
						inited = true;
						Con_DPrintf("WASAPI: buffer size: %d\n", bufferFrameCount);
					}
				}
			}
			else
			{
				switch(hr)
				{
				case AUDCLNT_E_UNSUPPORTED_FORMAT:
					Con_Printf("WASAPI Initialize: AUDCLNT_E_UNSUPPORTED_FORMAT\n");
					break;
				case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
					Con_Printf("WASAPI Initialize: AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED\n");
					break;
				case AUDCLNT_E_EXCLUSIVE_MODE_ONLY:
					Con_Printf("WASAPI Initialize: AUDCLNT_E_EXCLUSIVE_MODE_ONLY\n");
					break;
				case AUDCLNT_E_DEVICE_IN_USE:
					Con_Printf("WASAPI Initialize: AUDCLNT_E_DEVICE_IN_USE\n");
					break;
				case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:
					Con_Printf("WASAPI Initialize: AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED\n");
					break;
				case E_INVALIDARG:
					Con_Printf("WASAPI Initialize: E_INVALIDARG\n");
					break;
				default:
					Con_Printf("pAudioClient->lpVtbl->Initialize failed (%x)\n", (unsigned int)hr);
				}
			}
#if _WIN32_WINNT >= 0x0A00
			if (pAudioClient3)
			{
				pAudioClient3->lpVtbl->Release(pAudioClient3);
	            pAudioClient3 = NULL;
			}
#endif
		}

		if (pwfx)
			CoTaskMemFree(pwfx);

		pDevice->lpVtbl->Release(pDevice);
		pDevice = NULL;
	}

	if (inited)
		sc->Shutdown	= WASAPI_Shutdown;
	else
		Con_Printf("Unable to initialise WASAPI\n");
	sc->Lock		= WASAPI_Lock;
	sc->Unlock		= WASAPI_Unlock;
	sc->Submit		= WASAPI_Submit;
	sc->GetDMAPos	= WASAPI_GetDMAPos;

	//wake up the main thread now that we know if it worked.
	Sys_ConditionSignal(cond);

	//extra crap to get the OS to favour waking us up on demand.
	{
		HANDLE (WINAPI *pAvSetMmThreadCharacteristics)(LPCTSTR TaskName, LPDWORD TaskIndex);
		dllfunction_t funcs[] = {{(void*)&pAvSetMmThreadCharacteristics, "AvSetMmThreadCharacteristicsA"}, {NULL}};
		DWORD taskIndex = 0;

		if (Sys_LoadLibrary("avrt.dll", funcs))
			pAvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
	}
	
	if (inited)
	{
		if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&pEnumerator)))
		{
			pEnumerator->lpVtbl->RegisterEndpointNotificationCallback(pEnumerator, &WASAPI_Notifications);
		}
	}

	while(sc->Shutdown != NULL)
	{
		UINT32 numFramesPadding = 0;
		if (exclusive || SUCCEEDED(pAudioClient->lpVtbl->GetCurrentPadding(pAudioClient, &numFramesPadding)))
		{
			INT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
			BYTE *pData;
			if (numFramesAvailable > 0 && SUCCEEDED(pRenderClient->lpVtbl->GetBuffer(pRenderClient, numFramesAvailable, &pData)))
			{
				sc->sn.buffer = pData;
				sc->sn.samples = numFramesAvailable * sc->sn.numchannels;
				sc->samplequeue = sc->sn.samples;
				S_MixerThread(sc);
				sc->snd_sent += numFramesAvailable * sc->sn.numchannels;

				pRenderClient->lpVtbl->ReleaseBuffer(pRenderClient, numFramesAvailable, 0);
			}
		}

		if (inited)
		{
			pAudioClient->lpVtbl->Start(pAudioClient);
			inited = false;
		}

		if (hEvent && WaitForSingleObject(hEvent, 2000) != WAIT_OBJECT_0)
		{
			Con_Printf("WASAPI timeout\n");
			break;
		}
	}
	
	if (pEnumerator)
	{
		pEnumerator->lpVtbl->UnregisterEndpointNotificationCallback(pEnumerator, &WASAPI_Notifications);
		pEnumerator->lpVtbl->Release(pEnumerator);
		wasapi_release_pending = qfalse;
		//pEnumerator = NULL;
	}
	if (wasapi_output_device_name)
	{
		CoTaskMemFree(wasapi_output_device_name);
		wasapi_output_device_name = NULL;
	}
		
	if (inited)
		pAudioClient->lpVtbl->Stop(pAudioClient);
	if (pRenderClient)
		pRenderClient->lpVtbl->Release(pRenderClient);
	if (pAudioClient)
		pAudioClient->lpVtbl->Release(pAudioClient);
	return 0;
}

static qboolean QDECL WASAPI_InitCard (soundcardinfo_t *sc, const char *cardname)
{
	void *cond;
	Q_strncpyz(sc->name, cardname?cardname:"", sizeof(sc->name));

	sc->selfpainting = true;
	sc->handle = cond = Sys_CreateConditional();
	Sys_LockConditional(cond);
	sc->thread = Sys_CreateThread("wasapimixer", WASAPI_Thread, sc, THREADP_NORMAL, 0);
	if (!sc->thread)
	{
		Con_Printf ("Unable to create sound mixing thread\n");
		return false;
	}
//	MessageBox(0,"main thread waiting", "...", 0);

	//wait for the thread to finish (along with all its error con printfs etc
	if (!Sys_ConditionWait(cond))
		Con_Printf ("Looks like the sound thread isn't starting up\n");
	Sys_UnlockConditional(cond);
	Sys_DestroyConditional(cond);
	COM_MainThreadWork();	//flush any prints from the worker thread, so that things make sense

	if (sc->Shutdown == NULL)
	{
		Sys_WaitOnThread(sc->thread);
		sc->thread = NULL;
		return false;
	}
	return true;
}

static qboolean QDECL WASAPI_Enumerate (void (QDECL *callback) (const char *drivername, const char *devicecode, const char *readablename))
{
	FORCE_DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName,           0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);    // DEVPROP_TYPE_STRING

	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDeviceCollection *pCollection = NULL;
	CoInitialize(NULL);
	if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&pEnumerator)))
	{
		if (SUCCEEDED(pEnumerator->lpVtbl->EnumAudioEndpoints(pEnumerator, eRender, DEVICE_STATE_ACTIVE, &pCollection)))
		{
			IMMDevice *pEndpoint;
			IPropertyStore *pProps;
			LPWSTR pwszID;
			UINT count, i;
			if (FAILED(pCollection->lpVtbl->GetCount(pCollection, &count)))
				count = 0;
			for (i = 0; i < count; i++)
			{
				if (SUCCEEDED(pCollection->lpVtbl->Item(pCollection, i, &pEndpoint)))
				{
					if (SUCCEEDED(pEndpoint->lpVtbl->GetId(pEndpoint, &pwszID)))
					{
						if (SUCCEEDED(pEndpoint->lpVtbl->OpenPropertyStore(pEndpoint, STGM_READ, &pProps)))
						{
							PROPVARIANT varName;
							PropVariantInit(&varName);
							if (SUCCEEDED(pProps->lpVtbl->GetValue(pProps, &PKEY_Device_FriendlyName, &varName)))
							{
								char nicename[256];
								char internalname[256];
								strcpy(nicename, AUDIODRIVERNAME ": ");
								narrowen(nicename+strlen(AUDIODRIVERNAME)+2, sizeof(nicename)-(strlen(AUDIODRIVERNAME)+2), varName.pwszVal);
								narrowen(internalname, sizeof(internalname), pwszID);
								callback(AUDIODRIVERNAME, internalname, nicename);
							}
							PropVariantClear(&varName);
							pProps->lpVtbl->Release(pProps);
						}
						CoTaskMemFree(pwszID);
					}
					pEndpoint->lpVtbl->Release(pEndpoint);
				}
			}

			pCollection->lpVtbl->Release(pCollection);
		}
		
		pEnumerator->lpVtbl->Release(pEnumerator);
		return true;
	}
	return true;	//if we couldn't enumerate stuff, we won't be able to initialise anything anyway, so there's no point in doing any default device crap
}

sounddriver_t WASAPI_Output =
{
	AUDIODRIVERNAME,
	WASAPI_InitCard,
	WASAPI_Enumerate,
	WASAPI_RegisterCvars
};

#endif
