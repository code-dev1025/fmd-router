// PKEY_Device_FriendlyName is declared, not defined, unless INITGUID is in
// scope -- and initguid.h has to come before the headers that declare it. This
// is the one translation unit that defines those symbols for the whole program.
#include <initguid.h>

#include "Devices.h"

#include <audioclient.h>  // the AUDCLNT_E_* codes describeHresult explains
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cwctype>

namespace fmdr {

namespace {

std::wstring toLower(std::wstring s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](wchar_t c) { return wchar_t(std::towlower(c)); });
	return s;
}

std::wstring friendlyName(IMMDevice* device) {
	Com<IPropertyStore> props;
	if (FAILED(device->OpenPropertyStore(STGM_READ, props.put())))
		return L"(unnamed device)";

	PROPVARIANT v;
	PropVariantInit(&v);
	std::wstring name = L"(unnamed device)";
	if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
		name = v.pwszVal;
	PropVariantClear(&v);
	return name;
}

std::wstring deviceId(IMMDevice* device) {
	LPWSTR raw = nullptr;
	if (FAILED(device->GetId(&raw)) || !raw)
		return std::wstring();
	std::wstring id(raw);
	CoTaskMemFree(raw);
	return id;
}

} // namespace


std::wstring describeHresult(const wchar_t* what, HRESULT hr) {
	// A few HRESULTs come up often enough here that the raw code helps nobody.
	const wchar_t* known = nullptr;
	switch (hr) {
		case AUDCLNT_E_DEVICE_IN_USE:
			known = L"the device is already open in exclusive mode by another app";
			break;
		case AUDCLNT_E_DEVICE_INVALIDATED:
			known = L"the device was unplugged or its format was changed";
			break;
		case AUDCLNT_E_UNSUPPORTED_FORMAT:
			known = L"the endpoint will not accept its own mix format";
			break;
		case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
			known = L"the audio endpoint could not be created (is the device disabled?)";
			break;
		case E_ACCESSDENIED:
			known = L"access denied -- check Settings > Privacy > Microphone, "
			        L"which gates WASAPI capture even for a virtual cable";
			break;
		default:
			break;
	}

	wchar_t buf[512];
	if (known)
		swprintf_s(buf, L"%s failed: %s (0x%08lX)", what, known, static_cast<unsigned long>(hr));
	else
		swprintf_s(buf, L"%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
	return std::wstring(buf);
}


bool looksLikeVirtualCable(const std::wstring& name) {
	const std::wstring n = toLower(name);
	return n.find(L"cable") != std::wstring::npos
	    || n.find(L"vb-audio") != std::wstring::npos
	    || n.find(L"voicemeeter") != std::wstring::npos
	    || n.find(L"virtual audio") != std::wstring::npos;
}


bool enumerateDevices(EDataFlow flow, std::vector<DeviceInfo>& out, std::wstring& err) {
	out.clear();

	Com<IMMDeviceEnumerator> enumerator;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                              __uuidof(IMMDeviceEnumerator), enumerator.putVoid());
	if (FAILED(hr)) {
		err = describeHresult(L"Creating the device enumerator", hr);
		return false;
	}

	// The default endpoint is only used to mark a row in the list; not having
	// one (no sound card at all) is not a reason to fail the enumeration.
	std::wstring defaultId;
	Com<IMMDevice> defaultDevice;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, defaultDevice.put())))
		defaultId = deviceId(defaultDevice.get());

	Com<IMMDeviceCollection> collection;
	hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put());
	if (FAILED(hr)) {
		err = describeHresult(L"Listing audio endpoints", hr);
		return false;
	}

	UINT count = 0;
	if (FAILED(collection->GetCount(&count))) {
		err = L"Listing audio endpoints failed: the collection reported no count";
		return false;
	}

	for (UINT i = 0; i < count; i++) {
		Com<IMMDevice> device;
		if (FAILED(collection->Item(i, device.put())))
			continue;

		DeviceInfo info;
		info.id = deviceId(device.get());
		if (info.id.empty())
			continue;
		info.name = friendlyName(device.get());
		info.isDefault = (!defaultId.empty() && info.id == defaultId);
		info.isVirtualCable = looksLikeVirtualCable(info.name);
		out.push_back(std::move(info));
	}

	return true;
}


bool openDevice(const std::wstring& id, EDataFlow flow, Com<IMMDevice>& out, std::wstring& err) {
	Com<IMMDeviceEnumerator> enumerator;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                              __uuidof(IMMDeviceEnumerator), enumerator.putVoid());
	if (FAILED(hr)) {
		err = describeHresult(L"Creating the device enumerator", hr);
		return false;
	}

	if (id.empty())
		hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, out.put());
	else
		hr = enumerator->GetDevice(id.c_str(), out.put());

	if (FAILED(hr)) {
		err = describeHresult(L"Opening the audio device", hr);
		return false;
	}
	return true;
}

} // namespace fmdr
