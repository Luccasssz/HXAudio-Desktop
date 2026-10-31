#pragma once
#include "RecoveryTransaction.h"
#include <windows.h>
#include <bcrypt.h>
#include <limits>
#include <cstring>
#include <utility>

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

// Strict, bounded decoder for an immutable canonical record. The fingerprint
// is stored separately; parsing or digest equality does not establish trust.
struct RecordReader {
    const std::vector<uint8_t>& data;
    size_t pos = 0;
    bool U32(uint32_t& value) {
        if (pos > data.size() || data.size() - pos < 4) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[pos++]) << (8 * i);
        return true;
    }
    bool Bytes(std::vector<uint8_t>& value, uint32_t limit) {
        uint32_t length = 0;
        if (!U32(length) || length > limit || pos > data.size() || length > data.size() - pos) return false;
        value.assign(data.begin() + pos, data.begin() + pos + length); pos += length; return true;
    }
    bool String(std::wstring& value) {
        std::vector<uint8_t> raw;
        if (!Bytes(raw, 65536) || raw.size() % sizeof(wchar_t)) return false;
        value.resize(raw.size() / sizeof(wchar_t));
        if (!raw.empty()) std::memcpy(&value[0], raw.data(), raw.size());
        return value.find(L'\0') == std::wstring::npos;
    }
    bool Value(RawValue& value) {
        uint32_t exists = 0, type = 0;
        if (!U32(exists) || exists > 1 || !U32(type) || !Bytes(value.bytes, 1048576)) return false;
        value.exists = exists != 0; value.type = type;
        return value.exists || (type == 0 && value.bytes.empty());
    }
};
inline bool DecodeRecord(const std::vector<uint8_t>& data, Record& out) {
    if (data.size() > 8 * 1024 * 1024) return false;
    RecordReader reader{data}; Record result;
    uint32_t confirmed = 0, count = 0;
    if (!reader.U32(result.schema) || !reader.String(result.transactionId) ||
        !reader.String(result.identity.endpoint) || !reader.String(result.identity.deviceInstance) ||
        !reader.String(result.identity.driverPackage) || !reader.String(result.identity.driverVersion) ||
        !reader.String(result.driverProvider) || !reader.U32(confirmed) || confirmed > 1 ||
        !reader.U32(count) || count == 0 || count > 256) return false;
    result.installedConfirmed = confirmed != 0;
    for (uint32_t i = 0; i < count; ++i) {
        Property p; RawValue expected;
        if (!reader.String(p.name) || !reader.Value(p.original) || !reader.Value(expected) ||
            !reader.Value(p.installed)) return false;
        result.properties.push_back(std::move(p)); result.expectedInstalled.push_back(std::move(expected));
    }
    if (reader.pos != data.size()) return false;
    out = std::move(result);
    return true;
}
}
