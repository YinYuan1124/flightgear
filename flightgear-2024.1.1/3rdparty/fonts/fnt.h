/*
     PLIB - A Suite of Portable Game Libraries
     Copyright (C) 1998,2002  Steve Baker
 
     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Library General Public
     License as published by the Free Software Foundation; either
     version 2 of the License, or (at your option) any later version.
 
     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     Library General Public License for more details.
 
     You should have received a copy of the GNU Library General Public
     License along with this library; if not, write to the Free Software
     Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 
     For further information visit http://plib.sourceforge.net

     $Id: fnt.h 1923 2004-04-06 12:53:17Z sjbaker $
*/


#ifndef _FNT_H_
#define _FNT_H_  1

#include <stdio.h>
#include <string>   // 新增 wrapString 接口需要 std::string（PUI 主题自动换行）

#include <plib/sg.h>
#include <osg/GL>

#define FNTMAX_CHAR  256
#define FNT_TRUE  1
#define FNT_FALSE 0

class SGPath;

class fntFont
{
public:
  fntFont () ;

  virtual ~fntFont () ;

  virtual void getBBox ( const char *s, float pointsize, float italic,
                                  float *left, float *right,
                                  float *bot , float *top  ) = 0 ;
  virtual void putch ( sgVec3 curpos, float pointsize, float italic, char  c ) = 0 ;
  virtual void puts  ( sgVec3 curpos, float pointsize, float italic, const char *s ) = 0 ;
  virtual void begin () = 0 ;
  virtual void end   () = 0 ;

  virtual int load ( const SGPath& fname, GLenum mag = GL_NEAREST,
                                  GLenum min = GL_LINEAR_MIPMAP_LINEAR ) = 0 ;

  virtual void setFixedPitch ( int fix ) = 0 ;
  virtual int   isFixedPitch () const    = 0 ;

  virtual void  setWidth     ( float w ) = 0 ;
  virtual void  setGap       ( float g ) = 0 ;

  virtual float getWidth     () const    = 0 ;
  virtual float getGap       () const    = 0 ;

  virtual int hasGlyph ( char c ) const = 0 ;

  /*
   * 【ABI 铁律】以下六个主题相关虚函数是 FlightGear 二次开发新增的，
   * 必须且只能追加在 hasGlyph 之后，绝对不能插入/重排上面任何既有虚函数：
   * 预编译的 plib_pui.lib（puFont 在库内调用 getBBox/putch/puts/begin/end 等）
   * 是按旧虚表槽位编译的，重排会导致调用错位直接崩溃。
   *
   * 这些函数一律提供非纯虚默认实现（空体/返回默认值），保证：
   *   1) 旧后端（以及 plib 库内可能存在的其它派生类）无需改动即可继续工作；
   *   2) 默认配置下的行为与加入这些接口之前完全一致。
   */

  // 行距系数：换行时垂直步进 = pointsize × 该值；1.0 = 现状（行高等于字号）。
  virtual void  setLineSpacing ( float v ) { }                       // 行距系数；1.0=现状
  virtual float getLineSpacing () const { return 1.0f ; }

  // 文字描边：widthPx 为像素宽度，<=0 表示关闭；颜色为直通 alpha 的 RGBA。
  virtual void  setOutline ( float widthPx, float r, float g, float b, float a ) { }  // 0=关

  // 字符串级显示列表缓存开关（同一字符串重复绘制时复用已编译的 GL 显示列表）。
  virtual void  setStringCache ( bool on ) { }

  // 字形图集边长（像素，2 的幂）；首次光栅化前调用才生效。
  virtual void  setAtlasSize ( int px ) { }

  // 按最大宽度对字符串做自动换行，返回插入 '\n' 后的新字符串。
  // 默认实现原样返回（不做任何换行），由具体后端按需覆写。
  virtual std::string wrapString ( const char * s, float pointsize, float maxWidth ) ;
} ;


class fntTexFont : public fntFont
{
private:
  GLuint texture  ;
  int bound       ;
  int fixed_pitch ;

  float width     ; /* The width of a character in fixed-width mode */
  float gap       ; /* Set the gap between characters */

  // 行距系数（二次开发新增）：换行垂直步进 = pointsize × line_spacing。
  // 用类内初始化保证两个构造函数都自动获得默认值 1.0（即原有的行高行为）。
  float line_spacing = 1.0f ;

  int   exists  [ FNTMAX_CHAR ] ; /* TRUE if character exists in tex-map */

  /*
    The quadrilaterals that describe the characters
    in the font are drawn with the following texture
    and spatial coordinates. The texture coordinates
    are in (S,T) space, with (0,0) at the bottom left
    of the image and (1,1) at the top-right.

    The spatial coordinates are relative to the current
    'cursor' position. They should be scaled such that
    1.0 represent the height of a capital letter. Hence,
    characters like 'y' which have a descender will be
    given a negative v_bot. Most capitals will have
    v_bot==0.0 and v_top==1.0.
  */

  /* Nominal baseline widths */

  float widths  [ FNTMAX_CHAR ] ;

  /* Texture coordinates */

  float t_top   [ FNTMAX_CHAR ] ; /* Top    edge of each character [0..1] */
  float t_bot   [ FNTMAX_CHAR ] ; /* Bottom edge of each character [0..1] */
  float t_left  [ FNTMAX_CHAR ] ; /* Left   edge of each character [0..1] */
  float t_right [ FNTMAX_CHAR ] ; /* Right  edge of each character [0..1] */

  /* Vertex coordinates. */

  float v_top   [ FNTMAX_CHAR ] ;
  float v_bot   [ FNTMAX_CHAR ] ;
  float v_left  [ FNTMAX_CHAR ] ;
  float v_right [ FNTMAX_CHAR ] ;

  void bind_texture ()
  {
    glEnable      ( GL_TEXTURE_2D ) ;
#ifdef GL_VERSION_1_1
    glBindTexture ( GL_TEXTURE_2D, texture ) ;
#else
    /* For ancient SGI machines */
    glBindTextureEXT ( GL_TEXTURE_2D, texture ) ;
#endif
  }

  float low_putch ( sgVec3 curpos, float pointsize, float italic, char c ) ;

  int loadTXF ( const SGPath& path, GLenum mag = GL_NEAREST,
                             GLenum min = GL_LINEAR_MIPMAP_LINEAR ) ;
public:

  fntTexFont ()
  {
    bound       = FNT_FALSE ;
    fixed_pitch = FNT_TRUE  ;
    texture     =   0   ;
    width       =  1.0f ;
    gap         =  0.1f ;

    memset ( exists, FNT_FALSE, FNTMAX_CHAR * sizeof(int) ) ;
  }

  fntTexFont ( const SGPath& path, GLenum mag = GL_NEAREST, 
                            GLenum min = GL_LINEAR_MIPMAP_LINEAR ) : fntFont ()
  {
    bound       = FNT_FALSE ;
    fixed_pitch = FNT_TRUE  ;
    texture     =   0   ;
    width       =  1.0f ;
    gap         =  0.1f ;

    memset ( exists, FNT_FALSE, FNTMAX_CHAR * sizeof(int) ) ;
    load ( path, mag, min ) ;
  }

  ~fntTexFont ()
  {
    if ( texture != 0 )
    {
#ifdef GL_VERSION_1_1
      glDeleteTextures ( 1, &texture ) ;
#else
      /* For ancient SGI machines */
      glDeleteTexturesEXT ( 1, &texture ) ;
#endif
    }
  }

  int load ( const SGPath& path, GLenum mag = GL_NEAREST, 
                          GLenum min = GL_LINEAR_MIPMAP_LINEAR ) override ;

  void setFixedPitch ( int fix ) { fixed_pitch = fix ;  } 
  int   isFixedPitch () const    { return fixed_pitch ; } 

  void  setWidth     ( float w ) { width     = w ; } 
  void  setGap       ( float g ) { gap       = g ; } 

  float getWidth     () const { return width     ; } 
  float getGap       () const { return gap       ; } 


  void setGlyph ( char c, float wid,
                  float tex_left, float tex_right,
                  float tex_bot , float tex_top  ,
                  float vtx_left, float vtx_right,
                  float vtx_bot , float vtx_top  ) ;
  void setGlyph ( char c,
                  float tex_left, float tex_right,
                  float tex_bot , float tex_top  ,
                  float vtx_left, float vtx_right,
                  float vtx_bot , float vtx_top  ) /* deprecated */
  {
    setGlyph ( c, vtx_right,
               tex_left, tex_right, tex_bot, tex_top,
               vtx_left, vtx_right, vtx_bot, vtx_top ) ;
  }
  
  int  getGlyph ( char c, float* wid,
                  float *tex_left = NULL, float *tex_right = NULL,
                  float *tex_bot  = NULL, float *tex_top   = NULL,
                  float *vtx_left = NULL, float *vtx_right = NULL,
                  float *vtx_bot  = NULL, float *vtx_top   = NULL) ;
  int  getGlyph ( char c,
                  float *tex_left = NULL, float *tex_right = NULL,
                  float *tex_bot  = NULL, float *tex_top   = NULL,
                  float *vtx_left = NULL, float *vtx_right = NULL,
                  float *vtx_bot  = NULL, float *vtx_top   = NULL) /* deprecated */
  {
    return getGlyph ( c, NULL,
                      tex_left, tex_right, tex_bot, tex_top,
                      vtx_left, vtx_right, vtx_bot, vtx_top ) ;
  }

  int hasGlyph ( char c ) const { return exists[ (GLubyte) c ] ; }

  // 行距配置（二次开发新增）；非法值（<=0）回退为 1.0，避免把整行文字压到重叠。
  void  setLineSpacing ( float v ) override { line_spacing = ( v > 0.0f ) ? v : 1.0f ; }
  float getLineSpacing () const override { return line_spacing ; }

  void getBBox ( const char *s, float pointsize, float italic,
                 float *left, float *right,
                 float *bot , float *top  ) ;
  
  void begin ()
  {
    bind_texture () ;
    bound = FNT_TRUE ;
  }

  void end ()
  {
    bound = FNT_FALSE ;
  }

  void puts ( sgVec3 curpos, float pointsize, float italic, const char *s ) ;

  void putch ( sgVec3 curpos, float pointsize, float italic, char c )
  {
    if ( ! bound )
      bind_texture () ;

    low_putch ( curpos, pointsize, italic, c ) ;
  }

} ;



class fntBitmapFont : public fntFont
{
 protected:
    
    const GLubyte **data;
    int first;
    int count;
    int height;
    float xorig, yorig;
    int fix;
    float wid, gap;

    // 行距系数（二次开发新增）：位图后端的换行步进单位是字体像素高度 height，
    // 因此行距 = height × line_spacing（与 pointsize 无关）。默认 1.0 = 原行为。
    float line_spacing = 1.0f;

 public:

    // data is a null-terminated list of glyph images:
    //   data[i][0]    - image width
    //   data[i][1..n] - packed bitmap

    fntBitmapFont ( const GLubyte **data, int first, int height, 
		    float xorig, float yorig ) ;

    virtual ~fntBitmapFont () ;

    virtual void getBBox ( const char *s, float pointsize, float italic,
			   float *left, float *right,
			   float *bot , float *top ) ;

    virtual void putch ( sgVec3 curpos, float pointsize, float italic, char c ) ;
    virtual void puts ( sgVec3 curpos, float pointsize, float italic, const char *s ) ;

    virtual void begin () ;
    virtual void end () ;
    
    virtual int load ( const SGPath& fname, GLenum mag, GLenum min ) override { return -1; }
    
    virtual void setFixedPitch ( int f )    { fix = f;    }
    virtual int   isFixedPitch () const     { return fix; }
    
    virtual void  setWidth     ( float w )  { wid = w;    }
    virtual void  setGap       ( float g )  { gap = g;    }
    
    virtual float getWidth     () const     { return wid; }
    virtual float getGap       () const     { return gap; }
    
    virtual int hasGlyph ( char c ) const ;

    // 行距配置（二次开发新增）；非法值（<=0）回退为 1.0。
    void  setLineSpacing ( float v ) override { line_spacing = ( v > 0.0f ) ? v : 1.0f ; }
    float getLineSpacing () const override { return line_spacing ; }
};


/* Builtin Bitmap Fonts */

#define FNT_BITMAP_8_BY_13         0
#define FNT_BITMAP_9_BY_15         1
#define FNT_BITMAP_HELVETICA_10    2
#define FNT_BITMAP_HELVETICA_12    3
#define FNT_BITMAP_HELVETICA_18    4
#define FNT_BITMAP_TIMES_ROMAN_10  5
#define FNT_BITMAP_TIMES_ROMAN_24  6

fntBitmapFont *fntGetBitmapFont(int id);



// 基于 FreeType 的 Unicode 字体（中文化新增）。加载 .ttf/.otf 文件，
// 把 UTF-8 字符串按需光栅化进一张"覆盖率图集"纹理并动态绘制，
// 因此能显示中文等 CJK 多字节文字（fntTexFont 只能渲染 .txf 纹理字体
// 里的 256 个单字节字符，无法显示 CJK）。
class fntUnicodeFont : public fntFont
{
public:
  fntUnicodeFont ();
  virtual ~fntUnicodeFont ();

  int load ( const SGPath& fname, GLenum mag = GL_NEAREST,
                                  GLenum min = GL_LINEAR_MIPMAP_LINEAR ) override;

  void getBBox ( const char *s, float pointsize, float italic,
                 float *left, float *right,
                 float *bot , float *top  ) override;

  void putch ( sgVec3 curpos, float pointsize, float italic, char c ) override;
  void puts  ( sgVec3 curpos, float pointsize, float italic, const char *s ) override;

  void begin () override;
  void end   () override;

  void setFixedPitch ( int fix ) override { fixed_pitch = fix ; }
  int  isFixedPitch () const override  { return fixed_pitch ; }

  void  setWidth     ( float w ) override { width = w ; }
  void  setGap       ( float g ) override { gap = g ; }
  float getWidth     () const override { return width ; }
  float getGap       () const override { return gap ; }

  int hasGlyph ( char c ) const override { return 1 ; }

  // ---- 主题排版扩展（二次开发新增，实现在 fntUnicode.cxx）----
  // 行距：换行垂直步进 = pointsize × lineSpacing；默认 1.0（行高等于字号）。
  void  setLineSpacing ( float v ) override ;
  float getLineSpacing () const override ;
  // 文字描边（像素宽度 + RGBA 颜色）；widthPx <= 0 关闭。
  void  setOutline ( float widthPx, float r, float g, float b, float a ) override ;
  // 字符串级显示列表缓存开关；关闭时退回逐字形绘制。
  void  setStringCache ( bool on ) override ;
  // 字形图集边长（2 的幂）；在首次光栅化之前调用才生效。
  void  setAtlasSize ( int px ) override ;
  // 按最大宽度自动换行（断点优先空格、CJK 可任意断、保留原有 '\n'）。
  std::string wrapString ( const char * s, float pointsize, float maxWidth ) override ;

private:
  struct Impl ;
  Impl *impl ;

  float width ;
  float gap ;
  int fixed_pitch ;
  int bound ;
} ;



class fntRenderer
{
  fntFont *font ;

  sgVec3 curpos ;

  float pointsize ;
  float italic ;

public:
  fntRenderer ()
  {
    start2f ( 0.0f, 0.0f ) ;
    font = NULL ;
    pointsize = 10 ;
    italic = 0 ;
  }

  void start3fv ( sgVec3 pos ) { sgCopyVec3 ( curpos, pos ) ; }
  void start2fv ( sgVec2 pos ) { sgCopyVec2 ( curpos, pos ) ; curpos[2]=0.0f ; }
  void start2f  ( float x, float y ) { sgSetVec3 ( curpos, x, y, 0.0f ) ; }
  void start3f  ( float x, float y, float z ) { sgSetVec3 ( curpos, x, y, z ) ; }

  void getCursor ( float *x, float *y, float *z ) const
  {
    if ( x != NULL ) *x = curpos [ 0 ] ;
    if ( y != NULL ) *y = curpos [ 1 ] ;
    if ( z != NULL ) *z = curpos [ 2 ] ;
  }

  void     setFont ( fntFont *f ) { font = f ; }
  fntFont *getFont () const { return font ; }

  void  setSlant     ( float i ) { italic    = i ; } 
  void  setPointSize ( float p ) { pointsize = p ; }

  float getSlant     () const { return italic    ; } 
  float getPointSize () const { return pointsize ; } 

  void begin () { font->begin () ; }
  void end   () { font->end   () ; }

  void putch ( char  c ) { font->putch ( curpos, pointsize, italic, c ) ; }
  void puts  ( const char *s ) { font->puts  ( curpos, pointsize, italic, s ) ; }
} ;


void fntInit () ;

#endif

