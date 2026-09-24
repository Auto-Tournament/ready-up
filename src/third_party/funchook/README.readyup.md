Vendored upstream funchook
==========================

- Upstream: https://github.com/kubo/funchook
- Version:  v1.1.3 (tag), commit 88388db3c69e16c1560fee65c6857d75f5ce6fd5
- License:  GPLv2+ with linking exception (see LICENSE)
- Files:    include/funchook.h and the x86_64/unix/distorm subset of src/ copied verbatim.
- config/config.h replaces the CMake-generated config.h (x86_64 Linux, distorm backend).

Disassembler: diStorm 3.5.2 (commit ab59d6e193948cfa5d1482fb6c7e64870e9e93b9, the submodule pinned by
funchook v1.1.3), vendored in ../distorm (BSD-3-Clause, see ../distorm/COPYING).

funchook decodes the target prologue, relocates every whole instruction it displaces into
the trampoline (fixing RIP-relative operands and rel8/rel32 branches), and refuses to hook
(returns an error) if it cannot do so safely. Do not replace it with a fixed-length patcher.
