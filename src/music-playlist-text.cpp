#include <obs-module.h>

#include "playlist-state.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

#define TEXT_SOURCE_ID "obs-music-playlist-text"

struct MusicPlaylistText {
    std::shared_ptr<PlaylistState> state;

    FT_Library freetype = nullptr;
    FT_Face face = nullptr;

    std::mutex pixel_mutex;

    std::vector<uint8_t> pixels;

    uint32_t width = 1;
    uint32_t height = 1;

    bool dirty = true;

    std::string rendered_text;

    gs_texture_t *texture = nullptr;

    std::chrono::steady_clock::time_point last_render_log;
};

static std::string find_font()
{
    const char *linux_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    };

    const char *windows_paths[] = {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
    };

    const char **paths = nullptr;
    size_t num_paths = 0;

#ifdef _WIN32
    paths = windows_paths;
    num_paths = sizeof(windows_paths) / sizeof(windows_paths[0]);
#else
    paths = linux_paths;
    num_paths = sizeof(linux_paths) / sizeof(linux_paths[0]);
#endif

    for (size_t i = 0; i < num_paths; ++i) {
        FILE *file = fopen(paths[i], "rb");

        if (file) {
            fclose(file);
            return paths[i];
        }
    }

    return {};
}

static std::vector<uint32_t> decode_utf8(
    const std::string &text)
{
    std::vector<uint32_t> result;

    for (std::size_t i = 0; i < text.size();) {
        const uint8_t c =
            static_cast<uint8_t>(text[i]);

        if (c < 0x80) {
            result.push_back(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0 &&
                   i + 1 < text.size()) {
            uint32_t cp =
                (static_cast<uint32_t>(c & 0x1F) << 6) |
                (static_cast<uint32_t>(
                     text[i + 1]) &
                 0x3F);

            result.push_back(cp);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 &&
                   i + 2 < text.size()) {
            uint32_t cp =
                (static_cast<uint32_t>(c & 0x0F) << 12) |
                ((static_cast<uint32_t>(
                     text[i + 1]) &
                  0x3F)
                 << 6) |
                (static_cast<uint32_t>(
                     text[i + 2]) &
                 0x3F);

            result.push_back(cp);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 &&
                   i + 3 < text.size()) {
            uint32_t cp =
                (static_cast<uint32_t>(c & 0x07) << 18) |
                ((static_cast<uint32_t>(
                     text[i + 1]) &
                  0x3F)
                 << 12) |
                ((static_cast<uint32_t>(
                     text[i + 2]) &
                  0x3F)
                 << 6) |
                (static_cast<uint32_t>(
                     text[i + 3]) &
                 0x3F);

            result.push_back(cp);
            i += 4;
        } else {
            result.push_back(0xFFFD);
            ++i;
        }
    }

    return result;
}

static void rasterize_text(
    MusicPlaylistText *text,
    const std::string &value)
{
    if (!text || !text->face)
        return;

    const std::vector<uint32_t> codepoints =
        decode_utf8(value);

    constexpr int FONT_SIZE = 24;
    constexpr int PADDING = 8;

    FT_Set_Pixel_Sizes(
        text->face,
        0,
        FONT_SIZE);

    int width = 1;
    int height = FONT_SIZE + PADDING * 2;

    for (uint32_t cp : codepoints) {
        if (FT_Load_Char(
                text->face,
                cp,
                FT_LOAD_RENDER) != 0) {
            continue;
        }

        width +=
            static_cast<int>(
                text->face->glyph->advance.x >> 6);
    }

    width += PADDING;

    if (width < 1)
        width = 1;

    if (height < 1)
        height = 1;

    std::vector<uint8_t> new_pixels(
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) *
        4,
        0);

    int pen_x = PADDING;

    for (uint32_t cp : codepoints) {
        if (FT_Load_Char(
                text->face,
                cp,
                FT_LOAD_RENDER) != 0) {
            continue;
        }

        FT_GlyphSlot glyph =
            text->face->glyph;

        const int glyph_top =
            glyph->bitmap_top;

        const int glyph_left =
            glyph->bitmap_left;

        for (int y = 0;
             y < static_cast<int>(
                     glyph->bitmap.rows);
             ++y) {

            for (int x = 0;
                 x < static_cast<int>(
                         glyph->bitmap.width);
                 ++x) {

                const int dst_x =
                    pen_x +
                    glyph_left +
                    x;

                const int dst_y =
                    PADDING +
                    FONT_SIZE -
                    glyph_top +
                    y;

                if (dst_x < 0 ||
                    dst_x >= width ||
                    dst_y < 0 ||
                    dst_y >= height) {
                    continue;
                }

                const uint8_t alpha =
                    glyph->bitmap.buffer[
                        y * glyph->bitmap.pitch +
                        x];

                const std::size_t index =
                    (static_cast<std::size_t>(
                         dst_y) *
                         width +
                     dst_x) *
                    4;

                /*
                 * Blanco + alpha.
                 */
                new_pixels[index + 0] = 255;
                new_pixels[index + 1] = 255;
                new_pixels[index + 2] = 255;
                new_pixels[index + 3] = alpha;
            }
        }

        pen_x +=
            static_cast<int>(
                glyph->advance.x >> 6);
    }

    {
        std::lock_guard<std::mutex> lock(
            text->pixel_mutex);

        text->pixels =
            std::move(new_pixels);

        text->width =
            static_cast<uint32_t>(width);

        text->height =
            static_cast<uint32_t>(height);

        text->dirty = true;
    }
}

static obs_properties_t *text_properties(void *)
{
    /*
     * IMPORTANTE:
     *
     * El Text Source NO tiene directory,
     * repeat_mode ni shuffle.
     *
     * Todo eso pertenece exclusivamente
     * al Audio Source.
     */
    return obs_properties_create();
}

static void text_defaults(obs_data_t *)
{
}

static void text_update(
    void *data,
    obs_data_t *)
{
    blog(LOG_DEBUG, "[obs-music-playlist] text_update called");

    auto *text =
        static_cast<MusicPlaylistText *>(data);

    if (!text)
        return;

    text->state =
        playlist_state_get();

    const std::string current =
        text->state
            ? text->state->current_track_no_ext()
            : "";

    blog(LOG_DEBUG, "[obs-music-playlist] text_update: current_track='%s'", current.c_str());

    text->rendered_text = current;

    rasterize_text(
        text,
        current);
}

static void *text_create(
    obs_data_t *settings,
    obs_source_t *)
{
    blog(LOG_DEBUG, "[obs-music-playlist] text_create called");

    auto *text =
        new MusicPlaylistText();

    if (FT_Init_FreeType(
            &text->freetype) != 0) {
        blog(LOG_ERROR, "[obs-music-playlist] FT_Init_FreeType failed");
        delete text;
        return nullptr;
    }

    const std::string font =
        find_font();

    if (font.empty()) {
        blog(LOG_ERROR, "[obs-music-playlist] No font found");
        FT_Done_FreeType(
            text->freetype);

        delete text;
        return nullptr;
    }

    blog(LOG_DEBUG, "[obs-music-playlist] Using font: %s", font.c_str());

    if (FT_New_Face(
            text->freetype,
            font.c_str(),
            0,
            &text->face) != 0) {

        blog(LOG_ERROR, "[obs-music-playlist] FT_New_Face failed for %s", font.c_str());
        FT_Done_FreeType(
            text->freetype);

        delete text;
        return nullptr;
    }

    blog(LOG_DEBUG, "[obs-music-playlist] Font loaded successfully");

    text_update(
        text,
        settings);

    blog(LOG_DEBUG, "[obs-music-playlist] text_create completed successfully");

    return text;
}

static void text_destroy(void *data)
{
    auto *text =
        static_cast<MusicPlaylistText *>(data);

    if (!text)
        return;

    if (text->texture) {
        gs_texture_destroy(
            text->texture);

        text->texture = nullptr;
    }

    if (text->face) {
        FT_Done_Face(
            text->face);

        text->face = nullptr;
    }

    if (text->freetype) {
        FT_Done_FreeType(
            text->freetype);

        text->freetype = nullptr;
    }

    delete text;
}

static void text_tick(
    void *data,
    float)
{
    auto *text =
        static_cast<MusicPlaylistText *>(data);

    if (!text)
        return;

    if (!text->state)
        text->state =
            playlist_state_get();

    if (!text->state)
        return;

    const std::string current =
        text->state->current_track_no_ext();

    if (current == text->rendered_text)
        return;

    text->rendered_text =
        current;

    blog(LOG_INFO, "[obs-music-playlist] text_tick: track changed to '%s'", current.c_str());

    rasterize_text(
        text,
        current);
}

static uint32_t text_get_width(void *data)
{
    auto *text =
        static_cast<MusicPlaylistText *>(data);

    if (!text)
        return 1;

    std::lock_guard<std::mutex> lock(
        text->pixel_mutex);

    return text->width ? text->width : 1;
}

static uint32_t text_get_height(void *data)
{
    auto *text =
        static_cast<MusicPlaylistText *>(data);

    if (!text)
        return 1;

    std::lock_guard<std::mutex> lock(
        text->pixel_mutex);

    return text->height ? text->height : 1;
}

static void text_render(void *data, gs_effect_t *effect)
{
    auto *text =
        static_cast<MusicPlaylistText *>(data);

    if (!text)
        return;

    std::vector<uint8_t> pixels;
    uint32_t width = 1;
    uint32_t height = 1;

    {
        std::lock_guard<std::mutex> lock(
            text->pixel_mutex);

        if (!text->dirty &&
            text->texture) {
            width = text->width;
            height = text->height;
        } else {
            pixels = text->pixels;

            width = text->width;
            height = text->height;

            text->dirty = false;
        }
    }

    if (!pixels.empty()) {
        if (text->texture) {
            gs_texture_destroy(
                text->texture);

            text->texture = nullptr;
        }

        // Debug: check pixel alpha values
        uint32_t non_zero_alpha = 0;
        for (size_t i = 3; i < pixels.size(); i += 4) {
            if (pixels[i] > 0) non_zero_alpha++;
        }
        
        const uint8_t *pixel_data = pixels.data();
        text->texture =
            gs_texture_create(
                width,
                height,
                GS_RGBA,
                1,
                &pixel_data,
                GS_DYNAMIC);

        auto now = std::chrono::steady_clock::now();
        if (now - text->last_render_log > std::chrono::seconds(1)) {
            blog(LOG_INFO, "[obs-music-playlist] text_render: created texture %ux%u, pixels=%zu, non_zero_alpha=%u", width, height, pixels.size(), non_zero_alpha);
            text->last_render_log = now;
        }
    }

    if (!text->texture)
        return;

    if (!effect)
        return;

    gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
    if (!tech) {
        blog(LOG_ERROR, "[obs-music-playlist] text_render: no 'Draw' technique in effect");
        return;
    }

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    gs_eparam_t *image =
        gs_effect_get_param_by_name(
            effect,
            "image");

    if (!image) {
        gs_blend_state_pop();
        return;
    }

    gs_effect_set_texture(
        image,
        text->texture);

    if (gs_technique_begin(tech)) {
        gs_technique_begin_pass(tech, 0);

        gs_draw_sprite(
            text->texture,
            0,
            width,
            height);

        gs_technique_end_pass(tech);
        gs_technique_end(tech);
    }

    gs_blend_state_pop();

    auto now = std::chrono::steady_clock::now();
    if (now - text->last_render_log > std::chrono::seconds(1)) {
        blog(LOG_INFO, "[obs-music-playlist] text_render: drawing %ux%u, texture=%p", width, height, text->texture);
        text->last_render_log = now;
    }
}

} // namespace

extern "C" obs_source_info music_playlist_text_source;

void initialize_music_playlist_text_source()
{
    music_playlist_text_source = {};

    music_playlist_text_source.id =
        TEXT_SOURCE_ID;

    music_playlist_text_source.type =
        OBS_SOURCE_TYPE_INPUT;

    music_playlist_text_source.output_flags =
        OBS_SOURCE_VIDEO;

    music_playlist_text_source.get_name =
        [](void *) {
            return "Music Playlist Text";
        };

    music_playlist_text_source.create =
        text_create;

    music_playlist_text_source.destroy =
        text_destroy;

    music_playlist_text_source.update =
        text_update;

    music_playlist_text_source.video_tick =
        text_tick;

    music_playlist_text_source.get_defaults =
        text_defaults;

    music_playlist_text_source.get_properties =
        text_properties;

    music_playlist_text_source.video_render =
        text_render;

    music_playlist_text_source.get_width =
        text_get_width;

    music_playlist_text_source.get_height =
        text_get_height;
}
