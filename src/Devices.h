#pragma once

#include "Com.h"

#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace fmdr {

struct DeviceInfo {
	std::wstring id;
	std::wstring name;
	bool isDefault = false;
	bool isVirtualCable = false;
};

/** Active endpoints for one direction, default first-flagged. */
bool enumerateDevices(EDataFlow flow, std::vector<DeviceInfo>& out, std::wstring& err);

/** True for the VB-Audio Virtual Cable endpoints, and for the other virtual
    devices people already have installed for this kind of routing, so the UI
    can point at the right one instead of making the user guess. */
bool looksLikeVirtualCable(const std::wstring& name);

/** Opens an endpoint by id. An empty id means "the current default". */
bool openDevice(const std::wstring& id, EDataFlow flow, Com<IMMDevice>& out, std::wstring& err);

} // namespace fmdr
