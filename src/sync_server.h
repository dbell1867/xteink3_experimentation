// USB-serial file sync for /books (PC side: tools/x3sync.py).
//
// Protocol: text lines starting with '@' (everything else on the link is log
// output the PC ignores), plus raw file bytes during PUT.
//   PC: @HELLO                      X3: @OK X3SYNC 1
//   PC: @LIST                       X3: @F <size> <crc32hex> <name> ... @END
//   PC: @PUT <name> <size> <crc32>  X3: @READY, then per 4 KB chunk @ACK <bytes so far>,
//                                       then @OK (CRC matched, file in place) or @ERR <why>
//   PC: @DEL <name>                 X3: @OK / @ERR
//   PC: @DONE                       X3: @BYE
// A PUT goes to "<name>.part" and is renamed only after the CRC matches, so an
// interrupted transfer never replaces a good book. Names must be plain file
// names (no '/', no ".."), so nothing outside /books can be touched.
#pragma once
#include <Arduino.h>
#include <FS.h>

#include <vector>

namespace sync_server {

struct Result {
  std::vector<String> changed;  // names written or deleted
  int failed = 0;
};

// Call from loop(). Reads any pending serial bytes; if a "@HELLO" line arrives,
// runs a whole session (blocking) and returns true. onStart() is called right
// after the reply, e.g. to put "Syncing..." on the screen.
bool poll(fs::FS& fs, Result& result, void (*onStart)());

}  // namespace sync_server
