#include "StatsSidecarFile.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "util/FileContentEquals.h"

namespace StatsSidecarFile {

bool writeAtomically(const char* tag, const char* path, const uint8_t* data, const size_t size,
                     const Validator isValid) {
  // Identical content needs no write; avoids SD wear on repeated saves.
  if (fileContentEquals(tag, path, data, size)) return true;

  const std::string tmpPath = std::string(path) + ".tmp";
  const std::string bakPath = std::string(path) + ".bak";

  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR(tag, "Could not remove stale temp file: %s", tmpPath.c_str());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite(tag, tmpPath, file)) {
    LOG_ERR(tag, "Could not open temp file: %s", tmpPath.c_str());
    return false;
  }
  const bool written = file.write(data, size) == size;
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!written || !synced || !closed) {
    LOG_ERR(tag, "Could not finish temp file: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!fileContentEquals(tag, tmpPath.c_str(), data, size)) {
    LOG_ERR(tag, "Temp file verification failed: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  const bool primaryExists = Storage.exists(path);
  const bool primaryValid = primaryExists && isValid != nullptr && isValid(path);
  const bool backupExists = Storage.exists(bakPath.c_str());
  if (primaryExists && !primaryValid && backupExists && isValid != nullptr && isValid(bakPath.c_str())) {
    // The primary is corrupt but the backup still decodes: keep the backup as
    // the recovery copy instead of rotating garbage over it.
    if (!Storage.remove(path)) {
      LOG_ERR(tag, "Could not remove corrupt file: %s", path);
      Storage.remove(tmpPath.c_str());
      return false;
    }
  } else {
    if (backupExists && primaryExists && !Storage.remove(bakPath.c_str())) {
      LOG_ERR(tag, "Could not remove old backup: %s", bakPath.c_str());
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (primaryExists && !Storage.rename(path, bakPath.c_str())) {
      LOG_ERR(tag, "Could not rotate backup: %s", path);
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }

  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR(tag, "Could not install file: %s", path);
    if (!Storage.exists(path) && Storage.exists(bakPath.c_str())) {
      Storage.rename(bakPath.c_str(), path);
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

}  // namespace StatsSidecarFile
