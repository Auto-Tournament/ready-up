#include "readyup/cs2_update_check.h"

#include "readyup/minijson.h"

#include <cctype>

namespace readyup::cs2update {

long long PatchVersionNumber(const std::string& text) {
  size_t i = 0;
  while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
  long long v = 0;
  int digits = 0;
  for (; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (std::isdigit(c)) {
      if (++digits > 12) return -1;
      v = v * 10 + (c - '0');
    } else if (c != '.') {
      break;
    }
  }
  return digits == 0 ? -1 : v;
}

Answer ParseAnswer(const std::string& body) {
  Answer a;
  minijson::ParseError err;
  const auto root = minijson::Parse(body, &err);
  if (!root) return a;
  const minijson::Value* r = root->get("response");
  if (!minijson::IsObject(r)) return a;
  const minijson::Value* success = r->get("success");
  if (!success || success->type != minijson::Value::Type::Bool || !success->b) return a;
  const minijson::Value* up = r->get("up_to_date");
  if (!up || up->type != minijson::Value::Type::Bool) return a;
  a.upToDate = up->b;
  if (!a.upToDate) {
    const auto req = minijson::AsInt(r->get("required_version"));
    if (!req || *req <= 0) return a;  // behind but no version to name: unusable
    a.required = *req;
  }
  a.ok = true;
  return a;
}

std::string CheckUrl(long long currentVersion) {
  return "https://api.steampowered.com/ISteamApps/UpToDateCheck/v1/?appid=" + std::to_string(kAppId) +
         "&version=" + std::to_string(currentVersion);
}

bool ShouldReport(const Answer& answer, long long lastReported) {
  return answer.ok && !answer.upToDate && answer.required > 0 && answer.required != lastReported;
}

}  // namespace readyup::cs2update
