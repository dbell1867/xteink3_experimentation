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

// "file" or "folder/file": letters, digits, '.', '_', '-'; no "..", no hidden names.
bool safeName(const String& n) {
  if (n.length() == 0 || n.length() > 96 || n.indexOf("..") >= 0 || n.endsWith(".part")) return false;
  int slashes = 0;
  for (size_t i = 0; i < n.length(); i++) {
    const char c = n[i];
    if (c == '/') slashes++;
    else if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
  }
  const int slash = n.indexOf('/');
  return slashes <= 1 && n[0] != '.' && n[0] != '/' && !n.endsWith("/") && (slash < 0 || n[slash + 1] != '.');
}

String folderOf(const String& name) {  // "xteink3/lesson-01.xtc" -> "/books/xteink3"
  const int slash = name.lastIndexOf('/');
  return slash < 0 ? String(DIR) : String(DIR) + "/" + name.substring(0, slash);
}

uint32_t fileCrc(File& f) {
  uint32_t crc = 0;
  for (int n; (n = f.read(buf, CHUNK)) > 0;) crc = esp_rom_crc32_le(crc, buf, n);
  return crc;
}

// A replaced or deleted .txt book must not keep page-index caches ("<name>.<px>.idx").
void dropIndexCaches(fs::FS& fs, const String& name) {
  const String folder = folderOf(name);
  const String base = name.substring(name.lastIndexOf('/') + 1);
  File dir = fs.open(folder);
  if (!dir) return;
  std::vector<String> doomed;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String n = f.name();
    if (n.startsWith(base + ".") && n.endsWith(".idx")) doomed.push_back(folder + "/" + n);
  }
  dir.close();
  for (const String& p : doomed) fs.remove(p);
}

// List the files of one folder; `prefix` is "" for /books itself or "folder/".
// Subfolders are entered only from the top level (one level deep).
void listDir(fs::FS& fs, const String& path, const String& prefix) {
  File dir = fs.open(path);
  if (!dir) return;
  std::vector<String> subdirs;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String n = f.name();
    if (f.isDirectory()) {
      if (prefix.isEmpty() && n[0] != '.') subdirs.push_back(n);
      continue;
    }
    if (n.endsWith(".idx") || n.endsWith(".part")) continue;
    const size_t size = f.size();
    Serial.printf("@F %u %08lx %s%s\n", (unsigned)size, (unsigned long)fileCrc(f), prefix.c_str(), n.c_str());
  }
  dir.close();
  for (const String& d : subdirs) listDir(fs, path + "/" + d, d + "/");
}

void cmdList(fs::FS& fs) {
  if (!fs.exists(DIR)) fs.mkdir(DIR);
  listDir(fs, DIR, "");
  Serial.println("@END");
}

bool cmdPut(fs::FS& fs, const String& name, size_t size, uint32_t want) {
  if (!safeName(name)) {
    Serial.println("@ERR bad name");
    return false;
  }
  if (!fs.exists(DIR)) fs.mkdir(DIR);
  const String folder = folderOf(name);
  if (!fs.exists(folder)) fs.mkdir(folder);
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

  Serial.println("@OK X3SYNC 2");
  if (onStart) onStart();
  result = Result{};
  String line;
  while (readLine(line, SESSION_IDLE_MS)) {
    if (line.startsWith("@DATE ")) {
      result.date = line.substring(6, 6 + 40);
      Serial.println("@OK");
    } else if (line == "@LIST") {
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
        const String folder = folderOf(name);
        if (folder != DIR) fs.rmdir(folder);  // only succeeds once the folder is empty
        result.changed.push_back(name);
        Serial.println("@OK");
      } else {
        Serial.println("@ERR cannot delete");
      }
    } else if (line == "@DONE") {
      result.completed = true;
      Serial.println("@BYE");
      break;
    } else if (line.length()) {
      Serial.printf("@ERR unknown command %s\n", line.c_str());
    }
  }
  return true;
}

}  // namespace sync_server
