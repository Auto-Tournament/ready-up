// Standalone mock platform for trying fleet.so on a dev server:
//
//   build/fleet_mock_platform [--port 28105] [--interval-ms 10000] [--timeout-ms 30000]
//
// Then in readyup.cfg:  [fleet]  url=http://127.0.0.1:28105  insecure_dev=1  enroll_code=RUE-TEST-TEST-TEST
// Prints every frame (secrets redacted). Commands on stdin:
//   close <code> [reason]   send a close frame          drop       drop the TCP connection
//   send <type> <json>      reliable message            ping       platform ping
//   quit
#include "mock_platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <unistd.h>

int main(int argc, char** argv) {
  int port = 28105;
  mock::Platform p;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--interval-ms")) p.heartbeatIntervalMs = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--timeout-ms")) p.heartbeatTimeoutMs = std::atoi(argv[i + 1]);
  }
  p.verbose = true;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!p.Start(port)) {
    std::fprintf(stderr, "cannot listen on 127.0.0.1:%d\n", port);
    return 1;
  }
  std::printf("[mock] listening on %s\n", p.BaseUrl().c_str());
  std::fflush(stdout);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") break;
    if (line == "drop") p.DropCurrent();
    else if (line == "ping") p.SendToServer("ping", "{\"t\":1}", false);
    else if (line.rfind("close ", 0) == 0) {
      const int code = std::atoi(line.c_str() + 6);
      const size_t sp = line.find(' ', 6);
      p.CloseCurrent(code, sp == std::string::npos ? "" : line.substr(sp + 1));
    } else if (line.rfind("send ", 0) == 0) {
      const size_t sp = line.find(' ', 5);
      if (sp != std::string::npos) p.SendToServer(line.substr(5, sp - 5), line.substr(sp + 1), true);
    }
    std::fflush(stdout);
  }
  // stdin closed (e.g. running under nohup): keep serving until killed.
  if (!std::cin) {
    for (;;) pause();
  }
  p.Stop();
  return 0;
}
