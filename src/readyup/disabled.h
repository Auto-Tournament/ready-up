#pragma once

#include <string>

namespace readyup {

// When disabled, ReadyUp should not install any hooks or perform any side-effects.
// The CS2 server should continue running normally (ReadyUp becomes inert).
bool IsDisabled();
std::string DisabledReason();
void Disable(std::string reason);

}  // namespace readyup

