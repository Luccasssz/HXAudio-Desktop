#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace HXAudioSafety {
// Purge must never advance to another backend or delete settings after failure.
template<class Apo, class Virtual, class Finalize>
int Purge(Apo apo, Virtual virt, Finalize finalize) {
    int rc = apo(); if (rc != 0) return rc;
    rc = virt(); if (rc != 0) return rc;
    return finalize();
}
inline bool SameId(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towupper(a[i]) != std::towupper(b[i])) return false;
    return true;
}
using Chain = std::vector<std::wstring>;
inline bool SameChain(const Chain& a, const Chain& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!SameId(a[i], b[i])) return false;
    return true;
}
// Offline planner only. Caller must provide a durable baseline and compare the
// live value atomically or under a validated coordination scheme before writes.
// No registry operation is performed by these functions.
inline bool PlanAppend(const Chain& baseline, const Chain& current,
    const std::wstring& own, Chain& result) {
    result.clear();
    if (own.empty()) return false;
    for (const auto& id : baseline) if (id.empty() || SameId(id, own)) return false;
    Chain expected = baseline; expected.push_back(own);
    if (SameChain(current, baseline) || SameChain(current, expected)) {
        result = expected; return true;
    }
    return false;
}
inline bool PlanRemove(const Chain& baseline, const Chain& current,
    const std::wstring& own, Chain& result) {
    result.clear();
    Chain installed;
    if (!PlanAppend(baseline, baseline, own, installed)) return false;
    if (SameChain(current, baseline) || SameChain(current, installed)) {
        result = baseline; return true;
    }
    return false;
}
}
