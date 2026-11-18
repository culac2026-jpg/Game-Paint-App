/*
 * GameArt Studio plugin API (C ABI, version 1).
 *
 * A plugin is a shared library (.dll) placed in the "plugins" folder next to
 * the executable or in the user's plugin folder. It exports
 *
 *     GAS_PLUGIN_EXPORT uint32_t gas_plugin_api_version(void);  // return GAS_PLUGIN_API_VERSION
 *     GAS_PLUGIN_EXPORT int32_t gas_plugin_init(GasHost* host); // register, return 0 on success
 *
 * and registers filters (they edit the active layer, inside the selection, as
 * one undo step) and exporters (File > Export with Plugin). Only plain C types
 * cross the boundary, so a plugin can be built with any compiler.
 *
 * Rules:
 *  - Pixels are RGBA8 with straight (non-premultiplied) alpha, rows top to
 *    bottom, `stride` bytes apart. A pixel with alpha 0 should be all zero.
 *  - Strings are UTF-8. Strings and arrays given to the host are copied during
 *    the register call; images given to a plugin are valid only during the call.
 *  - Callbacks return 0 on success; on failure they may write a message into
 *    `error` (at most errorSize bytes including the terminating zero).
 *  - Plugins run in the editor's process: a crash in a plugin crashes the app
 *    (autosave limits the damage). Structs only ever grow: the host checks
 *    `structSize` and fills new fields with defaults for older plugins.
 */
#ifndef GAS_PLUGIN_H
#define GAS_PLUGIN_H

#include <stdint.h>

#ifdef _WIN32
#define GAS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define GAS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GAS_PLUGIN_API_VERSION 1u

typedef struct GasImage {
    int32_t width;
    int32_t height;
    int32_t stride;  /* bytes per row (>= width * 4) */
    uint8_t* pixels; /* RGBA8, straight alpha */
    int32_t originX; /* canvas position of pixel (0, 0) */
    int32_t originY;
} GasImage;

typedef enum GasParamType {
    GAS_PARAM_INT = 0,
    GAS_PARAM_FLOAT = 1,
    GAS_PARAM_BOOL = 2,   /* 0 or 1 */
    GAS_PARAM_COLOR = 3,  /* value = 0xRRGGBBAA as a double */
    GAS_PARAM_CHOICE = 4  /* value = index into `choices` ("First|Second|Third") */
} GasParamType;

typedef struct GasParam {
    const char* key;   /* stable identifier */
    const char* label; /* shown in the dialog */
    int32_t type;      /* GasParamType */
    double minValue;
    double maxValue;
    double defaultValue;
    const char* choices; /* GAS_PARAM_CHOICE only, '|' separated; otherwise NULL */
} GasParam;

typedef struct GasFilter {
    uint32_t structSize; /* sizeof(GasFilter) */
    const char* id;      /* unique, e.g. "mystudio.posterize" */
    const char* name;    /* menu text */
    const char* description;
    int32_t paramCount;
    const GasParam* params;
    /* Edits `image` in place (the selection bounds, or the whole canvas).
     * `values` holds paramCount values in the order of `params`. Pixels
     * outside the selection are restored by the host afterwards. */
    int32_t (*apply)(void* user, GasImage* image, const double* values, char* error, int32_t errorSize);
    void* user;
} GasFilter;

typedef struct GasLayerInfo {
    const char* name; /* valid until the exporter returns */
    int32_t visible;
    float opacity; /* 0..1 */
    int32_t isVector;
    int32_t hasMask;
} GasLayerInfo;

typedef struct GasSliceInfo {
    const char* name; /* valid until the exporter returns */
    int32_t x, y, width, height;
    double pivotX, pivotY; /* relative to the slice, 0..1 = edges, y down */
    int32_t borderLeft, borderTop, borderRight, borderBottom; /* 9-slice, pixels */
} GasSliceInfo;

/* Read-only view of the document given to exporters. Images returned by the
 * render functions are owned by the host and stay valid until the exporter
 * returns. */
typedef struct GasDocument {
    uint32_t structSize;
    int32_t width;
    int32_t height;
    int32_t layerCount; /* index 0 = bottom */
    int32_t frameCount;
    int32_t activeFrame;
    int32_t sliceCount;
    void* host;
    int32_t (*layerInfo)(const struct GasDocument* doc, int32_t layer, GasLayerInfo* out);
    int32_t (*sliceInfo)(const struct GasDocument* doc, int32_t slice, GasSliceInfo* out);
    int32_t (*frameDuration)(const struct GasDocument* doc, int32_t frame); /* milliseconds, or -1 */
    /* The visible layers composited (blend modes, masks, effects), whole canvas. */
    int32_t (*renderFrame)(const struct GasDocument* doc, int32_t frame, GasImage* out);
    /* One layer's own pixels in a frame (no mask, effects or opacity), whole canvas. */
    int32_t (*renderLayer)(const struct GasDocument* doc, int32_t layer, int32_t frame, GasImage* out);
    /* Writes `image` (or its sub-rectangle x, y, w, h; w = 0 means all) as a PNG file. */
    int32_t (*writePng)(const struct GasDocument* doc, const GasImage* image, int32_t x, int32_t y, int32_t w,
                        int32_t h, const char* utf8Path);
} GasDocument;

typedef struct GasExporter {
    uint32_t structSize; /* sizeof(GasExporter) */
    const char* id;
    const char* name;       /* menu text */
    const char* fileFilter; /* save dialog filter, e.g. "CSV file (*.csv)" */
    const char* extension;  /* appended when missing, e.g. ".csv" */
    int32_t (*write)(void* user, const GasDocument* doc, const char* utf8Path, char* error, int32_t errorSize);
    void* user;
} GasExporter;

typedef struct GasHost {
    uint32_t apiVersion; /* GAS_PLUGIN_API_VERSION of the host */
    uint32_t structSize;
    void* context;
    /* Return 0 when registered; non-zero for a duplicate id or invalid data. */
    int32_t (*registerFilter)(struct GasHost* host, const GasFilter* filter);
    int32_t (*registerExporter)(struct GasHost* host, const GasExporter* exporter);
    void (*log)(struct GasHost* host, const char* message);
    /* Sets the plugin's display name (default: the library file name). */
    void (*setPluginName)(struct GasHost* host, const char* name);
} GasHost;

typedef uint32_t (*GasPluginApiVersionFn)(void);
typedef int32_t (*GasPluginInitFn)(GasHost* host);

#ifdef __cplusplus
}
#endif

#endif /* GAS_PLUGIN_H */
