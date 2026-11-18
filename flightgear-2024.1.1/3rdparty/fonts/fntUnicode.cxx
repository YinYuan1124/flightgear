// fntUnicode.cxx -- 基于 FreeType 的 Unicode 字体，供 FlightGear 的 PUI 界面使用。
//
// 本文件是 fntFont 接口的一个即插即用实现：用 FreeType 动态渲染 UTF-8 字符串，
// 从而让 PUI 菜单栏 / 对话框可以显示中文等 CJK 多字节文字。
// （fntTexFont 只能渲染 .txf 纹理字体里的 256 个单字节字符，无法显示 CJK。）
//
// 实现思路：按需把字形光栅化进一张"覆盖率图集"纹理（GL_LUMINANCE_ALPHA，
// 亮度与 alpha 均为笔画覆盖率），绘制时每个字形画一个带纹理的四边形。

#include "fntLocal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <simgear/misc/sg_path.hxx>
#include <simgear/debug/logstream.hxx>

#include <cstring>
#include <vector>
#include <unordered_map>

namespace
{
  // 图集纹理的边长（像素）。1024x1024 在 48px 光栅化字号下约可容纳 400 个
  // 汉字，足以覆盖菜单栏/对话框的常用字符集。
  const int ATLAS_SIZE = 1024;

  // 字形光栅化时的 em 尺寸（像素）。固定用 48px 光栅化、渲染时再缩小，
  // 以获得足够的超采样精度（GUI 实际显示字号约 6~21px）。
  const float RASTER_SIZE = 48.0f;

  // 在 0 级之外额外构建并逐字形上传的 mip 层数。
  // 只做 2 级（4 倍缩小）是因为图集里字形之间只有 2px 间隔，更深的 mip 层
  // 会因间隔被平均掉而导致相邻字形互相串色。由于只定义了 0..2 级，必须
  // 同时把 GL_TEXTURE_MAX_LEVEL 设为 2（见 open()），否则纹理 mipmap 不
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
  GLuint texture = 0 ;             // 字形图集纹理对象
  int pen_x = 0, pen_y = 0, row_h = 0 ;  // 图集打包游标：当前 x/y 位置与当前行高

  // 归一化系数：顶点空间中 1.0 代表字体的 ascender（基线到最高字形顶部的
  // 距离，单位像素）。之所以按 ascender 而不是按 em 高度归一化，是为了与
  // fntTexFont 保持一致 —— fntTexFont 把每个度量都除以 max_height
  // （即 ascender），这样同一个 pointsize 在两种字体后端下渲染出的视觉
  // 高度完全相同（em 高度约比大写字母高度大 30%，若按 em 归一化字会偏小）。
  float scale = 1.0f ;

  // 已光栅化字形的缓存：码点 -> 字形度量。
  std::unordered_map<uint32_t, Glyph> glyphs ;

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
      std::vector<unsigned char> zeros ( (size_t) ATLAS_SIZE * ATLAS_SIZE * 2, 0 ) ;
      for ( int level = 0 ; level <= MIP_LEVELS ; ++level )
        glTexImage2D ( GL_TEXTURE_2D, level, GL_LUMINANCE_ALPHA,
                       ATLAS_SIZE >> level, ATLAS_SIZE >> level, 0,
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

    return true ;
  }

  ~Impl ()
  {
    if ( texture != 0 ) glDeleteTextures ( 1, &texture ) ;
    if ( face ) FT_Done_Face ( face ) ;
    if ( library ) FT_Done_FreeType ( library ) ;
  }

  const Glyph * getGlyph ( uint32_t cp )
  {
    // 命中缓存则直接返回。
    auto it = glyphs.find ( cp ) ;
    if ( it != glyphs.end() )
      return &it->second ;

    // 不支持的码点返回一个占位字形（零尺寸方块，仅占据步进宽度）。
    Glyph g ;
    g.t_left = g.t_bot = g.t_right = g.t_top = 0.0f ;
    g.v_left = g.v_bot = g.v_right = g.v_top = 0.0f ;
    g.advance = 0.5f ;

    // 用 FreeType 把该码点光栅化成 8 位灰度位图（FT_LOAD_RENDER）。
    if ( FT_Load_Char ( face, cp, FT_LOAD_RENDER ) != 0 )
    {
      glyphs[cp] = g ;
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
      return &glyphs[cp] ;
    }

    // 当前行放不下时换到下一行；行间也留 2px 间隔。
    if ( pen_x + w + 2 > ATLAS_SIZE ) { pen_x = 0 ; pen_y += row_h + 2 ; row_h = 0 ; }
    // 图集已满（正常使用几乎不会发生）：返回空白字形。
    if ( pen_y + h + 2 > ATLAS_SIZE )
    {
      g.advance = face->glyph->advance.x / 64.0f / scale ;
      glyphs[cp] = g ;
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

    const float sx = 1.0f / ATLAS_SIZE ;
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
    return &glyphs[cp] ;
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

// 计算字符串的包围盒（用于布局测宽/测高）。
// 返回值为归一化坐标乘以 pointsize 后的结果。
void fntUnicodeFont::getBBox ( const char *s, float pointsize, float italic,
                               float *left, float *right,
                               float *bot , float *top  )
{
  float l = 0.0f, r = 0.0f, b = 0.0f, t = 0.0f ;
  float h = 0.0f, v = 0.0f ;   // h: 水平游标；v: 垂直游标（每换行减 1.0）

  if ( s )
  {
    const char *p = s ;
    while ( *p != '\0' )
    {
      // 换行：水平归零、垂直下移一行（与 puts 的换行步进一致）。
      if ( *p == '\n' ) { h = 0.0f ; v -= 1.0f ; p++ ; continue ; }

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

  // 每个字形画一个带纹理的三角形带（两个三角形拼成矩形）。
  glBegin ( GL_TRIANGLE_STRIP ) ;
    glTexCoord2f ( g->t_left, g->t_bot ) ;
    glVertex3f   ( curpos[0] + g->v_left  * pointsize,
                   curpos[1] + g->v_bot   * pointsize, curpos[2] ) ;
    glTexCoord2f ( g->t_left, g->t_top ) ;
    glVertex3f   ( curpos[0] + ( italic + g->v_left ) * pointsize,
                   curpos[1] + g->v_top   * pointsize, curpos[2] ) ;
    glTexCoord2f ( g->t_right, g->t_bot ) ;
    glVertex3f   ( curpos[0] + g->v_right * pointsize,
                   curpos[1] + g->v_bot   * pointsize, curpos[2] ) ;
    glTexCoord2f ( g->t_right, g->t_top ) ;
    glVertex3f   ( curpos[0] + ( italic + g->v_right ) * pointsize,
                   curpos[1] + g->v_top   * pointsize, curpos[2] ) ;
  glEnd () ;

  curpos[0] += ( g->advance + gap ) * pointsize ;
}

// 绘制 UTF-8 字符串（PUI 菜单栏/对话框文字的主路径）。
void fntUnicodeFont::puts ( sgVec3 curpos, float pointsize, float italic, const char *s )
{
  if ( s == nullptr )
    return ;

  if ( ! bound )
    begin () ;

  const SGfloat origx = curpos[0] ;
  const char *p = s ;

  while ( *p != '\0' )
  {
    // 换行：光标回到行首并下移一行。
    if ( *p == '\n' )
    {
      curpos[0] = origx ;
      curpos[1] -= pointsize ;
      p++ ;
      continue ;
    }

    // 逐码点解码 UTF-8（而不是逐字节），CJK 等多字节字符才能正确显示。
    uint32_t cp = utf8Decode ( &p ) ;
    if ( cp == 0 ) continue ;

    const Impl::Glyph *g = impl->getGlyph ( cp ) ;
    if ( !g ) continue ;

    // 空白字形（如空格）：只前进光标。
    if ( g->v_right == g->v_left && g->v_top == g->v_bot )
    {
      curpos[0] += ( g->advance + gap ) * pointsize ;
      continue ;
    }

    // 每个字形画一个带纹理的三角形带。
    glBegin ( GL_TRIANGLE_STRIP ) ;
      glTexCoord2f ( g->t_left, g->t_bot ) ;
      glVertex3f   ( curpos[0] + g->v_left  * pointsize,
                     curpos[1] + g->v_bot   * pointsize, curpos[2] ) ;
      glTexCoord2f ( g->t_left, g->t_top ) ;
      glVertex3f   ( curpos[0] + ( italic + g->v_left ) * pointsize,
                     curpos[1] + g->v_top   * pointsize, curpos[2] ) ;
      glTexCoord2f ( g->t_right, g->t_bot ) ;
      glVertex3f   ( curpos[0] + g->v_right * pointsize,
                     curpos[1] + g->v_bot   * pointsize, curpos[2] ) ;
      glTexCoord2f ( g->t_right, g->t_top ) ;
      glVertex3f   ( curpos[0] + ( italic + g->v_right ) * pointsize,
                     curpos[1] + g->v_top   * pointsize, curpos[2] ) ;
    glEnd () ;

    curpos[0] += ( g->advance + gap ) * pointsize ;
  }
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
