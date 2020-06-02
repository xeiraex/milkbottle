#include <deque>
#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include "wa_ipc.h"
#include "vis.h"
#include "api.h"
#include "resource.h"

#include "WWMFResampler.h"
#include "WWUtil.h"

#define LOG(format, ...) \
{ \
	wchar_t buffer[2048]; \
	swprintf_s(buffer, _countof(buffer), format L"\n", __VA_ARGS__); \
	OutputDebugStringW(buffer); \
}
#define ERR(format, ...) LOG(L"Error: " format, __VA_ARGS__)

#define CBCLASS LanguageService
class LanguageService : public api_language {
public:
	char *GetString(HINSTANCE hinst, HINSTANCE owner, UINT uID, char *str = NULL, size_t maxlen = 0) {
		if (str) {
			LoadStringA(owner, uID, str, maxlen);
			return str;
		}
		return "NULL";
	}
	wchar_t *GetStringW(HINSTANCE hinst, HINSTANCE owner, UINT uID, wchar_t *str = NULL, size_t maxlen = 0)	{
		static wchar_t buffer[512];
		if (!str) {
			str = buffer;
			maxlen = 512;
		}
		if (str) {
			LoadStringW(owner, uID, str, maxlen);
			return str;
		}
		return L"NULL";
	}
	HMENU LoadLMenu(HINSTANCE localised, HINSTANCE original, UINT id) {
		return ::LoadMenuA(original, (LPCSTR)id);
	}
	INT_PTR LDialogBoxParamW(HINSTANCE localised, HINSTANCE original, UINT id, HWND parent, DLGPROC proc, LPARAM param) {
		return DialogBoxParamW(original, MAKEINTRESOURCEW(id), parent, proc, param);
	}
	HWND CreateLDialogParamW(HINSTANCE localised, HINSTANCE original, UINT id, HWND parent, DLGPROC proc, LPARAM param) {
		return CreateDialogParamW(original, MAKEINTRESOURCEW(id), parent, proc, param);
	}
	START_DISPATCH
		CB(API_LANGUAGE_GETSTRING, GetString)
		CB(API_LANGUAGE_GETSTRINGW, GetStringW)
		CB(API_LANGUAGE_LOADLMENU, LoadLMenu)
		CB(API_LANGUAGE_LDIALOGBOXPARAMW, LDialogBoxParamW)
		CB(API_LANGUAGE_CREATELDIALOGPARAMW, CreateLDialogParamW)
		END_DISPATCH
};
static LanguageService languageService;
#undef CBCLASS

#define CBCLASS ApplicationService
class ApplicationService : public api_application {
public:
	START_DISPATCH
		END_DISPATCH
};
static ApplicationService applicationService;
#undef CBCLASS

#define CBCLASS SyscbService
class SyscbService : public api_syscb {
public:
	START_DISPATCH
		END_DISPATCH
};
static SyscbService syscbService;
#undef CBCLASS

#define CBCLASS FallbackService
class FallbackService : public Dispatchable {
public:
	START_DISPATCH
		END_DISPATCH
};
static FallbackService fallbackService;
#undef CBCLASS

#define CBCLASS ServiceFactory
class ServiceFactory : public waServiceFactory {
public:
	ServiceFactory(void *x) : x(x) {
	}
	void *getInterface(int global_lock) {
		return x;
	}
	START_DISPATCH
		CB(WASERVICEFACTORY_GETINTERFACE, getInterface)
		END_DISPATCH
		void *x;
};
static ServiceFactory languageServiceFactory(&languageService);
static ServiceFactory applicationServiceFactory(&applicationService);
static ServiceFactory syscbServiceFactory(&syscbService);
static ServiceFactory fallbackServiceFactory(&fallbackService);
#undef CBCLASS

#define CBCLASS ApiService
class ApiService : public api_service {
public:
	waServiceFactory *service_getServiceByGuid(GUID guid) {
		if (guid == languageApiGUID)
			return &languageServiceFactory;
		if (guid == applicationApiServiceGuid)
			return &applicationServiceFactory;
		if (guid == syscbApiServiceGuid)
			return &syscbServiceFactory;
		return &fallbackServiceFactory;
	}
	START_DISPATCH
		CB(API_SERVICE_SERVICE_GETSERVICEBYGUID, service_getServiceByGuid)
		END_DISPATCH
};
static ApiService apiService;
#undef CBCLASS

#define ID_START 1
#define ID_STOP 2
#define ID_CONFIG 3
#define ID_EXIT 4
#define STATE_RUNNING 1
#define STATE_STOPPED 2
#define STATE_CONFIG 3
#define STATE_EXIT 4

HWND mainWindow;
HMENU hMenu;
winampVisModule *milkdropModule;
LPWSTR noAudioString = L"No Suitable Device or Resampler Missing";
LPWSTR audioDeviceName = noAudioString;
int state = STATE_RUNNING;
int previousState = STATE_RUNNING;
bool deviceChanged;
bool noAudio;

BYTE* chunk = new BYTE[2*576];

// For handling messages expected to go to Winamp.
LRESULT WINAPI WinampWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_WA_IPC:
		switch (lParam) {
		case IPC_ISPLAYING:
			return true;
			break;
		case IPC_GETLISTPOS:
			return 0;
			break;
		case IPC_GETPLAYLISTTITLEW:
			return (LRESULT) audioDeviceName;
			break;
		case IPC_GET_API_SERVICE:
			return (LPARAM) &apiService;
			break;
		default:
			LOG(L"Unsupported Winamp IPC: %d", lParam);
			return 0;
			break;
		}
		break;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

LRESULT WINAPI MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_USER + 1:
		if (lParam == WM_LBUTTONUP) {
			if (state == STATE_STOPPED)
				state = STATE_RUNNING;
			else if (state == STATE_RUNNING)
				state = STATE_STOPPED;
		}
		else if (lParam == WM_RBUTTONUP && state != STATE_CONFIG) {
			POINT pt;
			GetCursorPos(&pt);
			HMENU hMenu = CreatePopupMenu();
			InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_EXIT, "Quit");
			InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_CONFIG, "Configure");
			if (state == STATE_RUNNING)
				InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_STOP, "Stop");
			else if (state == STATE_STOPPED)
				InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_START, "Start");
			TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
		}
		break;
	case WM_COMMAND:
		switch (wParam) {
		case LOWORD(ID_CONFIG):
			if (state != STATE_CONFIG && state != STATE_EXIT) {
				previousState = state;
				state = STATE_CONFIG;
			}
			break;
		case LOWORD(ID_START):
			if (state == STATE_STOPPED)
				state = STATE_RUNNING;
			break;
		case LOWORD(ID_STOP):
			if (state == STATE_RUNNING)
				state = STATE_STOPPED;
			break;
		case LOWORD(ID_EXIT):
			state = STATE_EXIT;
			break;
		}
		break;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

class MMNotificationClient : public IMMNotificationClient {
public:
	ULONG STDMETHODCALLTYPE AddRef() {
		return 0;
	}
	ULONG STDMETHODCALLTYPE Release() {
		return 0;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) {
		deviceChanged = true;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
		deviceChanged = true;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) {
		deviceChanged = true;
		return S_OK;
	}
};

long audioLoop(IMMDeviceEnumerator *pMMDeviceEnumerator, bool loopback) {
	HRESULT hr = S_OK;
	noAudio = false;

	IMMDevice *m_pMMDevice = NULL;
	IPropertyStore *pPropertyStore = NULL;
	PROPVARIANT pv;
	IAudioClient *pAudioClient = NULL;
	WAVEFORMATEX *pwfx = NULL;
	IAudioCaptureClient *pAudioCaptureClient = NULL;

	WWMFPcmFormat inputFormat;
	WWMFPcmFormat outputFormat;
	WWMFResampler resampler;
	WWMFSampleData sampleData;

	MSG msg;
	msg.message = WM_NULL;
	UINT32 nPasses = 0;
	UINT32 pnFrames = 0;
	UINT32 nNextPacketSize;
	BYTE *pData;
	UINT32 nNumFramesToRead;
	DWORD dwFlags;
	std::deque<byte> bufferLeft;
	std::deque<byte> bufferRight;

	hr = pMMDeviceEnumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &m_pMMDevice);
	if (FAILED(hr)) {
		ERR(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	if (S_FALSE == hr) {
		noAudio = true;
		goto cleanup;
	}

	hr = m_pMMDevice->OpenPropertyStore(STGM_READ, &pPropertyStore);
	if (FAILED(hr)) {
		ERR(L"IMMDevice::OpenPropertyStore failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	PropVariantInit(&pv);
	hr = pPropertyStore->GetValue(PKEY_Device_FriendlyName, &pv);
	if (FAILED(hr)) {
		ERR(L"IPropertyStore::GetValue failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	if (VT_LPWSTR != pv.vt) {
		ERR(L"PKEY_Device_FriendlyName variant type is %u - expected VT_LPWSTR", pv.vt);
		noAudio = true;
		goto cleanup;
	}

	audioDeviceName = pv.pwszVal;

	hr = m_pMMDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
	if (FAILED(hr)) {
		ERR(L"IMMDevice::Activate(IAudioClient) failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	hr = pAudioClient->GetMixFormat(&pwfx);
	if (FAILED(hr)) {
		ERR(L"IAudioClient::GetMixFormat failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	LOG(L"Stream Sample Rate: %u", pwfx->nSamplesPerSec);

	inputFormat.sampleFormat = WWMFBitFormatFloat;
	inputFormat.nChannels = pwfx->nChannels;
	inputFormat.sampleRate = pwfx->nSamplesPerSec;
	inputFormat.bits = pwfx->wBitsPerSample;

	outputFormat.sampleFormat = WWMFBitFormatInt;
	outputFormat.nChannels = 2;
	outputFormat.sampleRate = 44100;
	outputFormat.bits = 8;
	outputFormat.validBitsPerSample = 8;
	outputFormat.dwChannelMask = 3;

	bool useResampler = true;

	if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && pwfx->nChannels == 2 && pwfx->nSamplesPerSec == 44100) {
		useResampler = false;
		pwfx->wFormatTag = WAVE_FORMAT_PCM;
		pwfx->wBitsPerSample = 8;
		pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
		pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
	} else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		PWAVEFORMATEXTENSIBLE pEx = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pwfx);
		inputFormat.validBitsPerSample = pEx->Samples.wValidBitsPerSample;
		inputFormat.dwChannelMask = pEx->dwChannelMask;
		if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat) && pwfx->nChannels == 2 && pwfx->nSamplesPerSec == 44100) {
			useResampler = false;
			pEx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
			pEx->Samples.wValidBitsPerSample = 8;
			pwfx->wBitsPerSample = 8;
			pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
			pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
		}
	}

	if (useResampler) {
		LOG(L"Using Audio Resampler DSP");
		hr = resampler.Initialize(inputFormat, outputFormat, 5);
		if (FAILED(hr)) {
			ERR(L"WWMFResampler::Initialize failed: hr = 0x%08x", hr);
			noAudio = true;
			resampler.Finalize();
			goto cleanup;
		}
	}

	hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 0, 0, pwfx, 0);
	if (FAILED(hr)) {
		ERR(L"IAudioClient::Initialize failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pAudioCaptureClient);
	if (FAILED(hr)) {
		ERR(L"IAudioClient::GetService(IAudioCaptureClient) failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	hr = pAudioClient->Start();
	if (FAILED(hr)) {
		ERR(L"IAudioClient::Start failed: hr = 0x%08x", hr);
		noAudio = true;
		goto cleanup;
	}

	while (state == STATE_RUNNING && !deviceChanged) {
		if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (WM_QUIT == msg.message)
				state = STATE_EXIT;
		} else {
			nPasses++;
			hr = pAudioCaptureClient->GetNextPacketSize(&nNextPacketSize);
			while (SUCCEEDED(hr) && nNextPacketSize > 0 && bufferLeft.size() < pwfx->nSamplesPerSec * 576 / 44100.0) {
				hr = pAudioCaptureClient->GetBuffer(&pData, &nNumFramesToRead, &dwFlags, NULL, NULL);
				if (FAILED(hr)) {
					ERR(L"IAudioCaptureClient::GetBuffer failed on pass %u after %u frames: hr = 0x%08x", nPasses, pnFrames, hr);
					pAudioClient->Stop();
					goto cleanup;
				}

				if (nNumFramesToRead == 0) {
					ERR(L"IAudioCaptureClient::GetBuffer said to read 0 frames on pass %u after %u frames", nPasses, pnFrames);
					pAudioClient->Stop();
					goto cleanup;
				}

				pnFrames += nNumFramesToRead;

				if (dwFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
					bufferLeft.clear();
					bufferRight.clear();
				}

				if (dwFlags & AUDCLNT_BUFFERFLAGS_SILENT)
					for (int i = 0; i < 576*2; i+=2) {
						bufferLeft.push_back(0);
						bufferRight.push_back(0);
					}
				else {
					if (useResampler) {
						hr = resampler.Resample(pData, nNumFramesToRead * pwfx->nBlockAlign, &sampleData);
						if (FAILED(hr)) {
							ERR(L"WWMFResampler::Resample failed: hr = 0x%08x", hr);
							sampleData.Release();
							resampler.Finalize();
							pAudioClient->Stop();
							noAudio = true;
							goto cleanup;
						}
					} else {
						sampleData.bytes = nNumFramesToRead * 2;
						sampleData.data = pData;
					}

					for (int i = 0; i < sampleData.bytes; i+=2) {
						bufferLeft.push_back(sampleData.data[i] - 128);
						bufferRight.push_back(sampleData.data[i + 1] - 128);
					}

					hr = pAudioCaptureClient->ReleaseBuffer(nNumFramesToRead);
					if (FAILED(hr)) {
						ERR(L"IAudioCaptureClient::ReleaseBuffer failed on pass %u after %u frames: hr = 0x%08x", nPasses, pnFrames, hr);
						pAudioClient->Stop();
						goto cleanup;
					}

					if (useResampler)
						sampleData.Release();
					else
						sampleData.Forget();
				}

				hr = pAudioCaptureClient->GetNextPacketSize(&nNextPacketSize);
			}

			if (!SUCCEEDED(hr)) {
				ERR(L"IAudioCaptureClient::GetNextPacketSize failed on pass %u after %u frames: hr = 0x%08x", nPasses, pnFrames, hr);
				deviceChanged = true;
				pAudioClient->Stop();
			} else {
				if (bufferLeft.size() >= 576) {
					for (int i = 0; i < 576; ++i) {
						chunk[i] = bufferLeft.front();
						bufferLeft.pop_front();
						chunk[576+i] = bufferRight.front();
						bufferRight.pop_front();
					}
				}
				memcpy(milkdropModule->waveformData, chunk, 2*576);
				milkdropModule->Render(milkdropModule);
			}
		}
	}

cleanup:
	if (useResampler) resampler.Finalize();
	SafeRelease(&pAudioCaptureClient);
	if (pwfx) CoTaskMemFree(pwfx);
	SafeRelease(&pAudioClient);
	audioDeviceName = noAudioString;
	if (&pv) PropVariantClear(&pv);
	SafeRelease(&pPropertyStore);
	SafeRelease(&m_pMMDevice);

	return hr;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
	char winampClassName[] = "Winamp";
	char winampWindowName[] = "Winamp";

	WNDCLASS winampClass;
	winampClass.style = 0;
	winampClass.lpfnWndProc = WinampWndProc;
	winampClass.cbClsExtra = 0;
	winampClass.cbWndExtra = 0;
	winampClass.hInstance = hInstance;
	winampClass.hIcon = NULL;
	winampClass.hCursor = NULL;
	winampClass.hbrBackground = NULL;
	winampClass.lpszMenuName = NULL;
	winampClass.lpszClassName = winampClassName;

	if (!RegisterClass(&winampClass)) {
		ERR(L"RegisterClass failed");
		MessageBox(NULL,"RegisterClass failed.", "Error", 0);
		return 1;
	}

	HWND winampWindow = CreateWindow(winampClassName, winampWindowName, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
	if (winampWindow == NULL) {
		ERR(L"CreateWindow failed");
		MessageBox(NULL,"CreateWindow failed.", "Error", 0);
		return 1;
	}

	char mainClassName[] = "MilkDrop 2";
	char mainWindowName[] = "MilkDrop 2";

	WNDCLASS mainClass;
	mainClass.style = 0;
	mainClass.lpfnWndProc = MainWndProc;
	mainClass.cbClsExtra = 0;
	mainClass.cbWndExtra = 0;
	mainClass.hInstance = hInstance;
	mainClass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PLUGIN_ICON));
	mainClass.hCursor = NULL;
	mainClass.hbrBackground = NULL;
	mainClass.lpszMenuName = NULL;
	mainClass.lpszClassName = mainClassName;

	if (!RegisterClass(&mainClass)) {
		ERR(L"RegisterClass failed");
		MessageBox(NULL,"RegisterClass failed.", "Error", 0);
		return 1;
	}

	mainWindow = CreateWindow(mainWindowName, NULL, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
	if (mainWindow == NULL) {
		ERR(L"CreateWindow failed");
		MessageBox(NULL,"CreateWindow failed.", "Error", 0);
		return 1;
	}

	NOTIFYICONDATA nid;
	nid.hWnd = mainWindow;
	nid.uCallbackMessage = WM_USER + 1;
	nid.hIcon = mainClass.hIcon;
	strcpy_s(nid.szTip, mainWindowName);
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	Shell_NotifyIcon(NIM_ADD, &nid);

	HMODULE milkdropLibrary = LoadLibrary("vis_milk2.dll");
	winampVisGetHeaderType header_getter = reinterpret_cast<winampVisGetHeaderType>(GetProcAddress(milkdropLibrary, "winampVisGetHeader"));
	winampVisHeader *milkdropHeader = header_getter(winampWindow);
	milkdropModule = milkdropHeader->getModule(0);
	milkdropModule->hDllInstance = milkdropLibrary;
	milkdropModule->hwndParent = winampWindow;

	HRESULT hr;
	hr = CoInitialize(NULL);
	if (FAILED(hr)) {
		ERR(L"CoInitialize failed: hr = 0x%08x", hr);
		return -__LINE__;
	}

	MSG msg;
	msg.message = WM_NULL;
	IMMDeviceEnumerator *pMMDeviceEnumerator = NULL;
	MMNotificationClient notificationClient;
	while (state != STATE_EXIT) {
		if (state == STATE_RUNNING)	{
			milkdropModule->Init(milkdropModule);
			while(state == STATE_RUNNING) {
				noAudio = false;
				hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pMMDeviceEnumerator);
				if (FAILED(hr)) {
					ERR(L"CoCreateInstance(IMMDeviceEnumerator) failed: hr = 0x%08x", hr);
					noAudio = true;
				} else {
					pMMDeviceEnumerator->RegisterEndpointNotificationCallback(&notificationClient);
					deviceChanged = false;
					audioLoop(pMMDeviceEnumerator, true);
					if (noAudio) {
						noAudio = false;
						audioLoop(pMMDeviceEnumerator, false);
					}
				}
				if (noAudio) {
					deviceChanged = false;
					memset(milkdropModule->waveformData, 0, 2*576);
					while (state == STATE_RUNNING && !deviceChanged) {
						if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
							TranslateMessage(&msg);
							DispatchMessage(&msg);
							if (WM_QUIT == msg.message) {
								state = STATE_EXIT;
							}
						} else {
							milkdropModule->Render(milkdropModule);
						}
					}
				}
				if (pMMDeviceEnumerator) {
					pMMDeviceEnumerator->UnregisterEndpointNotificationCallback(&notificationClient);
					SafeRelease(&pMMDeviceEnumerator);
				}
			}
			milkdropModule->Quit(milkdropModule);
			while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		} else if (state == STATE_STOPPED) {
			GetMessage(&msg, NULL, 0U, 0U);
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		} else if (state == STATE_CONFIG) {
			milkdropModule->Config(milkdropModule);
			state = previousState;
		}
	}

	Shell_NotifyIcon(NIM_DELETE, &nid);
	delete[] chunk;
	CoUninitialize();

	return 0;
}