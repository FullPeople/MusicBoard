#pragma once

#include <objbase.h>
#include <cstdio>

#include "core/failure.h"

namespace musicboard {

// COM belongs to the calling thread. Declare before any COM resource so it is
// released last. An existing apartment is usable without owning its reference.
class ComApartment final {
public:
    ComApartment() = default;
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment() { if (owned_) ::CoUninitialize(); }

    [[nodiscard]] bool initialize(DWORD model, Failure& failure) {
        if (owned_) return true;
        const HRESULT hr = ::CoInitializeEx(nullptr, model);
        owned_ = SUCCEEDED(hr);
        if (owned_ || hr == RPC_E_CHANGED_MODE) return true;
        char code[24]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        failure.set(fail_code::backend_unavailable, std::string("CoInitializeEx: ") + code);
        return false;
    }

private:
    bool owned_ = false;
};

} // namespace musicboard
