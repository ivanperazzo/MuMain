#include "stdafx.h"

#include "UI/Legacy/UITextCache.h"

#include "Render/GL/GLLog.h"

#include <gl/glew.h>
#include <cstdlib>

namespace UI::Legacy
{
    std::size_t TextRenderCache::KeyHash::operator()(const Key& k) const
    {
        // FNV-1a over the text bytes, then mix in color and font handle.
        std::uint64_t h = 1469598103934665603ull;
        for (wchar_t c : k.Text)
        {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned short>(c));
            h *= 1099511628211ull;
        }
        h ^= k.Color;            h *= 1099511628211ull;
        h ^= static_cast<std::uint64_t>(k.Font); h *= 1099511628211ull;
        return static_cast<std::size_t>(h);
    }

    TextRenderCache& TextRenderCache::Instance()
    {
        static TextRenderCache s_instance;
        return s_instance;
    }

    TextRenderCache::~TextRenderCache()
    {
        // The GL context is gone by static-destruction time on some platforms;
        // do not call GL here. Textures die with the context.
    }

    bool TextRenderCache::Enabled()
    {
        if (m_enabled < 0)
        {
            // Default ON (validated in-game: uinew 5.5->~1.4ms, ~99% hit, no
            // visual regression). MU_UITEXTCACHE=0 is the opt-out kill switch.
            char buf[8] = {};
            size_t n = 0;
            const bool explicitlyOff =
                (getenv_s(&n, buf, sizeof(buf), "MU_UITEXTCACHE") == 0 && n > 0 && atoi(buf) == 0);
            m_enabled = explicitlyOff ? 0 : 1;
            Render::GL::Log("[uitextcache] %s (MU_UITEXTCACHE default ON, =0 to disable)",
                m_enabled ? "enabled" : "disabled");
        }
        return m_enabled == 1;
    }

    void TextRenderCache::BeginText(float screenRateX, float screenRateY)
    {
        if (screenRateX != m_screenRateX || screenRateY != m_screenRateY)
        {
            // Screen rate changed -> font is re-selected at a new pixel size and
            // every cached texture size is stale. Drop them all.
            if (m_screenRateX != 0.f || m_screenRateY != 0.f)
                Clear();
            m_screenRateX = screenRateX;
            m_screenRateY = screenRateY;
        }

        ++m_frame;
        if ((m_frame % 600) == 0)
        {
            const unsigned long long total = m_hits + m_misses;
            const double rate = total ? (100.0 * static_cast<double>(m_hits) / static_cast<double>(total)) : 0.0;
            Render::GL::Log("[uitextcache] frame=%llu entries=%zu hit=%.1f%% (hits=%llu miss=%llu)",
                m_frame, m_map.size(), rate, m_hits, m_misses);
            m_hits = 0;
            m_misses = 0;
        }
    }

    const TextCacheEntry* TextRenderCache::Find(const wchar_t* text, std::uint32_t textColor, std::uintptr_t font)
    {
        Key key{ text, textColor, font };
        auto it = m_map.find(key);
        if (it == m_map.end())
        {
            ++m_misses;
            return nullptr;
        }
        ++m_hits;
        // Promote to most-recently-used.
        m_lru.splice(m_lru.begin(), m_lru, it->second.LruIt);
        return &it->second.Entry;
    }

    const TextCacheEntry* TextRenderCache::Insert(const wchar_t* text, std::uint32_t textColor, std::uintptr_t font,
                                                  unsigned int texture, int width, int height)
    {
        Key key{ text, textColor, font };

        // Replace any existing entry for this key (shouldn't normally happen: a
        // hit returns early), freeing its texture first.
        auto existing = m_map.find(key);
        if (existing != m_map.end())
        {
            if (existing->second.Entry.Texture != 0)
            {
                GLuint t = existing->second.Entry.Texture;
                glDeleteTextures(1, &t);
            }
            m_lru.erase(existing->second.LruIt);
            m_map.erase(existing);
        }

        m_lru.push_front(key);
        Node node;
        node.Entry = TextCacheEntry{ texture, width, height };
        node.LruIt = m_lru.begin();
        auto inserted = m_map.emplace(std::move(key), std::move(node)).first;

        // Evict LRU over capacity.
        while (m_map.size() > kCapacity && !m_lru.empty())
        {
            const Key& victim = m_lru.back();
            auto vit = m_map.find(victim);
            if (vit != m_map.end())
            {
                if (vit->second.Entry.Texture != 0)
                {
                    GLuint t = vit->second.Entry.Texture;
                    glDeleteTextures(1, &t);
                }
                m_map.erase(vit);
            }
            m_lru.pop_back();
        }

        return &inserted->second.Entry;
    }

    void TextRenderCache::Clear()
    {
        for (auto& kv : m_map)
        {
            if (kv.second.Entry.Texture != 0)
            {
                GLuint t = kv.second.Entry.Texture;
                glDeleteTextures(1, &t);
            }
        }
        m_map.clear();
        m_lru.clear();
    }
}
