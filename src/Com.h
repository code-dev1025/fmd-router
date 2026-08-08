#pragma once

#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps the COM entry points out of windows.h, and every
// user of this header wants them.
#include <objbase.h>

#include <string>

namespace fmdr {

/** Minimal intrusive COM pointer. Enough for the handful of interfaces this
    app touches, and small enough to read in one sitting. */
template <class T>
class Com {
public:
	Com() = default;

	Com(const Com& o) : p_(o.p_) {
		if (p_)
			p_->AddRef();
	}

	Com(Com&& o) noexcept : p_(o.p_) {
		o.p_ = nullptr;
	}

	~Com() {
		reset();
	}

	Com& operator=(const Com& o) {
		if (this != &o) {
			if (o.p_)
				o.p_->AddRef();
			reset();
			p_ = o.p_;
		}
		return *this;
	}

	Com& operator=(Com&& o) noexcept {
		if (this != &o) {
			reset();
			p_ = o.p_;
			o.p_ = nullptr;
		}
		return *this;
	}

	void reset() {
		if (p_) {
			p_->Release();
			p_ = nullptr;
		}
	}

	/** For the ppv out-parameter of Activate/CoCreateInstance/QueryInterface.
	    Releases first, so reusing one variable across two calls cannot leak. */
	T** put() {
		reset();
		return &p_;
	}

	void** putVoid() {
		reset();
		return reinterpret_cast<void**>(&p_);
	}

	T* get() const { return p_; }
	T* operator->() const { return p_; }
	explicit operator bool() const { return p_ != nullptr; }

private:
	T* p_ = nullptr;
};


/** RAII for CoInitializeEx on a worker thread. RPC_E_CHANGED_MODE is not an
    error worth failing over -- it only means the thread was already in an
    apartment, and the calls this app makes work in either. */
class ComScope {
public:
	ComScope() {
		const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		owns_ = SUCCEEDED(hr);
	}

	~ComScope() {
		if (owns_)
			CoUninitialize();
	}

	ComScope(const ComScope&) = delete;
	ComScope& operator=(const ComScope&) = delete;

private:
	bool owns_ = false;
};


/** Turns an HRESULT into something a user can act on. */
std::wstring describeHresult(const wchar_t* what, HRESULT hr);

} // namespace fmdr
