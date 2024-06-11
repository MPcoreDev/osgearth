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
#include <osgEarth/MapNodeOptions>
#include <osgEarth/StringUtils>


using namespace osgEarth;

std::string MapNodeOptions::OPTIONS_TAG = "__osgEarth::MapNodeOptions";

//----------------------------------------------------------------------------

static TerrainOptions s_defaultTerrainOptions;

//----------------------------------------------------------------------------

MapNodeOptions::MapNodeOptions( const Config& conf ) :
ConfigOptions          ( conf ),
_proxySettings         ( ProxySettings() ),
_enableLighting        ( true ),
_overlayBlending       ( true ),
_overlayTerrainBlending( true ),
_overlayMipMapping     ( false ),
_overlayTextureSize    ( 4096 ),
_terrainOptions        ( 0L ),
_overlayAttachStencil  ( false ),
_overlayResolutionRatio( 3.0f ),
_useCascadeDraping     ( false )
{
    _colors[defaultTheme]->set( 1.0f, 1.0f, 1.0f, 1.0f );
    mergeConfig( conf );
}

MapNodeOptions::MapNodeOptions( const TerrainOptions& to ) :
_proxySettings         ( ProxySettings() ),
_enableLighting        ( true ),
_overlayBlending       ( true ),
_overlayTerrainBlending( true ),
_overlayTextureSize    ( 4096 ),
_overlayMipMapping     ( false ),
_overlayAttachStencil  ( false ),
_overlayResolutionRatio( 3.0f ),
_terrainOptions        ( 0L ),
_useCascadeDraping     ( false )
{
    _colors[defaultTheme]->set( 1.0f, 1.0f, 1.0f, 1.0f );
    setTerrainOptions( to );
}

MapNodeOptions::MapNodeOptions( const MapNodeOptions& rhs ) :
_proxySettings         ( ProxySettings() ),
_enableLighting        ( true ),
_overlayBlending       ( true ),
_overlayTerrainBlending( true ),
_overlayTextureSize    ( 4096 ),
_overlayMipMapping     ( false ),
_overlayAttachStencil  ( false ),
_overlayResolutionRatio( 3.0f ),
_terrainOptions        ( 0L ),
_useCascadeDraping     ( false )
{
    _colors[defaultTheme]->set( 1.0f, 1.0f, 1.0f, 1.0f );
    mergeConfig( rhs.getConfig() );
}


MapNodeOptions::~MapNodeOptions()
{
    if ( _terrainOptions )
    {
        delete _terrainOptions;
        _terrainOptions = 0L;
    }
}

Config
MapNodeOptions::getConfig() const
{
    Config conf; // start with a fresh one since this is a FINAL object  // = ConfigOptions::getConfig();
    conf.key() = "options";

    conf.set( "proxy",                    _proxySettings );
    conf.set( "lighting",                 _enableLighting );
    conf.set( "terrain",                  _terrainOptionsConf );
    conf.set( "overlay_warping",          _overlayVertexWarping );
    conf.set( "overlay_blending",         _overlayBlending );
    conf.set( "overlay_terrain_blending", _overlayTerrainBlending );
    conf.set( "overlay_texture_size",     _overlayTextureSize );
    conf.set( "overlay_mipmapping",       _overlayMipMapping );
    conf.set( "overlay_attach_stencil",   _overlayAttachStencil );
    conf.set( "overlay_resolution_ratio", _overlayResolutionRatio );
    conf.set( "cascade_draping",          _useCascadeDraping );
    if(_colors[Theme::THEME_DARK].isSet())
        conf.set( "color_dark",            osgEarth::vec4fToHtmlColor(_colors[Theme::THEME_DARK].get()) );
    if(_colors[Theme::THEME_LIGHT].isSet())
        conf.set( "color_light",          osgEarth::vec4fToHtmlColor(_colors[Theme::THEME_LIGHT].get()) );

    return conf;
}

void
MapNodeOptions::mergeConfig( const Config& conf )
{
    ConfigOptions::mergeConfig( conf );

    conf.get( "proxy",                    _proxySettings );
    conf.get( "lighting",                 _enableLighting );
    conf.get( "overlay_warping",          _overlayVertexWarping );
    conf.get( "overlay_blending",         _overlayBlending );
    conf.get( "overlay_terrain_blending", _overlayTerrainBlending );
    conf.get( "overlay_texture_size",     _overlayTextureSize );
    conf.get( "overlay_mipmapping",       _overlayMipMapping );
    conf.get( "overlay_attach_stencil",   _overlayAttachStencil );
    conf.get( "overlay_resolution_ratio", _overlayResolutionRatio );
    conf.get( "cascade_draping",          _useCascadeDraping );
    if ( conf.hasValue( "color_dark" ) )
        _colors[Theme::THEME_DARK] = htmlColorToVec4f( conf.value( "color_dark" ));
    if ( conf.hasValue( "color_light" ) )
        _colors[Theme::THEME_LIGHT] = htmlColorToVec4f( conf.value( "color_light" ));

    if ( conf.hasChild( "terrain" ) )
    {
        _terrainOptionsConf = conf.child( "terrain" );
        if ( _terrainOptions )
        {
            delete _terrainOptions;
            _terrainOptions = 0L;
        }
    }
}

void
MapNodeOptions::setTerrainOptions( const TerrainOptions& options )
{
    _terrainOptionsConf = options.getConfig();
    if ( _terrainOptions )
    {
        delete _terrainOptions;
        _terrainOptions = 0L;
    }
}

const TerrainOptions&
MapNodeOptions::getTerrainOptions() const
{
    if ( _terrainOptionsConf.isSet() )
    {
        if ( !_terrainOptions )
        {
            const_cast<MapNodeOptions*>(this)->_terrainOptions = new TerrainOptions( _terrainOptionsConf.value() );
        }
        return *_terrainOptions;
    }
    else
    {
        return s_defaultTerrainOptions;
    }
}
