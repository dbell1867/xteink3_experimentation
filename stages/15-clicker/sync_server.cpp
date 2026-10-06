#include "sync_server.h"

#include <esp_rom_crc.h>

namespace sync_server {
namespace {
constexpr const char* DIR = "/books";
constexpr size_t CHUNK = 4096;
constexpr uint32_t SESSION_IDLE_MS = 20000;  // give up if the PC goes quiet
constexpr uint32_t CHUNK_TIMEOUT_MS = 3000;
uint8_t buf[CHUNK];

// Read one '\n'-terminated line (without '\r'/'\n'); false on timeout.
bool readLine(String& line, uint32_t timeoutMs) {
  line = "";
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (Serial.available()) {
      const char c = Serial.read();
      if (c == '\n') return true;
      if (c != '\r' && line.length() < 200) line += c;
    }
    delay(1);
  }
  return false;
}

bool safeName(const String& n) {
  return n.length() > 0 && n.length() < 64 && n.indexOf('/') < 0 && n.indexOf('\\') < 0 && n.indexOf("..") < 0 &&
         !n.endsWith(".part");
}

uint32_t fileCrc(File& f) {
  uint32_t crc = 0;
  for (int n; (n = f.read(buf, CHUNK)) > 0;) crc = esp_rom_crc32_le(crc, buf, n);
  return crc;
}

// A replaced or deleted .txt book must not keep page-index caches ("<name>.<px>.idx").
void dropIndexCaches(fs::FS& fs, const String& name) {
  File dir = fs.open(DIR);
  std::vector<String> doomed;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String n = f.name();
    if (n.startsWith(name + ".") && n.endsWith(".idx")) doomed.push_back(String(DIR) + "/" + n);
  }
  dir.close();
  for (const String& p : doomed) fs.remove(p);
}

void cmdList(fs::FS& fs) {
  File dir = fs.open(DIR);
  if (!dir) fs.mkdir(DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String n = f.name();
    if (f.isDirectory() || n.endsWith(".idx") || n.endsWith(".part")) continue;
    const size_t size = f.size();
    Serial.printf("@F %u %08lx %s\n", (unsigned)size, (unsigned long)fileCrc(f), n.c_str());
  }
  dir.close();
  Serial.println("@END");
}

bool cmdPut(fs::FS& fs, const String& name, size_t size, uint32_t want) {
  if (!safeName(name)) {
    Serial.println("@ERR bad name");
    return false;
  }
  if (!fs.exists(DIR)) fs.mkdir(DIR);
  const String part = String(DIR) + "/" + name + ".part";
  const String path = String(DIR) + "/" + name;
  File f = fs.open(part, FILE_WRITE);  // FILE_WRITE truncates any old .part
  if (!f) {
    Serial.println("@ERR cannot create file");
    return false;
  }
  Serial.println("@READY");
  size_t got = 0;
  uint32_t crc = 0;
  bool ok = true;
  while (got < size && ok) {
    const size_t want_n = min(CHUNK, size - got);
    size_t n = 0;
    const uint32_t t0 = millis();
    while (n < want_n && millis() - t0 < CHUNK_TIMEOUT_MS) {
      const int a = Serial.available();
      if (a > 0) n += Serial.read(buf + n, min<size_t>(a, want_n - n));
      else delay(1);
    }
    if (n < want_n) {
      ok = false;
      break;
    }
    crc = esp_rom_crc32_le(crc, buf, n);
    ok = f.write(buf, n) == n;
    got += n;
    Serial.printf("@ACK %u\n", (unsigned)got);  // the PC sends the next chunk only after this
  }
  f.close();  // buffered data reaches the card only on close (G25g)
  if (!ok || crc != want) {
    fs.remove(part);
    Serial.printf("@ERR %s\n", !ok ? "timeout/short write" : "crc mismatch");
    return false;
  }
  fs.remove(path);
  if (!fs.rename(part, path)) {
    Serial.println("@ERR rename failed");
    return false;
  }
  if (name.endsWith(".txt") || name.endsWith(".TXT")) dropIndexCaches(fs, name);
  Serial.println("@OK");
  return true;
}
}  // namespace

bool poll(fs::FS& fs, Result& result, void (*onStart)()) {
  static String pending;
  bool hello = false;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n') {
      hello = pending == "@HELLO";
      pending = "";
      if (hello) break;
    } else if (c != '\r' && pending.length() < 64) {
      pending += c;
    }
  }
  if (!hello) return false;

  Serial.println("@OK X3SYNC 1");
  if (onStart) onStart();
  result = Result{};
  String line;
  while (readLine(line, SESSION_IDLE_MS)) {
    if (line == "@LIST") {
      cmdList(fs);
    } else if (line.startsWith("@PUT ")) {
      // "@PUT <name> <size> <crc>" -- the name has no spaces (safeName also rejects odd ones)
      const int a = line.indexOf(' ', 5), b = line.indexOf(' ', a + 1);
      const String name = line.substring(5, a);
      const size_t size = line.substring(a + 1, b).toInt();
      const uint32_t crc = strtoul(line.substring(b + 1).c_str(), nullptr, 16);
      if (cmdPut(fs, name, size, crc)) result.changed.push_back(name);
      else result.failed++;
    } else if (line.startsWith("@DEL ")) {
      const String name = line.substring(5);
      if (safeName(name) && fs.remove(String(DIR) + "/" + name)) {
        dropIndexCaches(fs, name);
        result.changed.push_back(name);
        Serial.println("@OK");
      } else {
        Serial.println("@ERR cannot delete");
      }
    } else if (line == "@DONE") {
      Serial.println("@BYE");
      break;
    } else if (line.length()) {
      Serial.printf("@ERR unknown command %s\n", line.c_str());
    }
  }
  return true;
}

}  // namespace sync_server
