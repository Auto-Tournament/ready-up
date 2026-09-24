#pragma once

// Minimal libpq client for plugins (libs/, no core dependencies): one lazily (re)connected
// connection behind a mutex, parameterized queries, text results. Blocking: call it from a
// worker thread, never the game thread. Built with READYUP_NO_POSTGRES it compiles to stubs
// that report "postgres support not compiled in".

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace readyup::pgc {

// True if libpq was compiled in.
bool Compiled();

struct Result {
  std::vector<std::vector<std::string>> rows;  // text values; SQL NULL reads as ""
  int affected = 0;                            // PQcmdTuples for UPDATE/INSERT/DELETE
};

class Client {
 public:
  // `log(debug, line)` receives connect/error lines (debug=true for chatty ones). May be empty.
  explicit Client(std::function<void(bool debug, const std::string& line)> log = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Sets the libpq conninfo (and a password-free copy for logs). Drops any open connection.
  void Configure(const std::string& conninfo, const std::string& sanitized);
  bool Configured() const;

  // Runs `sql` with $1..$n text parameters. false + *err on failure (reconnects next call).
  bool Exec(const std::string& sql, const std::vector<std::string>& params, Result* out, std::string* err);

  // Closes the connection (safe to call any time; the next Exec reconnects).
  void Close();

 private:
  bool ConnectLocked(std::string* err);

  mutable std::mutex mu_;
  void* conn_ = nullptr;  // PGconn*
  std::string conninfo_;
  std::string sanitized_;
  std::function<void(bool, const std::string&)> log_;
};

}  // namespace readyup::pgc
