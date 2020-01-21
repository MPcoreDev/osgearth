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

#include <osgEarthAnnotation/MPAnnotationGroup>
#include <osgEarthFeatures/LabelSource>
#include <osgEarthFeatures/FeatureSourceIndexNode>
#include <osgEarthFeatures/FilterContext>

#define LC "[MPAnnoLabelSource] "

using namespace osgEarth;
using namespace osgEarth::Annotation;
using namespace osgEarth::Features;

// -----------------------------------------------------------
// This class is mainly copied from AnnotationLabelSource
// but with performance improvements
// -----------------------------------------------------------

class MPAnnotationLabelSource : public LabelSource
{

public:

    MPAnnotationLabelSource( const LabelSourceOptions& options )
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
        if ( style.get<TextSymbol>() == nullptr && style.get<IconSymbol>() == nullptr )
            return nullptr;

        // copy the style so we can (potentially) modify the text symbol.
        Style styleCopy = style;
        TextSymbol* text = styleCopy.get<TextSymbol>();
        IconSymbol* icon = styleCopy.get<IconSymbol>();
        AltitudeSymbol* alt = styleCopy.get<AltitudeSymbol>();

        // attach point for all drawables
        MPAnnotationGroup* root = new MPAnnotationGroup();

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

                if ( text->priority().isSet() )
                    tempStyle.get<TextSymbol>()->priority()->setLiteral( feature->eval( textPriorityExpr, &context ) );

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

            // actually build the scenegraph related to this feature
            long id = addDrawablesForOneFeature(feature, tempStyle, context.getDBOptions(), root);

            // tag the drawables for that the feature can be retrieved when picking
            if ( context.featureIndex() )
            {
                std::vector<unsigned int> drawableList = root->getDrawableList(id);
                for (auto index : drawableList )
                    context.featureIndex()->tagDrawable(root->getChild(index)->asDrawable(), feature);
            }
        }

        // may be unnecessary
        root->dirtyBound();

        return root;
    }

    inline long addDrawablesForOneFeature(Feature* feature, const Style& style, const osgDB::Options* readOptions, MPAnnotationGroup* root)
    {
        const osg::Vec3d center = feature->getGeometry()->getBounds().center();
        GeoPoint pos( feature->getSRS(), center.x(), center.y(), center.z(), ALTMODE_ABSOLUTE );

        return root->addAnnotation(style, pos, readOptions);
    }

};


//------------------------------------------------------------------------

class MPAnnotationLabelSourceDriver : public LabelSourceDriver
{
public:
    MPAnnotationLabelSourceDriver()
    {
        supportsExtension( "osgearth_label_mp_screen_annotations", "mission+ annotation label plugin" );
    }

    virtual const char* className() const
    {
        return "mission+ annotation label plugin";
    }

    virtual ReadResult readObject(const std::string& file_name, const Options* options) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        return new MPAnnotationLabelSource( getLabelSourceOptions(options) );
    }
};

REGISTER_OSGPLUGIN(osgearth_label_mp_screen_annotations, MPAnnotationLabelSourceDriver)