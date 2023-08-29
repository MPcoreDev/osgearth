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

#include "osg/Viewport"
#include "osgEarth/Notify"
#include <algorithm>
#include <osgEarthAnnotation/MPAnnotationGroupSG>
#include <osgEarthAnnotation/MPAnnotationDrawable>
#include <osgEarthAnnotation/AnnotationUtils>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Lighting>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/GeoMath>
#include <osgEarth/GLUtils>
#include <osgEarthFeatures/GeometryUtils>
#include <osg/Depth>
#define LC "[MPAnnotationGroupSG] "

using namespace osgEarth;
using namespace osgEarth::Annotation;


osg::observer_ptr<MPStateSetFontAltas> MPAnnotationGroupSG::s_atlasStateSet;


namespace
{
    const osg::Node::NodeMask nodeNoMask = 0xffffffff;
    const float PIby2 = osg::PI / 2.f;
    const GeoPoint undefPoint;
    const double finePriorityRange = 1000000.0;
    
    bool inViewport(float vpXmin, float vpXmax, float vpYmin, float vpYmax, const osg::Vec3d& pt)
    {
        return pt.x() >= vpXmin && pt.x() < vpXmax && pt.y() >= vpYmin && pt.y() < vpYmax;
    }

    // Callback to properly cull the MPAnnotationGroup2
    class AnnotationNodeGroupCullCallbackSG : public osg::NodeCallback
    {
    public:
        AnnotationNodeGroupCullCallbackSG( bool backCull = false ) : _backCull(backCull) { };

        virtual ~AnnotationNodeGroupCullCallbackSG() { }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            osg::ref_ptr<osgUtil::CullVisitor> cullVisitor = nv->asCullVisitor();
            
            if ( ! cullVisitor->isCulled(node->getBound()) )
            {
                const osg::Matrix& MVPW = *(cullVisitor->getMVPW());
                float vpXmin = cullVisitor->getViewport()->x();
                float vpXmax = cullVisitor->getViewport()->x() + cullVisitor->getViewport()->width();
                float vpYmin = cullVisitor->getViewport()->y();
                float vpYmax = cullVisitor->getViewport()->y() + cullVisitor->getViewport()->height();
                double alt = DBL_MAX;
                cullVisitor->getCurrentCamera()->getUserValue("altitude", alt);
                float eyePointLength2 = _backCull ? cullVisitor->getEyePoint().length2() : 0.;

                osg::ref_ptr<MPAnnotationGroupSG> annoGroup = static_cast<MPAnnotationGroupSG*>(node);

                for (unsigned int i = 0 ; i < annoGroup->getNumChildren() ; ++i)
                {
                    osg::ref_ptr<MPAnnotationDrawable> annoDrawable = static_cast<MPAnnotationDrawable*>(annoGroup->getChild(i));
                    bool searchForOtherCandidates = false;

                    // visibility management
                    if ( ! annoDrawable->isVisible() )
                    {
                         annoDrawable->setNodeMask(0);
                         continue;
                    }

                    // transform to screen
                    annoDrawable->_cull_anchorOnScreen = annoDrawable->_anchorPoint * MVPW;
                    annoDrawable->_cull_bboxSymetricOnScreen.set(annoDrawable->_cull_anchorOnScreen + annoDrawable->getBBoxSymetric()._min,
                                                                 annoDrawable->_cull_anchorOnScreen + annoDrawable->getBBoxSymetric()._max);

                    // check opposite side of the globe
                    if ( _backCull && (cullVisitor->getEyePoint() - annoDrawable->_anchorPoint).length2() > eyePointLength2 )
                    {
                        if (annoDrawable->_anchorCandidates.empty())
                        {
                            annoDrawable->setNodeMask(0);
                            continue;
                        }
                        else
                        {
                            searchForOtherCandidates = true;
                        }
                    }

                    // check multiple candidates case
                    if (! annoDrawable->_anchorCandidates.empty())
                    {
                        searchForOtherCandidates |= ! inViewport(vpXmin, vpXmax, vpYmin, vpYmax, annoDrawable->_cull_anchorOnScreen);

                        if (searchForOtherCandidates)
                        {
                            osg::Vec3d newAnchorOnScreen;
                            auto bestCandidate = std::find_if(annoDrawable->_anchorCandidates.begin(), annoDrawable->_anchorCandidates.end(),
                                [&](const osg::Vec3d& candidate) {
                                    newAnchorOnScreen = candidate * MVPW;
                                    return inViewport(vpXmin, vpXmax, vpYmin, vpYmax, newAnchorOnScreen) &&
                                        (! _backCull || (cullVisitor->getEyePoint() - candidate).length2() < eyePointLength2);
                                });

                            if (bestCandidate != annoDrawable->_anchorCandidates.end())
                            {
                                annoDrawable->setAnchorPoint(*bestCandidate);
                                annoDrawable->_cull_anchorOnScreen = newAnchorOnScreen;
                                if (annoDrawable->_placementInsideCircle)
                                {
                                    auto v = static_cast<osg::Vec3Array*>(annoDrawable->getVertexAttribArray(MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_ANCHOR));
                                    (*v)[0].set(*bestCandidate);
                                    (*v).dirty();

                                    v = static_cast<osg::Vec3Array*>(annoDrawable->getVertexAttribArray(MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_CENTER));
                                    osg::Vec3d circleCenterOnScreen = (*v)[0] * MVPW;
                                    annoDrawable->setInverted(circleCenterOnScreen.y() > newAnchorOnScreen.y());
                                }
                            }
                            else
                            {
                                annoDrawable->setNodeMask(0);
                                continue;
                            }
                        }
                    }

                    // chek if it is out of viewport
                    else if ( ! annoDrawable->isAutoFollowLine() && ! annoDrawable->screenClamping() && ! annoDrawable->polygonVisible() )
                    {
                        // out of viewport
                        if ( osg::maximum(annoDrawable->_cull_bboxSymetricOnScreen.xMin(), vpXmin) > osg::minimum(annoDrawable->_cull_bboxSymetricOnScreen.xMax(), vpXmax) ||
                             osg::maximum(annoDrawable->_cull_bboxSymetricOnScreen.yMin(), vpYmin) > osg::minimum(annoDrawable->_cull_bboxSymetricOnScreen.yMax(), vpYmax) )
                        {
                            annoDrawable->setNodeMask(0);
                            continue;
                        }
                    }

                    // in viewport
                    annoDrawable->setAltitude( alt );
                    bool annoIsHidden = annoDrawable->isFullyHidden();
                    annoDrawable->setNodeMask( annoIsHidden ? 0 : nodeNoMask );

                    // compute the screen angle if necessary
                    if ( ! annoIsHidden && annoDrawable->isAutoRotate() )
                    {
                        osg::Vec3 anchorToProj = annoDrawable->_lineEnd * MVPW;
                        anchorToProj -= annoDrawable->_cull_anchorOnScreen;
                        annoDrawable->_cull_rotationRadOnScreen = atan2(anchorToProj.y(), anchorToProj.x());
                        annoDrawable->setInverted( annoDrawable->_cull_rotationRadOnScreen < - PIby2 || annoDrawable->_cull_rotationRadOnScreen > PIby2);
                        if ( annoDrawable->isInverted() )
                        {
                            if ( annoDrawable->_cull_rotationRadOnScreen > 0. )
                                annoDrawable->_cull_rotationRadOnScreen -= osg::PI;
                            else
                                annoDrawable->_cull_rotationRadOnScreen += osg::PI;
                        }
                    }
                }

                traverse(node, nv);
            }
        }

        bool _backCull{false};
    };
}



// Need a custom bounding sphere for the culling process of the whole tile
osg::BoundingSphere MPAnnotationGroupSG::computeBound () const
{
    osg::BoundingSphere bsphere;

    for(osg::NodeList::const_iterator itr = _children.begin(); itr!=_children.end(); ++itr)
    {
        if (itr->valid())
        {
            osg::ref_ptr<const MPAnnotationDrawable> annoDrawable = static_cast<MPAnnotationDrawable*>(itr->get());
            bsphere.expandBy(annoDrawable->getAnchorPoint());
            if (annoDrawable->isAutoFollowLine() || annoDrawable->screenClamping() || annoDrawable->polygonVisible())
            {
                bsphere.expandBy(annoDrawable->getLineStartPoint());
                bsphere.expandBy(annoDrawable->getLineEndPoint());
            }
            else if (! annoDrawable->_anchorCandidates.empty())
            {
                for (const auto& candidate : annoDrawable->_anchorCandidates)
                    bsphere.expandBy(candidate);
            }
        }
    }
    return bsphere;
}

MPAnnotationGroupSG::MPAnnotationGroupSG(const osgDB::Options *readOptions , TextSymbol *textSym) : MPAnnotationGroup()
{
    // build the shared state set
    if ( ! s_atlasStateSet.lock(_atlasStateSet) )
    {
        static Threading::Mutex s_mutex;
        Threading::ScopedMutexLock lock(s_mutex);

        if ( ! s_atlasStateSet.lock(_atlasStateSet) )
        {

            if ( ! MPScreenSpaceLayoutSG::isExtensionLoaded()
                          || ! MPScreenSpaceLayoutSG::getOptions().iconAltas().isSet() )
            {
                OE_WARN << LC << "Impossible to create StateSet because MPScreenSpaceLayoutSG is not well defined." << "\n";
                return;
            }

            std::string atlasIconPath = MPScreenSpaceLayoutSG::getOptions().iconAltas().get();
            s_atlasStateSet = _atlasStateSet = new MPStateSetFontAltas(atlasIconPath, readOptions);
            // draw in the screen-space bin
            MPScreenSpaceLayoutSG::activate(_atlasStateSet.get());
            // no depth test in screen space layout
            _atlasStateSet->setMode( GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE );
        }
    }

    setStateSet(_atlasStateSet.get());

    bool backCull = textSym && textSym->backEarthCull().isSetTo(true);
    addCullCallback(new AnnotationNodeGroupCullCallbackSG(  backCull ));

    // This group makes its own shaders
    ShaderGenerator::setIgnoreHint(this, true);
}

long MPAnnotationGroupSG::addAnnotation(const Style& style, Geometry *geom, const osgDB::Options* readOptions, unsigned long long instanceIndex, const std::string& text)
{
    if ( ! _atlasStateSet.valid() )
        return -1;

    // if it is inside circle placement, we will create as much annotation as geometrries
    const TextSymbol* ts = style.get<TextSymbol>();
    if ( ts->placementTechnique().isSetTo(TextSymbol::PlacementTechnique::INSIDE_CIRCLE) && geom
        && geom->getComponentType() == Symbology::Geometry::TYPE_POLYGON && geom->getType() == Symbology::Geometry::TYPE_MULTI)
    {
        std::string allTexts = ts->content()->eval();
        StringTokenizer splitter( ";" );
        splitter.keepEmpties() = true;
        StringVector subTexts;
        splitter.tokenize( allTexts, subTexts );
        MultiGeometry* geomMulti = dynamic_cast<MultiGeometry*>(geom);
        if ( subTexts.size() != geomMulti->getComponents().size() )
        {
            OE_WARN << LC << "Text definition does not provide enough subtexts\n";
            return -1;
        }
        else
        {
            long ret = 0;
            for (int i = 0 ; i < geomMulti->getComponents().size() ; ++i)
            {
                auto geomPolygon = geomMulti->getComponents().at(i);
                std::string curText = subTexts[i];
                ret = addAnnotation(style, geomPolygon, readOptions, instanceIndex, curText);
            }
            return ret;
        }
    }

    // unique id for this annotation
    static long id{0};
    long localId = ++id;

    // buid the single geometry which will gather all sub items and LODs
    MPAnnotationDrawable* annoDrawable = new MPAnnotationDrawable(style, readOptions, _atlasStateSet.get(), text);
    annoDrawable->setId(localId);
    annoDrawable->setInstanceIndex(instanceIndex);
    annoDrawable->setCullingActive(false);
    annoDrawable->setDataVariance(DataVariance::DYNAMIC);

    // compute the anchor point as the centroid of the geometry
    if ( ts->placementTechnique().isSetTo(TextSymbol::PlacementTechnique::INSIDE_CIRCLE) )
        annoDrawable->updateCircleGeometry( geom );
    else
        annoDrawable->updateGeometry( geom );
    GeoPoint pos = annoDrawable->getPosition();
    //osg::BoundingSphere bSphere(annoDrawable->getBound());

    // priority
    osg::ref_ptr<const InstanceSymbol> instance = style.get<InstanceSymbol>();
    const IconSymbol* iconSym = instance.valid() ? instance->asIcon() : nullptr;
    
    
 
    double priority = 0.0;
    if (ts && ts->priority().isSet())
    {
        priority = style.getSymbol<TextSymbol>()->priority()->eval();    
        annoDrawable->setPriority(priority);        
    } 
    
    if(ts && ts->priorityFine().isSet())
    {
        priority += ( style.getSymbol<TextSymbol>()->priorityFine()->eval() / finePriorityRange );
        annoDrawable->setPriority(priority);   
    }
    
    if ( (iconSym && iconSym->declutter().isSetTo(false)) || (ts && ts->declutter().isSetTo(false)))
    {
        annoDrawable->_declutterActivated = false;
        annoDrawable->_originalDeclutterActivated = false;
    }

    // orientation
    // technic is to create a at 2500m from the anchor with the given bearing
    // then the projection in screenspace of both points will be used to compute the screen-space angle
    if (ts && (ts->geographicCourse().isSet() || (ts->autoRotateAlongLine().isSetTo(true) && ! ts->autoOffsetGeomWKT().isSet() )) )
    {
        double labelRotationRad = DBL_MAX;
        if (ts->geographicCourse().isSet())
        {
            labelRotationRad = osg::DegreesToRadians ( ts->geographicCourse()->eval() );
        }
        else
        {
            // get end point
            osg::Vec3d geomEnd = geom->getType() != Geometry::TYPE_MULTI ? geom->back() : static_cast<MultiGeometry*>(geom)->getComponents().front()->back();
            labelRotationRad = GeoMath::bearing(osg::DegreesToRadians(pos.y()),
                                                osg::DegreesToRadians(pos.x()),
                                                osg::DegreesToRadians(geomEnd.y()),
                                                osg::DegreesToRadians(geomEnd.x()));
        }

        double latRad;
        double longRad;
        GeoMath::destination(osg::DegreesToRadians(pos.y()),
                             osg::DegreesToRadians(pos.x()),
                             labelRotationRad,
                             2500.,
                             latRad,
                             longRad);

        osgEarth::GeoPoint lineEndPoint;
        lineEndPoint.set(osgEarth::SpatialReference::get("wgs84"),
                         osg::RadiansToDegrees(longRad),
                         osg::RadiansToDegrees(latRad),
                         0,
                         osgEarth::ALTMODE_ABSOLUTE);

        osg::Vec3d p1;
        lineEndPoint.toWorld(p1);
        annoDrawable->setLineEndPoint(p1);
        annoDrawable->setAutoRotate(true);
    }

    // sliding label

    osg::ref_ptr<Geometry> geomSupport = nullptr;
    LineString* geomLineString = nullptr;

    if( ts && ts->autoOffsetAlongLine().get() )
    {
        if( ts->autoOffsetGeomWKT().isSet() )
        {
            std::string lineSupport = ts->autoOffsetGeomWKT()->eval();
            if (! lineSupport.empty() )
                geomSupport = osgEarth::Features::GeometryUtils::geometryFromWKT(lineSupport);
        }
        else
        {
            geomSupport = geom;
        }
    }

    if( ts && (ts->autoOffsetGeomWKT().isSet() || ts->autoOffsetAlongLine().get() || ts->autoRotateAlongLine().get())
            && geomSupport && geomSupport->getComponentType() == Geometry::TYPE_LINESTRING )
    {
        if( geomSupport->getType() == Geometry::TYPE_LINESTRING)
        {
            geomLineString = dynamic_cast<LineString*>( geomSupport.get() );
        }
        else if (const MultiGeometry* geomMulti = dynamic_cast<MultiGeometry*>( geomSupport.get() ))
        {
            geomLineString = dynamic_cast<LineString*>( geomMulti->getComponents().front().get() );
        }
    }

    if ( ts && geomLineString && (ts->autoOffsetAlongLine().get() || ts->autoRotateAlongLine().get()) )
    {
        osg::Vec3d p1, p2;

        if( geomLineString )
        {
            GeoPoint geoStart( osgEarth::SpatialReference::get("wgs84"), geomLineString->front().x(), geomLineString->front().y(),
                    geomLineString->front().z(), ALTMODE_ABSOLUTE );
            GeoPoint geoEnd( osgEarth::SpatialReference::get("wgs84"), geomLineString->back().x(), geomLineString->back().y(),
                    geomLineString->back().z(), ALTMODE_ABSOLUTE );

            if( ts->autoOffsetGeomWKT().isSet() )
            {
                // Direction to the longest distance
                const osg::Vec3d center = pos.vec3d();
                if( (geoStart.vec3d() - center).length2() > (geoEnd.vec3d() - center).length2() )
                {
                    geoEnd.toWorld(p1);
                    geoStart.toWorld(p2);
                }
                else
                {
                    geoStart.toWorld(p1);
                    geoEnd.toWorld(p2);
                }
            }
            else
            {
                geoEnd.toWorld(p1);
                geoStart.toWorld(p2);
            }
        }

        annoDrawable->setLineStartPoint(p1);
        annoDrawable->setLineEndPoint(p2);
        if( ts->autoOffsetGeomWKT().isSet() )
            annoDrawable->setAnchorPoint(annoDrawable->getLineStartPoint());

        annoDrawable->setAutoFollowLine( ts->autoOffsetAlongLine().get() );
        annoDrawable->setAutoRotate( ts->autoRotateAlongLine().get() );
    }
    
    // process grid mora polygon (label)
    if (ts && ts->placementTechnique().isSetTo(
                TextSymbol::PlacementTechnique::POLYGON_VISIBLE))
    {
        osg::Vec3d p1, p2,p3,p4;    //used as the bounding box corners
        geomSupport = geom;

        Polygon *geomPolygon = nullptr;

        if (geomSupport->getType() == Geometry::TYPE_POLYGON)
        {
            geomPolygon = dynamic_cast<Polygon *>(geomSupport.get());
        }
        else
        {
            const MultiGeometry *geomMulti =
                    dynamic_cast<MultiGeometry *>(geomSupport.get());
            if (geomMulti)
                geomPolygon =
                        dynamic_cast<Polygon *>(geomMulti->getComponents().front().get());
        }

        if (geomPolygon)
        {
            Bounds bounds = geomPolygon->getBounds();
            const osgEarth::SpatialReference *srs =
                    osgEarth::SpatialReference::create("wgs84");

            GeoPoint gp1(srs, bounds.xMin(), bounds.yMin(), 0.0, ALTMODE_ABSOLUTE);
            GeoPoint gp2(srs, bounds.xMax(), bounds.yMax(), 0.0, ALTMODE_ABSOLUTE);
            GeoPoint gp3(srs, bounds.xMin(), bounds.yMax(), 0.0, ALTMODE_ABSOLUTE);
            GeoPoint gp4(srs, bounds.xMax(), bounds.yMin(), 0.0, ALTMODE_ABSOLUTE);
            gp1.toWorld(p1);
            gp2.toWorld(p2);
            gp3.toWorld(p3);
            gp4.toWorld(p4);
        }
        else
        {
            OE_WARN << "no geomPolygon avail" << std::endl;
        }
        annoDrawable->setLineStartPoint(p1);
        annoDrawable->setLineEndPoint(p2);
        annoDrawable->setPolygonPoint1(p3);
        annoDrawable->setPolygonPoint2(p4);
        annoDrawable->setPolygonVisible(true);
        if(ts->textPolygonAltitude().isSet())
        {
            annoDrawable->setTextPolygonAltitude(ts->textPolygonAltitude().get());
        }
    }
    // label placement technique
    
    if( ts && ts->placementTechnique().isSetTo( TextSymbol::PlacementTechnique::SCREEN_EDGE_ONLY ))
    {
        osg::Vec3d p1, p2;
        geomSupport = geom;
        if( geomSupport->getType() == Geometry::TYPE_LINESTRING)
        {
            geomLineString = dynamic_cast<LineString*>( geomSupport.get() );
        }
        else
        {
            const MultiGeometry* geomMulti = dynamic_cast<MultiGeometry*>(geomSupport.get());
            if( geomMulti )
                geomLineString = dynamic_cast<LineString*>( geomMulti->getComponents().front().get() );
        }
        
        if( geomLineString )
        {
            GeoPoint geoStart( osgEarth::SpatialReference::get("wgs84"),
                               geomLineString->front().x(),
                               geomLineString->front().y(),
                               geomLineString->front().z(),
                               ALTMODE_ABSOLUTE );
            GeoPoint geoEnd( osgEarth::SpatialReference::get("wgs84"),
                             geomLineString->back().x(),
                             geomLineString->back().y(),
                             geomLineString->back().z(),
                             ALTMODE_ABSOLUTE );            
            
            geoEnd.toWorld(p1);
            geoStart.toWorld(p2);
        
        }
        else
        {
            OE_WARN << "no geomLineString avail" << std::endl;
        }
        
        annoDrawable->setLineStartPoint(p1);
        annoDrawable->setLineEndPoint(p2);
        annoDrawable->setScreenClamping(true);        
    }

    this->addChild( annoDrawable );
    _mainGeomDrawableList[localId] = annoDrawable;
    return localId;
}


void
MPAnnotationGroupSG::setHighlight( long id, bool highlight )
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        static_cast<MPAnnotationDrawable*>( itr->second.get() )->setHighlight(highlight);

}


void
MPAnnotationGroupSG::clearHighlight()
{
    for (osg::NodeList::iterator itr = _children.begin(); itr != _children.end(); ++itr)
        static_cast<MPAnnotationDrawable*>((*itr).get())->setHighlight(false);
}

void
MPAnnotationGroupSG::setIconColor(long id, const Color& color)
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        static_cast<MPAnnotationDrawable*>( itr->second.get() )->setIconColor(color);
}

void
MPAnnotationGroupSG::updateIcon( long id, const std::string &icon )
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        static_cast<MPAnnotationDrawable*>( itr->second.get() )->updateIcon(icon);
}

void
MPAnnotationGroupSG::setVisible( long id, bool visible )
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        static_cast<MPAnnotationDrawable*>( itr->second.get() )->setVisible( visible );
}

// remove one annotation
void
MPAnnotationGroupSG::removeAnnotation( long id )
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
    {
        removeChild( itr->second.get() );
        _mainGeomDrawableList.erase( itr );
    }

    dirtyBound();
}

// change the position of this annotation
void
MPAnnotationGroupSG::updateGeometry(long id, osgEarth::Symbology::Geometry *geom , double geographicCourse)
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        static_cast<MPAnnotationDrawable*>( itr->second.get() )->updateGeometry( geom, geographicCourse );

    dirtyBound();
}

// change the position of this annotation
void
MPAnnotationGroupSG::updateGeometry(long id, GeoPoint &pos , double geographicCourse)
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        static_cast<MPAnnotationDrawable*>( itr->second.get() )->updateGeometry( pos, geographicCourse );

    dirtyBound();
}

const GeoPoint&
MPAnnotationGroupSG::getPosition(long id) const
{
    MainGeomList::const_iterator itr = _mainGeomDrawableList.find( id );
    if ( itr != _mainGeomDrawableList.end() )
        return static_cast<MPAnnotationDrawable*>( itr->second.get() )->getPosition();

    return undefPoint;
}
