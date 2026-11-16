// [IzunaRecomp] A sampling CPU profiler (Linux, Android): the hottest code of
// the busiest threads every 15 s, as offsets into the program, for
// symbolising against the unstripped build (izuna_cpu_profile.cpp).
#pragma once

namespace izuna::cpu_profile {
void Start();
}  // namespace izuna::cpu_profile
