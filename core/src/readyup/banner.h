#pragma once

namespace readyup {

void LogBanner();

// One line at every server start (also with banner=0): the licence and where to get a
// commercial one.
const char* LicenseNotice();
void LogLicenseNotice();

}  // namespace readyup

