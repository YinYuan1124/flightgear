
set (FNT_SOURCES
	fnt.cxx
	fntTXF.cxx
	fntBitmap.cxx
	# fntUnicode.cxx：基于 FreeType 的 Unicode 字体实现（中文化新增）。
	# 按需把 UTF-8 字形光栅化进图集纹理，让 PUI 菜单栏/对话框能显示
	# CJK 等 .txf 纹理字体（仅 256 个单字节字形）无法渲染的多字节文字。
	fntUnicode.cxx
)

add_library(PLIBFont STATIC ${FNT_SOURCES})

# 引入 FreeType 库（vcpkg 提供 freetype CONFIG 包），供 fntUnicode.cxx 使用。
find_package(freetype CONFIG REQUIRED)

target_link_libraries(PLIBFont SimGearCore freetype )

# we use <osg/GL> in fnt.h, so we need to use it
target_include_directories(PLIBFont PUBLIC 
	${CMAKE_CURRENT_SOURCE_DIR} 
	${OPENSCENEGRAPH_INCLUDE_DIRS})
target_include_directories(PLIBFont PRIVATE ${PLIB_INCLUDE_DIR})
