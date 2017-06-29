/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
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
#include "SplatLayer"
#include "SplatShaders"
#include "NoiseTextureFactory"
#include <osgEarth/VirtualProgram>
#include <osgEarthFeatures/FeatureSource>
#include <osgEarthFeatures/FeatureSourceLayer>
#include <osgUtil/CullVisitor>
#include <cstdlib> // getenv

#define LC "[SplatLayer] "

#define COVERAGE_SAMPLER "oe_splat_coverageTex"
#define SPLAT_SAMPLER    "oe_splatTex"
#define NOISE_SAMPLER    "oe_splat_noiseTex"
#define LUT_SAMPLER      "oe_splat_coverageLUT"

using namespace osgEarth::Splat;

namespace osgEarth { namespace Splat {
    REGISTER_OSGEARTH_LAYER(splat_imagery, SplatLayer);
} }

//........................................................................

Config
SplatLayerOptions::getConfig() const
{
    Config conf = VisibleLayerOptions::getConfig();
    conf.key() = "splat";
    conf.set("land_cover_layer", _landCoverLayerName);

    Config zones("zones");
    for (int i = 0; i < _zones.size(); ++i) {
        Config zone = _zones[i].getConfig();
        if (!zone.empty())
            zones.add(zone);
    }
    if (!zones.empty())
        conf.update(zones);
    return conf;
}

void
SplatLayerOptions::fromConfig(const Config& conf)
{
    conf.getIfSet("land_cover_layer", _landCoverLayerName);

    const Config* zones = conf.child_ptr("zones");
    if (zones) {
        const ConfigSet& children = zones->children();
        for (ConfigSet::const_iterator i = children.begin(); i != children.end(); ++i) {
            _zones.push_back(ZoneOptions(*i));
        }
    }
}

//........................................................................

SplatLayer::SplatLayer() :
VisibleLayer(&_optionsConcrete),
_options(&_optionsConcrete)
{
    init();
}

SplatLayer::SplatLayer(const SplatLayerOptions& options) :
VisibleLayer(&_optionsConcrete),
_options(&_optionsConcrete),
_optionsConcrete(options)
{
    init();
}

void
SplatLayer::init()
{
    VisibleLayer::init();

    _zonesConfigured = false;

    _editMode = (::getenv("OSGEARTH_SPLAT_EDIT") != 0L); // TODO deprecate
    _gpuNoise = (::getenv("OSGEARTH_SPLAT_GPU_NOISE") != 0L); // TODO deprecate

    setRenderType(osgEarth::Layer::RENDERTYPE_TILE);

    for (std::vector<ZoneOptions>::const_iterator i = options().zones().begin();
        i != options().zones().end();
        ++i)
    {
        osg::ref_ptr<Zone> zone = new Zone(*i);
        _zones.push_back(zone.get());
    }
}

void
SplatLayer::setLandCoverDictionary(LandCoverDictionary* layer)
{
    _landCoverDict = layer;
    if (layer)
        buildStateSets();
}

void
SplatLayer::setLandCoverLayer(LandCoverLayer* layer)
{
    _landCoverLayer = layer;
    if (layer) {
        buildStateSets();
    }
}

void
SplatLayer::addedToMap(const Map* map)
{
    if (!_landCoverDict.valid())
    {
        _landCoverDictListener.listen(map, this, &SplatLayer::setLandCoverDictionary);
    }

    if (!_landCoverLayer.valid() && options().landCoverLayer().isSet())
    {
        _landCoverListener.listen(map, options().landCoverLayer().get(), this, &SplatLayer::setLandCoverLayer);
    }

    for (Zones::iterator zone = _zones.begin(); zone != _zones.end(); ++zone)
    {
        zone->get()->configure(map, getReadOptions());
    }

    _zonesConfigured = true;
    
    buildStateSets();
}

void
SplatLayer::removedFromMap(const Map* map)
{
    //NOP
}

void
SplatLayer::setTerrainResources(TerrainResources* res)
{
    if (res)
    {
        // TODO.
        // These reservations are Layer-specific, so we should add the
        // capability to TerrainResources to support per-Layer reservations.
        if (_splatBinding.valid() == false)
        {
            if (res->reserveTextureImageUnitForLayer(_splatBinding, this, "Splat texture") == false)
            {
                OE_WARN << LC << "No texture unit available for splatting texture\n";
            }
        }

        if (_lutBinding.valid() == false)
        {
            if (res->reserveTextureImageUnitForLayer(_lutBinding, this, "Splatting LUT") == false)
            {
                OE_WARN << LC << "No texture unit available for splatting LUT\n";
            }
        }

        if (_noiseBinding.valid() == false)
        {
            if (res->reserveTextureImageUnitForLayer(_noiseBinding, this, "Splat noise sampler") == false)
            {
                OE_WARN << LC << "No texture unit available for splatting Noise function\n";
            }
        }

        if (_splatBinding.valid() && _lutBinding.valid())
        {
            buildStateSets();
        }
    }
}

bool
SplatLayer::preCull(osgUtil::CullVisitor* cv) const
{
    Layer::preCull(cv);

    // If we have zones, select the current one and apply its state set.
    if (_zones.size() > 0)
    {
        int zoneIndex = 0;
        osg::Vec3d vp = cv->getViewPoint();

        for(int z=_zones.size()-1; z > 0 && zoneIndex == 0; --z)
        {
            if ( _zones[z]->contains(vp) )
            {
                zoneIndex = z;
            }
        }

        osg::StateSet* zoneStateSet = 0L;
        Surface* surface = _zones[zoneIndex]->getSurface();
        if (surface)
        {
            zoneStateSet = surface->getStateSet();
        }

        if (zoneStateSet == 0L)
        {
            OE_FATAL << LC << "ASSERTION FAILURE - zoneStateSet is null\n";
            exit(-1);
        }
        
        cv->pushStateSet(zoneStateSet);
    }
    return true;
}

void
SplatLayer::postCull(osgUtil::CullVisitor* cv) const
{
    // If we have at least one zone, one stateset was pushed in preCull,
    // so pop it now.
    if (_zones.size() > 0)
        cv->popStateSet();

    Layer::postCull(cv);
}

void
SplatLayer::buildStateSets()
{
    // assert we have the necessary TIUs:
    if (_splatBinding.valid() == false || _lutBinding.valid() == false) {
        OE_DEBUG << LC << "buildStateSets deferred.. bindings not reserved\n";
        return;
    }

    if (!_zonesConfigured) {
        OE_DEBUG << LC << "buildStateSets deferred.. zones not yet configured\n";
        return;
    }
    
    osg::ref_ptr<LandCoverDictionary> landCoverDict;
    if (_landCoverDict.lock(landCoverDict) == false) {
        OE_DEBUG << LC << "buildStateSets deferred.. land cover dictionary not available\n";
        return;
    }
    
    osg::ref_ptr<LandCoverLayer> landCoverLayer;
    if (_landCoverLayer.lock(landCoverLayer) == false) {
        OE_DEBUG << LC << "buildStateSets deferred.. land cover layer not available\n";
        return;
    }

    // Load all the splatting textures
    for (Zones::iterator z = _zones.begin(); z != _zones.end(); ++z)
    {
        Zone* zone = z->get();
        Surface* surface = z->get()->getSurface();
        if (surface == 0L)
        {
            OE_WARN << LC << "No surface defined for zone " << zone->getName() << std::endl;
            return;
        }
        if (surface->loadTextures(landCoverDict.get(), getReadOptions()) == false)
        {
            OE_WARN << LC << "Texture load failed for zone " << zone->getName() << "\n";
            return;
        }
    }

    // Set up the zone-specific elements:
    for (Zones::iterator z = _zones.begin(); z != _zones.end(); ++z)
    {
        Zone* zone = z->get();

        osg::StateSet* zoneStateset = zone->getSurface()->getOrCreateStateSet();

        // The texture array for the zone:
        const SplatTextureDef& texdef = zone->getSurface()->getTextureDef();

        // apply the splatting texture catalog:
        zoneStateset->setTextureAttribute(_splatBinding.unit(), texdef._texture.get());

        // apply the buffer containing the coverage-to-splat LUT:
        zoneStateset->setTextureAttribute(_lutBinding.unit(), texdef._splatLUTBuffer.get());

        OE_DEBUG << LC << "Installed getRenderInfo for zone \"" << zone->getName() << "\" (uid=" << zone->getUID() << ")\n";
    }

    // Next set up the elements that apply to all zones:
    osg::StateSet* stateset = new osg::StateSet();

    // Bind the texture image unit:
    stateset->addUniform(new osg::Uniform(SPLAT_SAMPLER, _splatBinding.unit()));

    // install the uniform for the splat LUT.
    stateset->addUniform(new osg::Uniform(LUT_SAMPLER, _lutBinding.unit()));
        
    if (_noiseBinding.valid())
    {
        NoiseTextureFactory noise;
        osg::ref_ptr<osg::Texture> noiseTexture = noise.create(256u, 1u);
        stateset->setTextureAttribute(_noiseBinding.unit(), noiseTexture.get());
        stateset->addUniform(new osg::Uniform(NOISE_SAMPLER, _noiseBinding.unit()));
        stateset->setDefine("OE_SPLAT_HAVE_NOISE_SAMPLER");
    }

    osg::Uniform* lcTexUniform = new osg::Uniform(COVERAGE_SAMPLER, landCoverLayer->shareImageUnit().get());
    stateset->addUniform(lcTexUniform);

    stateset->addUniform(new osg::Uniform("oe_splat_scaleOffsetInt", 0));
    stateset->addUniform(new osg::Uniform("oe_splat_warp", 0.0f));
    stateset->addUniform(new osg::Uniform("oe_splat_blur", 1.0f));
    stateset->addUniform(new osg::Uniform("oe_splat_useBilinear", 1.0f));
    stateset->addUniform(new osg::Uniform("oe_splat_noiseScale", 12.0f));

    stateset->addUniform(new osg::Uniform("oe_splat_detailRange", 100000.0f));

    if (_editMode)
        stateset->setDefine("OE_SPLAT_EDIT_MODE");

    if (_gpuNoise)
        stateset->setDefine("OE_SPLAT_GPU_NOISE");

    stateset->setDefine("OE_USE_NORMAL_MAP");

    stateset->setDefine("OE_SPLAT_COVERAGE_TEXMAT", landCoverLayer->shareTexMatUniformName().get());

    SplattingShaders splatting;
    VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
    splatting.load(vp, splatting.VertModel);
    splatting.load(vp, splatting.VertView);
    splatting.load(vp, splatting.Frag);
    splatting.load(vp, splatting.Util);

    this->setStateSet(stateset);

    OE_DEBUG << LC << "Statesets built!! Ready!\n";
}