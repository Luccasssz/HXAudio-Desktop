#pragma once
#include "RecoveryTransaction.h"
#include <windows.h>
#include <bcrypt.h>
#include <limits>

namespace HXAudioRecovery {
inline void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline bool PutBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t length) {
    if (length > (std::numeric_limits<uint32_t>::max)()) return false;
    Put32(out, static_cast<uint32_t>(length));
    if (length) out.insert(out.end(), data, data + length);
    return true;
}
inline bool PutString(std::vector<uint8_t>& out, const std::wstring& value) {
    static_assert(sizeof(wchar_t) == 2, "canonical strings use UTF-16LE");
    return PutBytes(out, reinterpret_cast<const uint8_t*>(value.data()), value.size() * sizeof(wchar_t));
}
inline bool PutValue(std::vector<uint8_t>& out, const RawValue& value) {
    if (!value.exists && (value.type != 0 || !value.bytes.empty())) return false;
    Put32(out, value.exists ? 1u : 0u); Put32(out, value.type);
    return PutBytes(out, value.bytes.data(), value.bytes.size());
}
inline bool CanonicalRecord(const Record& record, std::vector<uint8_t>& data) {
    data.clear();
    Put32(data, record.schema);
    if (!PutString(data, record.transactionId) || !PutString(data, record.identity.endpoint) ||
        !PutString(data, record.identity.deviceInstance) || !PutString(data, record.identity.driverPackage) ||
        !PutString(data, record.identity.driverVersion) || !PutString(data, record.driverProvider) ||
        record.properties.size() > (std::numeric_limits<uint32_t>::max)()) return false;
    Put32(data, record.installedConfirmed ? 1u : 0u);
    Put32(data, static_cast<uint32_t>(record.properties.size()));
    if (record.expectedInstalled.size() != record.properties.size()) return false;
    for (size_t i = 0; i < record.properties.size(); ++i) {
        const auto& p = record.properties[i];
        if (!PutString(data, p.name) || !PutValue(data, p.original) ||
            !PutValue(data, record.expectedInstalled[i]) || !PutValue(data, p.installed)) return false;
    }
    return true;
}
inline bool ComputeFingerprint(const Record& record, std::vector<uint8_t>& digest) {
    digest.clear();
    std::vector<uint8_t> data;
    if (!CanonicalRecord(record, data)) return false;
    if (data.size() > (std::numeric_limits<ULONG>::max)()) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    digest.resize(32);
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, data.data(),
        static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) { digest.clear(); return false; }
    return true;
}
}
