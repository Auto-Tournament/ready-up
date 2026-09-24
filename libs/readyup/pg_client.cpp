#include "readyup/pg_client.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#if !defined(READYUP_NO_POSTGRES)
#include <libpq-fe.h>
#endif

namespace readyup::pgc {

#if defined(READYUP_NO_POSTGRES)

bool Compiled() { return false; }
Client::Client(std::function<void(bool, const std::string&)> log) : log_(std::move(log)) {}
Client::~Client() = default;
void Client::Configure(const std::string& c, const std::string& s) {
  std::lock_guard<std::mutex> lk(mu_);
  conninfo_ = c;
  sanitized_ = s;
}
bool Client::Configured() const {
  std::lock_guard<std::mutex> lk(mu_);
  return !conninfo_.empty();
}
bool Client::Exec(const std::string&, const std::vector<std::string>&, Result*, std::string* err) {
  if (err) *err = "postgres support not compiled in";
  return false;
}
void Client::Close() {}
bool Client::ConnectLocked(std::string* err) {
  if (err) *err = "postgres support not compiled in";
  return false;
}

#else

namespace {
std::string LastError(PGconn* c) {
  const char* m = c ? PQerrorMessage(c) : nullptr;
  std::string s = m ? m : "unknown error";
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}
}  // namespace

bool Compiled() { return true; }

Client::Client(std::function<void(bool, const std::string&)> log) : log_(std::move(log)) {}

Client::~Client() { Close(); }

void Client::Configure(const std::string& conninfo, const std::string& sanitized) {
  std::lock_guard<std::mutex> lk(mu_);
  if (conn_) {
    PQfinish(static_cast<PGconn*>(conn_));
    conn_ = nullptr;
  }
  conninfo_ = conninfo;
  sanitized_ = sanitized;
}

bool Client::Configured() const {
  std::lock_guard<std::mutex> lk(mu_);
  return !conninfo_.empty();
}

void Client::Close() {
  std::lock_guard<std::mutex> lk(mu_);
  if (conn_) {
    PQfinish(static_cast<PGconn*>(conn_));
    conn_ = nullptr;
  }
}

bool Client::ConnectLocked(std::string* err) {
  if (conninfo_.empty()) {
    if (err) *err = "db not configured (readyup_db.json missing/invalid)";
    return false;
  }
  if (conn_ && PQstatus(static_cast<PGconn*>(conn_)) == CONNECTION_OK) return true;
  if (conn_) {
    if (log_) log_(true, "db: connection not OK; reconnecting");
    PQfinish(static_cast<PGconn*>(conn_));
    conn_ = nullptr;
  }
  PGconn* c = PQconnectdb(conninfo_.c_str());
  if (!c || PQstatus(c) != CONNECTION_OK) {
    const std::string e = c ? LastError(c) : "PQconnectdb returned null";
    if (c) PQfinish(c);
    if (log_) log_(true, "db: connect failed (" + sanitized_ + "): " + e);
    if (err) *err = e;
    return false;
  }
  // Server NOTICEs (e.g. "relation already exists, skipping") would go to stderr; drop them.
  PQsetNoticeProcessor(c, [](void*, const char*) {}, nullptr);
  conn_ = c;
  if (log_) log_(false, "db: connected (" + sanitized_ + ")");
  return true;
}

bool Client::Exec(const std::string& sql, const std::vector<std::string>& params, Result* out, std::string* err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!ConnectLocked(err)) return false;
  auto* c = static_cast<PGconn*>(conn_);
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& p : params) values.push_back(p.c_str());
  PGresult* res = PQexecParams(c, sql.c_str(), static_cast<int>(values.size()), nullptr,
                               values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
  if (!res) {
    if (err) *err = LastError(c);
    return false;
  }
  const auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    const char* m = PQresultErrorMessage(res);
    if (err) *err = (m && *m) ? m : LastError(c);
    while (err && !err->empty() && (err->back() == '\n' || err->back() == '\r')) err->pop_back();
    PQclear(res);
    if (PQstatus(c) != CONNECTION_OK) {
      PQfinish(c);
      conn_ = nullptr;
    }
    return false;
  }
  if (out) {
    out->rows.clear();
    const int rows = PQntuples(res);
    const int cols = PQnfields(res);
    out->rows.reserve(rows > 0 ? static_cast<size_t>(rows) : 0u);
    for (int r = 0; r < rows; ++r) {
      std::vector<std::string> row;
      row.reserve(static_cast<size_t>(cols));
      for (int f = 0; f < cols; ++f) {
        const char* v = PQgetisnull(res, r, f) ? "" : PQgetvalue(res, r, f);
        row.emplace_back(v ? v : "");
      }
      out->rows.push_back(std::move(row));
    }
    const char* affected = PQcmdTuples(res);
    out->affected = (affected && *affected) ? std::atoi(affected) : 0;
  }
  PQclear(res);
  return true;
}

#endif

}  // namespace readyup::pgc
