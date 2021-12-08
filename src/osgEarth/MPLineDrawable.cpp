/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2019 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/MPLineDrawable>
#include <osgEarth/Shaders>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/LineFunctor>
#include <osgEarth/GLUtils>
#include <osgEarth/CullingUtils>

#include <osg/LineStipple>
#include <osg/LineWidth>
#include <osgUtil/Optimizer>

#include <osgDB/ObjectWrapper>

using namespace osgEarth;

const std::string MPLineDrawable::UNIFORM_HIGHLIGHT_COLOR = "oe_LineDrawable_highlight_color";

// static attribute binding locations. Changable by the user.
const int MPLineDrawable::SideVectorAttrLocation = 9;
const int MPLineDrawable::LengthAttrLocation     = 10;


// Implementation References:
// https://mattdesl.svbtle.com/drawing-lines-is-hard
// https://github.com/mattdesl/webgl-lines


#define LC "[MPLineGroup] "

namespace osgEarth { namespace Serializers { namespace MPLineGroup
{
    REGISTER_OBJECT_WRAPPER(
        MPLineGroup,
        new osgEarth::MPLineGroup,
        osgEarth::MPLineGroup,
        "osg::Object osg::Node osg::Group osg::Geode osgEarth::MPLineGroup")
    {
        // no properties
    }
} } }


namespace
{
    osg::DrawElements* makeDE(unsigned size)
    {
        osg::DrawElements* de =
            size > 0xFFFF ? (osg::DrawElements*)new osg::DrawElementsUInt(GL_TRIANGLES) :
                size > 0xFF ?   (osg::DrawElements*)new osg::DrawElementsUShort(GL_TRIANGLES) :
                                (osg::DrawElements*)new osg::DrawElementsUByte(GL_TRIANGLES);

        de->reserveElements(size);
        return de;
    }
}


MPLineGroup::MPLineGroup() : LineGroup()
{
    //nop
}

MPLineGroup::MPLineGroup(const MPLineGroup& rhs, const osg::CopyOp& copy) :
LineGroup(rhs, copy)
{
    //nop
}

MPLineGroup::~MPLineGroup()
{
    //nop
}


//...................................................................


#undef  LC
#define LC "[MPLineDrawable] "


namespace osgEarth { namespace Serializers { namespace MPLineDrawable
{
    REGISTER_OBJECT_WRAPPER(
        MPLineDrawable,
        new osgEarth::MPLineDrawable,
        osgEarth::MPLineDrawable,
        "osg::Object osg::Node osg::Drawable osg::Geometry osgEarth::MPLineDrawable")
    {
        ADD_UINT_SERIALIZER( Mode, GL_LINE_STRIP );
        ADD_INT_SERIALIZER( StippleFactor, 1 );
        ADD_USHORT_SERIALIZER( StipplePattern, 0xFFFF );
        ADD_BOOL_SERIALIZER( BindColorOverall, false );
        ADD_VEC4_SERIALIZER( Color, osg::Vec4(1,1,1,1) );
        ADD_FLOAT_SERIALIZER( LineWidth, 1.0f );
        ADD_VEC2_SERIALIZER( MPPatternParams, osg::Vec2(0., 0.) );
    }
} } }


MPLineDrawable::MPLineDrawable() :
osg::Geometry(),
_mode(GL_LINE_STRIP),
_factor(1),
_pattern(0xFFFF),
_color(1, 1, 1, 1),
_width(1.0f),
_mpPatternParams(osg::Vec2(0., 0.)),
_bindColorOverall(false),
_current(NULL),
_side(NULL),
_length(NULL),
_colors(NULL)
{
    setupShaders();
    setStipplePattern(_pattern);
}

MPLineDrawable::MPLineDrawable(GLenum mode) :
osg::Geometry(),
_mode(mode),
_factor(1),
_pattern(0xFFFF),
_color(1,1,1,1),
_width(1.0f),
_mpPatternParams(osg::Vec2(0., 0.)),
_bindColorOverall(false),
_current(NULL),
_side(NULL),
_length(NULL),
_colors(NULL)
{
    setupShaders();
    setStipplePattern(_pattern);
}

MPLineDrawable::MPLineDrawable(const MPLineDrawable& rhs, const osg::CopyOp& copy) :
osg::Geometry(rhs, copy),
_mode(rhs._mode),
_color(rhs._color),
_bindColorOverall(rhs._bindColorOverall),
_factor(rhs._factor),
_pattern(rhs._pattern),
_width(rhs._width),
_mpPatternParams(rhs._mpPatternParams),
_current(NULL),
_side(NULL),
_length(NULL),
_colors(NULL)
{
    _current = static_cast<osg::Vec3Array*>(getVertexArray());
    _side = static_cast<osg::Vec3Array*>(getVertexAttribArray(SideVectorAttrLocation));
    _length = static_cast<osg::Vec2Array*>(getVertexAttribArray(LengthAttrLocation));

    setupShaders();
    setStipplePattern(_pattern);
}

MPLineDrawable::~MPLineDrawable()
{
    //nop
}

void
MPLineDrawable::initialize()
{
    // Already initialized?
    if (_current)
        return;

    // See if the arrays already exist:
    _current = static_cast<osg::Vec3Array*>(getVertexArray());
    _side = static_cast<osg::Vec3Array*>(getVertexAttribArray(SideVectorAttrLocation));
    _length = static_cast<osg::Vec2Array*>(getVertexAttribArray(LengthAttrLocation));

    setUseVertexBufferObjects(_supportsVertexBufferObjects);
    setUseDisplayList(false);

    if (!_current)
    {
        _current = new osg::Vec3Array();
        _current->setBinding(osg::Array::BIND_PER_VERTEX);
        setVertexArray(_current);

        _side = new osg::Vec3Array();
        _side->setBinding(osg::Array::BIND_PER_VERTEX);
        _side->setNormalize(false);
        setVertexAttribArray(SideVectorAttrLocation, _side);

        _length = new osg::Vec2Array();
        _length->setBinding(osg::Array::BIND_PER_VERTEX);
        _length->setNormalize(false);
        setVertexAttribArray(LengthAttrLocation, _length);

        _colors = new osg::Vec4Array();
        _colors->setBinding(_bindColorOverall ? osg::Array::BIND_OVERALL : osg::Array::BIND_PER_VERTEX);
        setColorArray(_colors);
    }
}

void
MPLineDrawable::setMode(GLenum mode)
{
    if (_mode != mode)
    {
        _mode = mode;
    }
}

void
MPLineDrawable::setBindColorOverall(bool bindColorOverall)
{
    _bindColorOverall = bindColorOverall;
}

void
MPLineDrawable::setLineWidth(float value)
{
    _width = value;
    // note that the line width is increased because of antialiasing
    getOrCreateStateSet()->addUniform(new osg::Uniform("mp_LineWidth", value+1.f), 1);
}

void
MPLineDrawable::setStipplePattern(GLushort pattern)
{
    _pattern = pattern;
    GLUtils::setLineStipple(getOrCreateStateSet(), _factor, _pattern, 1);
}

void
MPLineDrawable::setStippleFactor(GLint factor)
{
    _factor = factor;
    GLUtils::setLineStipple(getOrCreateStateSet(), _factor, _pattern, 1);
}

void
MPLineDrawable::setMPPatternParams(float alpha, float threshold)
{
    setMPPatternParams(osg::Vec2(alpha, threshold));
}

void
MPLineDrawable::setMPPatternParams(const osg::Vec2& params)
{
    _mpPatternParams = params;

    // define the uniforms only if necessary
    if (params.x() > 0.f)
        GLUtils::setLineMPPatternParams(getOrCreateStateSet(), _mpPatternParams,
                                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
}

void
MPLineDrawable::setColor(const osg::Vec4& color)
{
    if (_color != color)
    {
        initialize();

        _color = color;

        if (_colors && _current)
        {
            unsigned size = _bindColorOverall ? 1 : _current->size();
            _colors->assign(size, _color);
            _colors->dirty();
        }
    }
}

void
MPLineDrawable::setTransformationMatrices(const osg::Matrixd& world2local, const osg::Matrixd& local2world)
{
    _world2local = world2local;
    _local2world = local2world;

    // Compute the center of the earth
    // (It will be used to compute the normals for each vertex)
    _earthCenterLocal = osg::Vec3d(0., 0., 0.) * _world2local;
}

void
MPLineDrawable::importVertexArray(osg::Vec3Array* verts)
{
    initialize();
    _current->clear();
    _side->clear();
    _length->clear();
    _colors->clear();
    bool loopMode = _mode == GL_LINE_LOOP;

    if (verts && verts->size() > 1)
    {
        // Remove too closed points if required
        if (_minimumSegmentLength.isSet())
        {
            double minLength2 = (*_minimumSegmentLength) * (*_minimumSegmentLength);
            osg::Vec3 v = *verts->begin();
            for (osg::Vec3Array::iterator itr = verts->begin(); itr != verts->end(); )
            {
                if (itr != verts->begin() && (v - *itr).length2() < minLength2)
                {
                    itr = verts->erase(itr);
                }
                else
                {
                    v = *itr;
                    itr++;
                }
            }
        }

        // Pre allocate the arrays
        unsigned actualSize = loopMode ? (verts->size()+1u)*2u : verts->size()*2u;
        if (actualSize > _current->size())
        {
            ArrayList arrays;
            getArrayList(arrays);
            for (ArrayList::iterator i = arrays.begin(); i != arrays.end(); ++i)
                if (i->get()->getBinding() != osg::Array::BIND_OVERALL)
                    i->get()->reserveArray(actualSize);
        }

        // Initialize the previous direction
        osg::Vec3d prevDirection = loopMode ? (*verts)[0]-(*verts).back() : (*verts)[1]-(*verts)[0];
        prevDirection.normalize();

        // Add a fake point vertex at the end to compute the right last side vector orientation
        if (loopMode)
        {
            // Check that the loop is not already closed in the data
            if ((*verts).front() != (*verts).back())
                verts->push_back( (*verts).front() );
        }
        else
        {
            verts->push_back( (*verts).back()*2. - (*verts)[verts->size()-2] );
        }

        // The reference side vector length
        const double ref = 1.;

        // Length of the line attribute (x is the length, y is -1 or 1)
        double length = 0.;

        // Then push two vertices for each source point
        for (unsigned i = 0; i < verts->size()-1 ; ++i)
        {
            const osg::Vec3d& cur  = (*verts)[i];
            const osg::Vec3d& next = (*verts)[i+1];
            osg::Vec3d curDirection = next - cur;
            curDirection.normalize();
            osg::Vec3d tangent = prevDirection + curDirection;
            osg::Vec3d normal  = cur - _earthCenterLocal;
            tangent.normalize();
            normal.normalize();
            osg::Vec3d side    = tangent ^ normal;
            side.normalize();
            double mitter = prevDirection * tangent;
            mitter = mitter == 0. ? ref : ref / mitter;
            if (mitter > 2.*ref) mitter = 2.*ref;

            _current->push_back(cur);
            _current->push_back(cur);

            _side->push_back(-side*mitter);
            _side->push_back( side*mitter);

            _length->push_back( osg::Vec2(length, -1.) );
            _length->push_back( osg::Vec2(length,  1.) );

            length += (next-cur).length();
            prevDirection = curDirection;
        }

        // Close the loop if applicable
        if (loopMode)
        {
            _current->push_back((*_current)[0]);
            _current->push_back((*_current)[1]);

            _side->push_back((*_side)[0]);
            _side->push_back((*_side)[1]);

            _length->push_back( osg::Vec2(length, -1.) );
            _length->push_back( osg::Vec2(length,  1.) );
        }

        if (_bindColorOverall)
        {
            _colors->push_back(_color);
            _colors->dirty();
        }
        else
        {
            _colors->assign(_current->size(), _color);
        }
    }

    dirty();
}

void
MPLineDrawable::appendQuad(osg::DrawElements* els, unsigned iA, unsigned iB, unsigned iC, unsigned iD)
{
    // triangle 1
    els->addElement(iC);
    els->addElement(iB);
    els->addElement(iA);
    // triangle 2
    els->addElement(iC);
    els->addElement(iD);
    els->addElement(iB);
}

void
MPLineDrawable::dirty()
{
    initialize();
    dirtyBound();
    _current->dirty();
    _length->dirty();
    _side->dirty();

    // rebuild primitive sets.
    if (getNumPrimitiveSets() > 0)
    {
        removePrimitiveSet(0, getNumPrimitiveSets());
    }

    if (_current->size() >= 4)
    {
        unsigned numVertices = _current->size();
        unsigned numEls = ( numVertices - 2 ) * 3;
        osg::DrawElements* els = makeDE(numEls);

        // Build two triangles per segment
        for (auto e = 2u ; e < numVertices ; e += 2u)
            appendQuad(els, e-2, e-1, e, e+1);

        addPrimitiveSet(els);
    }
}

osg::observer_ptr<osg::StateSet> MPLineDrawable::s_gpuStateSet;

void
MPLineDrawable::setupShaders()
{
    // Create the singleton state set for the line shader. This stateset will be
    // shared by all MPLineDrawable instances so OSG will sort them together.
    if (!_gpuStateSet.valid())
    {
        if (s_gpuStateSet.lock(_gpuStateSet) == false)
        {
            // serialize access and double-check:
            static Threading::Mutex s_mutex;
            Threading::ScopedMutexLock lock(s_mutex);

            if (s_gpuStateSet.lock(_gpuStateSet) == false)
            {
                s_gpuStateSet = _gpuStateSet = new osg::StateSet();

                VirtualProgram* vp = VirtualProgram::getOrCreate(s_gpuStateSet.get());
                vp->setName("osgEarth::MPLineDrawable");
                Shaders shaders;
                shaders.load(vp, shaders.MPLineDrawable);
                vp->addBindAttribLocation("mp_MPLineDrawable_side", MPLineDrawable::SideVectorAttrLocation);
                vp->addBindAttribLocation("mp_MPLineDrawable_length", MPLineDrawable::LengthAttrLocation);
                // set of a default highlight color (can be customized by overriding the uniform in a parent node)
                s_gpuStateSet->getOrCreateUniform(UNIFORM_HIGHLIGHT_COLOR.c_str(), osg::Uniform::FLOAT_VEC4)->set(osg::Vec4f(75.f / 255.f, 150.f / 255.f, 1.f, 1.f));
                // create default mp pattern uniforms so it is not necessary to define them for each line
                GLUtils::setLineMPPatternParams(s_gpuStateSet.get(), osg::Vec2(0., 0.), 1);
                s_gpuStateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            }
        }
    }
}

void
MPLineDrawable::accept(osg::NodeVisitor& nv)
{
    if (nv.validNodeMask(*this))
    {
        // Only push the shader if necessary.
        // The reason for this approach is go we can inject the singleton
        // LineDrawable shader yet still allow the user to customize
        // the node's StateSet.
        bool shade = nv.getVisitorType() == nv.CULL_VISITOR && _gpuStateSet.valid();

        osgUtil::CullVisitor* cv = shade ? Culling::asCullVisitor(nv) : nullptr;

        nv.pushOntoNodePath(this);

        if (cv)
            cv->pushStateSet(_gpuStateSet.get());

        nv.apply(*this);

        if (cv)
            cv->popStateSet();

        nv.popFromNodePath();
    }
}

void
MPLineDrawable::resizeGLObjectBuffers(unsigned maxSize)
{
    osg::Geometry::resizeGLObjectBuffers(maxSize);
    if (_gpuStateSet.valid())
        _gpuStateSet->resizeGLObjectBuffers(maxSize);
}

void
MPLineDrawable::releaseGLObjects(osg::State* state) const
{
    osg::Geometry::releaseGLObjects(state);
    if (_gpuStateSet.valid())
        _gpuStateSet->releaseGLObjects(state);
}
