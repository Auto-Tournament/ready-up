// Public keys for Auto Tournament license keys (license.h). Public keys are not secret.
//
// Rotation: add the new key here (keep the old ones, so keys signed with them still verify),
// ship a release, and only then does the website start signing with the new key. The same list
// is src/lib/license/public-keys.json in Auto-Tournament/website (GET /api/license/public-keys).
// kid = the first 16 base64url characters of SHA-256 of the raw 32-byte key; x = that raw key,
// base64url without padding.
#include "readyup/license.h"

namespace readyup::license {

const std::vector<PublicKey>& EmbeddedPublicKeys() {
  static const std::vector<PublicKey> kKeys = {
      {"tWl_YS3_AzLgqdkm", "YQMKIdrQtVz-QFV3Tw0AWa7RivnNtjK6PiJzskg8R0g"},
  };
  return kKeys;
}

}  // namespace readyup::license
