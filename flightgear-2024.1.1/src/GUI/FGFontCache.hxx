//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License as
//  published by the Free Software Foundation; either version 2 of the
//  License, or (at your option) any later version.
// 
//  This program is distributed in the hope that it will be useful, but
//  WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
#ifndef __FGFONTCACHE_HXX
#define __FGFONTCACHE_HXX

#include <simgear/misc/sg_path.hxx>

#include <map>
#include <algorithm>

// forward decls
class SGPropertyNode;
class puFont;
// 前向声明：纹理字体基类。中文化后字体可能是 fntTexFont（.txf 纹理字体）
// 或 fntUnicodeFont（FreeType 渲染的 .ttf/.otf 字体），统一按基类持有。
class fntFont;

/**
 * A class to keep all fonts available for future use.
 * This also assures a font isn't resident more than once.
 */
class FGFontCache {
private:
    FGFontCache(); // private constructor, use singleton instance() accessor

    // The parameters of a request to the cache.
    struct FntParams
    {
        const std::string name;
        const float size;
        const float slant;
        FntParams() : size(0.0f), slant(0.0f) {}
        FntParams(const FntParams& rhs)
            : name(rhs.name), size(rhs.size), slant(rhs.slant)
        {
        }
        FntParams(const std::string& name_, float size_, float slant_)
            : name(name_), size(size_), slant(slant_)
        {
        }

        bool operator<(const FntParams& other) const;
    };

    struct FontCacheEntry {
        FontCacheEntry(puFont* pu = 0) : pufont(pu), texfont(0) {}
        ~FontCacheEntry();

        // Font used by plib GUI code
        puFont *pufont;
        // 底层字体对象（中文化后为 fntFont 基类指针）：
        // 既可能是 .txf 的 fntTexFont，也可能是 .ttf/.otf 的 fntUnicodeFont。
        fntFont *texfont;

        bool ownsPUFont = false;
    };

    // Path to the font directory
    SGPath _path;

    typedef std::map<const std::string, fntFont*> TexFontMap;
    typedef std::map<const FntParams, FontCacheEntry*> PuFontMap;
    TexFontMap _texFonts;
    PuFontMap _cache;

    bool _initialized;
    FontCacheEntry* getfnt(const std::string& name, float size, float slant);
    void init();

public:
    // note this accesor is NOT thread-safe
    static FGFontCache* instance();
    static void shutdown();

    ~FGFontCache();

    puFont *get(const std::string& name, float size=15.0, float slant=0.0);
    puFont *get(SGPropertyNode *node);

    fntFont *getTexFont(const std::string& name, float size=15.0, float slant=0.0);

    /**
     * 按相对字体目录（$FG_ROOT/Fonts 或 FG_FONTS）的路径显式加载一个字体，
     * 返回底层 fntFont 对象（.txf -> fntTexFont，.ttf/.otf -> fntUnicodeFont）。
     *
     * 二次开发新增：ui_theme.xml 的主题字体允许位于 Fonts/cn 之类的子目录，
     * 而 initializeFonts() 只扫描顶层目录，因此主题初始化时用它显式加载。
     * 同名（相对路径）重复调用直接返回已有实例，不会重复加载；
     * 加载失败返回 nullptr 并输出 SG_WARN。
     */
    fntFont *loadFont(const std::string& relName);

    SGPath getfntpath(const std::string& name);
    /**
     * Preload all the fonts in the FlightGear font directory. It is
     * important to load the font textures early, with the proper
     * graphics context current, so that no plib (or our own) code
     * tries to load a font from disk when there's no current graphics
     * context.
     */
    bool initializeFonts();
};
#endif
