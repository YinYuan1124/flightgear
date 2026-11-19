// fntUnicode.cxx -- 基于 FreeType 的 Unicode 字体，供 FlightGear 的 PUI 界面使用。
//
// 本文件是 fntFont 接口的一个即插即用实现：用 FreeType 动态渲染 UTF-8 字符串，
// 从而让 PUI 菜单栏 / 对话框可以显示中文等 CJK 多字节文字。
// （fntTexFont 只能渲染 .txf 纹理字体里的 256 个单字节字符，无法显示 CJK。）
//
// 实现思路：按需把字形光栅化进一张"覆盖率图集"纹理（GL_LUMINANCE_ALPHA，
// 亮度与 alpha 均为笔画覆盖率），绘制时每个字形画一个带纹理的四边形。
//
// 二次开发新增（PUI 主题美化）：可配置行距、文字描边、字符串级显示列表缓存、
// 可扩容/可配置大小的字形图集，以及按最大宽度自动换行（wrapString）。

#include "fntLocal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <simgear/misc/sg_path.hxx>
#include <simgear/debug/logstream.hxx>

#include <cstring>
#include <cmath>
#include <functional>       // std::hash（字符串缓存键）
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace
{
  // 图集纹理的默认边长（像素）。1024x1024 在 48px 光栅化字号下约可容纳
  // 400 个汉字，足以覆盖菜单栏/对话框的常用字符集；主题可通过
  // setAtlasSize() 在首次光栅化前放大到 2048。
  const int ATLAS_SIZE = 1024;

  // 图集边长上限（像素）。扩容时按 2 的幂倍增，超过该值不再扩容，
  // 改为输出告警并返回占位字形（否则显存占用会失控）。
  const int MAX_ATLAS_SIZE = 2048;

  // 字形光栅化时的 em 尺寸（像素）。固定用 48px 光栅化、渲染时再缩小，
  // 以获得足够的超采样精度（GUI 实际显示字号约 6~21px）。
  const float RASTER_SIZE = 48.0f;

  // 在 0 级之外额外构建并逐字形上传的 mip 层数。
  // 只做 2 级（4 倍缩小）是因为图集里字形之间只有 2px 间隔，更深的 mip 层
  // 会因间隔被平均掉而导致相邻字形互相串色。由于只定义了 0..2 级，必须
  // 同时把 GL_TEXTURE_MAX_LEVEL 设为 2（见 createTexture()），否则纹理 mipmap 不
  // 完整，采样时会被当作禁用，文字会渲染成实心色块。
  const int MIP_LEVELS = 2 ;

  // Windows 的 GL/gl.h 只声明到 OpenGL 1.1，没有下面这两个 OpenGL 1.2 的
  // 枚举，这里按规范补上常量值（注意 MAX_LOD 是 0x813B，0x813A 是 MIN_LOD，
  // 两者极易混淆——曾经写错过一次）。
#ifndef GL_TEXTURE_MAX_LOD
#define GL_TEXTURE_MAX_LOD 0x813B
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

  // 解码一个 UTF-8 码点，并把 *ps 前进到该码点之后；出错时返回 0。
  uint32_t utf8Decode ( const char **ps )
  {
    const unsigned char *s = reinterpret_cast<const unsigned char *>( *ps ) ;
    uint32_t cp ;

    if ( s[0] < 0x80 ) { cp = s[0] ; *ps += 1 ; return cp ; }
    else if ( ( s[0] & 0xE0 ) == 0xC0 ) { cp = ( ( s[0] & 0x1F ) << 6 ) | ( s[1] & 0x3F ) ; *ps += 2 ; return cp ; }
    else if ( ( s[0] & 0xF0 ) == 0xE0 ) { cp = ( ( s[0] & 0x0F ) << 12 ) | ( ( s[1] & 0x3F ) << 6 ) | ( s[2] & 0x3F ) ; *ps += 3 ; return cp ; }
    else if ( ( s[0] & 0xF8 ) == 0xF0 ) { cp = ( ( s[0] & 0x07 ) << 18 ) | ( ( s[1] & 0x3F ) << 12 ) | ( ( s[2] & 0x3F ) << 6 ) | ( s[3] & 0x3F ) ; *ps += 4 ; return cp ; }
    else { *ps += 1 ; return 0 ; }
  }

  // 把一个 Unicode 码点编码回 UTF-8 字节串（wrapString 在插入 '\n' 后
  // 需要逐字符重建输出字符串，故需要反向编码）。
  void utf8Append ( std::string& out, uint32_t cp )
  {
    if ( cp < 0x80 )
    {
      out += (char) cp ;
    }
    else if ( cp < 0x800 )
    {
      out += (char) ( 0xC0 | ( cp >> 6 ) ) ;
      out += (char) ( 0x80 | ( cp & 0x3F ) ) ;
    }
    else if ( cp < 0x10000 )
    {
      out += (char) ( 0xE0 | ( cp >> 12 ) ) ;
      out += (char) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) ;
      out += (char) ( 0x80 | ( cp & 0x3F ) ) ;
    }
    else
    {
      out += (char) ( 0xF0 | ( cp >> 18 ) ) ;
      out += (char) ( 0x80 | ( ( cp >> 12 ) & 0x3F ) ) ;
      out += (char) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) ;
      out += (char) ( 0x80 | ( cp & 0x3F ) ) ;
    }
  }

  // 把 GL 当前颜色（0..1 浮点）量化打包成 8 位 RGBA，作为字符串缓存的键。
  // 显示列表里的顶点颜色在编译期固化，因此颜色必须进入缓存键。
  unsigned int packColor8 ( const GLfloat c[4] )
  {
    const auto q = []( float v ) -> unsigned int
    {
      if ( v <= 0.0f ) return 0 ;
      if ( v >= 1.0f ) return 255 ;
      return (unsigned int) ( v * 255.0f + 0.5f ) ;
    } ;
    return q ( c[0] ) | ( q ( c[1] ) << 8 ) | ( q ( c[2] ) << 16 ) | ( q ( c[3] ) << 24 ) ;
  }

  // 字符串显示列表缓存的键：字符串 + 量化后的 pointsize/italic + 8 位颜色 + 描边开关。
  // 之所以把 pointsize 与 italic 都放进键：显示列表里保存的是编译期固化的顶点
  // 坐标，字号/倾斜不同则几何不同，复用会画错。
  struct StringKey
  {
    std::string text ;
    int pointQ = 0 ;
    int italicQ = 0 ;
    unsigned int color = 0 ;
    bool outline = false ;

    bool operator== ( const StringKey& o ) const
    {
      return pointQ == o.pointQ && italicQ == o.italicQ &&
             color == o.color && outline == o.outline && text == o.text ;
    }
  } ;

  struct StringKeyHash
  {
    std::size_t operator() ( const StringKey& k ) const
    {
      std::size_t h = std::hash<std::string>() ( k.text ) ;
      h = h * 131u + (std::size_t) k.pointQ ;
      h = h * 131u + (std::size_t) k.italicQ ;
      h = h * 131u + (std::size_t) k.color ;
      h = h * 131u + (std::size_t) ( k.outline ? 1u : 0u ) ;
      return h ;
    }
  } ;
}

struct fntUnicodeFont::Impl
{
  struct Glyph
  {
    // 图集内的 UV 坐标（t_bot 对应字形的底边行）。
    float t_left, t_bot, t_right, t_top ;
    // 相对基线的顶点坐标，单位 = ascender 的几分之一（见下方 scale 说明）。
    float v_left, v_bot, v_right, v_top ;
    // 水平步进宽度（同上，按 ascender 归一化）。
    float advance ;
  } ;

  FT_Library library = nullptr ;   // FreeType 库句柄
  FT_Face face = nullptr ;         // 字体面（本字体对应一个 .ttf/.otf 文件）
  GLuint texture = 0 ;             // 字形图集纹理对象（惰性创建，0 = 尚未创建）
  int pen_x = 0, pen_y = 0, row_h = 0 ;  // 图集打包游标：当前 x/y 位置与当前行高

  // 归一化系数：顶点空间中 1.0 代表字体的 ascender（基线到最高字形顶部的
  // 距离，单位像素）。之所以按 ascender 而不是按 em 高度归一化，是为了与
  // fntTexFont 保持一致 —— fntTexFont 把每个度量都除以 max_height
  // （即 ascender），这样同一个 pointsize 在两种字体后端下渲染出的视觉
  // 高度完全相同（em 高度约比大写字母高度大 30%，若按 em 归一化字会偏小）。
  float scale = 1.0f ;

  // 已光栅化字形的缓存：码点 -> 字形度量。
  std::unordered_map<uint32_t, Glyph> glyphs ;

  // ---- 二次开发新增的主题排版参数 ----
  float lineSpacing = 1.0f ;       // 行距系数：换行垂直步进 = pointsize × 该值
  float outlineWidth = 0.0f ;      // 描边像素宽度，<= 0 表示关闭
  float outlineR = 0.0f ;          // 描边颜色（直通 alpha 的 RGBA）
  float outlineG = 0.0f ;
  float outlineB = 0.0f ;
  float outlineA = 1.0f ;
  bool  stringCache = false ;      // 是否启用字符串级显示列表缓存
  int   atlasSize = ATLAS_SIZE ;   // 图集当前/目标边长（2 的幂）
  bool  atlasFullWarned = false ;  // 图集满的告警只输出一次，避免刷屏

  // 字形代数：每往 glyphs 里新增一个字形（或图集扩容重建）就 +1。
  // 字符串显示列表在编译期固化了字形几何，代数变化说明字形数据已变，
  // 命中缓存时必须丢弃旧列表并重建。
  unsigned glyphGeneration = 0 ;

  // 字符串显示列表缓存：键 -> (GL 显示列表号, 编译时的字形代数)。
  struct StringListEntry
  {
    GLuint list = 0 ;
    unsigned generation = 0 ;
    // 回放结束时光标相对调用点的位移：endX = 最后一行末端的 x 偏移；
    // endY = -(行数-1)×字号×行距（多行下移）。命中缓存时用它们推进
    // curpos，保证缓存路径与直绘路径的光标语义完全一致。
    float endX = 0.0f ;
    float endY = 0.0f ;
  } ;
  std::unordered_map<StringKey, StringListEntry, StringKeyHash> stringLists ;

  // 已编译、尚未删除的全部显示列表号。重建/失效/析构时据此逐个 glDeleteLists，
  // 避免只删缓存表项而泄漏 GL 显示列表。
  std::unordered_set<GLuint> compiledLists ;

  // 按当前 atlasSize 创建（或重建）图集纹理：分配纹理对象、逐层清零 mip、
  // 设置过滤/包裹/mip 参数。二次开发后纹理改为惰性创建（首个字形才调用），
  // 扩容时也会整张重建。
  void createTexture ()
  {
    if ( texture != 0 )
    {
      glDeleteTextures ( 1, &texture ) ;
      texture = 0 ;
    }

    glGenTextures ( 1, &texture ) ;
    glBindTexture ( GL_TEXTURE_2D, texture ) ;
    glPixelStorei ( GL_UNPACK_ALIGNMENT, 1 ) ;
    // 覆盖率同时存入 GL_LUMINANCE_ALPHA 的亮度与 alpha 两个通道，与
    // fntTexFont 完全一致。PUI 用固定管线 GL_MODULATE 绘制文字：亮度通道
    // 调制颜色得到抗锯齿边缘（dst_rgb = 覆盖率 x 颜色）；alpha 通道则保证
    // 纹理与 fntTexFont 等价，无论 PUI 是否开启混合都渲染正确。
    //
    // 事先把每个 mip 层都初始化为透明黑：字形之间预留的 2px 间隔在过滤
    // 采样时不会把未初始化的垃圾数据混进字形边缘；每个字形在 getGlyph()
    // 里负责填充自己在每一层中的小区域。
    {
      std::vector<unsigned char> zeros ( (size_t) atlasSize * atlasSize * 2, 0 ) ;
      for ( int level = 0 ; level <= MIP_LEVELS ; ++level )
        glTexImage2D ( GL_TEXTURE_2D, level, GL_LUMINANCE_ALPHA,
                       atlasSize >> level, atlasSize >> level, 0,
                       GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, zeros.data() ) ;
    }
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR ) ;
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR ) ;
    // 【关键】只定义了 0..MIP_LEVELS 这几层 mip（见 getGlyph），必须把
    // GL_TEXTURE_MAX_LEVEL 显式设为 MIP_LEVELS：该参数的默认值是 1000，
    // OpenGL 会要求 1024x1024 纹理具备一直到 1x1 的完整 mip 金字塔才算
    // "mipmap 完整"。链不完整时纹理会被当作禁用、采样只输出顶点颜色，
    // 文字就渲染成了实心色块（本项目实际踩过这个坑）。
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, MIP_LEVELS ) ;
    // 再把采样 LOD 上限也钳到 MIP_LEVELS，双保险（MAX_LEVEL 本已限制采样）。
    glTexParameterf ( GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, (float) MIP_LEVELS ) ;
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP ) ;
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP ) ;
    glTexEnvi ( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE ) ;
  }

  // 惰性分配：只有真正要光栅化第一个字形时才创建纹理，这样
  // setAtlasSize() 在"首次光栅化前"调用都能生效。
  void ensureTexture ()
  {
    if ( texture == 0 )
      createTexture () ;
  }

  // 图集扩容：边长翻倍（上限 MAX_ATLAS_SIZE），清空字形表并按新尺寸
  // 重新光栅化所有已缓存字形。返回 false 表示已达上限、无法扩容。
  bool expandAtlas ()
  {
    if ( atlasSize >= MAX_ATLAS_SIZE )
      return false ;

    int newSize = atlasSize * 2 ;
    if ( newSize > MAX_ATLAS_SIZE )
      newSize = MAX_ATLAS_SIZE ;

    // 先把已缓存的码点收集起来（下面会清空 glyphs）。
    std::vector<uint32_t> cps ;
    cps.reserve ( glyphs.size() ) ;
    for ( const auto& kv : glyphs )
      cps.push_back ( kv.first ) ;

    glyphs.clear () ;
    pen_x = pen_y = row_h = 0 ;
    atlasSize = newSize ;
    createTexture () ;

    // 纹理对象已重建：所有字符串显示列表都引用了旧纹理/旧 UV，直接判为失效。
    ++glyphGeneration ;

    // 按新尺寸重新光栅化旧字形；此时禁止再次递归扩容（否则可能无限递归）。
    for ( uint32_t cp : cps )
      getGlyph ( cp, false ) ;

    return true ;
  }

  // 删除所有已编译的字符串显示列表。调用点必须保证 GL 上下文当前
  // （析构时与既有 glDeleteTextures 的约束相同）。
  void deleteStringLists ()
  {
    for ( GLuint list : compiledLists )
      glDeleteLists ( list, 1 ) ;
    compiledLists.clear () ;
    stringLists.clear () ;
  }

  // 在 (x, y, z) 处发射一个字形四边形。dx/dy 为归一化顶点偏移（描边用），
  // 绘制时会连同 pointsize 一起缩放，因此实际像素偏移 = dx * pointsize。
  void emitQuad ( const Glyph& g, float x, float y, float z,
                  float pointsize, float italic, float dx, float dy )
  {
    glBegin ( GL_TRIANGLE_STRIP ) ;
      glTexCoord2f ( g.t_left, g.t_bot ) ;
      glVertex3f   ( x + ( g.v_left  + dx ) * pointsize,
                     y + ( g.v_bot   + dy ) * pointsize, z ) ;
      glTexCoord2f ( g.t_left, g.t_top ) ;
      glVertex3f   ( x + ( italic + g.v_left  + dx ) * pointsize,
                     y + ( g.v_top   + dy ) * pointsize, z ) ;
      glTexCoord2f ( g.t_right, g.t_bot ) ;
      glVertex3f   ( x + ( g.v_right + dx ) * pointsize,
                     y + ( g.v_bot   + dy ) * pointsize, z ) ;
      glTexCoord2f ( g.t_right, g.t_top ) ;
      glVertex3f   ( x + ( italic + g.v_right + dx ) * pointsize,
                     y + ( g.v_top   + dy ) * pointsize, z ) ;
    glEnd () ;
  }

  // 绘制一个字形：开启描边时先以描边色在 4 个方向上偏移绘制（相当于
  // 用描边色把字形"加粗"一圈），再以调用方的正文颜色绘制主字形。
  // mainColor 必须由调用方在进入前用 glGetFloatv 保存，因为显示列表编译
  // 期间不允许调用 glGetFloatv。
  void emitGlyph ( const Glyph& g, float x, float y, float z,
                   float pointsize, float italic, const GLfloat mainColor[4] )
  {
    if ( outlineWidth > 0.0f && pointsize > 0.0f )
    {
      // 像素宽度换算成归一化偏移（顶点坐标稍后会乘 pointsize）。
      const float o = outlineWidth / pointsize ;
      static const float dirs[4][2] = { { -1.0f, 0.0f }, { 1.0f, 0.0f },
                                        { 0.0f, -1.0f }, { 0.0f, 1.0f } } ;
      glColor4f ( outlineR, outlineG, outlineB, outlineA ) ;
      for ( int i = 0 ; i < 4 ; ++i )
        emitQuad ( g, x, y, z, pointsize, italic, dirs[i][0] * o, dirs[i][1] * o ) ;
    }

    // 主字形：用 PUI 在调用 putch/puts 之前设置好的正文颜色。
    glColor4f ( mainColor[0], mainColor[1], mainColor[2], mainColor[3] ) ;
    emitQuad ( g, x, y, z, pointsize, italic, 0.0f, 0.0f ) ;
  }

  // 预先把字符串里所有码点光栅化进图集。必须在 glNewList 之外调用：
  // 首次光栅化可能触发图集扩容（glTexParameteri 等调用若发生在显示列表
  // 编译期间会被"录进"列表而不会立即生效，导致新纹理参数错误）。
  void preloadGlyphs ( const char *s )
  {
    const char *p = s ;
    while ( *p != '\0' )
    {
      if ( *p == '\n' ) { p++ ; continue ; }
      uint32_t cp = utf8Decode ( &p ) ;
      if ( cp == 0 ) continue ;
      getGlyph ( cp ) ;
    }
  }

  // 逐字形直接绘制一个字符串（不使用显示列表）。curpos 会被就地推进，
  // 语义与原来的 puts 完全一致。主绘制路径与缓存编译路径共用此函数，
  // 保证两条路径的换行/字距/描边行为绝对一致。
  void drawStringDirect ( sgVec3 curpos, float pointsize, float italic,
                          const char *s, const GLfloat mainColor[4], float gap )
  {
    const SGfloat origx = curpos[0] ;
    const char *p = s ;

    while ( *p != '\0' )
    {
      // 换行：光标回到行首并下移一行。行距 = 字号 × 行距系数
      //（默认 1.0，等价于原来的固定 pointsize）。
      if ( *p == '\n' )
      {
        curpos[0] = origx ;
        curpos[1] -= pointsize * lineSpacing ;
        p++ ;
        continue ;
      }

      // 逐码点解码 UTF-8（而不是逐字节），CJK 等多字节字符才能正确显示。
      uint32_t cp = utf8Decode ( &p ) ;
      if ( cp == 0 ) continue ;

      const Glyph *g = getGlyph ( cp ) ;
      if ( !g ) continue ;

      // 空白字形（如空格）：只前进光标。
      if ( g->v_right == g->v_left && g->v_top == g->v_bot )
      {
        curpos[0] += ( g->advance + gap ) * pointsize ;
        continue ;
      }

      emitGlyph ( *g, curpos[0], curpos[1], curpos[2], pointsize, italic, mainColor ) ;
      curpos[0] += ( g->advance + gap ) * pointsize ;
    }
  }

  // 字符串级显示列表缓存路径：命中且字形代数一致则直接回放，否则重新
  // 编译一个列表（几何在原点处编译，回放时用平移矩阵放到实际位置）。
  void drawCachedString ( sgVec3 curpos, float pointsize, float italic,
                          const char *s, const GLfloat mainColor[4], float gap )
  {
    StringKey key ;
    key.text    = s ;
    // pointsize/italic 量化到 1/100，足以区分不同字号/倾斜，又避免浮点噪声
    // 造成缓存永远不命中。
    key.pointQ  = (int) std::lround ( pointsize * 100.0f ) ;
    key.italicQ = (int) std::lround ( italic * 100.0f ) ;
    key.color   = packColor8 ( mainColor ) ;
    key.outline = ( outlineWidth > 0.0f ) ;

    auto it = stringLists.find ( key ) ;
    if ( it != stringLists.end() && it->second.list != 0 &&
         it->second.generation == glyphGeneration )
    {
      // 列表几何在原点处编译，回放时必须平移到调用位置（与下方编译
      // 分支的平移完全一致）；否则文字会画在 PUI 原点（窗口左下角）。
      glPushMatrix () ;
        glTranslatef ( curpos[0], curpos[1], curpos[2] ) ;
        glCallList ( it->second.list ) ;
      glPopMatrix () ;
      // 推进光标：缓存路径同样遵守 puts 的光标语义。
      curpos[0] += it->second.endX ;
      curpos[1] += it->second.endY ;
      return ;
    }

    // 缓存失效（首次编译，或期间新增过字形/图集扩容）：删除旧列表后重建。
    if ( it != stringLists.end() )
    {
      if ( it->second.list != 0 )
      {
        glDeleteLists ( it->second.list, 1 ) ;
        compiledLists.erase ( it->second.list ) ;
        it->second.list = 0 ;
      }
    }

    // 编译前先确保图集已存在，使列表里记录的 glBindTexture 指向正确纹理。
    ensureTexture () ;

    GLuint list = 0 ;
    bool stable = false ;

    // 编译期间可能光栅化新字形（代数 +1）甚至触发图集扩容（纹理对象重建），
    // 这会让"已编译完的部分"引用了旧纹理/旧 UV。此时丢弃重编；由于新字形
    // 已进入缓存，下一次编译通常立即稳定。
    // 编译结束后的光标位移（相对原点）：缓存命中分支回放后用它推进 curpos。
    // 声明在循环外，稳定编译后仍可读取。
    float endX = 0.0f ;
    float endY = 0.0f ;

    for ( int attempt = 0 ; attempt < 8 && ! stable ; ++attempt )
    {
      // 先把所有字形光栅化好（扩容/上传都在列表外完成），编译阶段只读缓存。
      preloadGlyphs ( s ) ;

      const unsigned genBefore = glyphGeneration ;
      list = glGenLists ( 1 ) ;
      glNewList ( list, GL_COMPILE ) ;
        // 记录纹理绑定（显示列表会保存并于回放时重新绑定）。
        glBindTexture ( GL_TEXTURE_2D, texture ) ;

        // 几何在原点处编译；回放时由外层 glTranslate 平移到 curpos。
        sgVec3 local ;
        sgSetVec3 ( local, 0.0f, 0.0f, 0.0f ) ;
        drawStringDirect ( local, pointsize, italic, s, mainColor, gap ) ;
      glEndList () ;

      if ( glyphGeneration == genBefore )
      {
        stable = true ;
        // drawStringDirect 就地推进光标，编译结束的 local 即末端位移。
        endX = local[0] ;
        endY = local[1] ;
      }
      else
      {
        glDeleteLists ( list, 1 ) ;
        list = 0 ;
      }
    }

    // 极端情况下（多次重编仍不稳定）退回直接绘制，保证画面正确。
    if ( ! stable || list == 0 )
    {
      drawStringDirect ( curpos, pointsize, italic, s, mainColor, gap ) ;
      return ;
    }

    StringListEntry& entry = stringLists[key] ;
    entry.list = list ;
    entry.generation = glyphGeneration ;
    // 编译结束时光标已推进到字符串末端（drawStringDirect 就地推进），
    // 把相对位移存进条目供缓存命中分支回放后推进 curpos。
    entry.endX = endX ;
    entry.endY = endY ;
    compiledLists.insert ( list ) ;

    glPushMatrix () ;
      glTranslatef ( curpos[0], curpos[1], curpos[2] ) ;
      glCallList ( list ) ;
    glPopMatrix () ;
  }

  // 光栅化并缓存一个字形。allowExpand 仅在最外层调用时为 true；
  // expandAtlas() 内部重新光栅化旧字形时传 false，避免无限递归扩容。
  const Glyph * getGlyph ( uint32_t cp, bool allowExpand = true )
  {
    // 命中缓存则直接返回。
    auto it = glyphs.find ( cp ) ;
    if ( it != glyphs.end() )
      return &it->second ;

    // 惰性创建图集纹理（首次光栅化）。
    ensureTexture () ;

    // 不支持的码点返回一个占位字形（零尺寸方块，仅占据步进宽度）。
    Glyph g ;
    g.t_left = g.t_bot = g.t_right = g.t_top = 0.0f ;
    g.v_left = g.v_bot = g.v_right = g.v_top = 0.0f ;
    g.advance = 0.5f ;

    // 用 FreeType 把该码点光栅化成 8 位灰度位图（FT_LOAD_RENDER）。
    if ( FT_Load_Char ( face, cp, FT_LOAD_RENDER ) != 0 )
    {
      glyphs[cp] = g ;
      ++glyphGeneration ;
      return &glyphs[cp] ;
    }

    FT_Bitmap& bmp = face->glyph->bitmap ;
    const int w = bmp.width ;
    const int h = bmp.rows ;

    // 空格等空白字形只有步进宽度有意义。
    if ( w == 0 || h == 0 )
    {
      g.advance = face->glyph->advance.x / 64.0f / scale ;
      glyphs[cp] = g ;
      ++glyphGeneration ;
      return &glyphs[cp] ;
    }

    // 当前行放不下时换到下一行；行间也留 2px 间隔。
    if ( pen_x + w + 2 > atlasSize ) { pen_x = 0 ; pen_y += row_h + 2 ; row_h = 0 ; }
    // 图集已满：默认情况下（上限 2048 以内）直接扩容重建并重试；
    // 已达上限时输出一次告警并返回占位字形（保留原有的"占位"语义）。
    if ( pen_y + h + 2 > atlasSize )
    {
      if ( allowExpand && expandAtlas() )
      {
        // 扩容会重建纹理并重光栅化旧字形，当前 FT_Bitmap 已失效，
        // 因此重新走一遍本函数（此时禁止再次扩容）。
        return getGlyph ( cp, false ) ;
      }

      if ( ! atlasFullWarned )
      {
        fntSetError ( SG_WARN,
          "fntUnicodeFont: glyph atlas is full (%dx%d), some characters "
          "will not be rendered", atlasSize, atlasSize ) ;
        atlasFullWarned = true ;
      }

      g.advance = face->glyph->advance.x / 64.0f / scale ;
      glyphs[cp] = g ;
      ++glyphGeneration ;
      return &glyphs[cp] ;
    }

    // FreeType 位图的每行可能带有填充字节（pitch 可能大于 w），
    // 先紧凑复制到连续缓冲区；每个纹素两个字节（亮度 == alpha == 覆盖率），
    // 与 fntTexFont 的存储方式一致。
    std::vector<unsigned char> buf ( (size_t) w * h * 2 ) ;
    for ( int y = 0 ; y < h ; ++y )
    {
      const unsigned char *src = bmp.buffer + (size_t) y * bmp.pitch ;
      unsigned char       *dst = &buf[ (size_t) y * w * 2 ] ;
      for ( int x = 0 ; x < w ; ++x )
      {
        dst[ x * 2     ] = src[ x ] ;
        dst[ x * 2 + 1 ] = src[ x ] ;
      }
    }

    glBindTexture ( GL_TEXTURE_2D, texture ) ;
    glPixelStorei ( GL_UNPACK_ALIGNMENT, 1 ) ;
    glTexSubImage2D ( GL_TEXTURE_2D, 0, pen_x, pen_y, w, h,
                      GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, buf.data() ) ;

    // 继续把该字形在 mip 1..MIP_LEVELS 层中的对应小区域也上传，每层都用
    // 2x2 盒式过滤在 CPU 上预过滤。（GL 1.x 路径没有 glGenerateMipmap，
    // 不能交给驱动生成。）
    //
    // 为什么必须做 mip：图集按 48px 光栅化、GUI 却以 ~15px 显示（约 3.2 倍
    // 缩小）。没有 mip 时，GL_LINEAR 每次都只采 2x2 个 0 级纹素，汉字细横画
    // （约 2~3 纹素宽）会被平均到 ~50% 覆盖率，笔画发灰发糊、忽明忽暗；
    // 有了预过滤的 mip 层，采样的是笔画的平均覆盖率（~80%+），笔画明亮且
    // 粗细均匀。
    {
      const unsigned char *src_data = buf.data() ;
      int src_w = w, src_h = h ;
      int src_x = pen_x, src_y = pen_y ;

      for ( int level = 1 ; level <= MIP_LEVELS ; ++level )
      {
        const int dw = ( src_w + 1 ) / 2 ;
        const int dh = ( src_h + 1 ) / 2 ;
        if ( dw <= 0 || dh <= 0 )
          break ;

        // 第 L 层的纹素坐标 = 第 0 层坐标除以 2^L（即对上一层坐标整除 2）。
        const int dx = src_x / 2 ;
        const int dy = src_y / 2 ;

        // 对该字形区域做 2x2 盒式过滤（边缘不足 2x2 时按实际纹素数平均）。
        std::vector<unsigned char> down ( (size_t) dw * dh * 2 ) ;
        for ( int yy = 0 ; yy < dh ; ++yy )
        {
          for ( int xx = 0 ; xx < dw ; ++xx )
          {
            int acc = 0, n = 0 ;
            for ( int sy = 0 ; sy < 2 ; ++sy )
            {
              for ( int sx = 0 ; sx < 2 ; ++sx )
              {
                const int ix = xx * 2 + sx ;
                const int iy = yy * 2 + sy ;
                if ( ix < src_w && iy < src_h )
                {
                  acc += src_data[ ( iy * src_w + ix ) * 2 ] ;
                  n++ ;
                }
              }
            }
            const unsigned char v = (unsigned char) ( acc / n ) ;
            down[ ( yy * dw + xx ) * 2     ] = v ;
            down[ ( yy * dw + xx ) * 2 + 1 ] = v ;
          }
        }

        glTexSubImage2D ( GL_TEXTURE_2D, level, dx, dy, dw, dh,
                          GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, down.data() ) ;

        // 下一层的过滤基于本层结果继续。
        src_data = down.data() ;
        src_w = dw ; src_h = dh ;
        src_x = dx ; src_y = dy ;
      }
    }

    // 图集边长可能在扩容后变化，UV 归一化必须用当前尺寸。
    const float sx = 1.0f / atlasSize ;
    // bitmap_left / bitmap_top 是位图左上角相对基线的偏移（像素），
    // 除以 scale 后得到归一化顶点坐标。
    const float bx = face->glyph->bitmap_left / scale ;
    const float by = face->glyph->bitmap_top  / scale ;

    g.t_left  = pen_x * sx ;
    g.t_right = ( pen_x + w ) * sx ;
    g.t_bot   = ( pen_y + h ) * sx ;   // 字形在图集中的底边行
    g.t_top   = pen_y * sx ;           // 字形在图集中的顶边行

    g.v_left  = bx ;
    g.v_right = bx + (float) w / scale ;
    g.v_bot   = by - (float) h / scale ;
    g.v_top   = by ;

    // FreeType 的 advance.x 以 26.6 定点数表示（1/64 像素）。
    g.advance = face->glyph->advance.x / 64.0f / scale ;

    pen_x += w + 2 ;
    if ( h > row_h ) row_h = h ;

    glyphs[cp] = g ;
    ++glyphGeneration ;
    return &glyphs[cp] ;
  }

  bool open ( const SGPath& path )
  {
    if ( FT_Init_FreeType ( &library ) != 0 )
      return false ;

    if ( FT_New_Face ( library, path.utf8Str().c_str(), 0, &face ) != 0 )
      return false ;

    // 把 em 尺寸设为 RASTER_SIZE 像素，之后按此尺寸光栅化所有字形。
    FT_Set_Pixel_Sizes ( face, 0, (FT_UInt) RASTER_SIZE ) ;

    // ascender 以 26.6 定点数表示（1/64 像素），换算成像素。
    // 若字体报告了非正的 ascender（正常情况下不会发生），回退到 em 尺寸。
    scale = face->size->metrics.ascender / 64.0f ;
    if ( scale <= 0.0f )
      scale = RASTER_SIZE ;

    // 二次开发：图集纹理改为惰性分配，这里不再创建纹理。
    // 这样 setAtlasSize() 在"首次光栅化之前"调用都能生效，
    // 也能避免只为探测字体而加载时白白占用显存。
    return true ;
  }

  ~Impl ()
  {
    // 删除所有已编译的字符串显示列表（若此时 GL 上下文已销毁，该调用与
    // 下面 glDeleteTextures 一样受既有限制——由驱动忽略或未定义）。
    deleteStringLists () ;
    if ( texture != 0 ) glDeleteTextures ( 1, &texture ) ;
    if ( face ) FT_Done_Face ( face ) ;
    if ( library ) FT_Done_FreeType ( library ) ;
  }
} ;


fntUnicodeFont::fntUnicodeFont ()
  : impl ( new Impl ), width ( 1.0f ), gap ( 0.1f ),
    fixed_pitch ( FNT_FALSE ), bound ( FNT_FALSE )
{
}

fntUnicodeFont::~fntUnicodeFont ()
{
  delete impl ;
}

int fntUnicodeFont::load ( const SGPath& path, GLenum mag, GLenum min )
{
  if ( ! impl->open ( path ) )
  {
    fntSetError ( SG_WARN,
      "fntUnicodeFont::load: Failed to open font '%s'",
      path.utf8Str().c_str() ) ;
    return FNT_FALSE ;
  }
  return FNT_TRUE ;
}

// ---- 主题排版参数设置（二次开发新增）----

// 行距系数：换行垂直步进 = pointsize × lineSpacing；非法值回退 1.0。
// 行距会连同 pointsize 一起被编译进字符串显示列表的顶点坐标，因此改动后
// 必须让已编译列表失效（字形代数 +1，按需重建）。
void fntUnicodeFont::setLineSpacing ( float v )
{
  impl->lineSpacing = ( v > 0.0f ) ? v : 1.0f ;
  ++impl->glyphGeneration ;
}

float fntUnicodeFont::getLineSpacing () const
{
  return impl->lineSpacing ;
}

// 描边：宽度 <= 0 关闭。参数改变后已编译的显示列表在视觉上会过时，
// 因此把字形代数 +1 让其全部失效、按需重建（不在这里调用 GL 删除，
// 以兼容"主题配置阶段可能没有当前 GL 上下文"的调用场景）。
void fntUnicodeFont::setOutline ( float widthPx, float r, float g, float b, float a )
{
  impl->outlineWidth = ( widthPx > 0.0f ) ? widthPx : 0.0f ;
  impl->outlineR = r ;
  impl->outlineG = g ;
  impl->outlineB = b ;
  impl->outlineA = a ;
  ++impl->glyphGeneration ;
}

// 字符串级显示列表缓存开关。关闭时 puts 退回逐字形直接绘制；
// 同时让已有列表失效，下次绘制时按需重建/清理。
void fntUnicodeFont::setStringCache ( bool on )
{
  impl->stringCache = on ;
  ++impl->glyphGeneration ;
}

// 图集边长：只允许在首次光栅化（纹理尚未创建）之前设置；
// 已经光栅化过字形则忽略并告警，避免中途重建导致已缓存的字形 UV 失效。
void fntUnicodeFont::setAtlasSize ( int px )
{
  if ( px <= 0 )
    return ;

  // 向上取到 2 的幂（mip 层尺寸与 UV 归一化都要求 2 的幂），并钳到上限。
  int size = 64 ;
  while ( size < px && size < MAX_ATLAS_SIZE )
    size <<= 1 ;
  if ( size > MAX_ATLAS_SIZE )
    size = MAX_ATLAS_SIZE ;

  if ( impl->texture != 0 )
  {
    fntSetError ( SG_WARN,
      "fntUnicodeFont::setAtlasSize: atlas already in use, ignored (%d px requested)",
      px ) ;
    return ;
  }

  impl->atlasSize = size ;
}

// 计算字符串的包围盒（用于布局测宽/测高）。
// 返回值为归一化坐标乘以 pointsize 后的结果。
void fntUnicodeFont::getBBox ( const char *s, float pointsize, float italic,
                               float *left, float *right,
                               float *bot , float *top  )
{
  float l = 0.0f, r = 0.0f, b = 0.0f, t = 0.0f ;
  float h = 0.0f, v = 0.0f ;   // h: 水平游标；v: 垂直游标（每换行减 lineSpacing）

  if ( s )
  {
    const char *p = s ;
    while ( *p != '\0' )
    {
      // 换行：水平归零、垂直下移一个行距（归一化单位 × lineSpacing，
      // 与 puts 的步进 pointsize × lineSpacing 保持同一口径）。
      if ( *p == '\n' ) { h = 0.0f ; v -= impl->lineSpacing ; p++ ; continue ; }

      uint32_t cp = utf8Decode ( &p ) ;
      if ( cp == 0 ) continue ;
      const Impl::Glyph *g = impl->getGlyph ( cp ) ;
      if ( !g ) continue ;

      if ( l > h + g->v_left ) l = h + g->v_left ;
      if ( r < h + g->v_right ) r = h + g->v_right ;
      if ( b > v + g->v_bot ) b = v + g->v_bot ;
      if ( t < v + g->v_top ) t = v + g->v_top ;

      h += g->advance + gap ;
    }
  }

  if ( left  ) *left  = l * pointsize ;
  if ( right ) *right = r * pointsize ;
  if ( bot   ) *bot   = b * pointsize ;
  if ( top   ) *top   = t * pointsize ;
}

// 绘制单个字节（ASCII 路径）。多字节 UTF-8 必须走 puts()。
void fntUnicodeFont::putch ( sgVec3 curpos, float pointsize, float italic, char c )
{
  if ( ! bound )
    begin () ;

  uint32_t cp = (unsigned char) c ;
  const Impl::Glyph *g = impl->getGlyph ( cp ) ;
  // 空白字形（如空格）：只前进光标，不画四边形。
  if ( !g || ( g->v_right == g->v_left && g->v_top == g->v_bot ) )
  {
    if ( cp == ' ' ) curpos[0] += pointsize * 0.5f ;
    return ;
  }

  // 保存 PUI 设置的正文颜色（描边绘制会改 glColor，结束时恢复）。
  GLfloat savedColor[4] ;
  glGetFloatv ( GL_CURRENT_COLOR, savedColor ) ;

  // 每个字形画一个带纹理的三角形带（两个三角形拼成矩形）；
  // 开启描边时 emitGlyph 会先以描边色画 4 个偏移副本。
  impl->emitGlyph ( *g, curpos[0], curpos[1], curpos[2],
                    pointsize, italic, savedColor ) ;

  // 恢复正文颜色，保证后续绘制不受描边颜色影响。
  glColor4fv ( savedColor ) ;

  curpos[0] += ( g->advance + gap ) * pointsize ;
}

// 绘制 UTF-8 字符串（PUI 菜单栏/对话框文字的主路径）。
void fntUnicodeFont::puts ( sgVec3 curpos, float pointsize, float italic, const char *s )
{
  if ( s == nullptr )
    return ;

  if ( ! bound )
    begin () ;

  // 保存 PUI 设置的正文颜色：描边路径需要临时改成描边色，
  // 显示列表路径需要把颜色编译进列表，两条路径结束后都要恢复。
  GLfloat savedColor[4] ;
  glGetFloatv ( GL_CURRENT_COLOR, savedColor ) ;

  // 字符串级显示列表缓存开启时走缓存路径（未开启则完全走原来的逐字形路径）。
  if ( impl->stringCache )
  {
    impl->drawCachedString ( curpos, pointsize, italic, s, savedColor, gap ) ;
  }
  else
  {
    // 逐字形直接绘制（换行步进 = 字号 × 行距系数）。
    impl->drawStringDirect ( curpos, pointsize, italic, s, savedColor, gap ) ;
  }

  // 恢复正文颜色（两条路径都会改写 glColor，必须恢复调用方状态）。
  glColor4fv ( savedColor ) ;
}

// 按最大宽度自动换行（二次开发新增，供 PUI 主题文字控件使用）。
// 规则：
//   1) 断点优先选空格：超宽时把最近一个空格替换成 '\n'；
//   2) CJK 区段允许在任意字符之间硬断行；
//   3) 原有 '\n' 原样保留；
//   4) maxWidth <= 0 或字符串为空时原样返回。
std::string fntUnicodeFont::wrapString ( const char *s, float pointsize, float maxWidth )
{
  if ( s == nullptr )
    return std::string () ;
  if ( maxWidth <= 0.0f || pointsize <= 0.0f )
    return std::string ( s ) ;

  std::string out ;
  out.reserve ( std::strlen ( s ) + 16 ) ;

  // 当前行的逐字符记录：在 out 中的起始字节偏移、宽度、以及最近空格的位置。
  std::vector<size_t> starts ;
  std::vector<float> widths ;
  size_t lastSpace = (size_t) -1 ;
  float lineWidth = 0.0f ;

  // 换行后清空当前行的统计状态。
  const auto resetLine = [&]()
  {
    starts.clear () ;
    widths.clear () ;
    lastSpace = (size_t) -1 ;
    lineWidth = 0.0f ;
  } ;

  const char *p = s ;
  while ( *p != '\0' )
  {
    // 原有换行原样保留。
    if ( *p == '\n' )
    {
      out += '\n' ;
      resetLine () ;
      p++ ;
      continue ;
    }

    uint32_t cp = utf8Decode ( &p ) ;
    if ( cp == 0 ) continue ;   // 非法字节：与 puts 的处理保持一致（跳过）

    // 行首空格直接丢弃：它不承载任何可见宽度，若保留会在下一次断行时
    // 被改写成 '\n'，在行首凭空产生一个空行。
    if ( cp == ' ' && starts.empty() )
      continue ;

    // 取该字符的步进宽度（与绘制完全同口径：(advance + gap) × pointsize）。
    float w = pointsize * 0.5f ;
    const Impl::Glyph *g = impl->getGlyph ( cp ) ;
    if ( g ) w = ( g->advance + gap ) * pointsize ;

    // 行内已有内容且当前字符会超宽 → 需要断行。
    if ( lineWidth + w > maxWidth && lineWidth > 0.0f )
    {
      if ( lastSpace != (size_t) -1 )
      {
        // 优先在最近的空格处断行：把该空格字节替换成 '\n'，
        // 然后重算空格之后内容的行宽（这部分内容属于新行）。
        out[ starts[lastSpace] ] = '\n' ;

        float newWidth = 0.0f ;
        for ( size_t i = lastSpace + 1 ; i < starts.size() ; ++i )
          newWidth += widths[i] ;

        // 丢弃空格及其之前的记录（它们已在新行之前）。
        starts.erase ( starts.begin(), starts.begin() + lastSpace + 1 ) ;
        widths.erase ( widths.begin(), widths.begin() + lastSpace + 1 ) ;
        lineWidth = newWidth ;
        lastSpace = (size_t) -1 ;

        // 若断行后仍放不下当前字符且新行已有内容 → 再硬断一次。
        if ( lineWidth + w > maxWidth && ! starts.empty() )
        {
          out += '\n' ;
          resetLine () ;
        }
      }
      else if ( ! starts.empty() )
      {
        // 没有空格可断（典型是 CJK 长串）：直接在当前字符前硬断。
        out += '\n' ;
        resetLine () ;
      }
    }

    // 记录该字符在输出中的位置并写入 UTF-8 字节。
    const size_t off = out.size () ;
    utf8Append ( out, cp ) ;
    starts.push_back ( off ) ;
    widths.push_back ( w ) ;
    lineWidth += w ;

    if ( cp == ' ' )
      lastSpace = starts.size() - 1 ;
  }

  return out ;
}

// 开始一批文字绘制：启用纹理并绑定图集。
void fntUnicodeFont::begin ()
{
  glEnable ( GL_TEXTURE_2D ) ;
  glBindTexture ( GL_TEXTURE_2D, impl->texture ) ;
  bound = FNT_TRUE ;
}

void fntUnicodeFont::end ()
{
  bound = FNT_FALSE ;
}
