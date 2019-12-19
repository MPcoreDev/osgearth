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
#include <osgEarthFeatures/LabelSource>
#include <osgEarthFeatures/FeatureSourceIndexNode>
#include <osgEarthFeatures/GeometryUtils>
#include <osgEarthFeatures/FilterContext>
#include <osgEarthAnnotation/LabelNode>
#include <osgEarthAnnotation/PlaceNode>
#include <osgEarthAnnotation/AttachedLabelNode>
#include <osgEarth/DepthOffset>
#include <osgEarth/VirtualProgram>
#include <osgEarth/StateSetCache>
#include <osgDB/FileNameUtils>
#include <osgUtil/Optimizer>

#define LC "[AnnoLabelSource] "

using namespace osgEarth;
using namespace osgEarth::Annotation;
using namespace osgEarth::Features;

class AnnotationLabelSource : public LabelSource
{
public:
    AnnotationLabelSource( const LabelSourceOptions& options )
        : LabelSource( options )
    {
        //nop
    }

    /**
     * Creates a complete set of positioned label nodes from a feature list.
     */
    osg::Node* createNode(
        const FeatureList&   input,
        const Style&         style,
        FilterContext&       context )
    {
        if ( style.get<TextSymbol>() == 0L && style.get<IconSymbol>() == 0L )
            return 0L;

        // copy the style so we can (potentially) modify the text symbol.
        Style styleCopy = style;
        TextSymbol* text = styleCopy.get<TextSymbol>();
        IconSymbol* icon = styleCopy.get<IconSymbol>();
        AltitudeSymbol* alt = styleCopy.get<AltitudeSymbol>();

        osg::Group* group = new osg::Group();

        StringExpression  textContentExpr ( text ? *text->content()  : StringExpression() );
        NumericExpression textPriorityExpr( text ? *text->priority() : NumericExpression() );
        NumericExpression textSizeExpr    ( text ? *text->size()     : NumericExpression() );
        NumericExpression textRotationExpr( text ? *text->onScreenRotation() : NumericExpression() );
        NumericExpression textCourseExpr  ( text ? *text->geographicCourse() : NumericExpression() );
        StringExpression  textOffsetSupportExpr ( text ? *text->autoOffsetGeomWKT()  : StringExpression() );
        StringExpression  iconUrlExpr     ( icon ? *icon->url()      : StringExpression() );
        NumericExpression iconScaleExpr   ( icon ? *icon->scale()    : NumericExpression() );
        NumericExpression iconHeadingExpr ( icon ? *icon->heading()  : NumericExpression() );
        NumericExpression vertOffsetExpr  ( alt  ? *alt->verticalOffset() : NumericExpression() );

        for( FeatureList::const_iterator i = input.begin(); i != input.end(); ++i )
        {
            Feature* feature = i->get();
            if ( !feature )
                continue;

            // run a symbol script if present.
            if ( text && text->script().isSet() )
            {
                StringExpression temp( text->script().get() );
                feature->eval( temp, &context );
            }

            // run a symbol script if present.
            if ( icon && icon->script().isSet() )
            {
                StringExpression temp( icon->script().get() );
                feature->eval( temp, &context );
            }

            const Geometry* geom = feature->getGeometry();
            if ( !geom )
                continue;

            Style tempStyle = styleCopy;

            // evaluate expressions into literals.
            // TODO: Later we could replace this with a generate "expression evaluator" type
            // that we could pass to PlaceNode in the DB options. -gw

            if ( text )
            {
                if ( text->content().isSet() )
                    tempStyle.get<TextSymbol>()->content()->setLiteral( feature->eval( textContentExpr, &context ) );

                if ( text->size().isSet() )
                    tempStyle.get<TextSymbol>()->size()->setLiteral( feature->eval(textSizeExpr, &context) );

                if ( text->onScreenRotation().isSet() )
                    tempStyle.get<TextSymbol>()->onScreenRotation()->setLiteral( feature->eval(textRotationExpr, &context) );

                if ( text->geographicCourse().isSet() )
                    tempStyle.get<TextSymbol>()->geographicCourse()->setLiteral( feature->eval(textCourseExpr, &context) );

                if ( text->autoOffsetGeomWKT().isSet() )
                    tempStyle.get<TextSymbol>()->autoOffsetGeomWKT()->setLiteral( feature->eval( textOffsetSupportExpr, &context ) );
            }

            if ( icon )
            {
                if ( icon->url().isSet() )
                    tempStyle.get<IconSymbol>()->url()->setLiteral( feature->eval(iconUrlExpr, &context) );

                if ( icon->scale().isSet() )
                    tempStyle.get<IconSymbol>()->scale()->setLiteral( feature->eval(iconScaleExpr, &context) );

                if ( icon->heading().isSet() )
                    tempStyle.get<IconSymbol>()->heading()->setLiteral( feature->eval(iconHeadingExpr, &context) );
            }

            GeoPositionNode *node = nullptr;
            if ((text != nullptr) && (text->attachedLabel() == true))
            {
                node = makeAttachedLabelNode(context, feature, tempStyle, textPriorityExpr);
            }
            else
            {
                node = makePlaceNode(
                    context,
                    feature,
                    tempStyle,
                    textPriorityExpr,
                    text &&
                        (text->autoOffsetAlongLine() == true || text->autoRotateAlongLine() == true || text->autoOffsetGeomWKT().isSet()));
            }

            if ( node )
            {
                node->setHorizonCulling(false);
                node->setOcclusionCulling(false);

                if (!textPriorityExpr.empty())
                {
                    float val = feature->eval(textPriorityExpr, &context);
                    node->setPriority( val >= 0.0f ? val : FLT_MAX );
                }

                if (alt && alt->technique() == alt->TECHNIQUE_SCENE && !vertOffsetExpr.empty())
                {
                    float val = feature->eval(vertOffsetExpr, &context);
                    const osg::Vec3d& off = node->getLocalOffset();
                    node->setLocalOffset(osg::Vec3d(off.x(), off.y(), val));
                }

                if ( context.featureIndex() )
                {
                    context.featureIndex()->tagNode(node, feature);
                }

                group->addChild( node );
            }
        }

        return group;
    }

    AttachedLabelNode* makeAttachedLabelNode(FilterContext &context,
                                             Feature *feature,
                                             const Style &style,
                                             NumericExpression &priorityExpr)
    {
        auto center = feature->getGeometry()->getBounds().center();
        auto geoCenter = buildGeoPoint(center, style.getSymbol<AltitudeSymbol>(), feature->getSRS());
        auto node = new AttachedLabelNode{};
        node->updateGeometry(context, feature);
        node->setStyle(style);
        node->setPosition(geoCenter);
        return node;
    }

    PlaceNode* makePlaceNode(FilterContext&     context,
                             Feature*           feature,
                             const Style&       style,
                             NumericExpression& priorityExpr,
                             bool autoLineFollowing = false)
    {
        const osg::Vec3d center = feature->getGeometry()->getBounds().center();
        GeoPoint geoCenter = buildGeoPoint(center, style.getSymbol<AltitudeSymbol>(), feature->getSRS());
        PlaceNode* node = 0L;

        if ( autoLineFollowing )
        {
            const Geometry* geom = feature->getGeometry();
            if( style.getSymbol<TextSymbol>()->autoOffsetGeomWKT().isSet() )
            {
                StringExpression autoOffset = *(style.getSymbol<TextSymbol>()->autoOffsetGeomWKT());
                std::string lineSupport = feature->eval(autoOffset, &context);
                if (! lineSupport.empty() )
                    geom = osgEarth::Features::GeometryUtils::geometryFromWKT(lineSupport);
            }

            const LineString* geomLineString = 0L;
            if( geom && geom->getComponentType() == Geometry::TYPE_LINESTRING )
            {
                if( geom->getType() == Geometry::TYPE_LINESTRING)
                {
                    geomLineString = dynamic_cast<const LineString*>( geom );
                }
                else
                {
                    const MultiGeometry* geomMulti = dynamic_cast<const MultiGeometry*>(geom);
                    if( geomMulti )
                        geomLineString = dynamic_cast<const LineString*>( geomMulti->getComponents().front().get() );
                }
            }

            if( geomLineString )
            {
                GeoPoint geoStart = buildGeoPoint(geomLineString->front(), style.getSymbol<AltitudeSymbol>(), feature->getSRS(), true);
                GeoPoint geoEnd = buildGeoPoint(geomLineString->back(), style.getSymbol<AltitudeSymbol>(), feature->getSRS(), true);

                if( style.getSymbol<TextSymbol>()->autoOffsetGeomWKT().isSet() )
                {
                    node = new PlaceNode();
                    node->setStyle(style, context.getDBOptions());
                    node->setPosition(geoCenter);
                    // Direction to the longest distance
                    if( (geoStart.vec3d() - geoCenter.vec3d()).length2() > (geoEnd.vec3d() - geoCenter.vec3d()).length2() )
                    {
                        node->setLineCoords(geoEnd, geoStart);
                    }
                    else
                    {
                        node->setLineCoords(geoStart, geoEnd);
                    }
                }
                else
                {
                    node = new PlaceNode();
                    node->setStyle(style, context.getDBOptions());
                    node->setPosition(geoCenter);
                    node->setLineCoords(geoStart, geoEnd);
                }
            }
        }

        else
        {
            node = new PlaceNode();
            node->setStyle(style, context.getDBOptions());
            node->setPosition(geoCenter);
        }
        return node;
    }

    const GeoPoint buildGeoPoint( const osg::Vec3d& point, const AltitudeSymbol* altSym, const SpatialReference* srs, bool forceAbsolute = false )
    {
        AltitudeMode mode = ALTMODE_ABSOLUTE;

        if (! forceAbsolute && altSym &&
            (altSym->clamping() == AltitudeSymbol::CLAMP_TO_TERRAIN || altSym->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN) &&
            altSym->technique() == AltitudeSymbol::TECHNIQUE_SCENE)
        {
            mode = ALTMODE_RELATIVE;
        }

        return GeoPoint( srs, point.x(), point.y(), point.z(), mode );
    }
};

//------------------------------------------------------------------------

class AnnotationLabelSourceDriver : public LabelSourceDriver
{
public:
    AnnotationLabelSourceDriver()
    {
        supportsExtension( "osgearth_label_annotation", "osgEarth annotation label plugin" );
    }

    virtual const char* className() const
    {
        return "osgEarth Annotation Label Plugin";
    }

    virtual ReadResult readObject(const std::string& file_name, const Options* options) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        return new AnnotationLabelSource( getLabelSourceOptions(options) );
    }
};

REGISTER_OSGPLUGIN(osgearth_label_annotation, AnnotationLabelSourceDriver)
