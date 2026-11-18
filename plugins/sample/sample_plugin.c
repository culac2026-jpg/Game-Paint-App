/*
 * Sample GameArt Studio plugin: three filters and one exporter, written in
 * plain C against gas/plugin/GasPlugin.h. Copy it as a starting point.
 */
#include <gas/plugin/GasPlugin.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t clampByte(double v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
}

static void setError(char* error, int32_t size, const char* message) {
    if (error && size > 0) {
        snprintf(error, (size_t)size, "%s", message);
    }
}

/* ---------------------------------------------------------------- Invert */

static const GasParam kInvertParams[] = {
    {"alpha", "Invert alpha too", GAS_PARAM_BOOL, 0, 1, 0, NULL},
};

static int32_t invertApply(void* user, GasImage* img, const double* values, char* error, int32_t errorSize) {
    (void)user;
    (void)error;
    (void)errorSize;
    const int alpha = values[0] != 0;
    for (int32_t y = 0; y < img->height; ++y) {
        uint8_t* p = img->pixels + (ptrdiff_t)y * img->stride;
        for (int32_t x = 0; x < img->width; ++x, p += 4) {
            if (p[3] == 0 && !alpha) {
                continue; /* empty pixels stay empty */
            }
            p[0] = (uint8_t)(255 - p[0]);
            p[1] = (uint8_t)(255 - p[1]);
            p[2] = (uint8_t)(255 - p[2]);
            if (alpha) {
                p[3] = (uint8_t)(255 - p[3]);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- Grayscale */

static const GasParam kGrayParams[] = {
    {"method", "Method", GAS_PARAM_CHOICE, 0, 2, 0, "Luminance (Rec. 709)|Average|Lightness"},
    {"amount", "Amount (%)", GAS_PARAM_INT, 0, 100, 100, NULL},
};

static int32_t grayApply(void* user, GasImage* img, const double* values, char* error, int32_t errorSize) {
    (void)user;
    (void)error;
    (void)errorSize;
    const int method = (int)values[0];
    const double amount = values[1] / 100.0;
    for (int32_t y = 0; y < img->height; ++y) {
        uint8_t* p = img->pixels + (ptrdiff_t)y * img->stride;
        for (int32_t x = 0; x < img->width; ++x, p += 4) {
            const double r = p[0], g = p[1], b = p[2];
            double v;
            if (method == 1) {
                v = (r + g + b) / 3.0;
            } else if (method == 2) {
                const double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b));
                v = (mx + mn) / 2.0;
            } else {
                v = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            }
            p[0] = clampByte(r + (v - r) * amount);
            p[1] = clampByte(g + (v - g) * amount);
            p[2] = clampByte(b + (v - b) * amount);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- Posterize */

static const GasParam kPosterizeParams[] = {
    {"levels", "Levels per channel", GAS_PARAM_INT, 2, 32, 4, NULL},
    {"hardAlpha", "Hard alpha (threshold 50%)", GAS_PARAM_BOOL, 0, 1, 0, NULL},
};

static int32_t posterizeApply(void* user, GasImage* img, const double* values, char* error, int32_t errorSize) {
    (void)user;
    const int levels = (int)values[0];
    const int hardAlpha = values[1] != 0;
    if (levels < 2) {
        setError(error, errorSize, "Levels must be at least 2");
        return 1;
    }
    for (int32_t y = 0; y < img->height; ++y) {
        uint8_t* p = img->pixels + (ptrdiff_t)y * img->stride;
        for (int32_t x = 0; x < img->width; ++x, p += 4) {
            for (int c = 0; c < 3; ++c) {
                const double step = 255.0 / (levels - 1);
                p[c] = clampByte(floor(p[c] / step + 0.5) * step);
            }
            if (hardAlpha) {
                p[3] = p[3] >= 128 ? 255 : 0;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- CSV exporter */

static int32_t csvWrite(void* user, const GasDocument* doc, const char* utf8Path, char* error, int32_t errorSize) {
    (void)user;
    FILE* f = NULL;
#ifdef _WIN32
    /* fopen takes the ANSI code page on Windows: convert the UTF-8 path. */
    wchar_t wide[1024];
    {
        /* Minimal UTF-8 -> UTF-16 (BMP and surrogate pairs). */
        const unsigned char* s = (const unsigned char*)utf8Path;
        int n = 0;
        while (*s && n < 1020) {
            unsigned cp;
            if (*s < 0x80) {
                cp = *s++;
            } else if ((*s & 0xE0) == 0xC0 && s[1]) {
                cp = ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
                s += 2;
            } else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) {
                cp = ((unsigned)(s[0] & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
                s += 3;
            } else if (s[1] && s[2] && s[3]) {
                cp = ((unsigned)(s[0] & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) |
                     ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
                s += 4;
            } else {
                break;
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                wide[n++] = (wchar_t)(0xD800 + (cp >> 10));
                wide[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
            } else {
                wide[n++] = (wchar_t)cp;
            }
        }
        wide[n] = 0;
    }
    f = _wfopen(wide, L"wb");
#else
    f = fopen(utf8Path, "wb");
#endif
    if (!f) {
        setError(error, errorSize, "Cannot create the file");
        return 1;
    }
    fprintf(f, "name,x,y,width,height,pivot_x,pivot_y,border_left,border_top,border_right,border_bottom\n");
    if (doc->sliceCount == 0) {
        /* No slices: one row for the whole canvas. */
        fprintf(f, "canvas,0,0,%d,%d,0.5,0.5,0,0,0,0\n", doc->width, doc->height);
    }
    for (int32_t i = 0; i < doc->sliceCount; ++i) {
        GasSliceInfo s;
        if (doc->sliceInfo(doc, i, &s) != 0) {
            continue;
        }
        /* Quote the name (CSV), doubling quotes. */
        fputc('"', f);
        for (const char* c = s.name; *c; ++c) {
            if (*c == '"') {
                fputc('"', f);
            }
            fputc(*c, f);
        }
        fprintf(f, "\",%d,%d,%d,%d,%.4f,%.4f,%d,%d,%d,%d\n", s.x, s.y, s.width, s.height, s.pivotX, s.pivotY,
                s.borderLeft, s.borderTop, s.borderRight, s.borderBottom);
    }
    const int failed = ferror(f);
    if (fclose(f) != 0 || failed) {
        setError(error, errorSize, "Failed to write the file");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- entry points */

GAS_PLUGIN_EXPORT uint32_t gas_plugin_api_version(void) { return GAS_PLUGIN_API_VERSION; }

GAS_PLUGIN_EXPORT int32_t gas_plugin_init(GasHost* host) {
    host->setPluginName(host, "Sample Filters");
    GasFilter f;
    memset(&f, 0, sizeof f);
    f.structSize = sizeof f;

    f.id = "sample.invert";
    f.name = "Invert Colors";
    f.description = "Inverts RGB (and optionally alpha); empty pixels stay empty.";
    f.paramCount = 1;
    f.params = kInvertParams;
    f.apply = invertApply;
    host->registerFilter(host, &f);

    f.id = "sample.grayscale";
    f.name = "Grayscale";
    f.description = "Desaturates by luminance, average or lightness.";
    f.paramCount = 2;
    f.params = kGrayParams;
    f.apply = grayApply;
    host->registerFilter(host, &f);

    f.id = "sample.posterize";
    f.name = "Posterize";
    f.description = "Reduces each channel to a few levels; optional hard alpha for pixel art.";
    f.paramCount = 2;
    f.params = kPosterizeParams;
    f.apply = posterizeApply;
    host->registerFilter(host, &f);

    GasExporter e;
    memset(&e, 0, sizeof e);
    e.structSize = sizeof e;
    e.id = "sample.slices-csv";
    e.name = "Slices as CSV";
    e.fileFilter = "CSV file (*.csv)";
    e.extension = ".csv";
    e.write = csvWrite;
    host->registerExporter(host, &e);
    host->log(host, "Sample plugin ready");
    return 0;
}
