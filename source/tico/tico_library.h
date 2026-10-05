// The game list shown when the host starts without a game.
#pragma once

#include <functional>
#include <string>

namespace TicoLibrary {

// Registers the overlay's library (the games in tico's ROM folders and the
// module's own) and its Settings > Library folder editor. `launch` gets the
// chosen game.
void Register(std::function<void(const std::string& path)> launch);
void Unregister();

} // namespace TicoLibrary
