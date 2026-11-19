// Copyright (C) 2017 James Turner
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

#ifndef PUICAMERA_HXX
#define PUICAMERA_HXX

#include <osg/Camera>
#include <osg/Version>
#include <osgViewer/View>

namespace osg
{
    class Texture2D;
    class Geometry;
}

namespace osgViewer {
class Viewer;
}

namespace osgGA {
class GUIEventHandler;
}

namespace flightgear
{

class PUICamera : public osg::Camera
{
public:
    static void initPUI();
    
    PUICamera();
    virtual ~PUICamera();

    osg::Object* cloneType() const override { return new PUICamera; }
    osg::Object* clone(const osg::CopyOp&) const override { return new PUICamera; }

    // osg::Camera already defines a resize() so use this name
    void resizeUi(int width, int height);

    void init(osg::Group* parent, osgViewer::View* view);

    // 当前 UI 逻辑尺寸（宽/高，已除以 gui-pixel-ratio），由 resizeUi 维护。
    // 与 puGetWindowSize 回调同源，供 PUIDrawable 在绘制期设置 glScissor
    // （glScissor 使用窗口像素坐标，与投影矩阵无关，必须用这套 scaled 尺寸）。
    int uiWidth() const { return _uiWidth; }
    int uiHeight() const { return _uiHeight; }

private:
    void manuallyResizeFBO(int width, int height);

    osg::Texture2D* _fboTexture = nullptr;
    osg::Geometry* _fullScreenQuad = nullptr;
    osgGA::GUIEventHandler* _eventHandler = nullptr;

    // frame-gating 用的相机 initial draw callback：在每帧清屏前按 puNeedRefresh()
    // 动态设置 clearMask（需要刷新=清颜色，跳过帧=不清），需用 ref_ptr 保活。
    osg::ref_ptr<osg::NodeCallback> _drawCallback;   // frame-gating 的 cull 回调（见 cxx 注释）

    int _uiWidth = 0;  // UI 逻辑宽度（scaled），0 表示尚未 resize
    int _uiHeight = 0; // UI 逻辑高度（scaled），0 表示尚未 resize

    static void puGetWindowSize(int *width, int *height);
    static int puGetWindow();
};

} // of namespace flightgear

#endif // PUICAMERA_HXX
