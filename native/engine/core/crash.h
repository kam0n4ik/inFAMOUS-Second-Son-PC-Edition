// SecondSonPC native runtime - crash handling.
//
// Installs an unhandled-exception filter that writes a minidump next to the log and a summary
// line to the log (without allocating), then lets the process terminate.
#pragma once

#include <string>

namespace sspc::crash {

void Install(const std::wstring& dump_directory);

}  // namespace sspc::crash
