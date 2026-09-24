#pragma once

namespace readyup {

#ifndef READYUP_SEMVER
#define READYUP_SEMVER "unknown"
#endif

#ifndef READYUP_BUILD_VERSION
#define READYUP_BUILD_VERSION "unknown"
#endif

inline const char* SemVer() {
  return READYUP_SEMVER;
}

inline const char* BuildVersion() {
  return READYUP_BUILD_VERSION;
}

}  // namespace readyup

