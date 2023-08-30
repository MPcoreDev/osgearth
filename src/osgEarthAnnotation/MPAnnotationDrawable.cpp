
#include "osg/Quat"
#include <cfloat>
#include <osgEarthAnnotation/MPAnnotationDrawable>
#include <osgEarthSymbology/InstanceSymbol>
#include <osgEarth/Registry>
#include <osgEarthAnnotation/AnnotationUtils>
#include <osgEarth/GeoMath>
#include <osgEarth/StringUtils>

#include <osgText/String>
#include <osgDB/FileNameUtils>
#include <osg/Math>



#define LC "[MPAnnotationDrawable] "

using namespace osgEarth::Annotation;
using namespace osgEarth::Symbology;
using namespace osgEarth;


namespace  {
    const std::string undef = "-32765";
    const Color magenta(1., 135./255., 195./255.);
    const Color moraHighColor("ffd700");
    const osg::BoundingBox bboxZero(0., 0., 0., 0., 0., 0.);
    const float rightMarginInnerRndBox(4.);
}


void ScreenSpaceDrawElements::draw(osg::State& state, bool useVertexBufferObjects) const
{
    // standard draw
    if (! _cluttered)
    {
        DrawElementsUByte::draw(state, useVertexBufferObjects);
    }

    // specific draw when culled. Draw only main icon.
    // source copied from DrawElementsUShort::draw()
    else
    {
        if (useVertexBufferObjects)
        {
            osg::GLBufferObject* ebo = getOrCreateGLBufferObject(state.getContextID());

            if (ebo)
            {
                state.getCurrentVertexArrayState()->bindElementBufferObject(ebo);
                if (_numInstances>=1) state.glDrawElementsInstanced(_mode, _drawSize, GL_UNSIGNED_BYTE, (const GLvoid *)(ebo->getOffset(getBufferIndex()) + _drawOffset), _numInstances);
                else glDrawElements(_mode, _drawSize, GL_UNSIGNED_BYTE, (const GLvoid *)(ebo->getOffset(getBufferIndex()) + _drawOffset) );
            }
            else
            {
                state.getCurrentVertexArrayState()->unbindElementBufferObject();
                if (_numInstances>=1) state.glDrawElementsInstanced(_mode, _drawSize, GL_UNSIGNED_BYTE, &front() + _drawOffset, _numInstances);
                else glDrawElements(_mode, _drawSize, GL_UNSIGNED_BYTE, &front() + _drawOffset);
            }
        }
        else
        {
            if (_numInstances>=1) state.glDrawElementsInstanced(_mode, _drawSize, GL_UNSIGNED_BYTE, &front() + _drawOffset, _numInstances);
            else glDrawElements(_mode, _drawSize, GL_UNSIGNED_BYTE, &front() + _drawOffset);
        }
    }
}


MPAnnotationDrawable::MPAnnotationDrawable(const Style &style, const osgDB::Options* readOptions, MPStateSetFontAltas* atlasStateSet, const std::string& text)
    : _readOptions(readOptions), _stateSetFontAltas(atlasStateSet)
{
    setUseVertexBufferObjects(true);
    setUseDisplayList(false);

    osg::ref_ptr<const InstanceSymbol> instance = style.get<InstanceSymbol>();
    const IconSymbol* iconSym = instance.valid() ? instance->asIcon() : nullptr;
    _context = iconSym && iconSym->url().isSet() ? iconSym->url()->uriContext() : URIContext();
    _scale = iconSym && iconSym->scale().isSet() ? iconSym->scale()->eval() : 1.;
    osg::ref_ptr<const BBoxSymbol> bboxSym = style.get<BBoxSymbol>();
    _bbox_margin = bboxSym && bboxSym->margin().isSet() ? bboxSym->margin().value() : 2.f;
    _icon_margin = iconSym && iconSym->margin().isSet() ? iconSym->margin().value() : 5.f;
    const TextSymbol* textSymbol = style.get<TextSymbol>();
    _mainFont = textSymbol && textSymbol->font().isSet() ? textSymbol->font().get()
                 : ( MPScreenSpaceLayoutSG::getOptions().defaultFont().isSet() ?
                         MPScreenSpaceLayoutSG::getOptions().defaultFont().get() : "" );
    _mainTextColor = textSymbol && textSymbol->fill().isSet() ? textSymbol->fill()->color() : Color::White;
    _mainFontSize = textSymbol && textSymbol->size().isSet() ? textSymbol->size()->eval() : 16.;
    _multi_text_margin = textSymbol && textSymbol->predefinedOrganisationMargin().isSet() ? textSymbol->predefinedOrganisationMargin().value() : 4.f;
    _altTextFirstLevel = textSymbol && textSymbol->minRange().isSet() ? textSymbol->minRange().get() : DBL_MAX;
    _altIconFirstLevel = iconSym && iconSym->minRange().isSet() ? iconSym->minRange().get() : DBL_MAX;
    _alt2ndLevel = textSymbol && textSymbol->minRange2ndlevel().isSet() ? textSymbol->minRange2ndlevel().value() : _altTextFirstLevel;


    if ( textSymbol && textSymbol->encoding().isSet() )
        _text_encoding = AnnotationUtils::convertTextSymbolEncoding(textSymbol->encoding().value());

    if ( textSymbol
                && ( textSymbol->placementTechnique().isSetTo( TextSymbol::PlacementTechnique::RIGHT_OR_LEFT_FROM_ICON )
                     || ( ! textSymbol->placementTechnique().isSet()
                          && MPScreenSpaceLayoutSG::getOptions().rightLeftPlacementActivated().isSetTo(true) ) )
                && ! textSymbol->autoOffsetAlongLine().isSetTo(true)
                && ! textSymbol->autoRotateAlongLine().isSetTo(true)
                && ! textSymbol->predefinedOrganisation().isSet()
                && iconSym )
        setRightLeftPlacementAvail(true);

    // actually build the vertices, primitives, colors...
    buildGeometry(style, text);

    // shift all vertices if pixel-offset is defined
    if ( textSymbol && textSymbol->pixelOffset().isSet() )
    {
        osg::Vec3 pixelShift(textSymbol->pixelOffset().get().x(), textSymbol->pixelOffset().get().y(), 0.);
        for ( unsigned int i = 0 ; i < _v->getNumElements() ; ++i )
            (*_v)[i] += pixelShift;
    }

//    // merge the LOD to make them easier to use
//    for ( unsigned int i = _LODlist.size()-1 ; i >= 1 ; i-- )
//    {
//        for ( unsigned int j = i-1 ; j >= 0 ; j-- )
//        {
//            _LODlist[i].
//        }
//    }
}

void MPAnnotationDrawable::buildGeometry(const osgEarth::Symbology::Style& style, const std::string& textOverride)
{
    _v = new osg::Vec3Array();
    _c = new osg::Vec4Array();
    _t = new osg::Vec2Array();
    _infoArray = new osg::Vec4Array();
    _infoArray->setPreserveDataType(true);
    _c2 = new osg::Vec4Array();
    _d = new ScreenSpaceDrawElements(GL_TRIANGLES);
    osg::BoundingBox mainBBoxText;
    osg::BoundingBox mainBBoxIcon;
    osg::ref_ptr<const InstanceSymbol> instance = style.get<InstanceSymbol>();
    const IconSymbol* iconSym = instance.valid() ? instance->asIcon() : nullptr;
    const TextSymbol* textSymbol = style.get<TextSymbol>();
    const BBoxSymbol* bboxsymbol = style.get<BBoxSymbol>();


    //-----------------------
    // Search for all icons

    StringVector iconList;
    if ( iconSym && iconSym->url().isSet() )
    {
        //if ( ! iconSym->alignment().isSet() || ! iconSym->alignment().isSetTo(IconSymbol::ALIGN_CENTER_CENTER) )
        //    OE_WARN << LC << "The icon alignment " << iconSym->alignment().value() << " is not supported yet.\n";

        std::string iconTxt = iconSym->url()->eval();
        Color iconColor = iconSym->color().getOrUse(Color::White);

        // check if there is a list of icons
        if ( iconTxt.find(";") != std::string::npos )
        {
            StringTokenizer splitter( ";" );
            splitter.keepEmpties() = true;
            splitter.tokenize( iconTxt, iconList );
            if (! iconList.empty() && ! iconList[0].empty() )
                iconTxt = iconList[0];
            else
                iconTxt = "";
        }

        // Build the main centered icon
        if (! iconTxt.empty() )
        {
            appendIcon(iconTxt, iconColor, _altIconFirstLevel);

            if ( _v->getNumElements() > 0)
            {
                // when decluttered the main icon is still drawn
                _drawInClutteredMode = true;
                if ( ! bboxsymbol )
                    _d->setNumEltOffset(0); // no bbox drawing / no offset needed

                _mainIconDrawIndex.insert(_mainIconDrawIndex.begin(), _d->begin(), _d->end());
                for ( unsigned int i = 0 ; i < _v->getNumElements() ; i++ )
                {
                    mainBBoxIcon.expandBy( _v->at(i) );
                    _mainIconVertices.push_back(i);
                }

                // manage the rotation of GEOM_BOX_ROUNDED_ORIENTED in case of pixel offset
                // note : the other types of bbox will not handle correctly the rotation in case of pixel offset
                if ( bboxsymbol && bboxsymbol->geom() == BBoxSymbol::GEOM_BOX_ROUNDED_ORIENTED && _v->size() >= 4 )
                {
                    float offset = textSymbol->pixelOffset().isSet() ? textSymbol->pixelOffset().get().x() : 0.;
                    ShiftInfo shiftData = buildShiftInfo( _v->size()-4, _v->size()-1, offset);
                    _rot_verticesToShift.push_back( shiftData );
                }
            }
        }
    }

    //-----------------------
    // Search for all texts

    StringVector textList;
    bool predefinedOrganisation = textSymbol && textSymbol->predefinedOrganisation().isSet();
    if ( textSymbol )
    {
        std::string mainText = textOverride.empty() ? textSymbol->content()->eval() : textOverride;
        StringTokenizer splitter( ";", "" );
        splitter.tokenize( mainText, textList );
        if (! textList.empty() )
            mainText = textList[0];

        // Build the main centered text
        if ( ! mainText.empty() && ! textSymbol->predefinedOrganisation().isSetTo("changeoverpoint")
             && ! textSymbol->predefinedOrganisation().isSetTo("mora"))
        {
            // add the separator if necessary
            if ( iconList.size() > 1 )
                mainText += " | ";
            else if ( ! textSymbol->predefinedOrganisation().isSetTo("airway")
                      && ! textSymbol->predefinedOrganisation().isSetTo("thresholdApproach")
                      && textList.size() >= 2 && textList[1].find("I:") != 0 )
                mainText += " | " + textList[1];

            int nbVert = appendText(mainText, _mainFont, _mainTextColor, _mainFontSize, _altTextFirstLevel, true);
            if (nbVert > 0)
            {
                TextSymbol::Alignment align = textSymbol->alignment().isSet() ?
                            textSymbol->alignment().get() : TextSymbol::Alignment::ALIGN_CENTER_CENTER;
                if ( mainBBoxIcon.valid() )
                    moveTextPosition(nbVert, mainBBoxIcon, align);
                else
                    moveTextPosition(nbVert, bboxZero, align);
                for ( int i = _v->getNumElements() - nbVert ; i >= 0 && i < static_cast<int>(_v->getNumElements()) ; ++i )
                    mainBBoxText.expandBy( (*_v)[i] );

                if ( bboxsymbol && bboxsymbol->geom() == BBoxSymbol::GEOM_BOX_ROUNDED_ORIENTED )
                {
                    float offset = textSymbol->pixelOffset().isSet() ? textSymbol->pixelOffset().get().x() : 0.;
                    ShiftInfo shiftData = buildShiftInfo( _v->size()-nbVert, _v->size()-1, offset);
                    _rot_verticesToShift.push_back( shiftData );
                }

                if ( _rightLeftPlacementAvail )
                    mvLeftShiftInfoForText(mainText, _v->size()-nbVert, _v->size()-1);
            }
        }
    }


    //-----------------------
    // Append the secondary icons to the right

    if ( iconList.size() > 1 && _v->size() > 0 )
    {
        float xMax = std::max( mainBBoxText.xMax(), mainBBoxIcon.xMax() );
        float newXMax = xMax;
        float deltaSize = 0.;
        for ( unsigned int i=1 ; i<iconList.size() ; ++i )
        {
            newXMax = appendIcon(iconList[i], Color::White, _altTextFirstLevel, xMax);
            deltaSize += (newXMax - xMax);
            xMax = newXMax;
            if ( _rightLeftPlacementAvail )
            {
                ShiftInfo shiftData = buildShiftInfo( _v->size()-4, _v->size()-1 );
                _mvLeft_verticesToShift.push_back( shiftData );
            }
        }

        // expand the text zone with these new icons (take into account only the last quad)
        for ( int i = _v->getNumElements() - 4 ; i >= 0 && i < static_cast<int>(_v->getNumElements()) ; ++i )
            if (mainBBoxText.valid())
                mainBBoxText.expandBy( (*_v)[i] );
            else
                mainBBoxIcon.expandBy( (*_v)[i] );

        // if text was expected to be center/center then shift it as required
        if ( textSymbol && textSymbol->alignment().isSetTo(TextSymbol::Alignment::ALIGN_CENTER_CENTER) )
        {
            deltaSize /= 2.;
            osg::Vec3 shift(deltaSize, 0., 0.);
            for (unsigned int i = 0 ; i < _v->size() ; i++)
                (*_v)[i].x() -= deltaSize;
            if (mainBBoxText.valid() )
            {
                mainBBoxText.xMin() -= deltaSize;
                mainBBoxText.xMax() -= deltaSize;
            }
            if (mainBBoxIcon.valid() )
            {
                mainBBoxIcon.xMin() -= deltaSize;
                mainBBoxIcon.xMax() -= deltaSize;
            }
        }
    }

    // build the other labels (to the right of existing icon/text)
    float reverseVideoXThreshold = 0.;
    if ( mainBBoxText.valid() && textSymbol && textList.size() > 1 && ! textSymbol->predefinedOrganisation().isSet() )
    {
        // other labels
        if ( iconList.size() >= 2 && textList.size() >= 2 && textList[1].find("I:") != 0)
        {
            int nbVert = appendText( textList[1], _mainFont, _mainTextColor, _mainFontSize, _altTextFirstLevel);
            if ( nbVert > 0 )
            {
                moveTextPosition( nbVert, mainBBoxText, TextSymbol::Alignment::ALIGN_LEFT_CENTER );
                // expand the text zone with this new label (take into account only the last quad)
                for ( int i = _v->getNumElements() - 4 ; i >= 0 && i < static_cast<int>(_v->getNumElements()) ; ++i )
                    mainBBoxText.expandBy( (*_v)[i] );

                if ( _rightLeftPlacementAvail )
                    mvLeftShiftInfoForText(textList[1], _v->size()-nbVert, _v->size()-1);
            }
        }

        // last label to the right on reverse video
        if ( textList[textList.size()-1].find("I:") == 0 )
        {
            // color must be inverted... but for now setting it to black is sufficient
            int nbVert = appendText( textList[textList.size()-1], _mainFont, Color::Black, _mainFontSize, _altTextFirstLevel);
            if ( nbVert > 0 )
            {
                moveTextPosition( nbVert, mainBBoxText, TextSymbol::Alignment::ALIGN_LEFT_CENTER );
                reverseVideoXThreshold = static_cast<int>(mainBBoxText.xMax() + _multi_text_margin/2.);
                // expand the text zone with this new label (take into account only the last quad)
                for ( int i = _v->getNumElements() - 4 ; i >= 0 && i < static_cast<int>(_v->getNumElements()) ; ++i )
                    mainBBoxText.expandBy( (*_v)[i] );

                if ( _rightLeftPlacementAvail )
                    mvLeftShiftInfoForText(textList[textList.size()-1], _v->size()-nbVert, _v->size()-1);
            }
        }
    }

    // ----------------------
    // Build BBox

    // The bounding box can enclose either the text, or the image, or both
    // the bbox symbol for change over point will be calculated after
    if ( bboxsymbol && (! predefinedOrganisation || ! textSymbol->predefinedOrganisation().isSetTo("changeoverpoint") ))
    {
        osg::Vec4 fillColor = bboxsymbol->fill().isSet() ? bboxsymbol->fill().get().color() : Color::Black;
        osg::Vec4 strokeColor = bboxsymbol->border().isSet() ? bboxsymbol->border().get().color() : Color::White;
        float borderThickness = bboxsymbol->border().isSet() && bboxsymbol->border().get().width().isSet() ? bboxsymbol->border().get().width().get() : 0.f;
        BBoxSymbol::BboxGroup groupType = bboxsymbol->group().isSet() ? bboxsymbol->group().get() :
                                                 ( mainBBoxIcon.valid() ? BBoxSymbol::GROUP_ICON_ONLY : BBoxSymbol::GROUP_TEXT_ONLY );
        BBoxSymbol::BboxGeom geomType = bboxsymbol->geom().isSet() ? bboxsymbol->geom().get() : BBoxSymbol::BboxGeom::GEOM_BOX;
        std::string dir = bboxsymbol->direction().isSet() ? bboxsymbol->direction()->eval() : "";
        bool oppose = false;
        if ( geomType == BBoxSymbol::BboxGeom::GEOM_BOX_ORIENTED_2WAYS && ! dir.empty() )
        {
            geomType = BBoxSymbol::BboxGeom::GEOM_BOX_ORIENTED;
            if (dir == "B")
                oppose = true;
        }

        if ( groupType == BBoxSymbol::GROUP_ICON_ONLY && mainBBoxIcon.valid() )
            appendBox(mainBBoxIcon, fillColor, strokeColor, geomType, oppose,
                      borderThickness, reverseVideoXThreshold, _bbox_margin);
        else if ( groupType == BBoxSymbol::GROUP_TEXT_ONLY && mainBBoxText.valid() )
            appendBox(mainBBoxText, fillColor, strokeColor, geomType, oppose,
                      borderThickness, reverseVideoXThreshold, _bbox_margin, _altTextFirstLevel);
        else if ( groupType == BBoxSymbol::GROUP_ICON_AND_TEXT && mainBBoxIcon.valid() && mainBBoxText.valid() )
        {
            osg::BoundingBox groupBbox( mainBBoxIcon );
            // expand the icon bbox to scale it like the text
            float txtHeight = mainBBoxText.yMax() - mainBBoxText.yMin();
            float iconHeight = groupBbox.yMax() - groupBbox.yMin();
            float correction = txtHeight - iconHeight;
            if ( correction > 0. )
            {
                correction = correction / 2.;
                groupBbox.xMin() -= correction;
            }
            else
            {
                correction = 0.;
            }
            if ( geomType == BBoxSymbol::BboxGeom::GEOM_BOX_ROUNDED_INNER )
                correction -= rightMarginInnerRndBox;
            groupBbox.expandBy( mainBBoxText );
            appendBox(groupBbox, fillColor, strokeColor, geomType, oppose, borderThickness, reverseVideoXThreshold, _bbox_margin);
            // add the xShift to do in the right LOD
            _LODlist[_LODlist.size()-1].shiftVec.push_back( ShiftInfo({_v->getNumElements()-1, _v->getNumElements()-2},
                                         osg::Vec3(mainBBoxText.xMax() - mainBBoxIcon.xMax() - correction, 0., 0.)) );
        }
        else if (! mainBBoxIcon.valid() && ! mainBBoxText.valid() )
        {
            appendBox(bboxZero, fillColor, strokeColor, geomType, oppose,
                      borderThickness, reverseVideoXThreshold, _bbox_margin);
        }
    }


    // ----------------------
    // Append the secondary texts in predefined organisation

    if ( textSymbol && textSymbol->predefinedOrganisation().isSetTo("airway") && textList.size() == 7 )
    {
        osg::BoundingBox refBbox( mainBBoxIcon );
        refBbox.expandBy( mainBBoxText );
        const osg::Vec3 marginVec(_multi_text_margin, _multi_text_margin, 0.);
        refBbox.set( refBbox._min - marginVec, refBbox._max + marginVec);

        TextSymbol::Alignment alignList[] = { TextSymbol::ALIGN_LEFT_BOTTOM, TextSymbol::ALIGN_LEFT_BOTTOM_BASE_LINE, TextSymbol::ALIGN_LEFT_TOP,
                                             TextSymbol::ALIGN_RIGHT_TOP, TextSymbol::ALIGN_RIGHT_BOTTOM_BASE_LINE, TextSymbol::ALIGN_RIGHT_BOTTOM };
        float fontSizeSmaller = _mainFontSize / 18.f * 15.f;
        float fontSizeList[] = {fontSizeSmaller, fontSizeSmaller, fontSizeSmaller, fontSizeSmaller, fontSizeSmaller, fontSizeSmaller};
        Color colorList[] = {_mainTextColor, _mainTextColor, _mainTextColor, magenta, _mainTextColor, _mainTextColor};
        // make the font regular for the secondary texts
        std::string subFont(_mainFont);
        replaceIn(subFont, "Bold", "Regular");

        // for each text (except the first one)
        for ( unsigned int i=1 ; i<textList.size() ; ++i )
        {
            int nbVert = appendText(textList[i], subFont, colorList[i-1], fontSizeList[i-1], _alt2ndLevel);
            if (nbVert > 0)
            {
                moveTextPosition(nbVert, refBbox, alignList[i-1]);
                // rotation management, shift in X
                if ( i == 2 || i == 5 ) // course out & course in
                {
                    float xMin = FLT_MAX;
                    float xMax = -FLT_MAX;
                    ShiftInfo shiftData;
                    for (unsigned int j = _v->size() - nbVert ; j < _v->size() ; j++)
                    {
                        float x = (*_v)[j].x();
                        if ( x < xMin )
                            xMin = x;
                        if ( x > xMax )
                            xMax = x;
                        shiftData.first.push_back(j);
                    }
                    shiftData.second = osg::Vec3(xMax + xMin, 0., 0.);
                    _rot_verticesToShift.push_back( shiftData );
                }
                // Box for RNP
                else if ( i == 1 )
                {
                    osg::BoundingBox box;
                    for (unsigned int j = _v->size() - nbVert ; j < _v->size() ; j++)
                        box.expandBy( (*_v)[j] );
                    appendBox( box, Color::Black, Color::White, BBoxSymbol::GEOM_BOX_NO_PICK, false, 1.f, 0.f, 3.f, _alt2ndLevel);
                }
            }
        }
    }

    // build change over point pattern
    else if ( textSymbol && textSymbol->predefinedOrganisation().isSetTo("changeoverpoint") && textList.size() == 2
              && ! textList[0].empty() && textList[0] != undef
              && ! textList[1].empty() && textList[1] != undef
              && bboxsymbol )
    {
        const osg::Vec3 marginVec(_multi_text_margin, _multi_text_margin, 0.);
        const osg::BoundingBox refBBox( -marginVec, marginVec );

        // right top text
        int nbVert = appendText(textList[0], _mainFont, _mainTextColor, _mainFontSize);
        osg::BoundingBox trBbox;
        if (nbVert > 0)
        {
            moveTextPosition(nbVert, refBBox, TextSymbol::ALIGN_LEFT_BOTTOM);
            ShiftInfo shiftData;
            for (unsigned int i = _v->size() - nbVert ; i < _v->size() ; i++)
            {
                shiftData.first.push_back(i);
                trBbox.expandBy( (*_v)[i] );
            }
            shiftData.second = trBbox._min + trBbox._max;
            _rot_verticesToShift.push_back( shiftData );
        }

        // left bottom text
        int nbVert2 = appendText(textList[1], _mainFont, _mainTextColor, _mainFontSize);
        osg::BoundingBox blBbox;
        if (nbVert2 > 0)
        {
            moveTextPosition(nbVert2, refBBox, TextSymbol::ALIGN_RIGHT_TOP);
            ShiftInfo shiftData;
            for (unsigned int i = _v->size() - nbVert2 ; i < _v->size() ; i++)
            {
                shiftData.first.push_back(i);
                blBbox.expandBy( (*_v)[i] );
            }
            shiftData.second = blBbox._min + blBbox._max;
            _rot_verticesToShift.push_back( shiftData );
        }

        // the "stair" symbol
        osg::Vec4 strokeColor = bboxsymbol->border().isSet() ? bboxsymbol->border().get().color() : Color::White;
        float borderThickness = bboxsymbol->border().isSet() && bboxsymbol->border().get().width().isSet() ? bboxsymbol->border().get().width().get() : 2.f;
        BBoxSymbol::BboxGeom geomType = bboxsymbol->geom().isSet() ? bboxsymbol->geom().get() : BBoxSymbol::BboxGeom::GEOM_STAIR;
        osg::BoundingBox box(trBbox);
        box.expandBy(blBbox);
        appendBox(box, Color::Gray, strokeColor, geomType, false, borderThickness, 0.f, _bbox_margin);
    }

    // build mora pattern
    else if ( textSymbol && textSymbol->predefinedOrganisation().isSetTo("mora") && textList.size() == 2
              && ! textList[0].empty() && textList[0] != undef
              && ! textList[1].empty() && textList[1] != undef )
    {
        int moraVal = std::stoi(textList[0]);
        osg::BoundingBox refBBox( -_multi_text_margin, -_multi_text_margin, 0., _multi_text_margin, _multi_text_margin, 0.);
        Color color = textSymbol->fill().isSet() ? textSymbol->fill().get().color() : magenta;
        if ( moraVal >= 10 )
            color = moraHighColor;

        // main label
        int nbVert = appendText(textList[0], _mainFont, color, _mainFontSize);
        if ( nbVert > 0 )
        {
            TextSymbol::Alignment align = textSymbol->alignment().isSet() ? textSymbol->alignment().get() : TextSymbol::ALIGN_RIGHT_CENTER;
            moveTextPosition(nbVert, refBBox, align);
        }

        // second label
        nbVert = appendText(textList[1], _mainFont, color, _mainFontSize * 0.7);
        if ( nbVert > 0 )
        {
            moveTextPosition(nbVert, refBBox, TextSymbol::ALIGN_LEFT_BOTTOM);
        }
    }

    // build threshold and approach
    else if ( textSymbol && textSymbol->predefinedOrganisation().isSetTo("thresholdApproach") && textList.size() == 2 )
    {
        osg::BoundingBox refBbox;
        for (unsigned int i = 0 ; i < _v->size() ; ++i)
            refBbox.expandBy( (*_v)[i] );
        const osg::Vec3 marginVec(_multi_text_margin+_bbox_margin, _multi_text_margin+_bbox_margin, 0.);
        refBbox.set( refBbox._min - marginVec, refBbox._max + marginVec);

        int nbVert = appendText( textList[1], _mainFont, _mainTextColor, _mainFontSize, _alt2ndLevel );
        if (nbVert > 0)
        {
            moveTextPosition(nbVert, refBbox, TextSymbol::ALIGN_RIGHT_CENTER);
            osg::Vec4 fillColor = bboxsymbol->fill().isSet() ? bboxsymbol->fill().get().color() : Color::Black;
            float borderThickness = bboxsymbol->border().isSet() && bboxsymbol->border().get().width().isSet() ? bboxsymbol->border().get().width().get() : 0.f;
            osg::BoundingBox box;
            for (unsigned int j = _v->size() - nbVert ; j < _v->size() ; j++)
                box.expandBy( (*_v)[j] );
            appendBox( box, fillColor, Color::White, BBoxSymbol::GEOM_BOX, false, 0.f, 0.f, _bbox_margin-borderThickness, _alt2ndLevel );

            float xMin = FLT_MAX;
            float xMax = -FLT_MAX;
            ShiftInfo shiftData;
            for (unsigned int j = _v->size() - nbVert-4 ; j < _v->size() ; j++)
            {
                float x = (*_v)[j].x();
                if ( x < xMin )
                    xMin = x;
                if ( x > xMax )
                    xMax = x;
                shiftData.first.push_back(j);
            }
            shiftData.second = osg::Vec3(xMax + xMin, 0., 0.);
            _rot_verticesToShift.push_back( shiftData );
            // approach labels are displayed by defaut and are hidden when zoomed in
            _invertLOD = true;
        }
    }

    setVertexArray( _v.get() );
    setTexCoordArray( 0u, _t.get(), osg::Array::BIND_PER_VERTEX );
    setColorArray( _c.get(), osg::Array::BIND_PER_VERTEX );
    setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_INFO, _infoArray.get(), osg::Array::BIND_PER_VERTEX );
    setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_COLOR2, _c2.get(), osg::Array::BIND_PER_VERTEX );

    if (! _placementInsideCircle)
    {
        _circleCenter = new osg::Vec3Array(osg::Array::BIND_OVERALL, 1);
        (*_circleCenter)[0].set(FLT_MAX, FLT_MAX, FLT_MAX);
        setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_CENTER, _circleCenter.get(), osg::Array::BIND_OVERALL );
    }

    addPrimitiveSet( _d.get() );

    if ( getVertexArray()->getVertexBufferObject() )
        getVertexArray()->getVertexBufferObject()->setUsage(GL_STATIC_DRAW_ARB);

    if ( _invertLOD )
    {
        _camAlt = DBL_MAX;
        _LOD = 0;
    }
    else
    {
        _camAlt = 0.;
        _LOD = _LODlist.size()-1;
    }
    const osg::BoundingSphere &bSphere = getBound();
    osg::Vec3 radius(bSphere.radius(), bSphere.radius(), 0.);
    _bboxSymetric.set( bSphere.center() - radius, bSphere.center() + radius);
}

float MPAnnotationDrawable::appendIcon(const std::string& urlPath, const osg::Vec4 &color, double alt, float xMax)
{
    if ( ! _stateSetFontAltas.valid() )
        return 0;

    std::string iconKey = osgDB::getNameLessAllExtensions(osgDB::getSimpleFileName(urlPath));

    MPStateSetFontAltas::IconMap::const_iterator itIcon = _stateSetFontAltas->mapIcons.find(iconKey);
    if ( itIcon == _stateSetFontAltas->mapIcons.end() )
    {
        OE_WARN << LC << "The icon atlas does not contain the image " << iconKey << "\n";
        return 0.;
    }

    const IconInfo& iconInfo = itIcon->second;

    // compute the x translation if necessary
    osg::Vec3 rightShift ( 0.f, 0.f, 0.f );
    if ( xMax != FLT_MAX )
        rightShift.x() += xMax + iconInfo.size.x()/ 2.f + _icon_margin;

    // push vertices
    _v->push_back( iconInfo.lb_v + rightShift );
    _v->push_back( iconInfo.lt_v + rightShift );
    _v->push_back( iconInfo.rt_v + rightShift );
    _v->push_back( iconInfo.rb_v + rightShift );

    // push colors
    _c->push_back( color );
    _c->push_back( color );
    _c->push_back( color );
    _c->push_back( color );
    _c2->push_back( color );
    _c2->push_back( color );
    _c2->push_back( color );
    _c2->push_back( color );

    // push texture coords
    _t->push_back( iconInfo.lb_t );
    _t->push_back( iconInfo.lt_t );
    _t->push_back( iconInfo.rt_t );
    _t->push_back( iconInfo.rb_t );

    // push the draw type
    _infoArray->push_back( osg::Vec4(iconInfo.size.x(), iconInfo.size.y(), MPStateSetFontAltas::TYPE_ICON, 0.f) );
    _infoArray->push_back( osg::Vec4(iconInfo.size.x(), iconInfo.size.y(), MPStateSetFontAltas::TYPE_ICON, 0.f) );
    _infoArray->push_back( osg::Vec4(iconInfo.size.x(), iconInfo.size.y(), MPStateSetFontAltas::TYPE_ICON, 0.f) );
    _infoArray->push_back( osg::Vec4(iconInfo.size.x(), iconInfo.size.y(), MPStateSetFontAltas::TYPE_ICON, 0.f) );

    // push the draw elements
    unsigned int last = _v->getNumElements() - 1;
    std::vector<GLubyte> drawIndices { GLubyte(last-3), GLubyte(last-2), GLubyte(last-1), GLubyte(last-3), GLubyte(last-1), GLubyte(last)};
    pushDrawElements(alt, drawIndices);

    return iconInfo.rt_v.x() + rightShift.x();
}

int MPAnnotationDrawable::appendBox(const osg::BoundingBox& bbox, const osg::Vec4 &fillColor, const osg::Vec4 &strokeColor,
                                     BBoxSymbol::BboxGeom geomType, bool oppose, float strokeWidth, float reverseVideoX,
                                    float margin, double alt)
{
    float height = bbox.yMax() - bbox.yMin() + 2. * margin;
    float xMaxMargin = margin;
    float xMinMargin = margin;

    MPStateSetFontAltas::DrawType drawType = MPStateSetFontAltas::TYPE_BBOX;
    switch (geomType)
    {
        case BBoxSymbol::GEOM_BOX_ROUNDED :
            xMaxMargin += height/2.;
            xMinMargin = xMaxMargin;
            drawType = MPStateSetFontAltas::TYPE_BBOX_ROUNDED;
            break;
        case BBoxSymbol::GEOM_BOX_ROUNDED_INNER :
            xMaxMargin += rightMarginInnerRndBox;
            drawType = MPStateSetFontAltas::TYPE_BBOX_ROUNDED;
            break;
        case BBoxSymbol::GEOM_BOX_ORIENTED :
        case BBoxSymbol::GEOM_BOX_ORIENTED_SYM :
            drawType = MPStateSetFontAltas::TYPE_BBOX_ONEARROW;
            xMaxMargin += height / 3.;
            break;
        case BBoxSymbol::GEOM_BOX_ORIENTED_2WAYS:
            drawType = MPStateSetFontAltas::TYPE_BBOX_TWOARROWS;
            xMaxMargin += height / 3.;
            xMinMargin = xMaxMargin;
            break;
        case BBoxSymbol::GEOM_STAIR:
            drawType = MPStateSetFontAltas::TYPE_BBOX_STAIR;
            break;
        case BBoxSymbol::GEOM_BOX_ROUNDED_ORIENTED:
            drawType = MPStateSetFontAltas::TYPE_BBOX_ROUNDED_ORIENTED;
            xMaxMargin += height / 3.;
            break;
        case BBoxSymbol::GEOM_BOX_STROKE_SIDES:
            drawType = MPStateSetFontAltas::TYPE_BBOX_STROKE_SIDED;
            break;
        case BBoxSymbol::GEOM_BOX_NO_PICK:
            drawType = MPStateSetFontAltas::TYPE_BBOX_NO_PICK;
            break;
        default: ;
    }

    // push vertices
    if ( ! oppose )
    {
        _v->push_back( osg::Vec3(bbox.xMin()-xMinMargin, bbox.yMin()-margin, 0 ) );
        _v->push_back( osg::Vec3(bbox.xMin()-xMinMargin, bbox.yMax()+margin, 0 ) );
        _v->push_back( osg::Vec3(bbox.xMax()+xMaxMargin, bbox.yMax()+margin, 0 ) );
        _v->push_back( osg::Vec3(bbox.xMax()+xMaxMargin, bbox.yMin()-margin, 0 ) );
    }
    else
    {
        _v->push_back( - osg::Vec3(bbox.xMin()-xMinMargin, bbox.yMin()-margin, 0 ) );
        _v->push_back( - osg::Vec3(bbox.xMin()-xMinMargin, bbox.yMax()+margin, 0 ) );
        _v->push_back( - osg::Vec3(bbox.xMax()+xMaxMargin, bbox.yMax()+margin, 0 ) );
        _v->push_back( - osg::Vec3(bbox.xMax()+xMaxMargin, bbox.yMin()-margin, 0 ) );
    }

    // push colors
    _c->push_back( fillColor );
    _c->push_back( fillColor );
    _c->push_back( fillColor );
    _c->push_back( fillColor );
    _c2->push_back( strokeColor );
    _c2->push_back( strokeColor );
    _c2->push_back( strokeColor );
    _c2->push_back( strokeColor );

    // push texture coords, they will be used by the shader for computing the internal shape
    _t->push_back( osg::Vec2(0., 0.) );
    _t->push_back( osg::Vec2(0., 1.) );
    _t->push_back( osg::Vec2(1., 1.) );
    _t->push_back( osg::Vec2(1., 0.) );

    // push the draw info
    float boxAdditionalInfo = strokeWidth / 10.;
    if ( reverseVideoX > 0. )
        boxAdditionalInfo += reverseVideoX - (bbox.xMin()-xMinMargin);
    float width = bbox.xMax() - bbox.xMin() + xMaxMargin + xMinMargin;
    _infoArray->push_back( osg::Vec4(width, height, drawType, boxAdditionalInfo) );
    _infoArray->push_back( osg::Vec4(width, height, drawType, boxAdditionalInfo) );
    _infoArray->push_back( osg::Vec4(width, height, drawType, boxAdditionalInfo) );
    _infoArray->push_back( osg::Vec4(width, height, drawType, boxAdditionalInfo) );

    // management of the rotation
    unsigned int last = _v->getNumElements() - 1;
    if ( drawType == MPStateSetFontAltas::TYPE_BBOX_ONEARROW || drawType == MPStateSetFontAltas::TYPE_BBOX_ROUNDED_ORIENTED )
    {
        _rot_verticesToInvert.push_back(last);
        _rot_verticesToInvert.push_back(last-1);
        _rot_verticesToInvert.push_back(last-2);
        _rot_verticesToInvert.push_back(last-3);
    }

    // management of bbox shift in case left label placement
    else if ( _rightLeftPlacementAvail )
    {
        _mvLeft_verticesToInvert.push_back(last);
        _mvLeft_verticesToInvert.push_back(last-1);
        _mvLeft_verticesToInvert.push_back(last-2);
        _mvLeft_verticesToInvert.push_back(last-3);
    }

    // push the draw elements
    std::vector<GLubyte> drawIndices { GLubyte(last-3), GLubyte(last-2), GLubyte(last-1), GLubyte(last-3), GLubyte(last-1), GLubyte(last)};
    return pushDrawElements(alt, drawIndices);
}

int MPAnnotationDrawable::appendText(const std::string& text, const std::string& font, const osg::Vec4& color, double fontSize, double alt, bool pushDrawAfter)
{
    if ( ! _stateSetFontAltas.valid() )
        return 0;

    // filter the text according to known rules
    if ( text.empty() || text == undef)
        return 0;

    std::string fontKey = osgDB::getNameLessAllExtensions(osgDB::getSimpleFileName(font));

    std::string tmpText(text);
    if ( text.find(":") == 1 )
    {
        // F: means that the value must be display with format FLxxx
        // D: means that the value must be display with format xxx°
        // R: means that the value must be display with format Rxxx
        StringVector textSplit;
        StringTokenizer splitter( ":", "" );
        splitter.tokenize( text, textSplit );

        if (textSplit.size() == 1)
            return 0;

        if (textSplit.size() == 2)
        {
            if (textSplit[1].empty() || textSplit[1] == undef)
                return 0;

            // convert to int
            int val;
            std::istringstream(textSplit[1]) >> val;

            // add the FL symbol if required
            if ( textSplit[0] == "F" )
            {
                tmpText = "FL" + std::to_string(val/100);
            }
            // add the R symbol if required
            else if ( textSplit[0] == "R" )
            {
                tmpText = "R" + std::to_string(val);
            }
            // add the ° symbol if required and make sure it has always three digits
            else if ( textSplit[0] == "D" )
            {
                if ( val < 10 ) tmpText = "00" + textSplit[1] + "°";
                else if ( val < 100 ) tmpText = "0" + textSplit[1] + "°";
                else tmpText = textSplit[1] + "°";
            }
            else
            {
                tmpText = textSplit[1];
            }
        }
    }

    // append each character one by one
    osg::Vec3 advance(0., 0., 0.);
    std::vector<unsigned int> drawIndices;
    int nbVertices = 0;
    float yMin = FLT_MAX;
    float yMax = -FLT_MAX;
    osgText::String textFormated(tmpText, _text_encoding);
    MPStateSetFontAltas::GlyphMap::const_iterator itGlyph;
    for ( osgText::String::const_iterator itrChar=textFormated.begin() ; itrChar!=textFormated.end() ; ++itrChar )
    {
        unsigned int charcode = *itrChar;

        // utf-8 non used character
        if ( charcode == 195 )
        {
            continue;
        }

        // case 'space' character then shift the cursor (use 'A' character as size reference)
        if ( charcode == 32 )
        {
            itGlyph = _stateSetFontAltas->mapGlyphs.find(fontKey + "/65");
            if ( itGlyph == _stateSetFontAltas->mapGlyphs.end() )
            {
                OE_WARN << LC << "The font atlas does not contain the character code 65\n";
                continue;
            }

            const GlyphInfo& glyphInfo = itGlyph->second;
            double scale = fontSize / _stateSetFontAltas->refFontSize;
            advance.x() += glyphInfo.advance * scale;
            continue;
        }

        itGlyph = _stateSetFontAltas->mapGlyphs.find(fontKey + "/" + std::to_string(charcode));
        if ( itGlyph == _stateSetFontAltas->mapGlyphs.end() )
        {
            OE_WARN << LC << "The font atlas does not contain the character code " << std::to_string(charcode) << "\n";
            continue;
        }

        const GlyphInfo& glyphInfo = itGlyph->second;
        double scale = ( fontSize / _stateSetFontAltas->refFontSize );

        _v->push_back( glyphInfo.lb_v * scale + advance );
        _v->push_back( glyphInfo.lt_v * scale + advance );
        _v->push_back( glyphInfo.rt_v * scale + advance );
        _v->push_back( glyphInfo.rb_v * scale + advance );

        // case '|' character then move it so that it is well aligned with the rest of the characters
        if ( charcode == 124 )
        {
            (*_v)[_v->size()-1].y() = (*_v)[_v->size()-4].y() = yMin;
            (*_v)[_v->size()-2].y() = (*_v)[_v->size()-3].y() = yMax;
        }

        yMin = osg::minimum(yMin, (*_v)[_v->size()-1].y());
        yMax = osg::maximum(yMax, (*_v)[_v->size()-2].y());

        _c->push_back( color );
        _c->push_back( color );
        _c->push_back( color );
        _c->push_back( color );
        _c2->push_back( color );
        _c2->push_back( color );
        _c2->push_back( color );
        _c2->push_back( color );

        _t->push_back( glyphInfo.lb_t );
        _t->push_back( glyphInfo.lt_t );
        _t->push_back( glyphInfo.rt_t );
        _t->push_back( glyphInfo.rb_t );

        // push the draw type
        float msdfUnit =  1. / (3. * scale);
        _infoArray->push_back( osg::Vec4(glyphInfo.size.x(), glyphInfo.size.y(), MPStateSetFontAltas::TYPE_CHARACTER_MSDF, msdfUnit) );
        _infoArray->push_back( osg::Vec4(glyphInfo.size.x(), glyphInfo.size.y(), MPStateSetFontAltas::TYPE_CHARACTER_MSDF, msdfUnit) );
        _infoArray->push_back( osg::Vec4(glyphInfo.size.x(), glyphInfo.size.y(), MPStateSetFontAltas::TYPE_CHARACTER_MSDF, msdfUnit) );
        _infoArray->push_back( osg::Vec4(glyphInfo.size.x(), glyphInfo.size.y(), MPStateSetFontAltas::TYPE_CHARACTER_MSDF, msdfUnit) );

        unsigned int last = _v->getNumElements() - 1;
        std::vector<GLubyte> drawIndices { GLubyte(last-3), GLubyte(last-2), GLubyte(last-1), GLubyte(last-3), GLubyte(last-1), GLubyte(last)};
        pushDrawElements(alt, drawIndices, pushDrawAfter);

        advance.x() += glyphInfo.advance * scale;
        nbVertices += 4;
    }

    return nbVertices;
}


int MPAnnotationDrawable::pushDrawElements(double alt, const std::vector<GLubyte> &pDrawElt, bool pushDrawAfter)
{
    int nbEltBefore = 0;
    int indexBefore = 0;
    for ( auto& level : _LODlist )
    {
        // insert a new level before this one
        if ( alt > level.altitudeMax )
        {
            _d->insert( _d->begin() + nbEltBefore, pDrawElt.begin(), pDrawElt.end() );
            _LODlist.insert( _LODlist.begin() + indexBefore, LODAnno(alt, pDrawElt) );
            return indexBefore;
        }
        // insert into an existing level
        else if ( level.altitudeMax == alt )
        {
            if ( pushDrawAfter )
                _d->insert( _d->begin() + nbEltBefore + level.drawElts.size(), pDrawElt.begin(), pDrawElt.end() );
            else
                _d->insert( _d->begin() + nbEltBefore, pDrawElt.begin(), pDrawElt.end() );
            level.drawElts.insert(level.drawElts.begin(), pDrawElt.begin(), pDrawElt.end());
            return indexBefore;
        }
        nbEltBefore += level.drawElts.size();
        indexBefore++;
    }

    // insert at the end
    _d->insert( _d->end(), pDrawElt.begin(), pDrawElt.end() );
    _LODlist.push_back( LODAnno(alt, pDrawElt) );
    return _LODlist.size()-1;
}


void MPAnnotationDrawable::moveTextPosition(int nbVertices, const osg::BoundingBox& refBBox, TextSymbol::Alignment alignment)
{
    osg::Vec3 translate;
    bool doTranslation = false;

    // bbox of the text
    osg::BoundingBox textBBox;
    for ( unsigned int i = _v->getNumElements()-nbVertices ; i < _v->getNumElements() ; ++i )
        textBBox.expandBy( _v->at(i) );

    // horizontal : center of the ref
    // vertical : center of the ref
    if ( alignment == TextSymbol::Alignment::ALIGN_CENTER_CENTER )
    {
        osg::Vec3 textCenter(textBBox.center());
        osg::Vec3 refCenter(refBBox.center());
        translate = - textCenter + refCenter;
        doTranslation = true;
    }

    // horizontal : center of the ref
    // vertical : bottom of the ref
    else if ( alignment == TextSymbol::Alignment::ALIGN_CENTER_BOTTOM )
    {
        osg::Vec3 textCenter(textBBox.center());
        osg::Vec3 refCenter(refBBox.center());
        translate = - textCenter + refCenter;
        translate.y() = 0;
        doTranslation = true;
    }

    // horizontal : right of the ref
    // vertical : center of the ref
    else if ( alignment == TextSymbol::Alignment::ALIGN_LEFT_CENTER )
    {
        osg::Vec3 textCenter(textBBox.center());
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refBBox.xMax() - textBBox.xMin() + _icon_margin, - textCenter.y() + refCenter.y(), 0. );
        doTranslation = true;
    }

    // horizontal : center of the ref = left of the text
    // vertical : top of the ref = bottom of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_LEFT_BOTTOM )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refCenter.x() - textBBox.xMin() + _multi_text_margin, refBBox.yMax() - textBBox.yMin() + _multi_text_margin, 0. );
        doTranslation = true;
    }

    // horizontal : center of the ref = left of the text
    // vertical : bottom of the ref = top of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_LEFT_TOP )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refCenter.x() - textBBox.xMin() + _multi_text_margin, - textBBox.yMax() + refBBox.yMin() - _multi_text_margin,  0. );
        doTranslation = true;
    }

    // horizontal : center of the ref = right of the text
    // vertical : bottom of the ref = top of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_RIGHT_TOP )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refCenter.x() - textBBox.xMax() - _multi_text_margin, -textBBox.yMax() + refBBox.yMin() - _multi_text_margin, 0. );
        doTranslation = true;
    }

    // horizontal : center of the ref = right of the text
    // vertical : top of the ref = bottom of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_RIGHT_BOTTOM )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refCenter.x() - textBBox.xMax() - _multi_text_margin, refBBox.yMax() - textBBox.yMin() + _multi_text_margin, 0. );
        doTranslation = true;
    }

    // horizontal : left of the ref = right of the text
    // vertical : center of the ref = center of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_RIGHT_CENTER )
    {
        osg::Vec3 refCenter(refBBox.center());
        osg::Vec3 textCenter(textBBox.center());
        translate.set( refBBox.xMin() - textBBox.xMax() - _multi_text_margin,  - textCenter.y() + refCenter.y(), 0. );
        doTranslation = true;
    }

    // horizontal : right of the ref
    // vertical : center of the ref = bottom of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_LEFT_BOTTOM_BASE_LINE )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refBBox.xMax() - textBBox.xMin() + _multi_text_margin, - textBBox.yMin() + refCenter.y() + _multi_text_margin/2., 0. );
        doTranslation = true;
    }

    // horizontal : left of the ref
    // vertical : center of the ref = bottom of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_RIGHT_BOTTOM_BASE_LINE )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( - textBBox.xMax() + refBBox.xMin() - _multi_text_margin, - textBBox.yMin() + refCenter.y() + _multi_text_margin/2., 0. );
        doTranslation = true;
    }



    // other cases are not treated
    else
    {
        OE_WARN << LC << "The text alignement " << alignment << " is not supported yet.";
    }

    // then translate
    if ( doTranslation )
        for ( unsigned int i = _v->getNumElements()-nbVertices ; i < _v->getNumElements() ; ++i )
            (*_v)[i] += translate;
}

void MPAnnotationDrawable::setAltitude(double alt)
{
    if ( _camAlt == alt || _isHighlighted )
        return;

    _v = static_cast<osg::Vec3Array*>(getVertexArray());
    //_infoArray = static_cast<osg::Vec4Array*>(getVertexArray());

    // case zoom out
    if ( alt > _camAlt )
    {
        for ( int i = _LOD ; i >= 0 ; i-- )
        {
            if ( alt > _LODlist[i].altitudeMax )
            {
                // hide elements
//                OE_WARN << LC << "    zoom out NEW LOD " << i-1 << "\n";
                if ( _invertLOD )
                {
                    for ( auto elt : _LODlist[i].drawElts )
                        _d->addElement(elt);
                }
                else
                {
                    _d->resizeElements(_d->size() - _LODlist[i].drawElts.size());
                }
                for ( const auto& xShift : _LODlist[i].shiftVec )
                {
                    for ( auto i : xShift.first )
                    {
                        if ( _placementLayout == TEXT_ON_RIGHT )
                        {
                            (*_v)[i] -= xShift.second;
                        }
                        else
                        {
                            (*_v)[i] += xShift.second;
                        }
                        (*_infoArray)[i].x() -= xShift.second.x();
                        (*_infoArray)[i-2].x() -= xShift.second.x();
                    }
                }
                _LOD = i-1;
                if ( _LODlist[i].shiftVec.size() > 0 )
                {
                    _v->dirty();
                    _infoArray->dirty();
                }
                dirty();
            }
        }
    }

    // case zoom in
    else// if ( alt < _camAlt )
    {
        for ( unsigned int i = static_cast<unsigned int>(_LOD+1) ; i < _LODlist.size() ; ++i )
        {
            if ( alt <= _LODlist[i].altitudeMax )
            {
                // push elements (or remove if logics are inverted)
                if ( _invertLOD )
                {
                    _d->resizeElements(_d->size() - _LODlist[i].drawElts.size());
                }
                else
                {
                    for ( auto elt : _LODlist[i].drawElts )
                        _d->addElement(elt);
                }

//                OE_WARN << LC << "    zoom in NEW LOD " << i << "\n";
                for ( const auto& shift : _LODlist[i].shiftVec )
                {
                    for ( auto i : shift.first )
                    {
                        if ( _placementLayout == TEXT_ON_RIGHT )
                        {
                            (*_v)[i] += shift.second;
                        }
                        else
                        {
                            (*_v)[i] -= shift.second;
                        }
                        (*_infoArray)[i].x() += shift.second.x();
                        (*_infoArray)[i-2].x() += shift.second.x();
                    }
                }
                _LOD = i;
                if ( _LODlist[i].shiftVec.size() > 0 )
                {
                    _v->dirty();
                    _infoArray->dirty();
                }
                dirty();
            }
        }
    }

    if ( _LOD == -1 && _d->size() > 0 )
    {
        _d->resizeElements(0);
        dirty();
    }

    _camAlt = alt;
}

void MPAnnotationDrawable::dirty()
{
    _d->dirty();
    dirtyBound();
}

void MPAnnotationDrawable::setInverted(bool inverted)
{
    if ( inverted == _inverted )
        return;

    _v = static_cast<osg::Vec3Array*>(getVertexArray());

    if (! _placementInsideCircle)
    {
        for ( auto i : _rot_verticesToInvert )
        {
            (*_v)[i] = - (*_v)[i];
        }
    }
    else
    {
        double yMax = DBL_MIN;
        for ( int i = 0 ; i < _v->size() ; ++i )
        {
            if ((*_v)[i].y() > yMax) yMax = (*_v)[i].y();
            (*_v)[i] = - (*_v)[i];
        }
        for ( int i = 0 ; i < _v->size() ; ++i )
        {
            (*_v)[i].y() += yMax;
        }
    }

    for ( const auto& shiftData : _rot_verticesToShift )
    {
        const osg::Vec3& shift = shiftData.second;
        for ( auto i : shiftData.first )
            if ( inverted )
                (*_v)[i] -= shift;
            else
                (*_v)[i] += shift;
    }

    _inverted = inverted;
    _v->dirty();

    if ( ! _cluttered )
        dirtyBound();
}


void MPAnnotationDrawable::setHighlight( bool highlight, optional<bool> declutterActive )
{
    if ( _isHighlighted != highlight )
    {
        _priority = ( highlight ? DBL_MAX : _originalPriority );
        if ( ! highlight )
            _declutterActivated = _originalDeclutterActivated;
        else if ( declutterActive.isSet() ) // AND highlight
            _declutterActivated = declutterActive.value();
        // show this annotation with its highest level of detail
        setAltitude(0.);
        _isHighlighted = highlight;
    }
}


void MPAnnotationDrawable::setIconColor(const Color &color )
{
    _c = static_cast<osg::Vec4Array* >(getColorArray());

    for ( auto i : _mainIconVertices )
        (*_c)[i] = color;

    _c->dirty();
}

void MPAnnotationDrawable::updateIcon(const std::string &icon)
{
    if ( ! _stateSetFontAltas.valid() )
    {
        OE_WARN << LC << "_stateSetFontAltas is not valid";
        return;
    }

    if(_mainIconVertices.size() != 4)
    {
        OE_WARN << LC << "Something is wrong with the mainIconVertices size";
        return;
    }

    // get the new icon info

    std::string iconKey = osgDB::getNameLessAllExtensions(osgDB::getSimpleFileName(icon));

    MPStateSetFontAltas::IconMap::const_iterator itIcon = _stateSetFontAltas->mapIcons.find(iconKey);
    if ( itIcon == _stateSetFontAltas->mapIcons.end() )
    {
        OE_WARN << LC << "The icon atlas does not contain the image " << iconKey << "\n";
        return;
    }

    const IconInfo& iconInfo = itIcon->second;

    // update the texture coordinates with the new icon

    _t = static_cast<osg::Vec2Array* >(getTexCoordArray(0u));

    (*_t)[_mainIconVertices[0]] = iconInfo.lb_t;
    (*_t)[_mainIconVertices[1]] = iconInfo.lt_t;
    (*_t)[_mainIconVertices[2]] = iconInfo.rt_t;
    (*_t)[_mainIconVertices[3]] = iconInfo.rb_t;

    _t->dirty();
}

void MPAnnotationDrawable::setVisible(bool visible)
{
    _isVisible = visible;
}

void MPAnnotationDrawable::activeClutteredDrawMode( bool cluttered )
{
    if ( cluttered == _cluttered )
        return;

    if ( _d.valid() )
        _d->_cluttered = cluttered;

    _cluttered = cluttered;
}

void MPAnnotationDrawable::setPlacementLayout( MPScreenSpaceGeometry::PlacementLayout placementLayout )
{
    if ( placementLayout == _placementLayout )
        return;

    _v = static_cast<osg::Vec3Array*>(getVertexArray());

    for ( auto i : _mvLeft_verticesToInvert )
    {
        (*_v)[i] = - (*_v)[i];
    }

    bool moveLeft = (placementLayout == PlacementLayout::TEXT_ON_LEFT);
    for ( const auto& shiftData : _mvLeft_verticesToShift )
    {
        const osg::Vec3& shift = shiftData.second;
        for ( auto i : shiftData.first )
            if ( moveLeft )
                (*_v)[i] -= shift;
            else
                (*_v)[i] += shift;
    }

    _v->dirty();

    if ( ! _cluttered )
        dirtyBound();

    _placementLayout = placementLayout;
}

void MPAnnotationDrawable::updateCircleGeometry(const Symbology::Geometry* geom )
{
    if (! geom || geom->getComponentType() != Symbology::Geometry::TYPE_POLYGON)
    {
        OE_WARN << LC << "updateCircleGeometry is called with a non polygon geometry\n";
        return;
    }

    osg::ref_ptr<const Polygon> geomPolygon = nullptr;
    if (geom->getType() == Symbology::Geometry::TYPE_MULTI)
    {
        const MultiGeometry* geomMulti = dynamic_cast<const MultiGeometry*>(geom);
        geomPolygon = dynamic_cast<const Polygon*>(geomMulti->getComponents().front().get());
    }
    else
    {
        geomPolygon = dynamic_cast<const Polygon*>( geom );
    }

    if (! geomPolygon)
    {
        OE_WARN << LC << "Error while parsing polygon as circle\n";
        return;
    }

    osg::Vec3d centroid{0., 0., 0.};
    unsigned int nbPoints = 0;
    osg::Vec3d posWorld;
    for(const auto& point : *geomPolygon)
    {
        GeoPoint pos( osgEarth::SpatialReference::get("wgs84"), point.x(), point.y(), point.z(), ALTMODE_ABSOLUTE );
        pos.toWorld(posWorld);
        centroid += posWorld;
        ++nbPoints;
    }

    centroid /= nbPoints;

    _circleCenter = new osg::Vec3Array(osg::Array::BIND_OVERALL, 1);
    (*_circleCenter)[0].set(centroid);
    setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_CENTER, _circleCenter.get(), osg::Array::BIND_OVERALL );

    osg::Vec3d first{geomPolygon->front()};
    GeoPoint posGeo( osgEarth::SpatialReference::get("wgs84"), first.x(), first.y(), first.z(), ALTMODE_ABSOLUTE );
    posGeo.toWorld(posWorld);

    _circleAnchor = new osg::Vec3Array(osg::Array::BIND_OVERALL, 1);
    (*_circleAnchor)[0].set(posWorld);
    setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_CIRCLE_ANCHOR, _circleAnchor.get(), osg::Array::BIND_OVERALL );

    // create 8 anchor candidates in [0, PI[
    osg::Vec3d axis(centroid);
    axis.normalize();
    for (int i = 0 ; i < 8 ; ++i)
    {
        osg::Quat quat(osg::PI * i / 4., axis);
        _anchorCandidates.emplace_back(quat*posWorld);
    }

    _placementInsideCircle = true;
    updateGeometry( posGeo, DBL_MAX );
}

void MPAnnotationDrawable::updateGeometry(const Symbology::Geometry *geom, double geographicCourse )
{
    const osg::Vec3d center{geom->getCentroid()};

    GeoPoint pos( osgEarth::SpatialReference::get("wgs84"), center.x(), center.y(), center.z(), ALTMODE_ABSOLUTE );
    updateGeometry( pos, geographicCourse );
}

void MPAnnotationDrawable::updateGeometry(GeoPoint &pos, double geographicCourse )
{
    // compute the anchor point as the centroid of the geometry
    osg::Vec3d p0;
    pos.toWorld(p0);
    setAnchorPoint(p0);
    _position = pos;
    //osg::BoundingSphere bSphere(annoDrawable->getBound());

    // orientation
    // technic is to create a at 2500m from the anchor with the given bearing
    // then the projection in screenspace of both points will be used to compute the screen-space angle
    if ( geographicCourse != DBL_MAX )
    {
        double labelRotationRad = osg::DegreesToRadians ( geographicCourse );

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
        setLineEndPoint(p1);
        setAutoRotate(true);
    }

    // dirty bound done at parent level
    //dirtyBound();
}


void MPAnnotationDrawable::mvLeftShiftInfoForText(const std::string& text, unsigned int iVertFirst,
                                                                            unsigned int /**iVertLast**/, float offset )
{
    std::string textNoSpace = text;
    osgEarth::replaceIn( textNoSpace, " ", ""); // remove spaces
    osgEarth::replaceIn( textNoSpace, "I:", ""); // remove I:
    std::string tmpText = "";

    for (unsigned int iCharIndex = 0; iCharIndex < textNoSpace.size() ; iCharIndex++ )
    {
        const char &c = textNoSpace[iCharIndex];

        if ( c == '|' )
        {
            // push prev word
            if ( ! tmpText.empty() && iCharIndex > 0 )
            {
                int iLast = iVertFirst + iCharIndex * 4 - 1;
                ShiftInfo shiftData = buildShiftInfo( iLast+1 - tmpText.size()*4 , iLast , offset );
                _mvLeft_verticesToShift.push_back( shiftData );
            }

            // push the "|"
            ShiftInfo shiftData = buildShiftInfo( iVertFirst + iCharIndex*4, iVertFirst + iCharIndex*4 + 3 , offset );
            _mvLeft_verticesToShift.push_back( shiftData );

            tmpText = "";
        }

        else
        {
            tmpText += c;
        }
    }

    if ( ! tmpText.empty() )
    {
        int iLast = iVertFirst + textNoSpace.size() * 4 - 1;
        ShiftInfo shiftData = buildShiftInfo( iLast+1 - tmpText.size()*4 , iLast , offset );
        _mvLeft_verticesToShift.push_back( shiftData );
    }
}

MPAnnotationDrawable::ShiftInfo MPAnnotationDrawable::buildShiftInfo( unsigned int iVertFirst, unsigned int iVertLast, float offset ) const
{
    // pre conditions
    if ( iVertFirst < 0 || iVertFirst >= _v->size() || iVertLast < 0 || iVertLast >= _v->size() )
        return ShiftInfo();

    float xMin = FLT_MAX;
    float xMax = -FLT_MAX;
    ShiftInfo shiftData;
    for (unsigned int j = iVertFirst ; j <= iVertLast ; j++)
    {
        float x = (*_v)[j].x();
        if ( x < xMin )
            xMin = x;
        if ( x > xMax )
            xMax = x;
        shiftData.first.push_back(j);
    }
    shiftData.second = osg::Vec3(xMax + xMin + 2.*offset, 0., 0.);

    return shiftData;
}
