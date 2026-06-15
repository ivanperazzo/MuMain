#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

// String -> GL-texture cache for the legacy GDI text renderer
// (CUIRenderTextOriginal). The legacy path rasterizes every string with GDI
// (TextOut), converts the 24bpp result to RGBA and uploads it with
// glTexSubImage2D once PER STRING PER FRAME, with no reuse. The HUD redraws the
// same labels/counters/names every frame, so identical text is re-rasterized
// constantly -- the dominant cost of the UI pass (`uinew`, docs/perf/06).
//
// This cache stores the fully rasterized string as its own GL texture keyed by
// (text + baked color + font). On a hit the renderer skips TextOut + the RGBA
// conversion + the upload and just draws the cached texture as a quad. Layout
// (box clamp / sort / clip) stays per-call and is applied via the quad's UVs,
// so the same texture serves a string regardless of its box/alignment.
//
// Default ON (validated in-game); MU_UITEXTCACHE=0 is the kill switch. When
// disabled the renderer uses the legacy rasterize-every-frame path verbatim.
namespace UI::Legacy
{
    // One cached, fully rasterized string. The texture is RGBA8 with
    // GL_NEAREST/GL_CLAMP_TO_EDGE, matching the BITMAP_FONT scratch texture.
    struct TextCacheEntry
    {
        unsigned int Texture = 0;   // owned GL texture name
        int          Width   = 0;   // glyph region width  in real pixels (full string)
        int          Height  = 0;   // glyph region height in real pixels
    };

    // Process-wide LRU cache. Not thread-safe: the legacy UI renders on the
    // single render/GL thread, same as the rest of this subsystem.
    class TextRenderCache
    {
    public:
        static TextRenderCache& Instance();

        // Reads MU_UITEXTCACHE once and memoizes it. False => the caller must
        // fall back to the legacy rasterize-every-frame path.
        bool Enabled();

        // Runtime toggle (future in-game antilag panel). Overrides the env default.
        void SetEnabled(bool on) { m_enabled = on ? 1 : 0; }

        // Call once per cache-eligible RenderText. Bumps the frame counter,
        // flushes the whole cache if the screen rate changed (cached sizes would
        // be stale), and periodically logs the hit/miss ratio.
        void BeginText(float screenRateX, float screenRateY);

        // Returns the entry for this key and promotes it in the LRU, or nullptr
        // on a miss. Hits/misses are counted here.
        const TextCacheEntry* Find(const wchar_t* text, std::uint32_t textColor, std::uintptr_t font);

        // Stores a freshly created texture (takes ownership of `texture`) and
        // returns the stored entry. Evicts the least-recently-used entry (and
        // deletes its GL texture) when over capacity.
        const TextCacheEntry* Insert(const wchar_t* text, std::uint32_t textColor, std::uintptr_t font,
                                     unsigned int texture, int width, int height);

        // Deletes every cached GL texture and empties the cache.
        void Clear();

    private:
        TextRenderCache() = default;
        ~TextRenderCache();
        TextRenderCache(const TextRenderCache&) = delete;
        TextRenderCache& operator=(const TextRenderCache&) = delete;

        struct Key
        {
            std::wstring  Text;
            std::uint32_t Color = 0;
            std::uintptr_t Font = 0;
            bool operator==(const Key& o) const
            {
                return Color == o.Color && Font == o.Font && Text == o.Text;
            }
        };
        struct KeyHash
        {
            std::size_t operator()(const Key& k) const;
        };

        using LruList = std::list<Key>;
        struct Node
        {
            TextCacheEntry    Entry;
            LruList::iterator LruIt;
        };

        static constexpr std::size_t kCapacity = 2048;

        std::unordered_map<Key, Node, KeyHash> m_map;
        LruList                                m_lru;          // front = most recent

        int   m_enabled       = -1;     // -1 = not yet probed, 0/1 = cached flag
        float m_screenRateX   = 0.f;
        float m_screenRateY   = 0.f;
        unsigned long long m_frame = 0;
        unsigned long long m_hits  = 0;
        unsigned long long m_misses = 0;
    };
}
