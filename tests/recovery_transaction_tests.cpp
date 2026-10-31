#include "../DriverSetup/RecoveryTransaction.h"
#include <iostream>
#include <map>
#include <stdexcept>
using namespace HXAudioRecovery;
struct Fake : Adapter {
    Identity identity{L"endpoint", L"device", L"oem.inf", L"1.0"};
    std::wstring provider = L"Realtek";
    std::map<std::wstring, RawValue> values;
    Phase phase = Phase::Pending;
    uint32_t progress = 0;
    bool trusted = true, valid = true, denyWrite = false;
    int writes = 0, saves = 0, crashAt = -1, crashAfterWrite = -1;
    bool TrustedOrigin() override { return trusted; }
    bool ValidFingerprint(const Record&) override { return valid; }
    bool ReadIdentity(Identity& id, std::wstring& p) override { id = identity; p = provider; return true; }
    bool ReadValue(const std::wstring& name, RawValue& out) override {
        auto it = values.find(name); out = it == values.end() ? RawValue{} : it->second; return true;
    }
    bool WriteValue(const std::wstring& name, const RawValue& value) override {
        if (denyWrite) return false;
        ++writes; values[name] = value;
        if (writes == crashAfterWrite) throw std::runtime_error("injected after write");
        return true;
    }
    bool SaveProgress(Phase p, uint32_t n) override {
        phase = p; progress = n; ++saves;
        if (saves == crashAt) throw std::runtime_error("injected process stop");
        return true;
    }
};
static Record MakeRecord() {
    Record r; r.transactionId = L"txn"; r.identity = {L"endpoint", L"device", L"oem.inf", L"1.0"};
    r.driverProvider = L"Realtek"; r.installedConfirmed = true;
    const RawValue a{true, 7, {1,2,3}}, b{true, 7, {1,2,3,4}};
    const RawValue c{true, 1, {4,0}}, d{true, 1, {5,0}};
    r.properties = {{L"effects", a, b, {}}, {L"other", c, d, {}}};
    r.expectedInstalled = {b, d};
    return r;
}
static Fake Installed(const Record& r) {
    Fake f; for (const auto& p : r.properties) f.values[p.name] = p.installed; return f;
}
int main() {
    const Record r = MakeRecord();
    Fake good = Installed(r);
    if (Run(r, good) != Result::Success || good.phase != Phase::Archived || good.writes != 2) return 1;
    if (Run(r, good) != Result::Success || good.writes != 2) return 2;
    Fake old = Installed(r); old.trusted = false;
    if (Run(r, old) != Result::Untrusted || old.writes) return 3;
    Fake damaged = Installed(r); damaged.valid = false;
    if (Run(r, damaged) != Result::Invalid || damaged.writes) return 4;
    Fake updated = Installed(r); updated.identity.driverVersion = L"2.0";
    if (Run(r, updated) != Result::IdentityChanged || updated.writes) return 5;
    Fake moved = Installed(r); moved.identity.endpoint = L"other";
    if (Run(r, moved) != Result::IdentityChanged || moved.writes) return 6;
    Fake changed = Installed(r); changed.values[L"other"].bytes = {8};
    if (Run(r, changed) != Result::Conflict || changed.writes || changed.phase != Phase::Conflict) return 7;
    Fake denied = Installed(r); denied.denyWrite = true;
    if (Run(r, denied) != Result::IoError || denied.phase != Phase::Restoring) return 8;
    denied.denyWrite = false;
    if (Run(r, denied) != Result::Success) return 9;
    // Simulate interruption after each durable progress boundary, including
    // after the first property's write and before its completion record.
    for (int boundary = 1; boundary <= good.saves; ++boundary) {
        Fake f = Installed(r); f.crashAt = boundary;
        try { (void)Run(r, f); } catch (const std::runtime_error&) {}
        f.crashAt = -1;
        if (Run(r, f) != Result::Success || f.phase != Phase::Archived) return 10;
        for (const auto& p : r.properties)
            if (!HXAudioRestorePlan::Equal(f.values[p.name], p.original)) return 11;
    }
    for (int write = 1; write <= 2; ++write) {
        Fake interrupted = Installed(r); interrupted.crashAfterWrite = write;
        try { (void)Run(r, interrupted); } catch (const std::runtime_error&) {}
        interrupted.crashAfterWrite = -1;
        if (Run(r, interrupted) != Result::Success || interrupted.phase != Phase::Archived) return 13;
    }
    Record legacy = r; legacy.schema = 0;
    Fake f = Installed(r);
    if (Run(legacy, f) != Result::Invalid || f.writes) return 12;
    std::cout << "PASS transaction engine preflight, conflicts and simulated durable boundaries\n";
}
