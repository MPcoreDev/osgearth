/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 *
 * This library is open source and may be redistributed and/or modified under
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * OpenSceneGraph Public License for more details.
*/


#include <osgEarthAnnotation/MPStateSetFontAltas>
#include <osgEarth/GLUtils>
#include <osgEarth/VirtualProgram>
#include <osgEarthAnnotation/Shaders>
#include <osgEarthSymbology/Color>
#include <osgEarth/Registry>

#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osg/Texture2D>


#define LC "[MPStateSetFontAltas] "


using namespace osgEarth::Annotation;

const int MPStateSetFontAltas::ATTRIB_ANNO_INFO = osg::Drawable::ATTRIBUTE_6;
const int MPStateSetFontAltas::ATTRIB_ANNO_COLOR2 = osg::Drawable::ATTRIBUTE_7;
const int MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_CENTER = osg::Drawable::ATTRIBUTE_7+1;
const int MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_ANCHOR = osg::Drawable::ATTRIBUTE_7+2;

const std::string MPStateSetFontAltas::UNIFORM_HIGHLIGHT_FILL_COLOR = "oe_anno_highlightFillColor";
const std::string MPStateSetFontAltas::UNIFORM_HIGHLIGHT_STROKE_COLOR = "oe_anno_highlightStrokeColor";
const std::string MPStateSetFontAltas::UNIFORM_HIGHLIGHT_STROKE_WIDTH = "oe_anno_highlightStrokeWidth";

namespace  {
    const osgEarth::Symbology::Color    _defaultHighlightFillColor     { 0x3a6caaff };
    const osgEarth::Symbology::Color    _defaultHighlightStrokeColor   { 0x4b96ffff };
    const float                         _defaultHighlightStrokeWidth   { 3.f };
}


GlyphInfo::GlyphInfo(double x, double y, double scale, double w, double h, double cursorX, double cursorY, double pAdvance, double textureSize) :
    advance(pAdvance/scale), refScale(scale)
{
    // quad vertexes considering y=0 as baseline and scaled in font native unit
    lb_v.set(-cursorX / scale, -cursorY / scale, 0.);
    lt_v.set(-cursorX / scale, -cursorY / scale + h / scale, 0.);
    rt_v.set(-cursorX / scale + w / scale, -cursorY / scale + h / scale, 0.);
    rb_v.set(-cursorX / scale + w / scale, -cursorY / scale, 0.);

    // corresponding texture coordinates
    lb_t.set(x / textureSize, (y+h) / textureSize);
    lt_t.set(x / textureSize, y / textureSize);
    rt_t.set( (x+w) / textureSize, y / textureSize);
    rb_t.set( (x+w) / textureSize, (y+h) / textureSize);

    size.set(rt_v.x() - lt_v.x(), rt_v.y() - rb_v.y());
}


IconInfo::IconInfo(double x, double y, double s, double textureSize)
{
    // quad vertexes
    double halftSize = s / 2.;
    lb_v.set(-halftSize, -halftSize, 0.);
    lt_v.set(-halftSize, halftSize, 0.);
    rt_v.set(halftSize, halftSize, 0.);
    rb_v.set(halftSize, -halftSize, 0.);

    // corresponding texture coordinates
    lb_t.set(x / textureSize, (y+s) / textureSize);
    lt_t.set(x / textureSize, y / textureSize);
    rt_t.set( (x+s) / textureSize, y / textureSize);
    rb_t.set( (x+s) / textureSize, (y+s) / textureSize);

    size.set(s, s);
}

MPStateSetFontAltas::MPStateSetFontAltas(const std::string &iconAtlasPath, const osgDB::Options *readOptions) : StateSet()
{
    // get the pixel density
    int dpi = Registry::instance()->getDeviceImageCategoryDensity();
    std::string iconAtlasPathWithDPI = osgDB::getNameLessExtension(iconAtlasPath) + std::to_string(dpi) + "." + osgDB::getFileExtension(iconAtlasPath);

    // load the icon atlas
    std::string fullIconAtlasPath = osgDB::findDataFile(iconAtlasPathWithDPI, readOptions, osgDB::CASE_INSENSITIVE);
    if ( fullIconAtlasPath.empty() )
    {
        // On iOS the icon atlas file is search inside default.earth as if it were a directory.
        // To prevent this behavior we force the search next to default.earth.
        fullIconAtlasPath = osgDB::findDataFile("../" + iconAtlasPathWithDPI, readOptions, osgDB::CASE_INSENSITIVE);
    }
    if ( fullIconAtlasPath.empty() || ! osgDB::fileExists(fullIconAtlasPath) )
    {
        OE_WARN << LC << "Unable to locate the icon atlas " << iconAtlasPathWithDPI << "\n";
        return;
    }
    URI imageURI = URI(fullIconAtlasPath, readOptions);
    osg::ref_ptr<osg::Image> imageIcon = imageURI.getImage(readOptions);
    if (! imageIcon.valid() )
    {
        OE_WARN << LC << "Unable to load the icon atlas " << fullIconAtlasPath << "\n";
        return;
    }

    // read the Icon conf file
    double textureSize = imageIcon.get()->s();
    URI atlasConfURI = URI(osgDB::getNameLessExtension(fullIconAtlasPath) + ".txt", readOptions);
    if (! osgDB::fileExists(atlasConfURI.full()))
    {
        OE_WARN << LC << "Unable to load the icon atlas config file " << atlasConfURI.full() << "\n";
        return;
    }

    std::ifstream in (std::ifstream(atlasConfURI.full().c_str()));
    std::string key;
    double x, y, scale, w, h, advance, cursorX, cursorY;
    for( std::string line ; getline( in, line ); )
    {
        // font
        if ( line.find("/") != std::string::npos )
        {
            std::istringstream inLine (line);
            inLine >> key >> x >> y >> scale >> w >> h >> cursorX >> cursorY >> advance;
            mapGlyphs[key] = GlyphInfo(x, y, scale, w, h, cursorX, cursorY, advance, textureSize);
        }
        // icon
        else
        {
            std::istringstream inLine (line);
            inLine >> key >> x >> y >> w;
            mapIcons[key] = IconInfo(x, y, w, textureSize);
        }
    }

    osg::ref_ptr<osg::Texture2D> textureIcon = new osg::Texture2D();
    textureIcon->setImage( imageIcon );

    // stateset stuff
    setDataVariance(osg::Object::DYNAMIC);
    setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    setMode(GL_BLEND, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    GLUtils::setLighting(this, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    setTextureAttributeAndModes(0, textureIcon, osg::StateAttribute::ON);

    // shaders
    VirtualProgram* vp = VirtualProgram::getOrCreate(this);
    vp->setName("MPAnnotation::stateSet");
    vp->addBindAttribLocation( "oe_anno_attr_info", ATTRIB_ANNO_INFO );
    vp->addBindAttribLocation( "oe_anno_attr_color2", ATTRIB_ANNO_COLOR2 );
    vp->addBindAttribLocation( "oe_anno_attr_circle_center", ATTRIB_ANNO_CIRCLE_CENTER );
    vp->addBindAttribLocation( "oe_anno_attr_circle_anchor", ATTRIB_ANNO_CIRCLE_ANCHOR );
    Shaders pkg;
    pkg.load( vp, pkg.MPAnno_Vertex );
    pkg.load( vp, pkg.MPAnno_Fragment );
    addUniform(new osg::Uniform("oe_anno_font_tex", 0));
    addUniform(new osg::Uniform(UNIFORM_HIGHLIGHT_FILL_COLOR.c_str(), _defaultHighlightFillColor));
    addUniform(new osg::Uniform(UNIFORM_HIGHLIGHT_STROKE_COLOR.c_str(), _defaultHighlightStrokeColor));
    addUniform(new osg::Uniform(UNIFORM_HIGHLIGHT_STROKE_WIDTH.c_str(), _defaultHighlightStrokeWidth));
    DefineList defineList;
    defineList["TYPE_CHARACTER_MSDF"] = osg::StateSet::DefinePair(std::to_string(TYPE_CHARACTER_MSDF) + ".", osg::StateAttribute::ON);
    defineList["TYPE_ICON"] = osg::StateSet::DefinePair(std::to_string(TYPE_ICON) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_NO_PICK"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_NO_PICK) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_STROKE_SIDED"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_STROKE_SIDED) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_ROUNDED"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_ROUNDED) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_ONEARROW"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_ONEARROW) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_TWOARROWS"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_TWOARROWS) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_STAIR"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_STAIR) + ".", osg::StateAttribute::ON);
    defineList["TYPE_BBOX_ROUNDED_ORIENTED"] = osg::StateSet::DefinePair(std::to_string(TYPE_BBOX_ROUNDED_ORIENTED) + ".", osg::StateAttribute::ON);
    setDefineList(defineList);
}
