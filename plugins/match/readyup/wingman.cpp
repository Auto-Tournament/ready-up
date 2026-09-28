#include "readyup/wingman.h"

namespace readyup {
namespace wingman {

std::vector<std::string> GameModeCommands(bool wingman) {
  return {"game_type 0", wingman ? "game_mode 2" : "game_mode 1"};
}

}  // namespace wingman
}  // namespace readyup
