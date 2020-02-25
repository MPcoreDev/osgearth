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
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include <osgEarthAnnotation/ModelNode>
#include <osgEarthAnnotation/AnnotationRegistry>
#include <osgEarthAnnotation/AnnotationUtils>
#include <osgEarthAnnotation/GeoPositionNodeAutoScaler>
#include <osgEarthSymbology/Style>
#include <osgEarthSymbology/InstanceSymbol>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/VirtualProgram>
#include <osgEarth/NodeUtils>

#define LC "[ModelNode] "

using namespace osgEarth;
using namespace osgEarth::Annotation;
using namespace osgEarth::Symbology;


//------------------------------------------------------------------------


ModelNode::ModelNode(MapNode*              mapNode,
                     const Style&          style,
                     const osgDB::Options* readOptions ) :
GeoPositionNode(),
_style( style ),
_readOptions( readOptions ),
_image( nullptr ),
_imageGeom( nullptr )
{
    construct();
    setMapNode(mapNode);
    compileModel();
}

void
ModelNode::construct()
{
    // nop
}

void
ModelNode::setStyle(const Style& style)
{
    _style = style;
    compileModel();
}

void
ModelNode::compileModel()
{
    osgEarth::clearChildren( getPositionAttitudeTransform() );

    osg::ref_ptr<const ModelSymbol> sym = _style.get<ModelSymbol>();

    if ( sym.valid() )
    {
        if ( ( sym->url().isSet() ) || (sym->getModel() != NULL) )
        {
            // Try to get a model from symbol
            osg::ref_ptr<osg::Node> node = sym->getModel();

            // Try to get a model from URI
            if ( !node.valid() )
            {
                URI uri = sym->url()->evalURI();

                if (! sym->modelType().isSetTo(ModelSymbol::TYPE_IMAGE))
                {
                    if ( sym->uriAliasMap()->empty() )
                    {
                        node = uri.getNode( _readOptions.get() );
                    }
                    else
                    {
                        // install an alias map if there's one in the symbology.
                        osg::ref_ptr<osgDB::Options> tempOptions = Registry::instance()->cloneOrCreateOptions(_readOptions.get());
                        tempOptions->setReadFileCallback( new URIAliasMapReadCallback(*sym->uriAliasMap(), uri.full()) );
                        node = uri.getNode( tempOptions.get() );
                    }
                }
                else
                {
                    //try to load an image from the uri provided
                    OE_DEBUG << LC << "try to load image " << uri.full() << std::endl;
                    _image = uri.getImage();

                    if( _image.valid() )
                    {
                        OE_DEBUG << LC << "creating single sided image geometry " << std::endl;
                        osg::ref_ptr<osg::Geode> geode = new osg::Geode();
                        geode->setName( "Image Geode" );

                        //try to create a geometry for this image (same geom as Placenode)
                        osg::Vec2s offset(0.0,0.0);
                        _imageGeom = AnnotationUtils::createImageGeometry( _image.get(), offset, 0, 0.0, 1.0 );
                        if ( _imageGeom )
                        {
                            _imageGeom->setName( "Image Geometry" );
                            OE_DEBUG << LC << "adding image geometry to scenegraph " << uri.full() << std::endl;
                            geode->addDrawable( _imageGeom );
                            geode->getOrCreateStateSet()->setMode( GL_CULL_FACE, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE );
                            node = geode;
                        }
                        else
                        {
                            OE_WARN << LC << "Could not create geometry for the image " << uri.full() << std::endl;
                        }
                    }
                    else
                    {
                        OE_WARN << LC << "Could not load model as image " << uri.full() << std::endl;
                    }
                }

                if ( !node.valid() )
                {
                    OE_WARN << LC << "No model and failed to load data from " << uri.full() << std::endl;
                }
            }

            if ( node.valid() )
            {
                if ( Registry::capabilities().supportsGLSL() )
                {
                    // generate shader code for the loaded model:
                    Registry::shaderGenerator().run(
                        node.get(),
                        "osgEarth.ModelNode",
                        Registry::stateSetCache() );
                }

                // install clamping/draping if necessary
                node = AnnotationUtils::installOverlayParent( node.get(), _style );

                getPositionAttitudeTransform()->addChild( node.get() );

                osg::Vec3d scale(1, 1, 1);

                if ( sym->scale().isSet() )
                {
                    double s = sym->scale()->eval();
                    scale.set(s, s, s);
                }
                if ( sym->scaleX().isSet() )
                    scale.x() = sym->scaleX()->eval();

                if ( sym->scaleY().isSet() )
                    scale.y() = sym->scaleY()->eval();

                if ( sym->scaleZ().isSet() )
                    scale.z() = sym->scaleZ()->eval();

                if (scale != osg::Vec3d(1,1,1))
                {
                    getPositionAttitudeTransform()->setScale( scale );
                }

                // auto scaling?
                if ( sym->autoScale() == true )
                {
                    setCullingActive(false);
                    _geoPosCallback = new GeoPositionNodeAutoScaler( osg::Vec3d(1,1,1), sym->minAutoScale().value(), sym->maxAutoScale().value() );
                    addCullCallback( _geoPosCallback.get() );
                }

                // rotational offsets?
                if (sym && (sym->heading().isSet() || sym->pitch().isSet() || sym->roll().isSet()) )
                {
                    osg::Matrix rot;
                    double heading = sym->heading().isSet() ? sym->heading()->eval() : 0.0;
                    double pitch   = sym->pitch().isSet()   ? sym->pitch()->eval()   : 0.0;
                    double roll    = sym->roll().isSet()    ? sym->roll()->eval()    : 0.0;
                    rot.makeRotate( 
                        osg::DegreesToRadians(heading), osg::Vec3(0,0,1),
                        osg::DegreesToRadians(pitch),   osg::Vec3(1,0,0),
                        osg::DegreesToRadians(roll),    osg::Vec3(0,1,0) );

                    getPositionAttitudeTransform()->setAttitude( rot.getRotate() );
                }

                this->applyRenderSymbology( _style );
            }
            else
            {
                OE_WARN << LC << "No model" << std::endl;
            }
        }
        else
        {
            OE_WARN << LC << "Symbology: no URI or model" << std::endl;
        }
    }
    else
    {
        OE_WARN << LC << "Insufficient symbology" << std::endl;
    }
}

//-------------------------------------------------------------------

OSGEARTH_REGISTER_ANNOTATION( model, osgEarth::Annotation::ModelNode );


ModelNode::ModelNode(const Config& conf, const osgDB::Options* readOptions) :
GeoPositionNode(conf, readOptions),
_readOptions(readOptions)
{
    construct();

    conf.get( "style", _style );

    std::string uri = conf.value("url");
    if ( !uri.empty() )
        _style.getOrCreate<ModelSymbol>()->url() = StringExpression(uri);

    //init();
    //setPosition(getPosition());

    compileModel();
}

Config
ModelNode::getConfig() const
{
    Config conf = GeoPositionNode::getConfig();
    conf.key() = "model";

    if ( !_style.empty() )
        conf.set( "style", _style );

    return conf;
}

void
ModelNode::replaceImage(const URI &uri)
{
    _image = uri.getImage();

    if(getPositionAttitudeTransform()->getNumChildren()==0)
    {
        OE_WARN << LC << "replaceImage: PositionAttitudeTransform has no child " << std::endl;
        return;
    }
    osg::ref_ptr<osg::Node> node = getPositionAttitudeTransform()->getChild(0);

    if( _image.valid() )
    {
        if(node.valid())
        {
            OE_DEBUG << LC << "creating image geometry " << std::endl;
            osg::ref_ptr<osg::Geode> geode = node->asGeode();
            if ( geode.valid() )
            {
                geode->setName( "Image Geode" );

                //try to create a geometry for this image (same geom as Placenode)
                osg::Vec2s offset(0.0,0.0);
                osg::ref_ptr<osg::Geometry> imageGeom = AnnotationUtils::createImageGeometry( _image.get(), offset, 0, 0.0, 1.0 );
                if ( imageGeom.valid() )
                {
                    imageGeom->setName( "Image Geometry" );
                    OE_DEBUG << LC << "adding image geometry to scenegraph " << uri.full() << std::endl;

                    osg::ref_ptr<osg::Drawable> drawable = geode->getDrawable(0);
                    if (drawable) {
                        OE_DEBUG << LC << "drawable found, replacing it " << std::endl;
                        bool ret = geode->replaceDrawable(drawable, imageGeom);
                        OE_DEBUG << LC << "replacement success " << ret << std::endl;
                    } else {
                        geode->addDrawable( imageGeom.get() );
                    }

                    if ( node.valid() )
                    {
                        if ( Registry::capabilities().supportsGLSL() )
                        {
                            // generate shader code for the loaded model:
                            Registry::shaderGenerator().run(
                                node.get(),
                                "osgEarth.ModelNode",
                                Registry::stateSetCache() );
                        }
                    }

                } else
                {
                    OE_WARN << LC << "replaceImage: Could not create geometry for the image " << uri.full() << std::endl;
                }
            } else
            {
                OE_WARN << LC << "replaceImage: Node is not a geode " << std::endl;
            }
        }
        else
        {
            OE_WARN << LC << "replaceImage: No valid Node found " << std::endl;
        }

    }
    else
    {
        OE_WARN << LC << "replaceImage: Could not load model as image " << uri.full() << std::endl;
    }
}

void
ModelNode::setAutoScale(bool autoScale, double minAutoScale, double maxAutoScale)
{
    OE_DEBUG << LC << "Setting autoScale " << (autoScale ? "true" : "false") << std::endl;
    if (_geoPosCallback.valid())
    {
        OE_DEBUG << LC << "Removing existing cull callback " << std::endl;
        removeCullCallback(_geoPosCallback.get());
    }
    // auto scaling?
    if ( autoScale == true )
    {
        OE_DEBUG << LC << "Setting new GeoPositionNodeAutoScaler " << std::endl;
        OE_DEBUG << LC << "minAutoScale: " << minAutoScale << std::endl;
        OE_DEBUG << LC << "maxAutoScale: " << maxAutoScale << std::endl;
        setCullingActive(false);
        _geoPosCallback = new GeoPositionNodeAutoScaler( osg::Vec3d(1,1,1), minAutoScale, maxAutoScale );
        addCullCallback(_geoPosCallback.get());
    } else {
        setCullingActive(true);
    }
}
