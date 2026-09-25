#include "readyup/banner.h"

#include "readyup/config.h"
#include "readyup/logging.h"

#ifndef READYUP_BUILD_VERSION
#define READYUP_BUILD_VERSION "unknown"
#endif

namespace readyup {

void LogBanner() {
  if (!BannerEnabled()) return;

  static constexpr const char* kBanner = R"(
 ______     ______     ______     _____     __  __        __  __     ______  
╱╲  == ╲   ╱╲  ___╲   ╱╲  __ ╲   ╱╲  __─.  ╱╲ ╲_╲ ╲      ╱╲ ╲╱╲ ╲   ╱╲  == ╲ 
╲ ╲  __<   ╲ ╲  __╲   ╲ ╲  __ ╲  ╲ ╲ ╲╱╲ ╲ ╲ ╲____ ╲     ╲ ╲ ╲_╲ ╲  ╲ ╲  _─╱ 
 ╲ ╲_╲ ╲_╲  ╲ ╲_____╲  ╲ ╲_╲ ╲_╲  ╲ ╲____─  ╲╱╲_____╲     ╲ ╲_____╲  ╲ ╲_╲   
  ╲╱_╱ ╱_╱   ╲╱_____╱   ╲╱_╱╲╱_╱   ╲╱____╱   ╲╱_____╱      ╲╱_____╱   ╲╱_╱   
                                                                             
)";

  PrintRaw(
      "\n%s\n"
      " Ready Up initialized (build %s)\n"
      " Author: Sivert Gullberg Hansen\n"
      " Repo:   https://github.com/Auto-Tournament/ready-up\n"
      "\n",
      kBanner,
      READYUP_BUILD_VERSION);
}

const char* LicenseNotice() {
  return "license: Ready Up is PolyForm Noncommercial 1.0.0: free for noncommercial use; commercial use needs a "
         "paid license (https://autotournament.gg/pricing, sivert@autotournament.gg)";
}

void LogLicenseNotice() { PrintLine(LicenseNotice()); }

}  // namespace readyup

