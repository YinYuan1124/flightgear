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

#ifndef FG_CANVASSYSTEMADAPTER_HXX_
#define FG_CANVASSYSTEMADAPTER_HXX_

#include <simgear/canvas/CanvasSystemAdapter.hxx>

#include <map>      // 字体对象缓存使用 std::map

namespace canvas
{
  class FGCanvasSystemAdapter:
    public simgear::canvas::SystemAdapter
  {
    public:
      simgear::canvas::FontPtr getFont(const std::string& name) const override;
      void addCamera(osg::Camera* camera) const override;
      void removeCamera(osg::Camera* camera) const override;
      osg::ref_ptr<osg::Image> getImage(const std::string& path) const override;
      SGSubsystem* getSubsystem(const std::string& name) const override;
      simgear::HTTP::Client* getHTTPClient() const override;

    private:
      // 字体对象缓存：键是解析后的字体文件名（相对于 FG_ROOT/Fonts，例如 "cn/simhei.ttf"），
      // 值是已加载的 osgText::Font。Canvas 每次应用 font 样式都会走到 getFont，
      // 缓存可避免重复 readFontFile 导致的字形图集重复生成与额外显存占用。
      // 该适配器是 Canvas 的进程级全局静态单例（Canvas::_system_adapter），
      // 其生命周期与进程一致，因此缓存无需额外清理；访问均发生在主线程。
      // 声明为 mutable 是因为 getFont 是 const 成员函数，但仍需在内部填充缓存。
      mutable std::map<std::string, simgear::canvas::FontPtr> _font_cache;
  };
}

#endif /* FG_CANVASSYSTEMADAPTER_HXX_ */
