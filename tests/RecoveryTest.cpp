#include "gas/core/Document.h"
#include "gas/core/ProjectIO.h"
#include "gas/core/Recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace gas;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("gas_recovery_" + std::to_string(stamp));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void paint(Document& doc, Point p, Rgba8 c) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    img->setPixel(p, c);
    doc.endEdit("Paint");
}

} // namespace

TEST(Recovery, SaveListAndRemove) {
    TempDir dir;
    RecoveryStore store(dir.path() / "recovery"); // created on first save
    EXPECT_TRUE(store.entries().empty());
    Document doc(40, 30);
    paint(doc, {5, 6}, {10, 20, 30, 255});
    const std::u8string original = u8"D:/Dự án/nhân vật.gas";
    ASSERT_TRUE(store.save(doc, "session-1", original, "nhân vật.gas", 1000).ok);
    const std::vector<RecoveryEntry> entries = store.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].session, "session-1");
    EXPECT_EQ(entries[0].originalPath, original);
    EXPECT_EQ(entries[0].title, "nhân vật.gas");
    EXPECT_EQ(entries[0].savedAt, 1000);
    EXPECT_GT(entries[0].bytes, 0u);

    LoadResult loaded = loadProject(entries[0].project);
    ASSERT_TRUE(loaded.document) << loaded.error;
    EXPECT_EQ(loaded.document->width(), 40);
    EXPECT_EQ(loaded.document->flattenedImage().pixel({5, 6}), (Rgba8{10, 20, 30, 255}));

    EXPECT_TRUE(store.remove("session-1"));
    EXPECT_TRUE(store.entries().empty());
}

TEST(Recovery, AutosaveOverwritesTheSameSession) {
    TempDir dir;
    RecoveryStore store(dir.path());
    Document doc(16, 16);
    ASSERT_TRUE(store.save(doc, "abc", {}, "Untitled", 10).ok);
    paint(doc, {1, 1}, {255, 0, 0, 255});
    ASSERT_TRUE(store.save(doc, "abc", {}, "Untitled", 20).ok);
    const auto entries = store.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].savedAt, 20);
    EXPECT_TRUE(entries[0].originalPath.empty());
    EXPECT_EQ(loadProject(entries[0].project).document->flattenedImage().pixel({1, 1}).r, 255);
}

TEST(Recovery, NewestFirstAndIncompleteEntriesSkipped) {
    TempDir dir;
    RecoveryStore store(dir.path());
    Document doc(8, 8);
    ASSERT_TRUE(store.save(doc, "old", {}, "Old", 100).ok);
    ASSERT_TRUE(store.save(doc, "new", {}, "New", 200).ok);
    // A project without info (crash between the two writes) and a garbage info file.
    ASSERT_TRUE(saveProject(doc, dir.path() / "orphan.gas").ok);
    std::ofstream(dir.path() / "junk.info") << "not a recovery file\n";
    std::ofstream(dir.path() / "junk.gas") << "x";
    const auto entries = store.entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].session, "new");
    EXPECT_EQ(entries[1].session, "old");
}

TEST(Recovery, RejectsUnsafeSessionIds) {
    TempDir dir;
    RecoveryStore store(dir.path());
    Document doc(8, 8);
    EXPECT_FALSE(store.save(doc, "../escape", {}, "x", 1).ok);
    EXPECT_FALSE(store.save(doc, "", {}, "x", 1).ok);
    EXPECT_FALSE(store.save(doc, std::string(65, 'a'), {}, "x", 1).ok);
    EXPECT_TRUE(RecoveryStore::validSession("Ab-9_z"));
    EXPECT_FALSE(store.remove("a/b"));
}

TEST(Recovery, MultiLineTitlesSurvive) {
    TempDir dir;
    RecoveryStore store(dir.path());
    Document doc(8, 8);
    ASSERT_TRUE(store.save(doc, "s", u8"C:\\a\\b.gas", "line1\nline2 \\ end", 5).ok);
    const auto entries = store.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].title, "line1\nline2 \\ end");
    EXPECT_EQ(entries[0].originalPath, u8"C:\\a\\b.gas");
}

TEST(Recovery, MarkUnsavedMakesTheDocumentModified) {
    Document doc(8, 8);
    EXPECT_FALSE(doc.isModified());
    doc.markUnsaved();
    EXPECT_TRUE(doc.isModified());
    doc.markSaved();
    EXPECT_FALSE(doc.isModified());
}

TEST(Backup, SaveKeepsThePreviousVersion) {
    TempDir dir;
    const fs::path file = dir.path() / "art.gas";
    Document doc(8, 8);
    paint(doc, {0, 0}, {1, 2, 3, 255});
    ASSERT_TRUE(saveProject(doc, file, {.backup = true}).ok);
    EXPECT_FALSE(fs::exists(backupPathFor(file))); // nothing to back up yet
    paint(doc, {0, 0}, {9, 9, 9, 255});
    ASSERT_TRUE(saveProject(doc, file, {.backup = true}).ok);
    ASSERT_TRUE(fs::exists(backupPathFor(file)));
    EXPECT_EQ(loadProject(backupPathFor(file)).document->flattenedImage().pixel({0, 0}).r, 1);
    EXPECT_EQ(loadProject(file).document->flattenedImage().pixel({0, 0}).r, 9);
    // Without the option the backup is left alone.
    paint(doc, {0, 0}, {5, 5, 5, 255});
    ASSERT_TRUE(saveProject(doc, file).ok);
    EXPECT_EQ(loadProject(backupPathFor(file)).document->flattenedImage().pixel({0, 0}).r, 1);
}

TEST(Backup, SerializeMatchesTheSavedFile) {
    TempDir dir;
    Document doc(20, 20);
    paint(doc, {3, 4}, {7, 8, 9, 255});
    ASSERT_TRUE(saveProject(doc, dir.path() / "a.gas").ok);
    std::ifstream in(dir.path() / "a.gas", std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(file, serializeProject(doc));
    LoadResult parsed = parseProject(file.data(), file.size());
    ASSERT_TRUE(parsed.document);
    EXPECT_EQ(parsed.document->flattenedImage().pixel({3, 4}).g, 8);
}
