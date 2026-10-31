#include "../DriverSetup/RestorePlan.h"
#include <iostream>
#include <vector>
using namespace HXAudioRestorePlan;
int main() {
    const Identity id{L"endpoint", L"device", L"oem.inf", L"1.0"};
    const RawValue original{true, 7, {1, 2, 3, 0}};
    const RawValue installed{true, 7, {1, 2, 3, 4, 0}};
    Property p{L"Effect", original, installed, installed};
    std::vector<Decision> plan;
    if (!Plan(id, id, {p}, plan) || plan != std::vector<Decision>{Decision::RestoreOriginal}) return 1;
    p.current = original;
    if (!Plan(id, id, {p}, plan) || plan[0] != Decision::AlreadyOriginal) return 2;
    p.current = installed; p.current.type = 1;
    if (Plan(id, id, {p}, plan) || !plan.empty()) return 3;
    p.current = installed; p.current.bytes = {2, 1, 3, 4, 0};
    if (Plan(id, id, {p}, plan) || !plan.empty()) return 4;
    p.current = installed;
    Identity updated = id; updated.driverVersion = L"2.0";
    if (Plan(id, updated, {p}, plan)) return 5;
    updated = id; updated.endpoint = L"other";
    if (Plan(id, updated, {p}, plan)) return 6;
    updated = id; updated.deviceInstance.clear();
    if (Plan(id, updated, {p}, plan)) return 7;
    Property conflict = p; conflict.name = L"Second"; conflict.current.bytes = {9};
    if (Plan(id, id, {p, conflict}, plan) || !plan.empty()) return 8;
    Property duplicate = p; duplicate.name = L"effect";
    if (Plan(id, id, {p, duplicate}, plan)) return 9;
    Property missing{L"Absent", {}, installed, installed};
    if (!Plan(id, id, {missing}, plan) || plan[0] != Decision::RestoreOriginal) return 10;
    missing.current = {};
    if (!Plan(id, id, {missing}, plan) || plan[0] != Decision::AlreadyOriginal) return 11;
    if (Plan(id, id, {}, plan)) return 12;
    std::cout << "PASS offline exact-byte three-way planning; no registry writes\n";
}
