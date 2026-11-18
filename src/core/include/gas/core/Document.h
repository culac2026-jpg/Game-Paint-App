#pragma once

#include "gas/core/TiledImage.h"

namespace gas {

// v0.1: a single raster layer. Layer tree, undo/redo and save/open arrive in v0.3.
class Document {
public:
    Document(int width, int height);

    int width() const { return image_.width(); }
    int height() const { return image_.height(); }

    TiledImage& image() { return image_; }
    const TiledImage& image() const { return image_; }

private:
    TiledImage image_;
};

} // namespace gas
