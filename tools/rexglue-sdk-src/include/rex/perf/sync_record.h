#pragma once
/**
 * @file        perf/sync_record.h
 * @brief       Timestamped CSV recording for audio/video/input sync analysis.
 *
 * With --sync_record=true the runtime writes, next to the executable's working dir:
 *   sync_audio.csv  heard_ms,rms,low       one row per 5.3 ms game audio frame, stamped with
 *                                          when it reaches the speakers (estimate)
 *   sync_taps.csv   button,press_ms,seen_ms  each controller press: when it happened on the
 *                                          pad and when the game first read it
 *   sync_events.csv ms,source,event,a,b    timing diagnostics (music clock, XMA voices)
 * All times are milliseconds on one steady clock, so the files can be lined up.
 */

#include <cstdint>
#include <string>

namespace rex::sync_record {

enum class Stream { kAudio, kTaps, kEvents };

bool Enabled();
// Steady-clock time in nanoseconds (the clock all records use).
int64_t NowNs();
// Appends one CSV line (without newline) to the stream's file.
void Write(Stream stream, const std::string& line);

}  // namespace rex::sync_record
