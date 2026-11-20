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


#include "config.h"

#include "PUICamera.hxx"

#include <osg/StateSet>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/Version>
#include <osg/RenderInfo>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/BlendFunc>
// osg::Depth：给合成 quad 显式声明"始终通过但不写深度"的深度状态（见 init）
#include <osg/Depth>

#include <osg/NodeVisitor>
#include <osgGA/GUIEventAdapter>
#include <osgGA/GUIEventHandler>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>

#include <simgear/scene/util/SGNodeMasks.hxx>

#include <GUI/FlightGear_pu.h>
// 主题配置：预乘 Alpha 开关与 frame-gating 开关（设计契约见 docs/pui-theme-design.md §3.5）。
// Viewer 模块引用 GUI 模块的头文件在 MSVC 静态链接下无碍（最终都链进 fgfs）。
#include <GUI/fgui_theme.hxx>
#include <Main/fg_props.hxx>
#include <Main/globals.hxx>
#include <Main/locale.hxx>
#include <Viewer/CameraGroup.hxx>
#include <Viewer/FGEventHandler.hxx>
#include <Viewer/renderer.hxx>

#include <Input/input.hxx>
#include <Input/FGMouseInput.hxx>

// Old versions of PUI are missing these defines
#ifndef PU_SCROLL_UP_BUTTON
#define PU_SCROLL_UP_BUTTON 3
#endif
#ifndef PU_SCROLL_DOWN_BUTTON
#define PU_SCROLL_DOWN_BUTTON 4
#endif

using namespace flightgear;

double static_pixelRatio = 1.0;

class PUIDrawable : public osg::Drawable
{
public:
    // 构造时记录所属 PUICamera：绘制期需要查询 UI 逻辑尺寸（glScissor 用），
    // 并由所属相机判定 frame-gating 的尺寸/清屏行为。这里只保存裸指针——
    // 本 drawable 是 PUICamera 的 Geode 子节点，相机生命周期必然覆盖它，
    // 用 ref_ptr 反而会形成环引用。OSG 的 clone() 路径传 nullptr，
    // 此时跳过 scissor（clone 路径不属于正常渲染流程）。
    explicit PUIDrawable(PUICamera* camera) :
        _camera(camera)
    {
        setUseDisplayList(false);
        setDataVariance(Object::DYNAMIC);
    }

    void drawImplementation(osg::RenderInfo& renderInfo) const override
    {
        // 说明：设计文档曾规划 frame-gating（puNeedRefresh() 为假时跳过
        // puDisplay 并保住上一帧 FBO 内容），复核发现 OSG 3.6.5 的
        // RenderStage 在 cull 回调之前就缓存了 clearMask（CullVisitor::apply
        // 先 setClearMask 再跑 cull callback），任何回调时机都会晚一帧生效，
        // 产生整层闪烁/消失。该特性已整体移除（2026-11-19）：每帧照常
        // 清屏 + 重绘 PUI，文字性能由 fntUnicode 的字符串显示列表缓存保证。
        // ui_theme*.xml 里的 <frame-gating> 键保留但不再被读取。

        osg::State* state = renderInfo.getState();
        state->setActiveTextureUnit(0);
        state->setClientActiveTextureUnit(0);
        state->disableAllVertexArrays();

        state->applyMode(GL_FOG, false);
        state->applyMode(GL_DEPTH_TEST, false);
        state->applyMode(GL_LIGHTING, false);

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(~0u);

        glEnable    ( GL_BLEND        ) ;
        // 预乘 Alpha 修正（设计文档 §3.5）：
        // FBO 每帧被清成 (0,0,0,0)，PUI 按直通 alpha 混合往里画时，写入纹理的
        // 实际是 RGB=C*a、A=a*a（alpha 被平方）；而合成 quad 端一直按预乘色用
        // glBlendFunc(ONE, ONE_MINUS_SRC_ALPHA) 采样，于是半透明面板比设定更透、
        // 重叠控件 alpha 越叠越错。主题启用且 premultiplied=1 时，源因子改用
        // GL_ONE（GL 1.1 即提供，无需 glBlendFuncSeparate——Windows 的 opengl32.lib
        // 只导出 GL 1.1 符号），配合主题色加载阶段已预乘，得到 RGB=C*a、A=a 的
        // 严格预乘结果，quad 端既有混合函数无需改动。premultiplied=0 时保持原函数。
        if (fguiTheme::enabled() && fguiTheme::metrics().premultiplied) {
            glBlendFunc ( GL_ONE, GL_ONE_MINUS_SRC_ALPHA ) ;
        } else {
            glBlendFunc ( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA ) ;
        }

        // 绘制期禁止深度写入：本 FBO 只挂颜色附件、没有深度缓冲，且合成 quad 已
        // 关闭 GL_DEPTH_TEST；这里再显式关闭深度写掩码，防止 PUI/字形绘制路径
        // （默认深度写为开启）意外推进深度值，污染同一 GUI pass 的后续绘制。
        // 该状态位于 glPushAttrib 保护内，退出时会自动恢复。
        glDepthMask ( GL_FALSE ) ;

        // reset pixel storage stuff for PLIB FNT drawing via glBitmap
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        // 把绘制范围裁剪到整个 UI 逻辑窗口。glScissor 的参数是窗口像素坐标
        // （原点在窗口左下角），与当前投影矩阵/视图矩阵无关，因此这里直接用与
        // puGetWindowSize 回调同源的 scaled 尺寸（已除以 static_pixelRatio），
        // 而不是未缩放的窗口宽高。该裁剪位于 glPushAttrib 保护内，puDisplay()
        // 结束后随 glPopAttrib() 一起恢复，不会泄漏给后续 GUI pass 的绘制。
        if (_camera && _camera->uiWidth() > 0 && _camera->uiHeight() > 0) {
            glEnable ( GL_SCISSOR_TEST );
            glScissor( 0, 0, _camera->uiWidth(), _camera->uiHeight() );
        }

        puDisplay();

        glPopClientAttrib();
        glPopAttrib();
    }

    osg::Object* cloneType() const override { return new PUIDrawable(nullptr); }
    osg::Object* clone(const osg::CopyOp&) const override { return new PUIDrawable(nullptr); }

private:
    // 所属 PUICamera（裸指针，理由见构造函数注释）
    PUICamera* _camera = nullptr;
};

class PUIEventHandler : public osgGA::GUIEventHandler
{
public:
    PUIEventHandler(PUICamera* cam) :
        _puiCamera(cam)
    {
        _mouse0RightButtonNode = fgGetNode("/devices/status/mice/mouse[0]/button[2]", true);
    }
    
    bool handle(const osgGA::GUIEventAdapter &ea, osgGA::GUIActionAdapter &aa, osg::Object *, osg::NodeVisitor *nv) override
    {
        if (ea.getHandled()) return false;
        
        // PUI expects increasing downward mouse coords
        const int fixedY = (ea.getMouseYOrientation() == osgGA::GUIEventAdapter::Y_INCREASING_UPWARDS) ?
                            ea.getWindowHeight() - ea.getY() : ea.getY();
        const int scaledX = static_cast<int>(ea.getX() / static_pixelRatio);
        const int scaledY = static_cast<int>(fixedY / static_pixelRatio);
        
        switch(ea.getEventType())
        {
            case(osgGA::GUIEventAdapter::DRAG):
                if (!_is_dragging)
                    return false;
                // No break
            case(osgGA::GUIEventAdapter::MOVE):
                return puMouse(scaledX, scaledY);

            case(osgGA::GUIEventAdapter::PUSH):
            case(osgGA::GUIEventAdapter::RELEASE):
            {
                // during splash/reset, either of these can return nullptr
                const auto input = globals->get_subsystem<FGInput>();
                const auto mouseSubsystem = input ? input->get_subsystem<FGMouseInput>() : nullptr;
                if (mouseSubsystem && !mouseSubsystem->isActiveModePassThrough()) {
                    return false;
                }

                bool mouse_up = (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE);
                bool handled = puMouse(osgButtonToPUI(ea), mouse_up, scaledX, scaledY);
                if (!mouse_up && handled)
                {
                    _is_dragging = true;
                }
                // Release drag if no more buttons are pressed
                else if (mouse_up && !ea.getButtonMask())
                {
                    _is_dragging = false;
                }
                return handled;
            }

            case(osgGA::GUIEventAdapter::KEYDOWN):
            case(osgGA::GUIEventAdapter::KEYUP):
            {
                const bool isKeyRelease = (ea.getEventType() == osgGA::GUIEventAdapter::KEYUP);
                const int key = flightgear::FGEventHandler::translateKey(ea);
                bool handled = puKeyboard(key, isKeyRelease);
                return handled;
            }

            case osgGA::GUIEventAdapter::SCROLL:
            {
                const int button = buttonForScrollEvent(ea);
                if (button != PU_NOBUTTON) {
                    // sent both down and up events for a single scroll, for
                    // compatability
                    bool handled = puMouse(button, PU_DOWN, scaledX, scaledY);
                    handled |= puMouse(button, PU_UP, scaledX, scaledY);
                    return handled;
                }
                return false;
            }

            case (osgGA::GUIEventAdapter::RESIZE):
                _puiCamera->resizeUi(ea.getWindowWidth(), ea.getWindowHeight());
                break;

            default:
                return false;
        }
        return false;
    }
private:
    int osgButtonToPUI(const osgGA::GUIEventAdapter &ea) const
    {
        switch (ea.getButton()) {
        case osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON:
            return 0;
        case osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON:
            return 1;
        case osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON:
            return 2;
        }

        return 0;
    }
    
    int buttonForScrollEvent(const osgGA::GUIEventAdapter &ea) const
    {
        if (ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_2D) {
            int button = PU_NOBUTTON;
            if (ea.getScrollingDeltaY() > 0)
                button = PU_SCROLL_UP_BUTTON;
            else if (ea.getScrollingDeltaY() < 0)
                button = PU_SCROLL_DOWN_BUTTON;
            
#if defined(SG_MAC)
            // bug https://code.google.com/p/flightgear-bugs/issues/detail?id=1286
            // Mac (Cocoa) interprets shift+wheel as horizontal scroll
            if (ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_SHIFT) {
                if (ea.getScrollingDeltaX() > 0) {
                    button = PU_SCROLL_UP_BUTTON;
                } else if (ea.getScrollingDeltaX() < 0) {
                    button = PU_SCROLL_DOWN_BUTTON;
                }
            }
#endif
            return button;
        } else if (ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP) {
            return PU_SCROLL_UP_BUTTON;
        }

        return PU_SCROLL_DOWN_BUTTON;
    }

    PUICamera*  _puiCamera = nullptr;
    bool        _is_dragging = false;

    SGPropertyNode_ptr _mouse0RightButtonNode;
};

// The pu getWindow callback is supposed to return a window ID that
// would allow drawing a GUI on different windows. All that stuff is
// broken in multi-threaded OSG, and we only have one GUI "window"
// anyway, so just return a constant.
int PUICamera::puGetWindow()
{
    return 1;
}

void PUICamera::puGetWindowSize(int* width, int* height)
{
    *width = 0;
    *height = 0;
    osg::Camera* camera = getGUICamera(CameraGroup::getDefault());
    if (!camera)
        return;

    osg::Viewport* vport = camera->getViewport();
    *width = static_cast<int>(vport->width() / static_pixelRatio);
    *height = static_cast<int>(vport->height() / static_pixelRatio);
}

void PUICamera::initPUI()
{
    puSetWindowFuncs(PUICamera::puGetWindow, nullptr,
                     PUICamera::puGetWindowSize, nullptr);
    puRealInit();
}
PUICamera::PUICamera() :
    osg::Camera()
{
}

PUICamera::~PUICamera()
{
    SG_LOG(SG_GL, SG_INFO, "Deleting PUI camera");

    // depending on if we're doing shutdown or reset, various things can be
    // null here.
    auto renderer = globals->get_renderer();
    auto view = renderer ? renderer->getView() : nullptr;
    if (view) {
        view->removeEventHandler(_eventHandler);
    }
}

void PUICamera::init(osg::Group* parent, osgViewer::View* view)
{
    setName("PUI FBO camera");
    
    _fboTexture = new osg::Texture2D;
    _fboTexture->setInternalFormat(GL_RGBA);
    _fboTexture->setResizeNonPowerOfTwoHint(false);
    _fboTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    _fboTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

    // setup the camera as render to texture
    setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    setViewMatrix(osg::Matrix::identity());
    setClearMask( GL_COLOR_BUFFER_BIT );
    setClearColor( osg::Vec4( 0.0, 0.0, 0.0, 0.0 ) );
    setAllowEventFocus(false);
    setCullingActive(false);
    setRenderTargetImplementation( osg::Camera::FRAME_BUFFER_OBJECT );
    setRenderOrder(osg::Camera::PRE_RENDER);
    attach(osg::Camera::COLOR_BUFFER, _fboTexture);

    // set the camera's node mask, ensure the pick bit is clear
    setNodeMask(SG_NODEMASK_GUI_BIT);

// geode+drawable to call puDisplay, as a child of this FBO-camera
    osg::Geode* geode = new osg::Geode;
    geode->setName("PUIDrawableGeode");
    // 传入 this：绘制期需要查询 UI 逻辑尺寸（glScissor）与 frame-gating 状态
    geode->addDrawable(new PUIDrawable(this));
    addChild(geode);

// geometry (full-screen quad) to draw the output
    _fullScreenQuad = new osg::Geometry;
    _fullScreenQuad = osg::createTexturedQuadGeometry(osg::Vec3(0.0, 0.0, 0.0),
                                               osg::Vec3(200.0, 0.0, 0.0),
                                               osg::Vec3(0.0, 200.0, 0.0));
    _fullScreenQuad->setSupportsDisplayList(false);
    _fullScreenQuad->setName("PUI fullscreen quad");

// state used for drawing the quad (not for rendering PUI, that's done in the
// drawbale above)
    osg::StateSet* stateSet = _fullScreenQuad->getOrCreateStateSet();
    // 渲染 bin 从 1001 提到 1002：QML 的 QQuickDrawable 也使用 bin 1001，
    // 且其节点在 renderer.cxx 中晚于 PUI 加入 GUI 相机，同 bin 内会盖住 PUI。
    // 1002 保证 PUI 绝对置顶（Canvas 桌面仍是 bin 1000，在 PUI 之下）。
    stateSet->setRenderBinDetails(1002, "RenderBin");
    stateSet->setTextureMode(0, GL_TEXTURE_2D, osg::StateAttribute::ON);
    stateSet->setTextureAttribute(0, _fboTexture);

    // use GL_ONE becuase we pre-multiplied by alpha when building the FBO texture
    stateSet->setAttribute(new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));
    stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
    stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    stateSet->setMode(GL_FOG, osg::StateAttribute::OFF);
    stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
    // 显式声明深度状态：始终通过深度测试（ALWAYS）但不写深度（writeMask=false），
    // 与 splash.cxx 的写法一致。本 camera pass 没有深度附件且 GL_DEPTH_TEST 已关闭，
    // 这里再声明一次可防止 quad 合成污染 GUI pass 内后续节点的深度状态。
    stateSet->setAttribute(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false));

    bool use_vertex_attribute_aliasing = false;
    auto guiCamera = getGUICamera(CameraGroup::getDefault());
    if (guiCamera) {
        auto gc = guiCamera->getGraphicsContext();
        use_vertex_attribute_aliasing = gc->getState()->getUseVertexAttributeAliasing();
    }

    if (use_vertex_attribute_aliasing) {
        const std::string vertex_source =
            "#version 330 core\n"
            "\n"
            "layout(location = 0) in vec4 pos;\n"
            "layout(location = 3) in vec4 multitexcoord0;\n"
            "\n"
            "out vec2 texcoord;\n"
            "\n"
            "uniform mat4 osg_ModelViewProjectionMatrix;\n"
            "\n"
            "void main()\n"
            "{\n"
            "    gl_Position = osg_ModelViewProjectionMatrix * pos;\n"
            "    texcoord = multitexcoord0.st;\n"
            "}\n";
        osg::Shader *vertex_shader = new osg::Shader(osg::Shader::VERTEX, vertex_source);

        const std::string fragment_source =
            "#version 330 core\n"
            "\n"
            "layout(location = 0) out vec4 fragColor;\n"
            "\n"
            "in vec2 texcoord;\n"
            "\n"
            "uniform sampler2D tex;\n"
            "\n"
            "void main()\n"
            "{\n"
            "    fragColor = texture(tex, texcoord);\n"
            "}\n";
        osg::Shader *fragment_shader = new osg::Shader(osg::Shader::FRAGMENT, fragment_source);

        osg::Program *program = new osg::Program;
        program->addShader(vertex_shader);
        program->addShader(fragment_shader);
        stateSet->setAttribute(program);
        stateSet->addUniform(new osg::Uniform("tex", 0));
    }

// geode to display the FSquad in the parent scene (which is GUI camera)
    osg::Geode* fsQuadGeode = new osg::Geode;
    fsQuadGeode->addDrawable(_fullScreenQuad);
    fsQuadGeode->setName("PUI fullscreen Geode");
    fsQuadGeode->setNodeMask(SG_NODEMASK_GUI_BIT);
    parent->addChild(this);
    parent->addChild(fsQuadGeode);
    
    osg::Camera* camera = getGUICamera(CameraGroup::getDefault());
    if (camera) {
        osg::Viewport* vport = camera->getViewport();
        resizeUi(vport->width(), vport->height());
    }

    // push_front so we keep the order of event handlers the opposite of
    // the rendering order (i.e top-most UI layer has the front-most event
    // handler)
    _eventHandler = new PUIEventHandler(this);
    view->getEventHandlers().push_front(_eventHandler);
}

// remove once we require OSG 3.4
void PUICamera::manuallyResizeFBO(int width, int height)
{
    _fboTexture->setTextureSize(width, height);
    _fboTexture->dirtyTextureObject();
}

void PUICamera::resizeUi(int width, int height)
{
    static_pixelRatio = fgGetDouble("/sim/rendering/gui-pixel-ratio", 1.0);
    const int scaledWidth = static_cast<int>(width / static_pixelRatio);
    const int scaledHeight = static_cast<int>(height / static_pixelRatio);
    
    setViewport(0, 0, scaledWidth, scaledHeight);

    osg::Camera::resize(scaledWidth, scaledHeight);
    resizeAttachments(scaledWidth, scaledHeight);

    // 记录 UI 逻辑尺寸：与 puGetWindowSize 回调同源（窗口宽高除以 static_pixelRatio），
    // 供 PUIDrawable 在绘制期设置 glScissor 使用。FBO 附件刚被重建（内容为空），
    // 因此必须强制下一帧重绘——否则 frame-gating 开启时会跳过 puDisplay()，
    // 屏幕上会短暂显示空 UI。
    _uiWidth = scaledWidth;
    _uiHeight = scaledHeight;
    puPostRefresh();

    const float puiZ = 1.0;
    // resize the full-screen quad
    osg::Vec3Array* fsQuadVertices = static_cast<osg::Vec3Array*>(_fullScreenQuad->getVertexArray());
    (*fsQuadVertices)[0] = osg::Vec3(0.0, height, puiZ);
    (*fsQuadVertices)[1] = osg::Vec3(0.0, 0.0, puiZ);
    (*fsQuadVertices)[2] = osg::Vec3(width, 0.0, puiZ);
    (*fsQuadVertices)[3] = osg::Vec3(width, height, puiZ);

    fsQuadVertices->dirty();
}

