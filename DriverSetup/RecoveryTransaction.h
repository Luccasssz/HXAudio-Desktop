#pragma once
#include "RestorePlan.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Transaction engine for isolated adapters only. The Windows registry has no
// generic compare-and-swap; this interface must not be wired to MMDevices until
// a coordination protocol with OEM writers has been demonstrated safe.
namespace HXAudioRecovery {
using HXAudioRestorePlan::Identity;
using HXAudioRestorePlan::RawValue;
using HXAudioRestorePlan::Property;
using HXAudioRestorePlan::Decision;

enum class Phase : uint32_t {
    Pending = 1, Restoring, Verify, Archived, Conflict, Legacy, Attention
};
enum class Result { Success, Conflict, Untrusted, Invalid, IoError, IdentityChanged };

struct Record {
    uint32_t schema = 1;
    std::wstring transactionId;
    Identity identity;
    std::wstring driverProvider;
    std::vector<Property> properties;
    std::vector<RawValue> expectedInstalled;
    bool installedConfirmed = false;
    std::vector<uint8_t> fingerprint;
};

// The adapter must authenticate the origin and ACL of the immutable record;
// a matching hash by itself is not authentication. Journal writes must reach
// durable storage before returning success. Test-only adapters provide these
// functions for a disposable registry subtree.
struct Adapter {
    virtual ~Adapter() = default;
    // Hard gate: true only for a disposable environment whose writers are
    // controlled by the test harness. No live registry adapter may return true.
    virtual bool IsIsolated() = 0;
    virtual bool TrustedOrigin() = 0;
    virtual bool ValidFingerprint(const Record&) = 0;
    virtual bool ReadIdentity(Identity&, std::wstring&) = 0;
    virtual bool ReadValue(const std::wstring&, RawValue&) = 0;
    virtual bool WriteValue(const std::wstring&, const RawValue&) = 0;
    virtual bool SaveProgress(Phase, uint32_t) = 0;
};

inline Result Run(const Record& record, Adapter& adapter) {
    if (!adapter.IsIsolated()) return Result::Untrusted;
    if (record.schema != 1 || !record.installedConfirmed ||
        record.transactionId.empty() || record.driverProvider.empty() ||
        !HXAudioRestorePlan::ValidIdentity(record.identity) || record.properties.empty() ||
        record.expectedInstalled.size() != record.properties.size())
        return Result::Invalid;
    for (size_t i = 0; i < record.properties.size(); ++i)
        if (!HXAudioRestorePlan::Equal(record.expectedInstalled[i], record.properties[i].installed)) return Result::Invalid;
    if (!adapter.TrustedOrigin()) return Result::Untrusted;
    if (!adapter.ValidFingerprint(record)) return Result::Invalid;
    auto identityMatches = [&]() {
        Identity actual; std::wstring provider;
        return adapter.ReadIdentity(actual, provider) &&
            HXAudioRestorePlan::Equal(record.identity, actual) && provider == record.driverProvider;
    };
    if (!identityMatches()) return Result::IdentityChanged;

    // Preflight every property before the first write, including duplicates.
    std::vector<Property> observed = record.properties;
    for (auto& property : observed)
        if (!adapter.ReadValue(property.name, property.current)) return Result::IoError;
    std::vector<Decision> plan;
    if (!HXAudioRestorePlan::Plan(record.identity, record.identity, observed, plan)) {
        if (!adapter.SaveProgress(Phase::Conflict, 0)) return Result::IoError;
        return Result::Conflict;
    }
    if (!adapter.SaveProgress(Phase::Restoring, 0)) return Result::IoError;
    for (size_t i = 0; i < observed.size(); ++i) {
        if (!identityMatches()) return Result::IdentityChanged;
        RawValue current;
        if (!adapter.ReadValue(observed[i].name, current)) return Result::IoError;
        if (!HXAudioRestorePlan::Equal(current, observed[i].original)) {
            if (!HXAudioRestorePlan::Equal(current, observed[i].installed)) {
                if (!adapter.SaveProgress(Phase::Conflict, static_cast<uint32_t>(i))) return Result::IoError;
                return Result::Conflict;
            }
            // Flush intent before the write. A restart repeats the full compare.
            if (!adapter.SaveProgress(Phase::Restoring, static_cast<uint32_t>(i))) return Result::IoError;
            if (!identityMatches() || !adapter.ReadValue(observed[i].name, current)) return Result::IoError;
            if (!HXAudioRestorePlan::Equal(current, observed[i].installed)) {
                if (!adapter.SaveProgress(Phase::Conflict, static_cast<uint32_t>(i))) return Result::IoError;
                return Result::Conflict;
            }
            if (!adapter.WriteValue(observed[i].name, observed[i].original)) return Result::IoError;
        }
        if (!adapter.ReadValue(observed[i].name, current) ||
            !HXAudioRestorePlan::Equal(current, observed[i].original)) return Result::IoError;
        if (!adapter.SaveProgress(Phase::Restoring, static_cast<uint32_t>(i + 1))) return Result::IoError;
    }
    if (!adapter.SaveProgress(Phase::Verify, static_cast<uint32_t>(observed.size()))) return Result::IoError;
    if (!identityMatches()) return Result::IdentityChanged;
    for (const auto& property : observed) {
        RawValue current;
        if (!adapter.ReadValue(property.name, current)) return Result::IoError;
        if (!HXAudioRestorePlan::Equal(current, property.original)) {
            if (!adapter.SaveProgress(Phase::Conflict, 0)) return Result::IoError;
            return Result::Conflict;
        }
    }
    if (!adapter.SaveProgress(Phase::Archived, static_cast<uint32_t>(observed.size()))) return Result::IoError;
    return Result::Success;
}
}
