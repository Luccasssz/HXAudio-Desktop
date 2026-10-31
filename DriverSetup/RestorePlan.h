#pragma once

// Offline planning only. This is deliberately not connected to registry writes.
// A fingerprint, journal durability, trusted identity acquisition and locking
// must be implemented and reviewed before this can authorize restoration.
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace HXAudioRestorePlan {
struct RawValue {
    bool exists = false;
    uint32_t type = 0;
    std::vector<uint8_t> bytes;
};
inline bool Equal(const RawValue& a, const RawValue& b) {
    if (a.exists != b.exists) return false;
    return !a.exists || (a.type == b.type && a.bytes == b.bytes);
}
struct Identity {
    std::wstring endpoint, deviceInstance, driverPackage, driverVersion;
};
inline bool ValidIdentity(const Identity& id) {
    return !id.endpoint.empty() && !id.deviceInstance.empty() &&
        !id.driverPackage.empty() && !id.driverVersion.empty();
}
inline bool Equal(const Identity& a, const Identity& b) {
    return a.endpoint == b.endpoint && a.deviceInstance == b.deviceInstance &&
        a.driverPackage == b.driverPackage && a.driverVersion == b.driverVersion;
}
inline bool SameName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towupper(a[i]) != std::towupper(b[i])) return false;
    return true;
}
struct Property {
    std::wstring name;
    RawValue original, installed, current;
};
enum class Decision { AlreadyOriginal, RestoreOriginal, Conflict };
inline Decision Compare(const Property& p) {
    if (Equal(p.current, p.original)) return Decision::AlreadyOriginal;
    if (Equal(p.current, p.installed)) return Decision::RestoreOriginal;
    return Decision::Conflict;
}
// All properties are preflighted before any caller can use the plan. Recheck
// identity and every value at write time; Windows registry lacks an atomic CAS.
inline bool Plan(const Identity& recorded, const Identity& live,
    const std::vector<Property>& properties, std::vector<Decision>& out) {
    out.clear();
    if (!ValidIdentity(recorded) || !ValidIdentity(live) ||
        !Equal(recorded, live) || properties.empty()) return false;
    for (size_t i = 0; i < properties.size(); ++i) {
        if (properties[i].name.empty()) { out.clear(); return false; }
        for (size_t j = 0; j < i; ++j)
            if (SameName(properties[j].name, properties[i].name)) { out.clear(); return false; }
        const Decision decision = Compare(properties[i]);
        if (decision == Decision::Conflict) { out.clear(); return false; }
        out.push_back(decision);
    }
    return true;
}
}
