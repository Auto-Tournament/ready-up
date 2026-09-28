Vendored upstream Monocypher
============================

- Upstream: https://github.com/LoupVaillant/Monocypher
- Version:  4.0.3 (tag), commit ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f
- License:  BSD-2-Clause OR CC0-1.0 (see LICENCE.md)
- Files:    src/monocypher.{c,h} and src/optional/monocypher-ed25519.{c,h}, copied verbatim.

Used only by core/src/readyup/license.cpp: `crypto_ed25519_check` (Ed25519 with SHA-512,
RFC 8032; rejects non-canonical signatures like OpenSSL) to verify Auto Tournament license keys
offline. Built as its own static library (readyup_monocypher) with warnings off.
