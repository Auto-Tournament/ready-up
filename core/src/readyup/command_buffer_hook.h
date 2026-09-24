#pragma once

namespace readyup {

// Best-effort hook to intercept server console commands (including RCON) so
// `ru ...` can work with arguments without relying on CS2 alias arg expansion.
void InstallCommandBufferHook();

// Enqueue a server command into the engine command buffer (best-effort).
// Returns false if the command buffer isn't available yet.
bool EnqueueServerCommand(const char* text);

// True once CCommandBuffer::AddText was hooked (GOT patch of the exported symbol).
bool CommandBufferHookInstalled();
// True once the engine called AddText at least once (EnqueueServerCommand can work).
bool CommandBufferSeen();

}  // namespace readyup

