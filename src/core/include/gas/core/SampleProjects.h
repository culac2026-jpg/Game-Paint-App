#pragma once

#include <memory>
#include <string>
#include <vector>

namespace gas {

class Document;

// A deterministic set of documents that together use every feature the
// project format stores (layers, blend modes, masks, effects, selection
// edits, vector layers, slices with pivots and 9-slice borders, animation
// frames and tags, large sparse canvases). The QA tests and the benchmark
// build them; the app could offer them as examples.
struct SampleProject {
    std::string name; // file-name friendly
    std::string description;
    std::unique_ptr<Document> document;
};

std::vector<SampleProject> buildSampleProjects();

} // namespace gas
