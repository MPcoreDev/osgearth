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
#include <osgEarth/MPScreenSpaceLayoutSG>
#include <osgEarth/Utils>
#include <osgEarth/Containers>
#include <osgEarth/Extension>
#include <osg/LineSegment>

// -----------------------------------------------------------
// This class is mainly copied from ScreenSpaceLayout.cpp
// but with performance improvements
// -----------------------------------------------------------


#define LC "[MPScreenSpaceLayoutSG] "

using namespace osgEarth;

//----------------------------------------------------------------------------

namespace
{

// Sort wrapper to satisfy the template processor.
struct MPSortContainer
{
    MPSortContainer( DeclutterSortFunctor& f ) : _f(f) { }
    const DeclutterSortFunctor& _f;
    bool operator()( const osgUtil::RenderLeaf* lhs, const osgUtil::RenderLeaf* rhs ) const
    {
        return _f(lhs, rhs);
    }
};

// Group drawables that are attached to the same feature, then sort them by priority
struct SortByPriority : public DeclutterSortFunctor
{
    bool operator()( const osgUtil::RenderLeaf* lhs, const osgUtil::RenderLeaf* rhs ) const
    {
        const MPScreenSpaceGeometry* lhsanno = static_cast<const MPScreenSpaceGeometry*>(lhs->_drawable.get());
        const MPScreenSpaceGeometry* rhsanno = static_cast<const MPScreenSpaceGeometry*>(rhs->_drawable.get());

        double lhsPriority = lhsanno->_priority ;
        double rhsPriority = rhsanno->_priority;
        if ( lhsPriority != rhsPriority )
            return lhsPriority > rhsPriority;

        // then fallback on traversal order.
#if OSG_VERSION_GREATER_THAN(3,6,0)
        return lhs->_traversalOrderNumber < rhs->_traversalOrderNumber;
#else
        int diff = lhs->_traversalNumber - rhs->_traversalNumber;
        return diff < 0.0f;
#endif
    }
};

// Data structure shared across entire layout system.
struct MPScreenSpaceSGLayoutContext : public osg::Referenced
{
    ScreenSpaceLayoutOptions _options;
};

typedef osg::BoundingBox RenderLeafBox;

// Data structure stored one-per-View.
struct PerCamInfo
{
    PerCamInfo() : _lastTimeStamp(0), _firstFrame(true) { }

    // remembers the state of each drawable from the previous pass
    //DrawableMemory _memory;

    // re-usable structures (to avoid unnecessary re-allocation)
    osgUtil::RenderBin::RenderLeafList _passed;
    osgUtil::RenderBin::RenderLeafList _failed;
    std::vector<RenderLeafBox>         _used;

    // time stamp of the previous pass, for calculating animation speed
    osg::Timer_t _lastTimeStamp;
    bool _firstFrame;
    osg::Matrix _lastCamVPW;
};


static bool s_mp_sg_declutteringEnabledGlobally = true;

static bool s_mp_sg_extension_loaded = false;

const osg::Vec3d offset1px(1., 1., 0.);
const osg::Vec3d offset3px(3., 3., 0.);
}

//----------------------------------------------------------------------------

template<typename T>
struct LCGIterator
{
    T& _vec;
    unsigned _seed;
    unsigned _n;
    unsigned _index;
    unsigned _a, _c;
    LCGIterator(T& vec) : _vec(vec), _seed(0u), _index(0u) {
        _n = vec.size();
        _a = _n+1;
        _c = 15487457u; // a very large prime
    }
    bool hasMore() const {
        return _index < _n;
    }
    const typename T::value_type& next() {
        _seed = (_a*_seed + _c) % _n;
        _index++;
        return _vec[_seed];
    }
};

/**
 * A custom RenderLeaf sorting algorithm for decluttering objects.
 *
 * First we sort the leaves front-to-back so that objects closer to the camera
 * get higher priority. If you have installed a custom sorting functor,
 * this is used instead.
 *
 * Next, we go though all the drawables and remove any that try to occupy
 * already-occupied real estate in the 2D viewport. Objects that fail the test
 * go on a "failed" list and are either completely removed from the display
 * or transitioned to a secondary visual state (scaled down, alpha'd down)
 * dependeing on the options setup.
 *
 * Drawables with the same parent (i.e., Geode) are treated as a group. As
 * soon as one passes the occlusion test, all its siblings will automatically
 * pass as well.
 */
struct /*internal*/ MPDeclutterSortSG : public osgUtil::RenderBin::SortCallback
{
    DeclutterSortFunctor* _customSortFunctor;
    MPScreenSpaceSGLayoutContext* _context;

    PerObjectFastMap<osg::Camera*, PerCamInfo> _perCam;

    /**
     * Constructs the new sorter.
     * @param f Custom declutter sorting predicate. Pass NULL to use the
     *          default sorter (sort by distance-to-camera).
     */
    MPDeclutterSortSG( MPScreenSpaceSGLayoutContext* context, DeclutterSortFunctor* f = nullptr )
        : _context(context), _customSortFunctor(f)
    {
        //nop
    }

    // Update the offset so that the drawable is always visible and constraint on a line
    void updateOffsetForAutoLabelOnLine(const osg::BoundingBox& box, const osg::Viewport* vp,
                                        const osg::Vec3f& loc, const MPScreenSpaceGeometry* layoutData,
                                        const osg::Matrix& camVPW, osg::Vec3f& offset, const osg::Vec3f& to)
    {
        // impossible to work when z greater then 1
        // TODO improve
        if (/*loc.z() < -1 ||*/ loc.z() > 1)
            return;

        //        OE_WARN << "------------------------------------------\n";
        //        OE_WARN << "loc " << loc.x() << " " << loc.y() << "\n";
        //        OE_WARN << "to " << to.x() << " " << to.y() << "\n";

        float vpX = static_cast<float>(vp->x());
        float vpY = static_cast<float>(vp->y());
        float vpWidth = static_cast<float>(vp->width());
        float vpHeight = static_cast<float>(vp->height());

        // inits
        float leftMin = vpX - box.xMin() + offset.x();
        float rightMax = vpX + vpWidth - box.xMax() + offset.x();
        float bottomMin = vpY - box.yMin() + offset.y();
        float topMax = vpY + vpHeight - box.yMax() + offset.y();
        bool isResolved = false;
        bool maxPointIsDef = false;
        osg::Vec3f linePt;
        bool toIsDef = to.x() != 0.f && to.y() != 0.f && to.z() != 0.f;

        // must go to the right
        if (loc.x() < leftMin)
        {
            if (toIsDef)
            {
                linePt = to;
            }
            else
            {
                linePt = layoutData->getLineEndPoint() * camVPW;
                if (linePt.x() < loc.x() || linePt.z() < -1 || linePt.z() > 1)
                    linePt = layoutData->getLineStartPoint() * camVPW;
            }
            maxPointIsDef = true;

            if (linePt.x() >= (leftMin - (box.xMax() - box.xMin())))
            {
                float ratio = (leftMin - loc.x()) / (linePt.x() - loc.x());
                if (ratio < 1)
                    offset.set(leftMin - loc.x(), ratio * (linePt.y() - loc.y()), 0.f);
                else
                    offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved = ratio >= 1.f || ((loc.y() + offset.y()) > bottomMin && (loc.y() + offset.y()) < topMax);
            }
            else
            {
                // out of screen : used closest point
                offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved = true;
            }
        }

        // must go up
        if (!isResolved && loc.y() < bottomMin)
        {
            if (!maxPointIsDef)
            {
                if (toIsDef)
                {
                    linePt = to;
                }
                else
                {
                    linePt = layoutData->getLineEndPoint() * camVPW;
                    if (linePt.y() < loc.y() || linePt.z() < -1 || linePt.z() > 1)
                        linePt = layoutData->getLineStartPoint() * camVPW;
                }
                maxPointIsDef = true;
            }

            if (linePt.y() >= (bottomMin - (box.yMax() - box.yMin())))
            {
                float ratio = (bottomMin - loc.y()) / (linePt.y() - loc.y());
                if (ratio < 1)
                    offset.set(ratio * (linePt.x() - loc.x()), bottomMin - loc.y(), 0.f);
                else
                    offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved =
                        ratio >= 1.f || ((loc.x() + offset.x()) > leftMin && (loc.x() + offset.x()) < rightMax);
            }
            else
            {
                // out of screen : used closest point
                offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved = true;
            }
        }

        // must go to the left
        if (!isResolved && loc.x() > rightMax)
        {
            if (!maxPointIsDef)
            {
                if (toIsDef)
                {
                    linePt = to;
                }
                else
                {
                    linePt = layoutData->getLineEndPoint() * camVPW;
                    if (linePt.x() > loc.x() || linePt.z() < -1 || linePt.z() > 1)
                        linePt = layoutData->getLineStartPoint() * camVPW;
                }
                maxPointIsDef = true;
            }

            if (linePt.x() <= (rightMax + (box.xMax() - box.xMin())))
            {
                float ratio = (rightMax - loc.x()) / (linePt.x() - loc.x());
                if (ratio < 1)
                    offset.set(rightMax - loc.x(), ratio * (linePt.y() - loc.y()), 0.f);
                else
                    offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved =
                        ratio >= 1.f || ((loc.y() + offset.y()) > bottomMin && (loc.y() + offset.y()) < topMax);
            }
            else
            {
                // out of screen : used closest point
                offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved = true;
            }
        }

        // must go down
        if (!isResolved && loc.y() > topMax)
        {
            if (!maxPointIsDef)
            {
                if (toIsDef)
                {
                    linePt = to;
                }
                else
                {
                    linePt = layoutData->getLineEndPoint() * camVPW;
                    if (linePt.y() > loc.y() || linePt.z() < -1 || linePt.z() > 1)
                        linePt = layoutData->getLineStartPoint() * camVPW;
                }
                maxPointIsDef = true;
            }

            if (linePt.y() <= (topMax + (box.yMax() - box.yMin())))
            {
                float ratio = (topMax - loc.y()) / (linePt.y() - loc.y());
                if (ratio < 1)
                    offset.set(ratio * (linePt.x() - loc.x()), topMax - loc.y(), 0.f);
                else
                    offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved =
                        ratio >= 1.f || ((loc.x() + offset.x()) > leftMin && (loc.x() + offset.x()) < rightMax);
            }
            else
            {
                // out of screen : used closest point
                offset.set(linePt.x() - loc.x(), linePt.y() - loc.y(), 0.f);
                isResolved = true;
            }
        }
    }

    // override.
    // Sorts the bin. This runs in the CULL thread after the CULL traversal has completed.
    void sortImplementation(osgUtil::RenderBin* bin)
    {
        const ScreenSpaceLayoutOptions& options = _context->_options;

        osgUtil::RenderBin::RenderLeafList& leaves = bin->getRenderLeafList();

        bin->copyLeavesFromStateGraphListToRenderLeafList();

        // nothing to sort? bail out
        if ( leaves.size() == 0 )
            return;

        // if there's a custom sorting function installed
        if ( _customSortFunctor && s_mp_sg_declutteringEnabledGlobally )
            std::sort( leaves.begin(), leaves.end(), MPSortContainer( *_customSortFunctor ) );

        // access the view-specific persistent data:
        osg::Camera* cam = bin->getStage()->getCamera();

        // bail out if this camera is a master camera with no GC
        // (e.g., in a multi-screen layout)
        if (cam == nullptr || (cam->getGraphicsContext() == nullptr && !cam->isRenderToTextureCamera()))
            return;

        PerCamInfo& local = _perCam.get( cam );

        osg::Timer_t now = osg::Timer::instance()->tick();
        if (local._firstFrame)
        {
            local._firstFrame = false;
            local._lastTimeStamp = now;
        }

        // calculate the elapsed time since the previous pass; we'll use this for
        // the animations
        double elapsedSeconds = osg::Timer::instance()->delta_s(local._lastTimeStamp, now);
        local._lastTimeStamp = now;

        // Reset the local re-usable containers
        local._passed.clear();          // drawables that pass occlusion test
        local._failed.clear();          // drawables that fail occlusion test
        local._used.clear();            // list of occupied bounding boxes in screen space

        // compute a window matrix so we can do window-space culling.
        const osg::Viewport* vp = cam->getViewport();
        double vpXMin = vp->x();
        double vpYMin = vp->y();
        //        double vpWidth = vp->width();
        //        double vpHeight = vp->height();
        //        double vpXMax = vpXMin + vp->width();
        //        double vpYMax = vpYMin + vp->height();
        osg::Matrix windowMatrix = vp->computeWindowMatrix();
        osg::Vec3d eye, center, up, look;
        cam->getViewMatrixAsLookAt(eye, center, up);
        look = center - eye;
        look.normalize();

        int screenMapNbCol = options.screenGridNbCol().get();
        int screenMapNbRow = options.screenGridNbRow().get();
        const bool useScreenGrid = options.useScreenGrid().get();
        double mapSizeX = vp->width() / screenMapNbCol;
        double mapSizeY = vp->height() / screenMapNbRow;
        std::vector<std::vector<std::vector<RenderLeafBox>>> _usedMap(screenMapNbCol, std::vector<std::vector<RenderLeafBox>>(screenMapNbRow));

        unsigned limit = *options.maxObjects();

        double minAnimationScale = static_cast<double>(*options.minAnimationScale());
        float inAnimationTime = *options.inAnimationTime();
        float outAnimationTime = *options.outAnimationTime();

        //        bool snapToPixel = options.snapToPixel() == true;

        osg::Matrix camVPW;
        camVPW.postMult(cam->getViewMatrix());
        camVPW.postMult(cam->getProjectionMatrix());
        camVPW.postMult(windowMatrix);

        // has the camera moved?
        //        bool camChanged = camVPW != local._lastCamVPW;
        local._lastCamVPW = camVPW;

        osg::Vec3f offset;

        // Go through each leaf and test for visibility.
        // Enforce the "max objects" limit along the way.
        for(osgUtil::RenderBin::RenderLeafList::iterator i = leaves.begin();
            i != leaves.end() && local._passed.size() < limit;
            ++i )
        {
            osgUtil::RenderLeaf* leaf = *i;
            if ( ! leaf->_drawable.valid() )
                continue;

            MPScreenSpaceGeometry* annoDrawable = static_cast<MPScreenSpaceGeometry*>(leaf->_drawable.get());

            // transform the bounding box of the drawable into window-space.
            //osg::BoundingBox box = annoDrawable->isAutoFollowLine() ? annoDrawable->getBBox() : annoDrawable->getBoundingBox();
            osg::BoundingBox box = annoDrawable->getBoundingBox();

            double angle = 0;
            osg::Quat rot;
            osg::Vec3d to;
            osg::Vec3f pos(annoDrawable->_cull_anchorOnScreen);
            bool visible = true;

            // local transformation data
            // and management of the label orientation (must be always readable)
            if (annoDrawable->isAutoRotate())
            {
                angle = annoDrawable->_cull_rotationRadOnScreen;
            }

            // handle the local rotation
            if ( angle != 0. )
            {
                rot.makeRotate ( angle, osg::Vec3d(0, 0, 1) );
                osg::Vec3f ld = rot * ( osg::Vec3f(box.xMin(), box.yMin(), 0.) );
                osg::Vec3f lu = rot * ( osg::Vec3f(box.xMin(), box.yMax(), 0.) );
                osg::Vec3f ru = rot * ( osg::Vec3f(box.xMax(), box.yMax(), 0.) );
                osg::Vec3f rd = rot * ( osg::Vec3f(box.xMax(), box.yMin(), 0.) );
                if ( angle > - osg::PI / 2. && angle < osg::PI / 2. )
                    box.set( osg::minimum(ld.x(), lu.x()), osg::minimum(ld.y(), rd.y()), 0,
                             osg::maximum(rd.x(), ru.x()), osg::maximum(lu.y(), ru.y()), 0 );
                else
                    box.set(osg::minimum(rd.x(), ru.x()), osg::minimum(lu.y(), ru.y()), 0,
                            osg::maximum(ld.x(), lu.x()), osg::maximum(ld.y(), rd.y()), 0);
            }

            // adapt the offset for auto sliding label
            if (annoDrawable->isAutoFollowLine())
            {
                osg::Vec3f slidingOffset;
                updateOffsetForAutoLabelOnLine(box, vp, pos, annoDrawable, camVPW, slidingOffset, to);
                pos += slidingOffset;
            }
            
            
            if(annoDrawable->screenClamping()){
                
                // Calculate the "clip to world" matrix = MVPinv.
                osg::Matrix MVP = (cam->getViewMatrix()) * cam->getProjectionMatrix();
                osg::Matrix MVPinv;
                MVPinv.invert(MVP);
                    
                osg::Vec3d pw1=annoDrawable->getLineStartPoint();
                osg::Vec3d pw2=annoDrawable->getLineEndPoint();
                
                osg::Vec3d pc1=pw1*MVP;
                osg::Vec3d pc2=pw2*MVP;
                               
                
                bool p1_in_width=pc1.x()<1.0 && pc1.x()>-1.0;
                bool p1_in_height=pc1.y()<1.0 && pc1.y()>-1.0;
             
                bool p1_inside_screen = p1_in_width && p1_in_height;
                
                bool p2_in_width=pc2.x()<1.0 && pc2.x()>-1.0;
                bool p2_in_height=pc2.y()<1.0 && pc2.y()>-1.0;
         
                bool p2_inside_screen = p2_in_width && p2_in_height;                
                
                
                bool line_inside_screen=p1_inside_screen && p2_inside_screen;  
                
             
                
                osg::BoundingBox bb (-1.0,-1.0,-1.0,1.0,1.0,1.0); 
                visible = ( !line_inside_screen) && !bb.contains(pc1);
           
                osg::ref_ptr<osg::LineSegment> seg;
                
                if(visible){ 
                    
                    if(!seg.valid())
                        seg = new osg::LineSegment (pc1,pc2);
                    else
                        seg->set(pc1,pc2);
                    float r1=0,r2=0;
                    visible = seg->intersectAndComputeRatios(bb,r1,r2) ;
                    
                    if(visible)
                    {
    
    //                    if( layoutData->getInstanceIndex() ==0) 
    //                    {
                          //  OE_DEBUG<<" r1= "<<r1<<"r2"<<r2 <<std::endl;
                            
                            pos= (pw1 + (pw2-pw1)*r1)*MVP;
                            
                            0.5*(pw1.length()+pw2.length());
                            
    //                    }
    //                    else
    //                    {
    //                         OE_DEBUG<<" r2= "<<r2<<std::endl;
    //                        pos= (pc1 - (pc2-pc1)*r2);
    //                    }
                        
                        float x_bbox=0;
                        float y_bbox=0;
                        
                        if(pos.x() >=0.99){
                            x_bbox = box.xMax() ;
                        }else if(pos.x() <=-0.99){
                            x_bbox = box.xMin() ;
                        }
                        
                        if(pos.y() >=0.99){
                            y_bbox = box.yMax() ;
                        }else if(pos.y() <=-0.99){
                            y_bbox = box.yMin() ;
                        }
                        
                        
                        pos = pos * windowMatrix;
                        pos.x()-=x_bbox;
                        pos.y()-=y_bbox;
                    }
                     
                     
//                    OE_DEBUG<<" geomLineString "<<layoutData->getId()<<" "<<layoutData->getInstanceIndex() 
//                           <<" p1"<<pw1.x()<<" "<<pw1.y()<<" "<<pw1.z()<<"  p2"<<pw2.x()<<" "<<pw2.y()<<" "<<pw2.z()<<std::endl;
//                    OE_DEBUG<<layoutData->getInstanceIndex() <<" p2="<<pc2.x()<<" "<<pc2.y()<<" "<<pc2.z()
//                           <<" p1="<<pc1.x()<<" "<<pc1.y()<<" "<<pc1.z()
                             
//                          <<" i="<<pos.x()<<" "<<pos.y()<<" "<<pos.z()<<std::endl;
                }
                
          }


            //            // Expand the box if this object is currently not visible, so that it takes a little
            //            // more room for it to before visible once again.
            //            DrawableInfo& info = local._memory[drawable];
            //            float buffer = info._visible ? 1.0f : 3.0f;
            //DrawableInfo& info = local._memory[annoDrawable];

            const osg::Vec3 &buffer = annoDrawable->_declutter_visible ? offset1px : offset3px;

            //            // The "declutter" box is the box we use to reserve screen space.
            //            // This must be unquantized regardless of whether snapToPixel is set.
            //            box.set(
            //                        floor(layoutData->_cull_anchorOnScreen.x() + box.xMin())-buffer,
            //                        floor(layoutData->_cull_anchorOnScreen.y() + box.yMin())-buffer,
            //                        layoutData->_cull_anchorOnScreen.z(),
            //                        ceil(layoutData->_cull_anchorOnScreen.x() + box.xMax())+buffer,
            //                        ceil(layoutData->_cull_anchorOnScreen.y() + box.yMax())+buffer,
            //                        layoutData->_cull_anchorOnScreen.z() );

            //            // if snapping is enabled, only snap when the camera stops moving.
            //            bool quantize = snapToPixel;
            //            if ( quantize && !camChanged )
            //            {
            //                // Quanitize the window draw coordinates to mitigate text rendering filtering anomalies.
            //                // Drawing text glyphs on pixel boundaries mitigates aliasing.
            //                // Adding 0.5 will cause the GPU to sample the glyph texels exactly on center.
            //                layoutData->_cull_anchorOnScreen.x() = floor(layoutData->_cull_anchorOnScreen.x()) + 0.5;
            //                layoutData->_cull_anchorOnScreen.y() = floor(layoutData->_cull_anchorOnScreen.y()) + 0.5;
            //            }

            box.set( box._min + pos - buffer, box._max + pos + buffer);

            int mapStartX,  mapStartY, mapEndX, mapEndY;

            if ( s_mp_sg_declutteringEnabledGlobally )
            {

                // A max priority => never occlude.
                double priority = annoDrawable->_priority;

                if ( useScreenGrid )
                {
                    mapStartX = osg::clampTo(static_cast<int>(floor((box.xMin() - vpXMin) / mapSizeX)), 0, screenMapNbCol-1);
                    mapStartY = osg::clampTo(static_cast<int>(floor((box.yMin() - vpYMin) / mapSizeY)), 0, screenMapNbRow-1);
                    mapEndX = osg::clampTo(static_cast<int>(floor((box.xMax() - vpXMin) / mapSizeX)), 0, screenMapNbCol-1);
                    mapEndY = osg::clampTo(static_cast<int>(floor((box.yMax() - vpYMin) / mapSizeY)), 0, screenMapNbRow-1);
                }

                if ( priority == DBL_MAX || ! annoDrawable->_declutterActivated)
                {
                    visible = true;
                }

                else
                {

                    // declutter only on screen cells that intersects the current bbox cells
                    if ( useScreenGrid )
                    {
                        for ( int mapX = mapStartX ; mapX <= mapEndX && visible ; ++mapX )
                            for ( int mapY = mapStartY ; mapY <= mapEndY && visible ; ++mapY )
                                for( std::vector<RenderLeafBox>::const_iterator j = _usedMap[mapX][mapY].begin(); j != _usedMap[mapX][mapY].end(); ++j )
                                {
                                    // only need a 2D test since we're in clip space
                                    bool isClear = osg::maximum(box.xMin(), j->xMin()) > osg::minimum(box.xMax(), j->xMax()) ||
                                            osg::maximum(box.yMin(), j->yMin()) > osg::minimum(box.yMax(), j->yMax());

                                    // if there's an overlap (and the conflict isn't from the same drawable
                                    // parent, which is acceptable), then the leaf is culled.
                                    if ( ! isClear )
                                    {
                                        visible = false;
                                        break;
                                    }
                                }
                    }

                    // declutter in full screen
                    else
                    {
                        // weed out any drawables that are obscured by closer drawables.
                        // TODO: think about a more efficient algorithm - right now we are just using
                        // brute force to compare all bbox's
                        for( std::vector<RenderLeafBox>::const_iterator j = local._used.begin(); j != local._used.end(); ++j )
                        {
                            // only need a 2D test since we're in clip space
                            bool isClear = osg::maximum(box.xMin(),j->xMin()) > osg::minimum(box.xMax(),j->xMax()) ||
                                    osg::maximum(box.yMin(),j->yMin()) > osg::minimum(box.yMax(),j->yMax());

                            // if there's an overlap then the leaf is culled.
                            if ( ! isClear )
                            {
                                visible = false;
                                break;
                            }
                        }
                    }
                }

                if ( visible )
                {
                    // passed the test, so add the leaf's bbox to the "used" list, and add the leaf
                    // to the final draw list.
                    if ( annoDrawable->_declutterActivated )
                        local._used.push_back( box );
                    local._passed.push_back( leaf );

                    if ( useScreenGrid )
                        for ( int mapX = mapStartX ; mapX <= mapEndX ; ++mapX )
                            for ( int mapY = mapStartY ; mapY <= mapEndY ; ++mapY )
                                _usedMap[mapX][mapY].push_back( local._used.back() );
                }

                if ( ! visible )
                {
                    local._failed.push_back( leaf );
                }
            }

            // no declutter
            else
            {
                // passed the test, so add the leaf's bbox to the "used" list, and add the leaf
                // to the final draw list.
                local._used.push_back( box );
                local._passed.push_back( leaf );
            }

            osg::Matrix newModelView;
            newModelView.makeTranslate(static_cast<double>(pos.x()), static_cast<double>(pos.y()), 0);
            if (! rot.zeroRotation())
                newModelView.preMultRotate(rot);
            //newModelView.preMultTranslate(offset);

            // Leaf modelview matrixes are shared (by objects in the traversal stack) so we
            // cannot just replace it unfortunately. Have to make a new one. Perhaps a nice
            // allocation pool is in order here
            leaf->_modelview = new osg::RefMatrix(newModelView);

        } // end for each leaf

        // copy the final draw list back into the bin, rejecting any leaves whose parents
        // are in the cull list.
        if ( s_mp_sg_declutteringEnabledGlobally )
        {
            leaves.clear();
            for( osgUtil::RenderBin::RenderLeafList::const_iterator i=local._passed.begin(); i != local._passed.end(); ++i )
            {
                osgUtil::RenderLeaf* leaf     = *i;
                MPScreenSpaceGeometry* annoDrawable = static_cast<MPScreenSpaceGeometry*>(leaf->_drawable.get());
                //DrawableInfo& info = local._memory[annoDrawable];
                bool fullyIn = true;
                bool displaySomethingInCluttered = annoDrawable->drawInClutteredMode();
                if ( displaySomethingInCluttered )
                    annoDrawable->activeClutteredDrawMode( false );

                // scale in until at full scale:
                if ( annoDrawable->_declutter_lastScale != 1. )
                {
                    fullyIn = false;
                    if ( annoDrawable->_declutter_lastScale == 0. )
                        annoDrawable->_declutter_lastScale = minAnimationScale;
                    annoDrawable->_declutter_lastScale += elapsedSeconds / osg::maximum(inAnimationTime, 0.001f);
                    if ( annoDrawable->_declutter_lastScale > 1. )
                        annoDrawable->_declutter_lastScale = 1.;
                }

                if ( annoDrawable->_declutter_lastScale != 1. )
                    leaf->_modelview->preMult( osg::Matrix::scale(annoDrawable->_declutter_lastScale, annoDrawable->_declutter_lastScale, 1) );

                leaves.push_back( leaf );

                annoDrawable->_declutter_isInitialised = true;
                annoDrawable->_declutter_visible = true;
            }

            // next, go through the FAILED list and sort them into failure bins so we can draw
            // them using a different technique if necessary.
            for( osgUtil::RenderBin::RenderLeafList::const_iterator i=local._failed.begin(); i != local._failed.end(); ++i )
            {
                osgUtil::RenderLeaf* leaf =     *i;
                MPScreenSpaceGeometry* annoDrawable = static_cast<MPScreenSpaceGeometry*>(leaf->_drawable.get());
                bool displaySomethingInCluttered = annoDrawable->drawInClutteredMode();
                if ( displaySomethingInCluttered )
                    annoDrawable->activeClutteredDrawMode( true );

                //DrawableInfo& info = local._memory[drawable];

                if (annoDrawable->_declutter_isInitialised)
                {
                    // the drawable is failed but has not reached its out animation
                    if ( annoDrawable->_declutter_lastScale > minAnimationScale )
                    {
                        // case no out animation exists
                        if ( outAnimationTime <= 0.001f)
                        {
                            if ( displaySomethingInCluttered )
                                annoDrawable->_declutter_lastScale = minAnimationScale;
                            else
                                annoDrawable->_declutter_lastScale = 0.;
                        }

                        // case out animation exists
                        else
                        {
                            annoDrawable->_declutter_lastScale -= elapsedSeconds / osg::maximum(outAnimationTime, 0.001f);
                            if ( annoDrawable->_declutter_lastScale <= minAnimationScale )
                            {
                                if ( displaySomethingInCluttered )
                                    annoDrawable->_declutter_lastScale = minAnimationScale;
                                else
                                    annoDrawable->_declutter_lastScale = 0.;
                            }
                        }
                    }
                }
                else
                {
                    // prevent first-frame "pop out"
                    if ( displaySomethingInCluttered )
                        annoDrawable->_declutter_lastScale = minAnimationScale;
                    else
                        annoDrawable->_declutter_lastScale = 0.;
                }

                if ( annoDrawable->_declutter_lastScale > 0. )
                {
                    leaves.push_back( leaf );

                    // scale it:
                    if ( annoDrawable->_declutter_lastScale != 1. )
                        leaf->_modelview->preMult( osg::Matrix::scale(annoDrawable->_declutter_lastScale, annoDrawable->_declutter_lastScale, 1) );
                }

                annoDrawable->_declutter_isInitialised = true;
                annoDrawable->_declutter_visible = false;
            }
        }
    }
};

namespace
{
/**
 * Custom draw routine for our declutter render bin.
 */
struct MPDeclutterDraw : public osgUtil::RenderBin::DrawCallback
{
    MPScreenSpaceSGLayoutContext*              _context;
    PerThread< osg::ref_ptr<osg::RefMatrix> > _ortho2D;

    /**
     * Constructs the decluttering draw callback.
     * @param context A shared context among all decluttering objects.
     */
    MPDeclutterDraw( MPScreenSpaceSGLayoutContext* context )
        : _context( context )
    {
    }

    /**
     * Draws a bin. Most of this code is copied from osgUtil::RenderBin::drawImplementation.
     * The modifications are (a) skipping code to render child bins, (b) setting a bin-global
     * projection matrix in orthographic space, and (c) calling our custom "renderLeaf()" method
     * instead of RenderLeaf::render()
     */
    void drawImplementation( osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous )
    {
        osg::State& state = *renderInfo.getState();

        unsigned int numToPop = (previous ? osgUtil::StateGraph::numToPop(previous->_parent) : 0);
        if (numToPop>1) --numToPop;
        unsigned int insertStateSetPosition = state.getStateSetStackSize() - numToPop;

        if (bin->getStateSet())
        {
            state.insertStateSet(insertStateSetPosition, bin->getStateSet());
        }

        // apply a window-space projection matrix.
        const osg::Viewport* vp = renderInfo.getCurrentCamera()->getViewport();
        if ( vp )
        {
            //TODO see which is faster

            osg::ref_ptr<osg::RefMatrix>& m = _ortho2D.get();
            if ( !m.valid() )
                m = new osg::RefMatrix();

            //m->makeOrtho2D( vp->x(), vp->x()+vp->width()-1, vp->y(), vp->y()+vp->height()-1 );
            m->makeOrtho( vp->x(), vp->x()+vp->width()-1, vp->y(), vp->y()+vp->height()-1, -1000, 1000);
            state.applyProjectionMatrix( m.get() );
        }

        // render the list
        osgUtil::RenderBin::RenderLeafList& leaves = bin->getRenderLeafList();

        for(osgUtil::RenderBin::RenderLeafList::reverse_iterator rlitr = leaves.rbegin();
            rlitr!= leaves.rend();
            ++rlitr)
        {
            osgUtil::RenderLeaf* rl = *rlitr;
            renderLeaf( rl, renderInfo, previous );
            previous = rl;
        }

        if ( bin->getStateSet() )
        {
            state.removeStateSet(insertStateSetPosition);
        }
    }

    /**
     * Renders a single leaf. We already applied the projection matrix, so here we only
     * need to apply a modelview matrix that specifies the ortho offset of the drawable.
     *
     * Most of this code is copied from RenderLeaf::draw() -- but I removed all the code
     * dealing with nested bins, since decluttering does not support them.
     */
    void renderLeaf( osgUtil::RenderLeaf* leaf, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous )
    {
        osg::State& state = *renderInfo.getState();

        // don't draw this leaf if the abort rendering flag has been set.
        if (state.getAbortRendering())
        {
            //cout << "early abort"<<endl;
            return;
        }

        state.applyModelViewMatrix( leaf->_modelview.get() );

        if (previous)
        {
            // apply state if required.
            osgUtil::StateGraph* prev_rg = previous->_parent;
            osgUtil::StateGraph* prev_rg_parent = prev_rg->_parent;
            osgUtil::StateGraph* rg = leaf->_parent;
            if (prev_rg_parent!=rg->_parent)
            {
                osgUtil::StateGraph::moveStateGraph(state,prev_rg_parent,rg->_parent);

                // send state changes and matrix changes to OpenGL.
                state.apply(rg->getStateSet());

            }
            else if (rg!=prev_rg)
            {
                // send state changes and matrix changes to OpenGL.
                state.apply(rg->getStateSet());
            }
        }
        else
        {
            // apply state if required.
            osgUtil::StateGraph::moveStateGraph(state, nullptr, leaf->_parent->_parent);

            state.apply(leaf->_parent->getStateSet());
        }

        // if we are using osg::Program which requires OSG's generated uniforms to track
        // modelview and projection matrices then apply them now.
        if (state.getUseModelViewAndProjectionUniforms())
            state.applyModelViewAndProjectionUniformsIfRequired();

        // draw the drawable
        leaf->_drawable->draw(renderInfo);

        if (leaf->_dynamic)
            state.decrementDynamicObjectCount();
    }
};
}

//----------------------------------------------------------------------------

/**
 * The actual custom render bin
 * This wants to be in the global scope for the dynamic registration to work,
 * hence the annoyinging long class name
 */
class osgEarthMPScreenSpaceLayoutSGRenderBin : public osgUtil::RenderBin
{
public:
    osgEarthMPScreenSpaceLayoutSGRenderBin()
    {
        this->setName( OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN );
        _context = new MPScreenSpaceSGLayoutContext();
        clearSortingFunctor();
        setDrawCallback( new MPDeclutterDraw(_context.get()) );

        // needs its own state set for special magic.
        osg::StateSet* stateSet = new osg::StateSet();
        this->setStateSet( stateSet );
    }

    osgEarthMPScreenSpaceLayoutSGRenderBin(const osgEarthMPScreenSpaceLayoutSGRenderBin& rhs, const osg::CopyOp& copy)
        : osgUtil::RenderBin(rhs, copy),
          _f(rhs._f.get()),
          _context(rhs._context.get())
    {
    }

    virtual osg::Object* clone(const osg::CopyOp& copyop) const
    {
        return new osgEarthMPScreenSpaceLayoutSGRenderBin(*this, copyop);
    }

    void setSortingFunctor( DeclutterSortFunctor* f )
    {
        _f = f;
        setSortCallback( new MPDeclutterSortSG(_context.get(), f) );
    }

    void clearSortingFunctor()
    {
        setSortCallback( new MPDeclutterSortSG(_context.get()) );
    }

    osg::ref_ptr<DeclutterSortFunctor> _f;
    osg::ref_ptr<MPScreenSpaceSGLayoutContext> _context;
};


//----------------------------------------------------------------------------

void
MPScreenSpaceLayoutSG::activate(osg::StateSet* stateSet) //, int binNum)
{
    if ( stateSet )
    {
        int binNum = getOptions().renderOrder().get();

        // the OVERRIDE prevents subsequent statesets from disabling the layout bin
        stateSet->setRenderBinDetails(
                    binNum,
                    OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN,
                    osg::StateSet::OVERRIDE_PROTECTED_RENDERBIN_DETAILS);

        // Force a single shared layout bin per render stage
        stateSet->setNestRenderBins( false );

        // Range opacity is not supported for screen-space rendering
        stateSet->setDefine("OE_DISABLE_RANGE_OPACITY");
    }
}

void
MPScreenSpaceLayoutSG::deactivate(osg::StateSet* stateSet)
{
    if (stateSet)
    {
        stateSet->setRenderBinToInherit();
        stateSet->setNestRenderBins(true);
    }
}

void
MPScreenSpaceLayoutSG::setDeclutteringEnabled(bool enabled)
{
    s_mp_sg_declutteringEnabledGlobally = enabled;
}

void
MPScreenSpaceLayoutSG::setSortFunctor( DeclutterSortFunctor* functor )
{
    // pull our prototype
    osgEarthMPScreenSpaceLayoutSGRenderBin* bin = dynamic_cast<osgEarthMPScreenSpaceLayoutSGRenderBin*>(
                osgUtil::RenderBin::getRenderBinPrototype( OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN ) );

    if ( bin )
    {
        bin->setSortingFunctor( functor );
    }
}

void
MPScreenSpaceLayoutSG::clearSortFunctor()
{
    // pull our prototype
    osgEarthMPScreenSpaceLayoutSGRenderBin* bin = dynamic_cast<osgEarthMPScreenSpaceLayoutSGRenderBin*>(
                osgUtil::RenderBin::getRenderBinPrototype( OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN ) );

    if ( bin )
    {
        bin->clearSortingFunctor();
    }
}

void
MPScreenSpaceLayoutSG::setOptions( const ScreenSpaceLayoutOptions& options )
{
    // pull our prototype
    osgEarthMPScreenSpaceLayoutSGRenderBin* bin = dynamic_cast<osgEarthMPScreenSpaceLayoutSGRenderBin*>(
                osgUtil::RenderBin::getRenderBinPrototype( OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN ) );

    if ( bin )
    {
        MPScreenSpaceLayoutSG::setSortFunctor(new SortByPriority());

        if ( options.useScreenGrid().get() )
        {
            OE_INFO << LC << "Decluttering will use a screen grid of " <<
                       options.screenGridNbCol().get() << " * " <<options.screenGridNbRow().get() << "\n";
        }

        s_mp_sg_extension_loaded = true;
        OE_INFO << LC << "Use mp_screen_annotations as default text provider\n";

        // communicate the new options on the shared context.
        bin->_context->_options = options;
    }
}

const ScreenSpaceLayoutOptions&
MPScreenSpaceLayoutSG::getOptions()
{
    static ScreenSpaceLayoutOptions s_defaultOptions;

    // pull our prototype
    osgEarthMPScreenSpaceLayoutSGRenderBin* bin = dynamic_cast<osgEarthMPScreenSpaceLayoutSGRenderBin*>(
                osgUtil::RenderBin::getRenderBinPrototype( OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN ) );

    if ( bin )
        return bin->_context->_options;
    else
        return s_defaultOptions;
}

bool
MPScreenSpaceLayoutSG::isExtensionLoaded()
{
    return s_mp_sg_extension_loaded;
}


//----------------------------------------------------------------------------

/** the actual registration. */
extern "C" void osgEarth_mp_sg_declutter(void) {}
static osgEarthRegisterRenderBinProxy<osgEarthMPScreenSpaceLayoutSGRenderBin> s_regbin(OSGEARTH_MP_SCREEN_SPACE_LAYOUT_SG_BIN);


//----------------------------------------------------------------------------

// Extension for configuring the decluterring/SSL options from an Earth file.
namespace osgEarth
{
class MPScreenSpaceLayoutSGExtension : public Extension,
        public ScreenSpaceLayoutOptions
{
public:
    META_OE_Extension(osgEarth, MPScreenSpaceLayoutSGExtension, mp_screen_space_layout_sg)

    MPScreenSpaceLayoutSGExtension() { }

    MPScreenSpaceLayoutSGExtension(const ConfigOptions& co) : ScreenSpaceLayoutOptions(co)
    {
        // sets the global default options.
        MPScreenSpaceLayoutSG::setOptions(*this);
    }

    const ConfigOptions& getConfigOptions() const { return *this; }
};

REGISTER_OSGEARTH_EXTENSION(osgearth_mp_screen_space_layout_sg, MPScreenSpaceLayoutSGExtension);
REGISTER_OSGEARTH_EXTENSION(osgearth_mp_decluttering_sg,        MPScreenSpaceLayoutSGExtension);
}

