// Integrate Canvas into FlightGear
//
// Copyright (C) 2012  Thomas Geymayer <tomgey@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include "FGCanvasSystemAdapter.hxx"

#include <Main/globals.hxx>
#include <Network/HTTPClient.hxx>
#include <Viewer/renderer.hxx>

#include <osg/Version>
#include <osgDB/ReadFile>
#include <stdexcept>

#include <simgear/scene/util/SGReaderWriterOptions.hxx>

namespace canvas
{
  //----------------------------------------------------------------------------
  simgear::canvas::FontPtr
  FGCanvasSystemAdapter::getFont(const std::string& name) const
  {
    // 主题字体别名解析：Canvas GUI 控件统一使用 "ui-theme-gui" 作为字体名，
    // 实际字体文件由主题属性 /sim/gui/theme/font/name 决定（默认中文黑体 cn/simhei.ttf）。
    // 其余字体名（7-Segment、BoeingCDU 等专用仪表字体）保持原有解析流程不变，
    // 避免把全局字体强行重定向到中文字体而破坏仪表字形。
    std::string font_name = name;
    if( name == "ui-theme-gui" )
    {
      // 属性树在极早期阶段（单元测试、主题尚未加载等）可能尚不存在，需判空。
      font_name.clear();
      if( globals->get_props() )
        font_name = globals->get_props()->getStringValue("/sim/gui/theme/font/name");

      // 主题节点缺失或取值为空时回退到默认字体名，保证不因主题未加载而失败。
      if( font_name.empty() )
        font_name = "cn/simhei.ttf";
    }

    // 缓存键使用“解析后的字体文件名”而非调用方传入的别名：
    // 这样 "ui-theme-gui" 与显式的 "cn/simhei.ttf" 会共享同一个 osgText::Font 实例，
    // 命中缓存时直接返回旧对象，行为与未加缓存时完全等价（同一路径只会加载一次）。
    std::map<std::string, simgear::canvas::FontPtr>::const_iterator cached =
      _font_cache.find(font_name);
    if( cached != _font_cache.end() )
      return cached->second;

    SGPath path = globals->resolve_resource_path("Fonts/" + font_name);
    if( path.isNull() )
    {
      SG_LOG
      (
        SG_GL,
        SG_ALERT,
        "canvas::Text: No such font: " << font_name
      );
      return simgear::canvas::FontPtr();
    }

    SG_LOG
    (
      SG_GL,
      SG_DEBUG,
      "canvas::Text: using font file " << path
    );

    simgear::canvas::FontPtr font = osgText::readFontFile(path.utf8Str());
    if( !font )
      SG_LOG
      (
        SG_GL,
        SG_ALERT,
        "canvas::Text: Failed to open font file " << path
      );

    // 仅缓存加载成功的字体：失败结果不缓存，方便主题字体文件稍后补齐时自动重试。
    if( font )
      _font_cache[font_name] = font;

    return font;
  }

  //----------------------------------------------------------------------------
  void FGCanvasSystemAdapter::addCamera(osg::Camera* camera) const
  {
    globals->get_renderer()->addCamera(camera, false);
  }

  //----------------------------------------------------------------------------
  void FGCanvasSystemAdapter::removeCamera(osg::Camera* camera) const
  {
    if( globals->get_renderer() )
      globals->get_renderer()->removeCamera(camera);
  }

  //----------------------------------------------------------------------------
  osg::ref_ptr<osg::Image> FGCanvasSystemAdapter::getImage(const std::string& path) const
  {
    SGPath p(SGPath::fromUtf8(path));
    osg::ref_ptr<simgear::SGReaderWriterOptions> localReaderWriterOptions = simgear::SGReaderWriterOptions::copyOrCreate(osgDB::Registry::instance()->getOptions());
    localReaderWriterOptions->setLoadOriginHint(simgear::SGReaderWriterOptions::LoadOriginHint::ORIGIN_CANVAS);

    if( p.isAbsolute() )
    {
      const SGPath valid_path = SGPath(p).validate(false);
      if( !valid_path.isNull() )
          return osgDB::readRefImageFile(valid_path.utf8Str(), localReaderWriterOptions);

      SG_LOG(SG_IO, SG_ALERT, "canvas::Image: reading '" << path << "' denied");
    }
    else
    {
      SGPath tpath = globals->resolve_resource_path(path);
      if( !tpath.isNull() )
          return osgDB::readRefImageFile(tpath.utf8Str(), localReaderWriterOptions);

      SG_LOG(SG_IO, SG_ALERT, "canvas::Image: No such image: '" << path << "'");
    }

    return 0;
  }

  //----------------------------------------------------------------------------
  SGSubsystem*
  FGCanvasSystemAdapter::getSubsystem(const std::string& name) const
  {
    return globals->get_subsystem_mgr()->get_subsystem(name.c_str());
  }

  //----------------------------------------------------------------------------
  simgear::HTTP::Client* FGCanvasSystemAdapter::getHTTPClient() const
  {
    auto http = globals->get_subsystem<FGHTTPClient>();

    if( http )
      return http->client();

    SG_LOG( SG_IO,
            SG_ALERT,
            "FGCanvasSystemAdapter: Failed to get HTTP subsystem" );
    return nullptr;
  }

}
