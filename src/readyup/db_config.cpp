#include "readyup/db_config.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/path.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <string>

namespace readyup {
namespace {

std::once_flag g_dbCfgOnce;
std::optional<DbConfig> g_dbCfg;

static std::string ReadWholeFile(const std::string& path) {
  std::ifstream f(path);
  if (!f.good()) return {};
  std::string s;
  f.seekg(0, std::ios::end);
  s.resize(static_cast<size_t>(f.tellg()));
  f.seekg(0, std::ios::beg);
  f.read(&s[0], static_cast<std::streamsize>(s.size()));
  return s;
}

static void SkipWs(const std::string& s, size_t& i) {
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c) == 0) break;
    ++i;
  }
}

static bool Consume(const std::string& s, size_t& i, char ch) {
  SkipWs(s, i);
  if (i < s.size() && s[i] == ch) {
    ++i;
    return true;
  }
  return false;
}

static std::optional<std::string> ParseJsonString(const std::string& s, size_t& i) {
  SkipWs(s, i);
  if (i >= s.size() || s[i] != '"') return std::nullopt;
  ++i;
  std::string out;
  out.reserve(64);
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') return out;
    if (c == '\\') {
      if (i >= s.size()) return std::nullopt;
      char e = s[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default:
          // We intentionally don't implement full \uXXXX parsing; config is ASCII-ish.
          return std::nullopt;
      }
    } else {
      out.push_back(c);
    }
  }
  return std::nullopt;
}

static std::optional<long long> ParseJsonInt(const std::string& s, size_t& i) {
  SkipWs(s, i);
  bool neg = false;
  if (i < s.size() && s[i] == '-') {
    neg = true;
    ++i;
  }
  long long v = 0;
  bool any = false;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < '0' || c > '9') break;
    any = true;
    v = v * 10 + (s[i] - '0');
    ++i;
  }
  if (!any) return std::nullopt;
  return neg ? -v : v;
}

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static std::string StripPasswordKv(std::string conninfo) {
  // Best-effort: remove `password=...` token from libpq conninfo.
  // conninfo tokens are space-separated key=val; val may be quoted.
  std::string out;
  out.reserve(conninfo.size());
  size_t i = 0;
  while (i < conninfo.size()) {
    while (i < conninfo.size() && std::isspace(static_cast<unsigned char>(conninfo[i])) != 0) {
      out.push_back(conninfo[i++]);
    }
    const size_t keyStart = i;
    while (i < conninfo.size() && conninfo[i] != '=' && std::isspace(static_cast<unsigned char>(conninfo[i])) == 0) {
      ++i;
    }
    const std::string key = conninfo.substr(keyStart, i - keyStart);
    if (i >= conninfo.size() || conninfo[i] != '=') {
      // Not a key=value, just copy until next whitespace.
      while (i < conninfo.size() && std::isspace(static_cast<unsigned char>(conninfo[i])) == 0) {
        out.push_back(conninfo[i++]);
      }
      continue;
    }

    // We have key=
    if (key == "password") {
      // Skip "=<value>"
      ++i;  // '='
      if (i < conninfo.size() && conninfo[i] == '\'') {
        ++i;
        while (i < conninfo.size()) {
          char c = conninfo[i++];
          if (c == '\\') {
            if (i < conninfo.size()) ++i;
            continue;
          }
          if (c == '\'') break;
        }
      } else {
        while (i < conninfo.size() && std::isspace(static_cast<unsigned char>(conninfo[i])) == 0) ++i;
      }
      // Optionally remove trailing whitespace already copied: we didn't copy key, so nothing to undo.
      continue;
    }

    // Copy key=value token through its value.
    out.append(key);
    out.push_back('=');
    ++i;  // '='
    if (i < conninfo.size() && conninfo[i] == '\'') {
      out.push_back(conninfo[i++]);
      while (i < conninfo.size()) {
        char c = conninfo[i++];
        out.push_back(c);
        if (c == '\\') {
          if (i < conninfo.size()) out.push_back(conninfo[i++]);
          continue;
        }
        if (c == '\'') break;
      }
    } else {
      while (i < conninfo.size() && std::isspace(static_cast<unsigned char>(conninfo[i])) == 0) {
        out.push_back(conninfo[i++]);
      }
    }
  }
  return out;
}

// Minimal flat-object JSON lookup:
// Supports either:
//   { "conninfo": "..." }
// or:
//   { "host": "...", "port": 5432, "user": "...", "password": "...", "dbname": "...", "sslmode": "disable" }
struct ParsedFields {
  std::string conninfo;
  std::string host;
  std::string user;
  std::string password;
  std::string dbname;
  std::string sslmode;
  long long port = 0;
};

static std::optional<ParsedFields> ParseReadyUpDbJson(const std::string& s) {
  size_t i = 0;
  if (!Consume(s, i, '{')) return std::nullopt;

  ParsedFields f;
  bool first = true;
  while (true) {
    SkipWs(s, i);
    if (i < s.size() && s[i] == '}') {
      ++i;
      break;
    }
    if (!first) {
      if (!Consume(s, i, ',')) return std::nullopt;
    }
    first = false;

    auto key = ParseJsonString(s, i);
    if (!key) return std::nullopt;
    if (!Consume(s, i, ':')) return std::nullopt;

    if (*key == "port") {
      auto v = ParseJsonInt(s, i);
      if (!v) return std::nullopt;
      f.port = *v;
    } else {
      auto v = ParseJsonString(s, i);
      if (!v) return std::nullopt;
      if (*key == "conninfo") f.conninfo = *v;
      else if (*key == "host") f.host = *v;
      else if (*key == "user") f.user = *v;
      else if (*key == "password") f.password = *v;
      else if (*key == "dbname") f.dbname = *v;
      else if (*key == "sslmode") f.sslmode = *v;
      // Unknown keys ignored on purpose.
    }
  }

  return f;
}

static std::string QuoteConnValIfNeeded(const std::string& v) {
  // libpq conninfo supports single quotes.
  // We'll quote if whitespace present.
  if (v.find_first_of(" \t\r\n") == std::string::npos) return v;
  std::string out;
  out.reserve(v.size() + 2);
  out.push_back('\'');
  for (char c : v) {
    if (c == '\'' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

static std::string BuildConninfoFromFields(const ParsedFields& f) {
  std::string out;
  auto add = [&](const char* k, const std::string& v) {
    if (Trim(v).empty()) return;
    if (!out.empty() && out.back() != ' ') out.push_back(' ');
    out.append(k);
    out.push_back('=');
    out.append(QuoteConnValIfNeeded(v));
  };

  if (f.port > 0) {
    if (!out.empty() && out.back() != ' ') out.push_back(' ');
    out.append("port=");
    out.append(std::to_string(f.port));
  }
  add("host", f.host);
  add("user", f.user);
  add("password", f.password);
  add("dbname", f.dbname);
  add("sslmode", f.sslmode);
  return out;
}

}  // namespace

std::optional<DbConfig> ReadDbConfig() {
  const std::string dir = GetThisModuleDir();
  if (dir.empty()) return std::nullopt;
  const std::string path = dir + "/readyup_db.json";

  const std::string s = ReadWholeFile(path);
  if (s.empty()) return std::nullopt;

  auto fields = ParseReadyUpDbJson(s);
  if (!fields) {
    PrintLine("db config: failed to parse readyup_db.json (expected simple JSON object).");
    return std::nullopt;
  }

  std::string conninfo = Trim(fields->conninfo);
  if (conninfo.empty()) conninfo = BuildConninfoFromFields(*fields);
  conninfo = Trim(conninfo);
  if (conninfo.empty()) {
    PrintLine("db config: readyup_db.json present but no conninfo/fields provided.");
    return std::nullopt;
  }

  DbConfig cfg;
  cfg.conninfo = conninfo;
  cfg.conninfo_sanitized = StripPasswordKv(conninfo);
  return cfg;
}

const DbConfig* DbCfg() {
  std::call_once(g_dbCfgOnce, []() {
    if (DebugEnabled()) {
      const std::string dir = GetThisModuleDir();
      if (!dir.empty()) {
        Print("db config: loading from: %s/readyup_db.json\n", dir.c_str());
      } else {
        PrintLine("db config: loading from: (unknown module dir)");
      }
    }

    g_dbCfg = ReadDbConfig();
    if (g_dbCfg) {
      Print("db config: loaded (conninfo=%s)\n", g_dbCfg->conninfo_sanitized.c_str());
    } else {
      if (DebugEnabled()) {
        PrintLine("db config: not configured (readyup_db.json missing or invalid).");
      }
    }
  });
  return g_dbCfg ? &(*g_dbCfg) : nullptr;
}

}  // namespace readyup

