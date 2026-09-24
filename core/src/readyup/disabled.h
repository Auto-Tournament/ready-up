#pragma once

#include <string>

namespace readyup {

// When disabled, Ready Up should not install any hooks or perform any side-effects.
// The CS2 server should continue running normally (Ready Up becomes inert).
bool IsDisabled();
std::string DisabledReason();
void Disable(std::string reason);

}  // namespace readyup

