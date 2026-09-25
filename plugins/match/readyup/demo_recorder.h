#pragma once

// GOTV demo recording per map, plus a generic uploader.
//
//  - Recording starts when a map goes live: `tv_enable 1`, then
//    `tv_record "<ru_demo_path><name>.dem"` where <name> comes from
//    ru_demo_name_format.
//  - At map end the stop waits for the GOTV flush (tv_delay + 15 s, see
//    match_end.cpp), then `tv_stoprecord`.
//  - The upload runs on its own thread: wait until the file size settles (or search
//    the demo dirs for this match's newest demo), then send the file as
//    application/octet-stream with ru_demo_upload_method (POST or PUT) to
//    ru_demo_upload_url, with the configured headers, retrying network errors,
//    429 and 5xx (ru_demo_upload_attempts). No header names are built in: add what
//    the receiver needs with ru_demo_upload_header.
//
// Name / URL / header value tokens:
//   {TIME} (local, yyyy-mm-dd_HH-MM-SS)  {MATCH_ID}  {SLUG}  {MAP}  {MAP_NUMBER} (1-based)
//   {MAP_INDEX} (0-based)  {TEAM1}  {TEAM2}  {TEAM1_SCORE}  {TEAM2_SCORE}
//   upload only: {FILENAME}  {ROUND_NUMBER}
//
// Console / RCON settings:
//   ru_demo_recording_enabled 0|1          default 1
//   ru_demo_path <dir/>                    relative to csgo/, ends with '/', default "ReadyUp/"
//   ru_demo_name_format "<fmt>"            default "{TIME}_{MATCH_ID}_{MAP}_{TEAM1}_vs_{TEAM2}"
//   ru_demo_upload_url <url>|clear         empty = keep demos on disk only
//   ru_demo_upload_method POST|PUT         default POST
//   ru_demo_upload_header "<Name>" "<value>"   add/replace one header ("" value removes it)
//   ru_demo_upload_headers_clear
//   ru_demo_upload_attempts <1-10>         default 3
//   ru_demo_status                         print settings, recording and the last upload
// `tv_delay N` typed on the console is observed (not consumed) for the flush timing.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace readyup::demo {

struct Settings {
  bool recordingEnabled = true;
  std::string path = "ReadyUp/";
  std::string nameFormat = "{TIME}_{MATCH_ID}_{MAP}_{TEAM1}_vs_{TEAM2}";
  std::string uploadUrl;
  std::string uploadMethod = "POST";
  std::vector<std::pair<std::string, std::string>> uploadHeaders;
  int uploadAttempts = 3;
};

Settings Get();
void SetRecordingEnabled(bool on);
// Must not start with '/' or '.', must end with '/', no "//" or "..". Empty = csgo/ root.
bool SetPath(const std::string& path, std::string* err);
void SetNameFormat(const std::string& format);
void SetUploadUrl(const std::string& url);
bool SetUploadMethod(const std::string& method);
// Empty value removes the header.
void SetUploadHeader(const std::string& name, const std::string& value);
void ClearUploadHeaders();
void SetUploadAttempts(int attempts);

// tv_delay seen on the console. Match cvars (tv_delay / tv_delay1) win.
void ObserveTvDelay(int seconds);
// max(tv_delay, tv_delay1) from the match cvars, else the value seen on the console,
// else CS2's default (10).
int TvDelaySeconds();

// ---------------------------------------------------------------------------- lifecycle

struct RecordingInfo {
  long long matchid = 0;
  std::string slug;
  int mapNumber = 1;  // 1-based
  std::string mapName;
  std::string team1;
  std::string team2;
};

// Game thread. Returns false when recording is disabled or commands could not be queued.
bool StartRecording(const RecordingInfo& info);
bool IsRecording();
// Game thread. Stops after `delaySeconds` (the GOTV flush), then uploads when a URL is set.
// Returns false if nothing was recording.
bool StopAfterDelayAndUpload(double delaySeconds, int roundNumber, int team1Score, int team2Score);
// Game thread. Stops now, no upload (restart / admin end).
void StopNowWithoutUpload();

// ---------------------------------------------------------------------------- events

enum class DemoEventType { RecordingStarted, RecordingStopped, UploadStarted, UploadSucceeded, UploadFailed };

struct DemoEvent {
  DemoEventType type = DemoEventType::RecordingStarted;
  long long matchid = 0;
  int mapNumber = 1;        // 1-based
  std::string fileName;     // basename
  std::string path;         // relative to csgo/ (recording) or absolute (upload)
  double sizeMb = 0.0;      // upload events
  long httpStatus = 0;      // UploadSucceeded / UploadFailed
  std::string error;        // UploadFailed: "file_not_found", curl error or response excerpt
  int attempts = 0;
};

const char* EventTypeName(DemoEventType t);
std::string ToJson(const DemoEvent& e);

// Listeners run on the game thread (recording events) or the upload thread (upload events).
using Listener = std::function<void(const DemoEvent&)>;
void AddListener(Listener fn);

// ---------------------------------------------------------------------------- helpers

// Pure (tested). Replaces the tokens listed above; missing values stay empty.
struct TokenValues {
  std::string time;
  long long matchid = 0;
  std::string slug;
  std::string map;
  int mapNumber = 1;
  std::string team1;
  std::string team2;
  int team1Score = 0;
  int team2Score = 0;
  std::string fileName;
  int roundNumber = 0;
};
std::string ExpandTokens(const std::string& format, const TokenValues& v);
// Pure (tested): ExpandTokens + file-name safety (spaces -> '_', no path separators or quotes).
std::string FormatDemoFileName(const std::string& format, const TokenValues& v);

// Pure (tested): when the expected file is missing, the newest .dem whose name contains
// "_<matchid>_" (and the map name when given), not older than notBeforeEpoch.
struct DemoCandidate {
  std::string path;
  long long mtimeEpoch = 0;
};
std::string PickDemoForMatch(const std::vector<DemoCandidate>& files, const std::string& expectedFileName,
                             long long matchid, const std::string& mapName, long long notBeforeEpoch);

// Settings commands above. Returns true if the line was consumed.
bool HandleConsoleLine(const std::vector<std::string>& args);
std::vector<std::string> StatusLines();

}  // namespace readyup::demo
