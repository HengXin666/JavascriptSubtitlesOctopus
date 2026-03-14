/*
    SubtitleOctopus.js
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <cstdint>
#include <cmath>
#include <ass/ass.h>

#include "libass.cpp"

/* stb_image for VSFilterMod image rendering */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

/* ---- VSFilterMod Image Support ---- */
#define VSFMOD_IMG_CHANNELS 4
#define VSFMOD_IMG_CACHE_SIZE 64
#define VSFMOD_MAX_PATH_LEN 1024
#define VSFMOD_MAX_IMG_WIDTH 4096
#define VSFMOD_MAX_IMG_HEIGHT 4096

typedef struct {
    uint8_t *pixels;        /* RGBA pixel data */
    int width;
    int height;
    char path[VSFMOD_MAX_PATH_LEN];
    uint32_t hash;
    int ref_count;
} VSFModImage;

typedef struct {
    VSFModImage *images[VSFMOD_IMG_CHANNELS];
    bool active[VSFMOD_IMG_CHANNELS];
} VSFModImageState;

typedef struct {
    VSFModImage entries[VSFMOD_IMG_CACHE_SIZE];
    int count;
} VSFModImageCache;

static uint32_t vsfmod_hash_string(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static VSFModImage *vsfmod_cache_find(VSFModImageCache *cache, const char *path, uint32_t hash) {
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].hash == hash &&
            strcmp(cache->entries[i].path, path) == 0 &&
            cache->entries[i].pixels != NULL)
            return &cache->entries[i];
    }
    return NULL;
}

static VSFModImage *vsfmod_cache_alloc(VSFModImageCache *cache) {
    if (cache->count < VSFMOD_IMG_CACHE_SIZE)
        return &cache->entries[cache->count++];
    int min_idx = 0, min_ref = cache->entries[0].ref_count;
    for (int i = 1; i < VSFMOD_IMG_CACHE_SIZE; i++) {
        if (cache->entries[i].ref_count < min_ref) {
            min_ref = cache->entries[i].ref_count;
            min_idx = i;
        }
    }
    if (cache->entries[min_idx].pixels) free(cache->entries[min_idx].pixels);
    memset(&cache->entries[min_idx], 0, sizeof(VSFModImage));
    return &cache->entries[min_idx];
}

static VSFModImage *vsfmod_image_load(VSFModImageCache *cache, const char *path) {
    if (!path || !path[0] || strlen(path) >= VSFMOD_MAX_PATH_LEN) return NULL;
    uint32_t hash = vsfmod_hash_string(path);
    VSFModImage *cached = vsfmod_cache_find(cache, path, hash);
    if (cached) { cached->ref_count++; return cached; }
    int w, h, channels;
    uint8_t *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "[vsfiltermod] Failed to load image: %s (%s)\n", path, stbi_failure_reason());
        return NULL;
    }
    if (w > VSFMOD_MAX_IMG_WIDTH || h > VSFMOD_MAX_IMG_HEIGHT) {
        fprintf(stderr, "[vsfiltermod] Image too large: %s (%dx%d)\n", path, w, h);
        stbi_image_free(pixels); return NULL;
    }
    VSFModImage *img = vsfmod_cache_alloc(cache);
    if (!img) { stbi_image_free(pixels); return NULL; }
    img->pixels = pixels; img->width = w; img->height = h;
    strncpy(img->path, path, VSFMOD_MAX_PATH_LEN - 1);
    img->path[VSFMOD_MAX_PATH_LEN - 1] = '\0';
    img->hash = hash; img->ref_count = 1;
    return img;
}

static void vsfmod_image_release(VSFModImage *img) {
    if (img && img->ref_count > 0) img->ref_count--;
}

static void vsfmod_cache_init(VSFModImageCache *cache) { memset(cache, 0, sizeof(*cache)); }

static void vsfmod_cache_destroy(VSFModImageCache *cache) {
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].pixels) { free(cache->entries[i].pixels); cache->entries[i].pixels = NULL; }
    }
    cache->count = 0;
}

static void vsfmod_state_init(VSFModImageState *state) { memset(state, 0, sizeof(*state)); }

static void vsfmod_state_reset(VSFModImageState *state) {
    for (int i = 0; i < VSFMOD_IMG_CHANNELS; i++) {
        if (state->images[i]) { vsfmod_image_release(state->images[i]); state->images[i] = NULL; }
        state->active[i] = false;
    }
}

static void vsfmod_state_set_image(VSFModImageState *state, int channel, VSFModImage *img) {
    if (channel < 0 || channel >= VSFMOD_IMG_CHANNELS) return;
    if (state->images[channel]) vsfmod_image_release(state->images[channel]);
    state->images[channel] = img;
    state->active[channel] = (img != NULL);
    if (img) img->ref_count++;
}

static bool vsfmod_state_has_images(const VSFModImageState *state) {
    for (int i = 0; i < VSFMOD_IMG_CHANNELS; i++)
        if (state->active[i]) return true;
    return false;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
// make IDE happy
#define emscripten_get_now() 0.0
#endif

int log_level = 3;

class ReusableBuffer2D {
private:
    void *buffer;
    size_t size;
    int lessen_counter;

public:
    ReusableBuffer2D(): buffer(NULL), size(0), lessen_counter(0) {}

    ~ReusableBuffer2D() {
        free(buffer);
    }

    void clear() {
        free(buffer);
        buffer = NULL;
        size = 0;
        lessen_counter = 0;
    }

    /*
     * Request a raw pointer to a buffer being able to hold at least
     * x times y values of size member_size.
     * If zero is set to true, the requested region will be zero-initialised.
     * On failure NULL is returned.
     * The pointer is valid during the lifetime of the ReusableBuffer
     * object until the next call to get_rawbuf or clear.
     */
    void *get_rawbuf(size_t x, size_t y, size_t member_size, bool zero) {
        if (x > SIZE_MAX / member_size / y)
            return NULL;

        size_t new_size = x * y * member_size;
        if (!new_size) new_size = 1;
        if (size >= new_size) {
            if (size >= 1.3 * new_size) {
                // big reduction request
                lessen_counter++;
            } else {
                lessen_counter = 0;
            }
            if (lessen_counter < 10) {
                // not reducing the buffer yet
                if (zero)
                    memset(buffer, 0, new_size);
                return buffer;
            }
        }

        free(buffer);
        buffer = malloc(new_size);
        if (buffer) {
            size = new_size;
            memset(buffer, 0, size);
        } else
            size = 0;
        lessen_counter = 0;
        return buffer;
    }
};

void msg_callback(int level, const char *fmt, va_list va, void *data) {
    if (level > log_level) // 6 for verbose
        return;

    const int ERR_LEVEL = 1;
    FILE* stream = level <= ERR_LEVEL ? stderr : stdout;

    fprintf(stream, "libass: ");
    vfprintf(stream, fmt, va);
    fprintf(stream, "\n");
}

const float MIN_UINT8_CAST = 0.9 / 255;
const float MAX_UINT8_CAST = 255.9 / 255;

#define CLAMP_UINT8(value) ((value > MIN_UINT8_CAST) ? ((value < MAX_UINT8_CAST) ? (int)(value * 255) : 255) : 0)

typedef struct RenderBlendResult {
public:
    int changed;
    double blend_time;
    int dest_x, dest_y, dest_width, dest_height;
    unsigned char* image;
} RenderBlendResult;

/**
 * \brief Overwrite tag with whitespace to nullify its effect
 * Boundaries are inclusive at both ends.
 */
static void _remove_tag(char *begin, char *end) {
    if (end < begin)
        return;
    memset(begin, ' ', end - begin + 1);
}

/**
 * \param begin point to the first character of the tag name (after backslash)
 * \param end   last character that can be read; at least the name itself
                and the following character if any must be included
 * \return true if tag may cause animations, false if it will definitely not
 */
static bool _is_animated_tag(char *begin, char *end) {
    if (end <= begin)
        return false;

    size_t length = end - begin + 1;

    #define check_simple_tag(tag)  (sizeof(tag)-1 < length && !strncmp(begin, tag, sizeof(tag)-1))
    #define check_complex_tag(tag) (check_simple_tag(tag) && (begin[sizeof(tag)-1] == '(' \
                                        || begin[sizeof(tag)-1] == ' ' || begin[sizeof(tag)-1] == '\t'))
    switch (begin[0]) {
        case 'k': //-fallthrough
        case 'K':
            // Karaoke: k, kf, ko, K and kt ; no other valid ASS-tag starts with k/K
            return true;
        case 't':
            // Animated transform: no other valid tag begins with t
            // non-nested t-tags have to be complex tags even in single argument
            // form, but nested t-tags (which act like independent t-tags) are allowed to be
            // simple-tags without parentheses due to VSF-parsing quirk.
            // Since all valid simple t-tags require the existence of a complex t-tag, we only check for complex tags
            // to avoid false positives from invalid simple t-tags. This makes animation-dropping somewhat incorrect
            // but as animation detection remains accurate, we consider this to be "good enough"
            return check_complex_tag("t");
        case 'm':
            // Movement: complex tag; again no other valid tag begins with m
            // but ensure it's complex just to be sure
            return check_complex_tag("move");
        case 'f':
            // Fade: \fad and Fade (complex): \fade; both complex
            // there are several other valid tags beginning with f
            return check_complex_tag("fad") || check_complex_tag("fade");
    }

    return false;
    #undef check_complex_tag
    #undef check_simple_tag
}

/**
 * \param start First character after { (optionally spaces can be dropped)
 * \param end   Last character before } (optionally spaces can be dropped)
 * \param drop_animations If true animation tags will be discarded
 * \return true if after processing the event may contain animations
           (i.e. when dropping animations this is always false)
 */
static bool _is_block_animated(char *start, char *end, bool drop_animations)
{
    char *tag_start = NULL; // points to beginning backslash
    for (char *p = start; p <= end; p++) {
        if (*p == '\\') {
            // It is safe to go one before and beyond unconditionally
            // because the text passed in must be surronded by { }
            if (tag_start && _is_animated_tag(tag_start + 1, p - 1)) {
                if (!drop_animations)
                    return true;
                // For \t transforms this will assume the final state
                _remove_tag(tag_start, p - 1);
            }
            tag_start = p;
        }
    }

    if (tag_start && _is_animated_tag(tag_start + 1, end)) {
        if (!drop_animations)
            return true;
        _remove_tag(tag_start, end);
    }

    return false;
}

/**
 * \param event ASS event to be processed
 * \param drop_animations If true animation tags will be discarded
 * \return true if after processing the event may contain animations
           (i.e. when dropping animations this is always false)
 */
static bool _is_event_animated(ASS_Event *event, bool drop_animations) {
    // Event is animated if it has an Effect or animated override tags
    if (event->Effect && event->Effect[0] != '\0') {
        if (!drop_animations) return 1;
        event->Effect[0] = '\0';
    }

    // Search for override blocks
    // Only closed {...}-blocks are parsed by VSFilters and libass
    char *block_start = NULL; // points to opening {
    for (char *p = event->Text; *p != '\0'; p++) {
        switch (*p) {
            case '{':
                // Escaping the opening curly bracket to not start an override block is
                // a VSFilter-incompatible libass extension. But we only use libass, so...
                if (!block_start && (p == event->Text || *(p-1) != '\\'))
                    block_start = p;
                break;
            case '}':
                if (block_start && p - block_start > 2
                        && _is_block_animated(block_start + 1, p - 1, drop_animations))
                    return true;
                block_start = NULL;
                break;
            default:
                break;
        }
    }

    return false;
}

class SubtitleOctopus {
private:
    ReusableBuffer2D m_blend;
    RenderBlendResult m_blendResult;
    bool drop_animations;
    int scanned_events; // next unscanned event index
    VSFModImageCache m_image_cache;
    VSFModImageState m_image_state;
public:
    ASS_Library* ass_library;
    ASS_Renderer* ass_renderer;
    ASS_Track* track;

    int canvas_w;
    int canvas_h;

    int status;

    std::string defaultFont;

    SubtitleOctopus() {
        status = 0;
        ass_library = NULL;
        ass_renderer = NULL;
        track = NULL;
        canvas_w = 0;
        canvas_h = 0;
        drop_animations = false;
        scanned_events = 0;
        vsfmod_cache_init(&m_image_cache);
        vsfmod_state_init(&m_image_state);
    }

    void setLogLevel(int level) {
        log_level = level;
    }

    void setDropAnimations(int value) {
        drop_animations = !!value;
        if (drop_animations)
            scanAnimations(scanned_events);
    }

    /*
     * \brief Scan events starting at index i for animations
     * and discard animated tags when found.
     * Note that once animated tags were dropped they cannot be restored.
     * Updates the class member scanned_events to last scanned index.
     */
    void scanAnimations(int i) {
        for (; i < track->n_events; i++) {
             _is_event_animated(track->events + i, drop_animations);
        }
        scanned_events = i;
    }

    void initLibrary(int frame_w, int frame_h, char* default_font) {
        if (default_font != NULL) {
            defaultFont.assign(default_font);
        }

        ass_library = ass_library_init();
        if (!ass_library) {
            fprintf(stderr, "jso: ass_library_init failed!\n");
            exit(2);
        }

        ass_set_message_cb(ass_library, msg_callback, NULL);
        ass_set_extract_fonts(ass_library, 1);

        ass_renderer = ass_renderer_init(ass_library);
        if (!ass_renderer) {
            fprintf(stderr, "jso: ass_renderer_init failed!\n");
            exit(3);
        }

        resizeCanvas(frame_w, frame_h);

        reloadFonts();
        m_blend.clear();
    }

    /* TRACK */
    void createTrack(char* subfile) {
        reloadLibrary();
        track = ass_read_file(ass_library, subfile, NULL);
        if (!track) {
            fprintf(stderr, "jso: Failed to start a track\n");
            exit(4);
        }
        scanAnimations(0);
    }

    void createTrackMem(char *buf, unsigned long bufsize) {
        reloadLibrary();
        track = ass_read_memory(ass_library, buf, (size_t)bufsize, NULL);
        if (!track) {
            fprintf(stderr, "jso: Failed to start a track\n");
            exit(4);
        }
        scanAnimations(0);
    }

    void removeTrack() {
        if (track != NULL) {
            ass_free_track(track);
            track = NULL;
        }
    }
    /* TRACK */

    /* CANVAS */
    void resizeCanvas(int frame_w, int frame_h) {
        ass_set_frame_size(ass_renderer, frame_w, frame_h);
        canvas_h = frame_h;
        canvas_w = frame_w;
    }
    ASS_Image* renderImage(double time, int* changed) {
        ASS_Image *img = ass_render_frame(ass_renderer, track, (int) (time * 1000), changed);
        return img;
    }
    /* CANVAS */

    void quitLibrary() {
        removeTrack();
        ass_renderer_done(ass_renderer);
        ass_library_done(ass_library);
        m_blend.clear();
        vsfmod_cache_destroy(&m_image_cache);
        vsfmod_state_reset(&m_image_state);
    }
    void reloadLibrary() {
        quitLibrary();

        initLibrary(canvas_w, canvas_h, NULL);
    }

    void reloadFonts() {
        ass_set_fonts(ass_renderer, defaultFont.c_str(), NULL, ASS_FONTPROVIDER_FONTCONFIG, "/assets/fonts.conf", 1);
    }

    void setMargin(int top, int bottom, int left, int right) {
        ass_set_margins(ass_renderer, top, bottom, left, right);
    }

    int getEventCount() const {
        return track->n_events;
    }

    int allocEvent() {
        return ass_alloc_event(track);
    }

    void removeEvent(int eid) {
        ass_free_event(track, eid);
    }

    int getStyleCount() const {
        return track->n_styles;
    }

    int getStyleByName(const char* name) const {
        for (int n = 0; n < track->n_styles; n++) {
            if (track->styles[n].Name && strcmp(track->styles[n].Name, name) == 0)
                return n;
        }
        return 0;
    }

    int allocStyle() {
        return ass_alloc_style(track);
    }

    void removeStyle(int sid) {
        ass_free_event(track, sid);
    }

    void removeAllEvents() {
        ass_flush_events(track);
    }

    void setMemoryLimits(int glyph_limit, int bitmap_cache_limit) {
        printf("jso: setting total libass memory limits to: glyph=%d MiB, bitmap cache=%d MiB\n",
            glyph_limit, bitmap_cache_limit);
        ass_set_cache_limits(ass_renderer, glyph_limit, bitmap_cache_limit);
    }

    /* ---- VSFilterMod Image API ---- */

    /**
     * Set an image for a specific channel (0=primary, 1=secondary, 2=border, 3=shadow).
     * The image is loaded from the virtual filesystem path.
     * Pass empty string or NULL to clear a channel.
     */
    void setChannelImage(int channel, const char *path) {
        if (channel < 0 || channel >= VSFMOD_IMG_CHANNELS) return;
        if (!path || !path[0]) {
            vsfmod_state_set_image(&m_image_state, channel, NULL);
            return;
        }
        VSFModImage *img = vsfmod_image_load(&m_image_cache, path);
        if (img) {
            vsfmod_state_set_image(&m_image_state, channel, img);
            vsfmod_image_release(img); // state holds its own ref
        }
    }

    /**
     * Write a file to the virtual filesystem.
     * Used by JS to inject image files before calling setChannelImage.
     */
    void writeToFile(const char *path, const void *data, int size) {
#ifdef __EMSCRIPTEN__
        // Ensure directory exists
        std::string pathStr(path);
        size_t lastSlash = pathStr.rfind('/');
        if (lastSlash != std::string::npos) {
            std::string dir = pathStr.substr(0, lastSlash);
            EM_ASM({
                var dir = UTF8ToString($0);
                try { FS.mkdirTree(dir); } catch(e) {}
            }, dir.c_str());
        }

        FILE *fp = fopen(path, "wb");
        if (fp) {
            fwrite(data, 1, size, fp);
            fclose(fp);
        } else {
            fprintf(stderr, "jso: Failed to write file: %s\n", path);
        }
#endif
    }

    RenderBlendResult* renderBlend(double tm, int force) {
        m_blendResult.blend_time = 0.0;
        m_blendResult.image = NULL;

        ASS_Image *img = ass_render_frame(ass_renderer, track, (int)(tm * 1000), &m_blendResult.changed);
        if (img == NULL || (m_blendResult.changed == 0 && !force)) {
            return &m_blendResult;
        }

        double start_blend_time = emscripten_get_now();

        // find bounding rect first
        int min_x = img->dst_x, min_y = img->dst_y;
        int max_x = img->dst_x + img->w - 1, max_y = img->dst_y + img->h - 1;
        ASS_Image *cur;
        for (cur = img->next; cur != NULL; cur = cur->next) {
            if (cur->w == 0 || cur->h == 0) continue; // skip empty images
            if (cur->dst_x < min_x) min_x = cur->dst_x;
            if (cur->dst_y < min_y) min_y = cur->dst_y;
            int right = cur->dst_x + cur->w - 1;
            int bottom = cur->dst_y + cur->h - 1;
            if (right > max_x) max_x = right;
            if (bottom > max_y) max_y = bottom;
        }

        int width = max_x - min_x + 1, height = max_y - min_y + 1;

        if (width == 0 || height == 0) {
            // all images are empty
            return &m_blendResult;
        }

        // make float buffer for blending
        float* buf = (float*)m_blend.get_rawbuf(width, height, sizeof(float) * 4, true);
        if (buf == NULL) {
            fprintf(stderr, "jso: cannot allocate buffer for blending\n");
            return &m_blendResult;
        }

        // blend things in
        for (cur = img; cur != NULL; cur = cur->next) {
            int curw = cur->w, curh = cur->h;
            if (curw == 0 || curh == 0) continue; // skip empty images
            int a = (255 - (cur->color & 0xFF));
            if (a == 0) continue; // skip transparent images

            int curs = (cur->stride >= curw) ? cur->stride : curw;
            int curx = cur->dst_x - min_x, cury = cur->dst_y - min_y;

            unsigned char *bitmap = cur->bitmap;
            float normalized_a = a / 255.0;

            // VSFilterMod: 确定当前图层对应的通道
            // IMAGE_TYPE_CHARACTER(0) -> channel 0 (primary)
            // IMAGE_TYPE_OUTLINE(1) -> channel 2 (border)
            // IMAGE_TYPE_SHADOW(2) -> channel 3 (shadow)
            int channel = 0;
            switch (cur->type) {
                case 0: channel = 0; break; // primary fill
                case 1: channel = 2; break; // border/outline
                case 2: channel = 3; break; // shadow
                default: channel = 0; break;
            }

            // VSFilterMod: 优先使用事件级别的 \Nimg 纹理路径
            const VSFModImage *texture = NULL;
            if (cur->channel_images[channel] && cur->channel_images[channel][0]) {
                // 事件级别纹理: 从 ASS_Image 的 channel_images 路径加载
                VSFModImage *evt_img = vsfmod_image_load(&m_image_cache, cur->channel_images[channel]);
                if (evt_img) {
                    texture = evt_img;
                    // 不需要手动 release，缓存会管理生命周期
                }
            }
            // 回退到全局纹理
            if (!texture && m_image_state.active[channel]) {
                texture = m_image_state.images[channel];
            }

            if (texture && texture->pixels) {
                // Textured blend: use texture colors instead of solid color
                // VSFilterMod 语义: 图片从绘图区域的原始坐标空间平铺
                // 当有变换矩阵（\fsvp/\frz等）时，使用逆变换将 bitmap 坐标映射回原始坐标
                int tex_w = texture->width;
                int tex_h = texture->height;
                const uint8_t *tex_pixels = texture->pixels;

                // 判断是否需要使用逆变换矩阵
                bool use_inv = (cur->has_inv_transform != 0);

                int buf_line_coord = cury * width;
                for (int y = 0, bitmap_offset = 0; y < curh; y++, bitmap_offset += curs, buf_line_coord += width)
                {
                    for (int x = 0; x < curw; x++)
                    {
                        float pix_alpha = bitmap[bitmap_offset + x] * normalized_a / 255.0;
                        if (pix_alpha < MIN_UINT8_CAST) continue;

                        int tex_x, tex_y;
                        if (use_inv) {
                            // 使用逆变换矩阵：将 bitmap 屏幕坐标反推到原始绘图坐标
                            double sx = (double)(cur->dst_x + x);
                            double sy = (double)(cur->dst_y + y);
                            // inv_transform 映射：屏幕坐标 → 原始轮廓坐标 (26.6 定点数)
                            double ox = cur->inv_transform[0][0] * sx + cur->inv_transform[0][1] * sy + cur->inv_transform[0][2];
                            double oy = cur->inv_transform[1][0] * sx + cur->inv_transform[1][1] * sy + cur->inv_transform[1][2];
                            // 转换为像素坐标 (26.6 定点数 / 64)
                            int px = (int)floor(ox / 64.0);
                            int py = (int)floor(oy / 64.0);
                            // wrap 平铺（处理负数情况）
                            tex_x = ((px % tex_w) + tex_w) % tex_w;
                            tex_y = ((py % tex_h) + tex_h) % tex_h;
                        } else {
                            // 无变换：直接使用局部坐标平铺
                            tex_x = x % tex_w;
                            tex_y = y % tex_h;
                        }

                        const uint8_t *tp = tex_pixels + (tex_y * tex_w + tex_x) * 4;

                        // Combine glyph alpha with texture alpha
                        float tex_alpha = pix_alpha * (tp[3] / 255.0);
                        if (tex_alpha < MIN_UINT8_CAST) continue;

                        float inv_alpha = 1.0 - tex_alpha;
                        float r = tp[0] / 255.0;
                        float g = tp[1] / 255.0;
                        float b = tp[2] / 255.0;

                        int buf_coord = (buf_line_coord + curx + x) << 2;
                        float *buf_r = buf + buf_coord;
                        float *buf_g = buf + buf_coord + 1;
                        float *buf_b = buf + buf_coord + 2;
                        float *buf_a = buf + buf_coord + 3;

                        *buf_a = tex_alpha + *buf_a * inv_alpha;
                        *buf_r = r * tex_alpha + *buf_r * inv_alpha;
                        *buf_g = g * tex_alpha + *buf_g * inv_alpha;
                        *buf_b = b * tex_alpha + *buf_b * inv_alpha;
                    }
                }
            } else {
                // Original solid color blend
                float r = ((cur->color >> 24) & 0xFF) / 255.0;
                float g = ((cur->color >> 16) & 0xFF) / 255.0;
                float b = ((cur->color >> 8) & 0xFF) / 255.0;

                int buf_line_coord = cury * width;
                for (int y = 0, bitmap_offset = 0; y < curh; y++, bitmap_offset += curs, buf_line_coord += width)
                {
                    for (int x = 0; x < curw; x++)
                    {
                        float pix_alpha = bitmap[bitmap_offset + x] * normalized_a / 255.0;
                        float inv_alpha = 1.0 - pix_alpha;

                        int buf_coord = (buf_line_coord + curx + x) << 2;
                        float *buf_r = buf + buf_coord;
                        float *buf_g = buf + buf_coord + 1;
                        float *buf_b = buf + buf_coord + 2;
                        float *buf_a = buf + buf_coord + 3;

                        *buf_a = pix_alpha + *buf_a * inv_alpha;
                        *buf_r = r * pix_alpha + *buf_r * inv_alpha;
                        *buf_g = g * pix_alpha + *buf_g * inv_alpha;
                        *buf_b = b * pix_alpha + *buf_b * inv_alpha;
                    }
                }
            }
        }

        // now build the result;
        // NOTE: we use a "view" over [float,float,float,float] array of pixels,
        // so we _must_ go left-right top-bottom to not mangle the result
        unsigned int *result = (unsigned int*)buf;
        for (int y = 0, buf_line_coord = 0; y < height; y++, buf_line_coord += width) {
            for (int x = 0; x < width; x++) {
                unsigned int pixel = 0;
                int buf_coord = (buf_line_coord + x) << 2;
                float alpha = buf[buf_coord + 3];
                if (alpha > MIN_UINT8_CAST) {
                    // need to un-multiply the result
                    float value = buf[buf_coord] / alpha;
                    pixel |= CLAMP_UINT8(value); // R
                    value = buf[buf_coord + 1] / alpha;
                    pixel |= CLAMP_UINT8(value) << 8; // G
                    value = buf[buf_coord + 2] / alpha;
                    pixel |= CLAMP_UINT8(value) << 16; // B
                    pixel |= CLAMP_UINT8(alpha) << 24; // A
                }
                result[buf_line_coord + x] = pixel;
            }
        }

        // return the thing
        m_blendResult.dest_x = min_x;
        m_blendResult.dest_y = min_y;
        m_blendResult.dest_width = width;
        m_blendResult.dest_height = height;
        m_blendResult.blend_time = emscripten_get_now() - start_blend_time;
        m_blendResult.image = (unsigned char*)result;
        return &m_blendResult;
    }
};

int main(int argc, char *argv[]) { return 0; }

#ifdef __EMSCRIPTEN__
#include "./SubOctpInterface.cpp"
#endif
