
#include <osgEarthAnnotation/MPAnnotationDrawable>
#include <osgEarthSymbology/InstanceSymbol>
#include <osgEarth/Registry>
#include <osgEarthAnnotation/AnnotationUtils>

#include <osgText/String>
#include <osgDB/FileNameUtils>



#define LC "[MPAnnotationDrawable] "

using namespace osgEarth::Annotation;
using namespace osgEarth::Symbology;
using namespace osgEarth;


namespace  {
    const std::string undef = "-32765";
    const Color magenta(1., 135./255., 195./255.);
    const Color almostRed("fe332d");
    const osg::BoundingBox bboxZero(0., 0., 0., 0., 0., 0.);
}


void ScreenSpaceDrawElements::draw(osg::State& state, bool useVertexBufferObjects) const
{
    // standard draw
    if (! _cluttered)
    {
        DrawElementsUShort::draw(state, useVertexBufferObjects);
    }

    // specific draw when culled. Draw only main icon.
    // source copied from DrawElementsUShort::draw()
    else
    {
        GLenum mode = _mode;
        #if defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
            if (mode==GL_POLYGON) mode = GL_TRIANGLE_FAN;
            if (mode==GL_QUAD_STRIP) mode = GL_TRIANGLE_STRIP;
        #endif

        if (useVertexBufferObjects)
        {
            osg::GLBufferObject* ebo = getOrCreateGLBufferObject(state.getContextID());

            if (ebo)
            {
                state.getCurrentVertexArrayState()->bindElementBufferObject(ebo);
                if (_numInstances>=1) state.glDrawElementsInstanced(mode, _drawSize, GL_UNSIGNED_SHORT, (const GLvoid *)(ebo->getOffset(getBufferIndex()) + _drawOffset), _numInstances);
                else glDrawElements(mode, _drawSize, GL_UNSIGNED_SHORT, (const GLvoid *)(ebo->getOffset(getBufferIndex()) + _drawOffset) );
            }
            else
            {
                state.getCurrentVertexArrayState()->unbindElementBufferObject();
                if (_numInstances>=1) state.glDrawElementsInstanced(mode, _drawSize, GL_UNSIGNED_SHORT, &front() + _drawOffset, _numInstances);
                else glDrawElements(mode, _drawSize, GL_UNSIGNED_SHORT, &front() + _drawOffset);
            }
        }
        else
        {
            if (_numInstances>=1) state.glDrawElementsInstanced(mode, _drawSize, GL_UNSIGNED_SHORT, &front() + _drawOffset, _numInstances);
            else glDrawElements(mode, _drawSize, GL_UNSIGNED_SHORT, &front() + _drawOffset);
        }
    }
}


MPAnnotationDrawable::MPAnnotationDrawable(const Style &style, const osgDB::Options* readOptions, MPStateSetFontAltas* atlasStateSet )
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
    _mainFontSize = textSymbol && textSymbol->size().isSet() ? textSymbol->size()->eval() : 16.;
    _multi_text_margin = textSymbol && textSymbol->predefinedOrganisationMargin().isSet() ? textSymbol->predefinedOrganisationMargin().value() : 4.f;
    _altFirstLevel = textSymbol && textSymbol->minRange().isSet() ? textSymbol->minRange().get() : DBL_MAX;

    const TextSymbol* textSym = style.get<TextSymbol>();
    if ( textSym && textSym->encoding().isSet() )
        _text_encoding = AnnotationUtils::convertTextSymbolEncoding(textSym->encoding().value());

    buildGeometry(style);

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

void MPAnnotationDrawable::buildGeometry(const osgEarth::Symbology::Style& style)
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
    const TextSymbol* textSymbol = style.get<TextSymbol>();
    const BBoxSymbol* bboxsymbol = style.get<BBoxSymbol>();


    //-----------------------
    // Search for all icons

    StringVector iconList;
    osg::ref_ptr<const InstanceSymbol> instance = style.get<InstanceSymbol>();
    const IconSymbol* iconSym = instance.valid() ? instance->asIcon() : nullptr;
    if ( iconSym && iconSym->url().isSet() )
    {
        if ( ! iconSym->alignment().isSet() || ! iconSym->alignment().isSetTo(IconSymbol::ALIGN_CENTER_CENTER) )
            OE_WARN << LC << "The icon alignment " << iconSym->alignment().value() << " is not supported yet.\n";

        std::string iconTxt = iconSym->url()->eval();

        // check if there is a list of icons
        if ( iconTxt.find(";") != std::string::npos )
        {
            StringTokenizer splitter( ";", "" );
            splitter.tokenize( iconTxt, iconList );
            if (! iconList.empty() && ! iconList[0].empty() )
                iconTxt = iconList[0];
        }

        // push the main centered icon
        if (! iconTxt.empty() )
        {
            appendIcon(iconTxt);

            if ( _v->getNumElements() > 0)
            {
                // when decluttered the main icon is still drawn
                _drawInClutteredMode = true;
                _mainIconDrawIndex.insert(_mainIconDrawIndex.begin(), _d->begin(), _d->end());
                for ( unsigned int i = 0 ; i < _v->getNumElements() ; i++ )
                {
                    mainBBoxIcon.expandBy( _v->at(i) );
                    _mainIconVertices.push_back(i);
                }
//                osg::Vec3 margin(_icon_margin, _icon_margin, 0.);
//                mainBBoxIcon._min -= margin;
//                mainBBoxIcon._max += margin;

                if ( bboxsymbol && bboxsymbol->geom() == BBoxSymbol::GEOM_BOX_ROUNDED_ORIENTED && _v->size() >= 4 )
                {
                    float offset = textSymbol->pixelOffset().isSet() ? textSymbol->pixelOffset().get().x() : 0.;
                    float xMin = FLT_MAX;
                    float xMax = -FLT_MAX;
                    ShiftInfo shiftData;
                    for (unsigned int j = _v->size() - 4 ; j < _v->size() ; j++)
                    {
                        float x = (*_v)[j].x();
                        if ( x < xMin )
                            xMin = x;
                        if ( x > xMax )
                            xMax = x;
                        shiftData.first.push_back(j);
                    }
                    shiftData.second = osg::Vec3(xMax + xMin + 2.*offset, 0., 0.);
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
        std::string mainText = textSymbol->content()->eval();
        StringTokenizer splitter( ";", "" );
        splitter.tokenize( mainText, textList );
        if (! textList.empty() )
            mainText = textList[0];

        // push the main centered text
        if ( ! mainText.empty() && ! textSymbol->predefinedOrganisation().isSetTo("changeoverpoint")
             && ! textSymbol->predefinedOrganisation().isSetTo("mora"))
        {
            // add the separator if necessary
            if ( ! textSymbol->predefinedOrganisation().isSetTo("airway") )
            {
                if ( iconList.size() > 1 )
                    mainText += " | ";
                else if ( textList.size() >= 2 && textList[1].find("I:") != 0 )
                    mainText += " | " + textList[1];
            }

            osg::Vec4 color(textSymbol->fill().isSet() ? textSymbol->fill()->color() : Color::White);
            int nbVert = appendText(mainText, _mainFont, color, _mainFontSize, _altFirstLevel);
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
                    shiftData.second = osg::Vec3(xMax + xMin + 2.*offset, 0., 0.);
                    _rot_verticesToShift.push_back( shiftData );
                }
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
            newXMax = appendIcon(iconList[i], Color::White, _altFirstLevel, xMax);
            deltaSize += (newXMax - xMax);
            xMax = newXMax;
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
            osg::Vec4 color(textSymbol->fill().isSet() ? textSymbol->fill()->color() : Color::White);
            int nbVert = appendText( textList[1], _mainFont, color, _mainFontSize, _altFirstLevel);
            if ( nbVert > 0 )
            {
                moveTextPosition( nbVert, mainBBoxText, TextSymbol::Alignment::ALIGN_LEFT_CENTER );
                // expand the text zone with this new label (take into account only the last quad)
                for ( int i = _v->getNumElements() - 4 ; i >= 0 && i < static_cast<int>(_v->getNumElements()) ; ++i )
                    mainBBoxText.expandBy( (*_v)[i] );
            }
        }

        // last label to the right on reverse video
        if ( textList[textList.size()-1].find("I:") == 0 )
        {
            // color must be inverted... but for now setting it to black is sufficient
            int nbVert = appendText( textList[textList.size()-1], _mainFont, Color::Black, _mainFontSize, _altFirstLevel);
            if ( nbVert > 0 )
            {
                moveTextPosition( nbVert, mainBBoxText, TextSymbol::Alignment::ALIGN_LEFT_CENTER );
                reverseVideoXThreshold = static_cast<int>(mainBBoxText.xMax() + _multi_text_margin/2.);
                // expand the text zone with this new label (take into account only the last quad)
                for ( int i = _v->getNumElements() - 4 ; i >= 0 && i < static_cast<int>(_v->getNumElements()) ; ++i )
                    mainBBoxText.expandBy( (*_v)[i] );
            }
        }
    }

    // ----------------------
    // Build BBox

    // The bounding box can enclose either the text, or the image, or both
    // the bbox symbol for change over point will be calculated after
    osg::ref_ptr<osg::Drawable> bboxDrawable;
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
                      borderThickness, reverseVideoXThreshold, _bbox_margin, _altFirstLevel);
        else if ( groupType == BBoxSymbol::GROUP_ICON_AND_TEXT && mainBBoxIcon.valid() && mainBBoxText.valid() )
        {
            osg::BoundingBox groupBbox( mainBBoxIcon );
            groupBbox.expandBy( mainBBoxText );
            appendBox(groupBbox, fillColor, strokeColor, geomType, oppose,
                      borderThickness, reverseVideoXThreshold, _bbox_margin);
            // add the xShift to do in the right LOD
            _LODlist[_LODlist.size()-1].shiftVec.push_back( ShiftInfo({_v->getNumElements()-1, _v->getNumElements()-2},
                                                                      osg::Vec3(mainBBoxText.xMax() - mainBBoxIcon.xMax(), 0., 0.)) );
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
        Color colorOrg = textSymbol->fill().isSet() ? textSymbol->fill().get().color() : Color::White;
        Color colorList[] = {colorOrg, colorOrg, colorOrg, magenta, colorOrg, colorOrg};
        double alt2ndLevel = textSymbol->minRange2ndlevel().isSet() ? textSymbol->minRange2ndlevel().value() : _altFirstLevel;
        // make the font regular for the secondary texts
        std::string subFont(_mainFont);
        replaceIn(subFont, "Bold", "Regular");

        // for each text (except the first one)
        for ( unsigned int i=1 ; i<textList.size() ; ++i )
        {
            int nbVert = appendText(textList[i], subFont, colorList[i-1], fontSizeList[i-1], alt2ndLevel);
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
                    appendBox( box, Color::Black, Color::White, BBoxSymbol::GEOM_BOX, false, 1.f, 0.f, 3.f, alt2ndLevel);
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
        Color colorOrg = textSymbol->fill().isSet() ? textSymbol->fill().get().color() : Color::White;

        // right top text
        int nbVert = appendText(textList[0], _mainFont, colorOrg, _mainFontSize);
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
        int nbVert2 = appendText(textList[1], _mainFont, colorOrg, _mainFontSize);
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
        appendBox(box, Color::Gray, strokeColor, geomType, false, borderThickness, _bbox_margin);
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
            color = almostRed;

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

    setVertexArray( _v.get() );
    setTexCoordArray( 0u, _t.get(), osg::Array::BIND_PER_VERTEX );
    setColorArray( _c.get(), osg::Array::BIND_PER_VERTEX );
    setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_INFO, _infoArray.get(), osg::Array::BIND_PER_VERTEX );
    setVertexAttribArray( MPStateSetFontAltas::ATTRIB_ANNO_COLOR2, _c2.get(), osg::Array::BIND_PER_VERTEX );
    addPrimitiveSet( _d.get() );

    if ( getVertexArray()->getVertexBufferObject() )
        getVertexArray()->getVertexBufferObject()->setUsage(GL_STATIC_DRAW_ARB);

    _camAlt = 0.;
    _LOD = _LODlist.size()-1;
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
    std::vector<GLushort> drawIndices { GLushort(last-3), GLushort(last-2), GLushort(last-1), GLushort(last-3), GLushort(last-1), GLushort(last)};
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
        case BBoxSymbol::GEOM_BOX_ROUNDED_INNER :
            drawType = MPStateSetFontAltas::TYPE_BBOX_ROUNDED;
            break;
        case BBoxSymbol::GEOM_BOX_ORIENTED :
        case BBoxSymbol::GEOM_BOX_ORIENTED_SYM :
            drawType = MPStateSetFontAltas::TYPE_BBOX_ONEARROW;
            xMaxMargin += (bbox.yMax() - bbox.yMin()) / 3.;
            break;
        case BBoxSymbol::GEOM_BOX_ORIENTED_2WAYS:
            drawType = MPStateSetFontAltas::TYPE_BBOX_TWOARROWS;
            xMaxMargin += (bbox.yMax() - bbox.yMin()) / 3.;
            xMinMargin = xMaxMargin;
            break;
        case BBoxSymbol::GEOM_STAIR:
            drawType = MPStateSetFontAltas::TYPE_BBOX_STAIR;
            break;
        case BBoxSymbol::GEOM_BOX_ROUNDED_ORIENTED:
            drawType = MPStateSetFontAltas::TYPE_BBOX_ROUNDED_ORIENTED;
            xMaxMargin += (bbox.yMax() - bbox.yMin()) / 3.;
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

    // push the draw elements
    std::vector<GLushort> drawIndices { GLushort(last-3), GLushort(last-2), GLushort(last-1), GLushort(last-3), GLushort(last-1), GLushort(last)};
    return pushDrawElements(alt, drawIndices);
}

int MPAnnotationDrawable::appendText(const std::string& text, const std::string& font, const osg::Vec4& color, double fontSize, double alt)
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
    osgText::String textFormated(tmpText, _text_encoding);
    MPStateSetFontAltas::GlyphMap::const_iterator itGlyph;
    for ( osgText::String::const_iterator itrChar=textFormated.begin() ; itrChar!=textFormated.end() ; ++itrChar )
    {
        unsigned int charcode = *itrChar;

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
        double scale = ( fontSize / _stateSetFontAltas->refFontSize ) * Registry::instance()->getDevicePixelRatio();

        _v->push_back( glyphInfo.lb_v * scale + advance );
        _v->push_back( glyphInfo.lt_v * scale + advance );
        _v->push_back( glyphInfo.rt_v * scale + advance );
        _v->push_back( glyphInfo.rb_v * scale + advance );

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
        std::vector<GLushort> drawIndices { GLushort(last-3), GLushort(last-2), GLushort(last-1), GLushort(last-3), GLushort(last-1), GLushort(last)};
        pushDrawElements(alt, drawIndices);

        advance.x() += glyphInfo.advance * scale;
        nbVertices += 4;
    }

    return nbVertices;
}


int MPAnnotationDrawable::pushDrawElements(double alt, const std::vector<GLushort> &pDrawElt)
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
            _d->insert( _d->begin() + nbEltBefore /**+ level.drawElts.size()**/, pDrawElt.begin(), pDrawElt.end() );
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

//        translate.set( refBBox.xMax() - textBBox.xMin(), - textBBox.yMax() - refBBox.yMin(), 0. );
        doTranslation = true;
    }

    // horizontal : center of the ref = left of the text
    // vertical : bottom of the ref = top of the text
    else if ( alignment == TextSymbol::Alignment::ALIGN_LEFT_TOP )
    {
        osg::Vec3 refCenter(refBBox.center());
        translate.set( refCenter.x() - textBBox.xMin() + _multi_text_margin, - textBBox.yMax() + refBBox.yMin() - _multi_text_margin,  0. );
//        translate.set( refBBox.xMax() - textBBox.xMin(), - textBBox.yMax() - refBBox.yMin(), 0. );
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
        translate.set( refCenter.x() - textBBox.xMax() - _multi_text_margin,  - textCenter.y() + refCenter.y(), 0. );
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
            _v->at(i) += translate;
}

void MPAnnotationDrawable::setAltitude(double alt)
{
    if ( _camAlt == alt )
        return;

//    OE_WARN << LC << "CAM MOVE\n";
    _v = static_cast<osg::Vec3Array*>(getVertexArray());
    //_infoArray = static_cast<osg::Vec4Array*>(getVertexArray());

    // case zoom out
    if ( alt > _camAlt )
    {
//        OE_WARN << LC << "  zoom out\n";
        for ( int i = _LOD ; i >= 0 ; i-- )
        {
            if ( alt > _LODlist[i].altitudeMax )
            {
                // hide elements
//                OE_WARN << LC << "    zoom out NEW LOD " << i-1 << "\n";
                _d->resizeElements(_d->size() - _LODlist[i].drawElts.size());
                for ( const auto& xShift : _LODlist[i].shiftVec )
                {
                    for ( auto i : xShift.first )
                    {
                        (*_v)[i] -= xShift.second;
                        (*_infoArray)[i].x() -= xShift.second.x();
                        (*_infoArray)[i-2].x() -= xShift.second.x();
                        //(*_infoArray)[i].y() += shift.second.y();
                        //(*_infoArray)[i-2].y() += shift.second.y();
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
//        OE_WARN << LC << "  zoom in\n";
        for ( unsigned int i = static_cast<unsigned int>(_LOD+1) ; i < _LODlist.size() ; ++i )
        {
            if ( alt <= _LODlist[i].altitudeMax )
            {
                // push elements
                for ( auto elt : _LODlist[i].drawElts )
                    _d->addElement(elt);
//                OE_WARN << LC << "    zoom in NEW LOD " << i << "\n";
                for ( const auto& shift : _LODlist[i].shiftVec )
                {
                    for ( auto i : shift.first )
                    {
                        (*_v)[i] += shift.second;
                        (*_infoArray)[i].x() += shift.second.x();
                        (*_infoArray)[i-2].x() += shift.second.x();
                        //(*_infoArray)[i].y() += shift.second.y();
                        //(*_infoArray)[i-2].y() += shift.second.y();
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

    for ( auto i : _rot_verticesToInvert )
    {
        (*_v)[i] = - (*_v)[i];
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


void MPAnnotationDrawable::setHighlight( bool highlight )
{

}


void MPAnnotationDrawable::setIconColor(const Color &color )
{
    _c = static_cast<osg::Vec4Array* >(getColorArray());

    for ( auto i : _mainIconVertices )
        (*_c)[i] = color;
}


void MPAnnotationDrawable::activeClutteredMode( bool cluttered )
{
    if ( cluttered == _cluttered )
        return;

    _d->_cluttered = cluttered;

    _cluttered = cluttered;
}
