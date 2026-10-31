#include "../DriverSetup/SafetyLogic.h"
#include <iostream>
using namespace HXAudioSafety;
int main() {
    int a=0,b=0,c=0;
    if (Purge([&]{++a; return 0;},[&]{++b;return 0;},[&]{++c;return 0;}) || a!=1 || b!=1 || c!=1) return 1;
    a=b=c=0;
    if (Purge([&]{++a;return 56;},[&]{++b;return 0;},[&]{++c;return 0;})!=56 || a!=1 || b || c) return 2;
    a=b=c=0;
    if (Purge([&]{++a;return 0;},[&]{++b;return 8;},[&]{++c;return 0;})!=8 || a!=1 || b!=1 || c) return 3;
    a=b=c=0;
    if (Purge([&]{++a;return 0;},[&]{++b;return 0;},[&]{++c;return 5;})!=5 || c!=1) return 4;
    // Retry/idempotence: no action after a failure until a new request.
    if (Purge([]{return 0;},[]{return 0;},[]{return 0;}) != 0) return 5;
    const Chain oem{L"{0EBD8607-17BB-4AE7-AD76-E86F99A425E9}",L"{90705486-CFCC-4D2F-9671-B62846279A2C}"};
    const std::wstring hx=L"{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}";
    Chain plan, installed=oem; installed.push_back(hx);
    if (!PlanAppend(oem,oem,hx,plan) || !SameChain(plan,installed)) return 6;
    if (!PlanAppend(oem,installed,hx,plan) || !SameChain(plan,installed)) return 7;
    if (!PlanRemove(oem,installed,hx,plan) || !SameChain(plan,oem)) return 8;
    if (!PlanRemove(oem,oem,hx,plan) || !SameChain(plan,oem)) return 9;
    Chain update=oem; update.push_back(L"new-OEM-effect");
    if (PlanAppend(oem,update,hx,plan) || PlanRemove(oem,update,hx,plan)) return 10;
    Chain reordered=installed; std::swap(reordered[0],reordered[1]);
    if (PlanAppend(oem,reordered,hx,plan) || PlanRemove(oem,reordered,hx,plan)) return 11;
    Chain duplicate=installed; duplicate.push_back(hx);
    if (PlanAppend(oem,duplicate,hx,plan) || PlanRemove(oem,duplicate,hx,plan)) return 12;
    Chain recreated=oem;
    if (!PlanAppend(oem,recreated,hx,plan) || !SameChain(plan,installed)) return 13;
    std::cout << "PASS purge fault sequencing and CompositeFX offline planner\n";
}
