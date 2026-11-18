#pragma once

#include <gas/core/ProjectIO.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gas {

class Document;

// One autosaved session found in the recovery folder.
struct RecoveryEntry {
    std::string session;            // id given by the app (one per running window)
    std::filesystem::path project;  // "<dir>/<session>.gas"
    std::u8string originalPath;     // the document's file, empty for an untitled one
    std::string title;              // window title (UTF-8)
    std::int64_t savedAt = 0;       // seconds since the Unix epoch
    std::uintmax_t bytes = 0;
};

// Crash recovery storage: each running session periodically saves its
// document to "<dir>/<session>.gas" plus a small "<session>.info" text file
// (both written atomically, project first, so an entry is either complete or
// absent). A clean exit removes the session's files; whatever is left after
// a crash is offered for recovery. Deciding whether a session is still alive
// (another window) is the app's job (lock files).
class RecoveryStore {
public:
    explicit RecoveryStore(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return directory_; }

    // Session ids may use letters, digits, '-' and '_' (1..64 chars).
    static bool validSession(const std::string& session);

    IoResult save(const Document& document, const std::string& session, const std::u8string& originalPath,
                  const std::string& title, std::int64_t savedAt) const;
    // Removes the session's files (true if nothing is left).
    bool remove(const std::string& session) const;
    // Complete entries, newest first. Unreadable or partial ones are skipped.
    std::vector<RecoveryEntry> entries() const;

    std::filesystem::path projectPath(const std::string& session) const;
    std::filesystem::path infoPath(const std::string& session) const;

private:
    std::filesystem::path directory_;
};

} // namespace gas
