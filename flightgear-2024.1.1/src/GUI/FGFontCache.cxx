//
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
#include <config.h>

#include "FGFontCache.hxx"

// this is our one in 3rdparty
#include "fnt.h"

#if defined(HAVE_PUI)
#include "FlightGear_pu.h"
#endif

#include <simgear/props/props.hxx>
#include <simgear/misc/sg_dir.hxx>
// 二次开发新增：loadFont() 的失败路径需要输出 SG_WARN 日志。
#include <simgear/debug/logstream.hxx>
// 二次开发新增：扩展名大小写无关比较需要 std::tolower。
#include <cctype>
// 二次开发新增：字体回退告警去重需要 std::set。
#include <set>

#include <Main/globals.hxx>

////////////////////////////////////////////////////////////////////////
// FGFontCache class.
////////////////////////////////////////////////////////////////////////

static std::unique_ptr<FGFontCache> global_fontCacheInstance;

#if defined(HAVE_PUI)
extern puFont FONT_HELVETICA_14;
extern puFont FONT_SANS_12B;
extern puFont FONT_HELVETICA_12;
#endif
namespace
{
struct GuiFont
{
    const std::string name;
    puFont *font;
};

const std::initializer_list<GuiFont> guiFonts = {
    {"default", &PUFONT_HELVETICA_12},
    {"FIXED_8x13", &PUFONT_8_BY_13},
    {"FIXED_9x15", &PUFONT_9_BY_15},
    {"TIMES_10", &PUFONT_TIMES_ROMAN_10},
    {"TIMES_24", &PUFONT_TIMES_ROMAN_24},
    {"HELVETICA_10", &PUFONT_HELVETICA_10},
    {"HELVETICA_18", &PUFONT_HELVETICA_18},
#if defined(HAVE_PUI)
    {"HELVETICA_12", &FONT_HELVETICA_12},
    {"HELVETICA_14", &FONT_HELVETICA_14},
    {"SANS_12B", &FONT_SANS_12B},
#endif
};
}

FGFontCache* FGFontCache::instance()
{
    if (!global_fontCacheInstance.get()) {
        global_fontCacheInstance.reset(new FGFontCache);
    }

    return global_fontCacheInstance.get();
}

void FGFontCache::shutdown()
{
    global_fontCacheInstance.reset();
}

FGFontCache::FGFontCache() :
    _initialized(false)
{
}

FGFontCache::~FGFontCache()
{
    for (auto puFontIt : _cache) {
        delete puFontIt.second;
    }

    // these were created i initializeFonts
    for (auto texFontIt : _texFonts) {
        delete texFontIt.second;
    }
}

bool FGFontCache::FntParams::operator<(const FntParams& other) const
{
    int comp = name.compare(other.name);
    if (comp < 0)
        return true;
    else if (comp > 0)
        return false;
    if (size < other.size)
        return true;
    else if (size > other.size)
        return false;
    return slant < other.slant;
}

FGFontCache::FontCacheEntry*
FGFontCache::getfnt(const std::string& fontName, float size, float slant)
{
    FntParams fntParams(fontName, size, slant);
    PuFontMap::iterator i = _cache.find(fntParams);
    if (i != _cache.end())
        return i->second;
    // fonts are all preloaded into the _texFonts map
    TexFontMap::iterator texi = _texFonts.find(fontName);
    fntFont* texfont = 0;
    puFont* pufont = 0;
    if (texi != _texFonts.end()) {
        texfont = texi->second;
    } else {
        auto guifont = std::find_if(guiFonts.begin(), guiFonts.end(),
                                    [&fontName](const GuiFont& gf) {
                                        return gf.name == fontName;
                                    });

        if (guifont != guiFonts.end()) {
            pufont = guifont->font;
        }
    }

    // 二次开发修改：找不到请求的字体时，回退结果**不写入 _cache**。
    //
    // 原实现把"未找到 → 回退到内置位图字体"的条目也缓存起来。内置位图字体只有
    // 256 个单字节 Latin-1 字形，而界面文字是 UTF-8 中文：一旦某次查询发生在
    // 字体文件尚未装入 _texFonts 之前（例如 GUI 重置后 setupFont 先于
    // fguiTheme::preloadFont 生效），之后即使字体已经就绪，同一
    // (名字, 字号, 斜体) 的查询仍会命中这条负缓存，结果是整个界面（菜单栏、
    // 对话框）的中文被按 Latin-1 逐字节渲染成 "æ–‡ä»¶" 这类乱码
    //（2026-11-20 实际事故：点击重置后菜单栏变成乱码）。
    if (!pufont && !texfont) {
        // 每个字体名只告警一次，避免对话框创建时刷屏
        static std::set<std::string> warned;
        if (warned.insert(fontName).second) {
            SG_LOG(SG_GUI, SG_WARN,
                   "FGFontCache: 字体 '" << fontName
                   << "' 未找到，回退到内置位图字体（中文将显示为乱码）");
        }
        static FontCacheEntry* fallback = nullptr;
        if (!fallback) {
            fallback = new FontCacheEntry;
            fallback->pufont = guiFonts.begin()->font;
        }
        return fallback;   // 不缓存，下次查询会重新尝试解析
    }

    auto f = new FontCacheEntry;
    if (pufont) {
        f->pufont = pufont;
    } else {
        f->texfont = texfont;
        f->ownsPUFont = true; // ensure it gets cleaned up
        f->pufont = new puFont;
        f->pufont->initialize(f->texfont, size, slant);
    }

    // insert into the cache
    _cache[fntParams] = f;
    return f;
}

puFont *
FGFontCache::get(const std::string& name, float size, float slant)
{
    return getfnt(name, size, slant)->pufont;
}

fntFont *
FGFontCache::getTexFont(const std::string& name, float size, float slant)
{
    init();
    return getfnt(name, size, slant)->texfont;
}

// 显式加载一个字体文件（二次开发新增，供 ui_theme.xml 的主题字体使用）。
// 与 initializeFonts() 的"非递归扫描顶层目录"不同，这里接受相对字体目录的
// 子路径（如 "cn/simhei.ttf"），因此主题可以把中文字体放在 Fonts/cn/ 下。
fntFont *
FGFontCache::loadFont(const std::string& relName)
{
    init();

    // 同名（相对路径）已加载过：直接复用。同一个字体文件在进程内只保留一个
    // fntFont 实例，这样 setLineSpacing/setOutline 等主题参数对全部使用方生效。
    auto existing = _texFonts.find(relName);
    if (existing != _texFonts.end())
        return existing->second;

    if (relName.empty()) {
        SG_LOG(SG_GUI, SG_WARN, "FGFontCache::loadFont: empty font name");
        return nullptr;
    }

    SGPath path(_path);
    path.append(relName);
    if (!path.exists()) {
        SG_LOG(SG_GUI, SG_WARN, "FGFontCache::loadFont: font not found: "
               << path.utf8Str());
        return nullptr;
    }

    // 按扩展名选择后端：.txf 是 PLIB 纹理字体，.ttf/.otf 走 FreeType（可显示 CJK）。
    // 扩展名匹配忽略大小写：Windows 下字体文件常见 .TTF 大写形式，
    // 大小写敏感的字符串比较会把它误判为不支持。
    std::string ext = path.extension();
    for (char& c : ext)
        c = (char)std::tolower((unsigned char)c);
    fntFont* font = nullptr;
    if (ext == "txf") {
        font = new fntTexFont;
    } else if (ext == "ttf" || ext == "otf") {
        font = new fntUnicodeFont;
    } else {
        SG_LOG(SG_GUI, SG_WARN, "FGFontCache::loadFont: unsupported font format '"
               << ext << "' for " << relName);
        return nullptr;
    }

    if (!font->load(path)) {
        // load() 内部已输出具体错误，这里补一条带文件名的告警后释放对象。
        SG_LOG(SG_GUI, SG_WARN, "FGFontCache::loadFont: failed to load " << relName);
        delete font;
        return nullptr;
    }

    _texFonts[relName] = font;
    return font;
}

puFont *
FGFontCache::get(SGPropertyNode *node)
{
    if (!node)
        return get("Helvetica.txf", 15.0, 0.0);

    std::string name = node->getStringValue("name", "Helvetica.txf");
    float size = node->getFloatValue("size", 15.0);
    float slant = node->getFloatValue("slant", 0.0);

    return get(name, size, slant);
}

void FGFontCache::init()
{
    if (!_initialized) {
        char *envp = ::getenv("FG_FONTS");
        if (envp != NULL) {
            _path = SGPath::fromEnv("FG_FONTS");
        } else {
            _path = globals->get_fg_root();
            _path.append("Fonts");
        }
        _initialized = true;
    }
}

SGPath
FGFontCache::getfntpath(const std::string& name)
{
    init();
    SGPath path(_path);
    if (name != "") {
        path.append(name);
        if (path.exists())
            return path;
    }

    path = SGPath(_path);
    path.append("Helvetica.txf");
    
    return path;
}

bool FGFontCache::initializeFonts()
{
    static std::string fontext(".txf");
    init();

    auto dir = simgear::Dir(_path);
    for (auto p : dir.children(simgear::Dir::TYPE_FILE, fontext)) {
        fntTexFont* f = new fntTexFont;
        if (f->load(p))
            _texFonts[p.file()] = f;
        else
            delete f;
    }

    // 同时加载 TrueType/OpenType 字体（经 FreeType 渲染，中文化新增）：
    // 这样 PUI 菜单栏/对话框才能显示中文等 Unicode 多字节文字
    // （.txf 纹理字体只有 256 个单字节字形，无法显示 CJK）。
    for (auto ext : { ".ttf", ".otf" }) {
        for (auto p : dir.children(simgear::Dir::TYPE_FILE, ext)) {
            fntUnicodeFont* f = new fntUnicodeFont;
            if (f->load(p))
                _texFonts[p.file()] = f;
            else
                delete f;
        }
    }

    return true;
}

FGFontCache::FontCacheEntry::~FontCacheEntry()
{
    if (ownsPUFont) {
        delete pufont;
    }
}

