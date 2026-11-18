#include <gas/core/Recovery.h>

#include <gas/core/Document.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace gas {

namespace fs = std::filesystem;

namespace {

constexpr const char* kInfoHeader = "GameArtStudio recovery 1";

// Values are single lines: escape backslash and newlines.
std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string fromU8(const std::u8string& s) { return {s.begin(), s.end()}; }
std::u8string toU8(const std::string& s) { return {s.begin(), s.end()}; }

} // namespace

RecoveryStore::RecoveryStore(fs::path directory) : directory_(std::move(directory)) {}

bool RecoveryStore::validSession(const std::string& session) {
    if (session.empty() || session.size() > 64) {
        return false;
    }
    return std::all_of(session.begin(), session.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

fs::path RecoveryStore::projectPath(const std::string& session) const { return directory_ / (session + ".gas"); }
fs::path RecoveryStore::infoPath(const std::string& session) const { return directory_ / (session + ".info"); }

IoResult RecoveryStore::save(const Document& document, const std::string& session, const std::u8string& originalPath,
                             const std::string& title, std::int64_t savedAt) const {
    if (!validSession(session)) {
        return {false, "Invalid recovery session id"};
    }
    std::error_code ec;
    fs::create_directories(directory_, ec);
    if (ec) {
        return {false, "Cannot create the recovery folder: " + ec.message()};
    }
    if (IoResult r = saveProject(document, projectPath(session)); !r.ok) {
        return r;
    }
    std::ostringstream info;
    info << kInfoHeader << '\n'
         << "original = " << escape(fromU8(originalPath)) << '\n'
         << "title = " << escape(title) << '\n'
         << "saved = " << savedAt << '\n';
    const std::string text = info.str();
    return writeFileAtomic(infoPath(session), std::vector<std::uint8_t>(text.begin(), text.end()));
}

bool RecoveryStore::remove(const std::string& session) const {
    if (!validSession(session)) {
        return false;
    }
    std::error_code ec;
    fs::remove(infoPath(session), ec); // info first: a lone project file is never listed
    fs::remove(projectPath(session), ec);
    return !fs::exists(infoPath(session), ec) && !fs::exists(projectPath(session), ec);
}

std::vector<RecoveryEntry> RecoveryStore::entries() const {
    std::vector<RecoveryEntry> out;
    std::error_code ec;
    fs::directory_iterator it(directory_, ec);
    if (ec) {
        return out;
    }
    for (const fs::directory_entry& e : it) {
        if (e.path().extension() != ".info") {
            continue;
        }
        const std::string session = e.path().stem().string();
        if (!validSession(session)) {
            continue;
        }
        RecoveryEntry entry;
        entry.session = session;
        entry.project = projectPath(session);
        std::error_code sizeError;
        entry.bytes = fs::file_size(entry.project, sizeError);
        if (sizeError) {
            continue; // no project file
        }
        std::ifstream in(e.path(), std::ios::binary);
        std::string line;
        if (!std::getline(in, line) || line != kInfoHeader) {
            continue;
        }
        while (std::getline(in, line)) {
            const std::size_t eq = line.find(" = ");
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, eq), value = unescape(line.substr(eq + 3));
            if (key == "original") {
                entry.originalPath = toU8(value);
            } else if (key == "title") {
                entry.title = value;
            } else if (key == "saved") {
                try {
                    entry.savedAt = std::stoll(value);
                } catch (...) {
                    entry.savedAt = 0;
                }
            }
        }
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(), [](const RecoveryEntry& a, const RecoveryEntry& b) {
        return a.savedAt != b.savedAt ? a.savedAt > b.savedAt : a.session < b.session;
    });
    return out;
}

} // namespace gas
