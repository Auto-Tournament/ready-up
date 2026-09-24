#pragma once

namespace readyup {

// Runs a signature self-test. Returns true if all required signatures resolve
// uniquely (exactly one match).
bool RunSigTest(bool verbose);

// Runs signature self-test and terminates on failure.
void RunSigTestOrDie();

}  // namespace readyup

