#pragma once

#include <cstdint>

namespace wizengine {

// True when a TCP listener can be opened on this port right now.
//
// The check is a real bind: asking the OS is the only reliable answer, and it
// also matches how the server will fail if the port is taken. The probe socket
// is closed immediately, so there is a small window in which something else
// could claim the port before the server binds it - unavoidable without
// handing the socket over, and harmless here since the server reports its own
// bind failure anyway.
bool portIsFree(int port);

// First free port at or after `start`, skipping `count` consecutive ports that
// are needed together. Gives up after scanning `maxProbe` ports and returns
// `start` so the caller still tries (and reports a normal bind error).
int findFreePortRun(int start, int count, int maxProbe = 200);

}  // namespace wizengine
