#pragma once

#include <cstdint>

namespace readyup {

// Ready Up admin check (ru_api is_admin, `.ru plugin` / `.ru selftest` / `.ru reload` from chat).
// Who is an admin is plugin policy: the admin provider a plugin registered
// (set_admin_provider; readyup-match answers from the match config, MAT and the database).
// Without a provider nobody in chat is an admin (the server console always is).
// Any thread; may block while the provider answers.
bool IsReadyUpAdmin(uint64_t steamid64);

}  // namespace readyup
