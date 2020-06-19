
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2008-2016 Pelican Mapping
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
#include <osgEarth/ElevationPool>
#include <osgEarth/Map>
#include <osgEarth/Metrics>
#include <osgEarth/rtree.h>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/Registry>
#include <osgEarth/Containers>

#include <thread>
#include <chrono>

using namespace osgEarth;

#define LC "[ElevationPool] "

void
ElevationPool::MapCallbackAdapter::onMapModelChanged(const MapModelChange& c)
{
    _pool->clear();
}

ElevationPool::ElevationPool() :
    _index(NULL),
    _tileSize(257),
    _mapDataDirty(true),
    _workers(0)
{
    // small L2 cache to use if the caller doesn't supply a working set
    _L2 = new WorkingSet(32u);

    // adapter for detecting elevation layer changes
    _mapCallback = new MapCallbackAdapter();
}

ElevationPool::~ElevationPool()
{
    if (_L2)
        delete _L2;

    setMap(NULL);
}

void
ElevationPool::clear()
{
    _mapDataDirty = true;
}

void
ElevationPool::setMap(const Map* map)
{
    if (map != _map.get())
    {
        osg::ref_ptr<const Map> oldMap;
        if (_map.lock(oldMap))
        {
            oldMap->removeMapCallback(_mapCallback.get());
        }
    }

    _map = map;
    
    if (map)
    {
        _mapCallback->_pool = this;
        map->addMapCallback(_mapCallback.get());
        refresh(map);
    }
}

int
ElevationPool::getElevationRevision(const Map* map) const
{
    // yes, must do this every time because individual
    // layers can "bump" their revisions (dynamic layers)
    int revision = map ? static_cast<int>(map->getDataModelRevision()) : 0;

    for(auto i : _elevationLayers)
        if (i->getEnabled())
            revision += i->getRevision();
    return revision;
}

typedef RTree<unsigned, double, 2> MaxLevelIndex;

void
ElevationPool::sync(const Map* map, WorkingSet* ws)
{
    if (_mapDataDirty)
    {
        while(_workers > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        _refreshMutex.lock();
        if (_mapDataDirty) // double check
        {
            refresh(map);

            if (ws)
                ws->_lru.clear();

            _mapDataDirty = false;
        }
        _refreshMutex.unlock();
    }
}

void
ElevationPool::refresh(const Map* map)
{
    _elevationLayers.clear();

    if (_index)
        delete static_cast<MaxLevelIndex*>(_index);

    map->getLayers(_elevationLayers);

    MaxLevelIndex* index = new MaxLevelIndex();
    _index = index;

    double minv[2], maxv[2];
        
    for(auto i : _elevationLayers)
    {
        const ElevationLayer* layer = i.get();
        const DataExtentList& dataExtents = layer->getDataExtents();

        for(auto de = dataExtents.begin(); de != dataExtents.end(); ++de)
        {
            GeoExtent extentInMapSRS = map->getProfile()->clampAndTransformExtent(*de);

            minv[0] = extentInMapSRS.xMin(), minv[1] = extentInMapSRS.yMin();
            maxv[0] = extentInMapSRS.xMax(), maxv[1] = extentInMapSRS.yMax();

            // Check.
            unsigned maxLevel = layer->getProfile()->getEquivalentLOD(map->getProfile(), de->maxLevel().get());

            index->Insert(minv, maxv, maxLevel);
        }
    }

    _L2->_lru.clear();

    _globalLUT.lock();
    _globalLUT.clear();
    _globalLUT.unlock();
}

unsigned
ElevationPool::getLOD(double x, double y) const
{
    MaxLevelIndex* index = static_cast<MaxLevelIndex*>(_index);

    double minv[2], maxv[2];
    minv[0] = maxv[0] = x, minv[1] = maxv[1] = y;
    std::vector<unsigned> hits;
    index->Search(minv, maxv, &hits, 99);
    unsigned maxiestMaxLevel = 0u;
    for(auto h = hits.begin(); h != hits.end(); ++h)
    {
        maxiestMaxLevel = osg::maximum(maxiestMaxLevel, *h); 
    }
    return maxiestMaxLevel;
}

ElevationPool::WorkingSet::WorkingSet(unsigned size) :
    _lru(true, size)
{
    //nop
}

bool
ElevationPool::findExistingRaster(
    const Internal::RevElevationKey& key,
    WorkingSet* ws,
    osg::ref_ptr<ElevationTexture>& output,
    bool* fromWS,
    bool* fromL2,
    bool* fromLUT)
{   
    *fromWS = false;
    *fromL2 = false;
    *fromLUT = false;

    // First check the workingset. No mutex required since the
    // LRU has its own mutex. (TODO: maybe just combine mutexes here)
    if (ws)
    {
        WorkingSet::LRU::Record record;
        if (ws->_lru.get(key, record))
        {
            OE_DEBUG << LC << key._tilekey.str() << " - Cache hit (Working set)" << std::endl;
            output = record.value();
            *fromWS = true;
            return true;
        }
    }

    if (_L2)
    {
        WorkingSet::LRU::Record record;
        if (_L2->_lru.get(key, record))
        {
            OE_DEBUG << LC << key._tilekey.str() << " - Cache hit (L2 cache)" << std::endl;
            output = record.value();
            *fromL2 = true;
            return true;
        }
    }

    // Next check the system LUT -- see if someone somewhere else
    // already has it (the terrain or another WorkingSet)
    _globalLUT.lock();
    OE_DEBUG << "Global LUT size = " << _globalLUT.size() << std::endl;
    auto i =_globalLUT.find(key);
    if (i != _globalLUT.end())
    {
        i->second.lock(output);
        if (output.valid())
        {
            *fromLUT = true;
        }
        else
        {
            // observer was orphaned..remove it
            _globalLUT.erase(i);
        }
    }
    _globalLUT.unlock();

    // found it, so stick it in the L2 cache
    if (output.valid())
    {
        OE_DEBUG << LC << key._tilekey.str() << " - Cache hit (global LUT)" << std::endl;
    }

    return output.valid();
}

osg::ref_ptr<ElevationTexture>
ElevationPool::getOrCreateRaster(
    const Internal::RevElevationKey& key, 
    const Map* map, 
    bool getResolutions, 
    bool acceptLowerRes,
    WorkingSet* ws)
{
    OE_PROFILING_ZONE;

    // first check for pre-existing data for this key:
    osg::ref_ptr<ElevationTexture> result;
    bool fromWS, fromL2, fromLUT;
    if (findExistingRaster(key, ws, result, &fromWS, &fromL2, &fromLUT))
    {
        // only accept if cached record matches caller's request
        if (getResolutions == true && !result->getResolutions())
        {
            result = NULL;
        }
    }

    if (!result.valid())
    {
        // need to build NEW data for this key
        osg::ref_ptr<osg::HeightField> hf = HeightFieldUtils::createReferenceHeightField(
            key._tilekey.getExtent(),
            _tileSize, _tileSize,
            false,      // no border
            true);      // initialize to HAE (0.0) heights

        float* resolutions = NULL;
        if (getResolutions)
        {
            resolutions = new float[_tileSize*_tileSize];
        }

        TileKey keyToUse;
        bool populated = false;

        const ElevationLayerVector& layersToSample =
            ws && !ws->_elevationLayers.empty() ? ws->_elevationLayers :
            _elevationLayers;

        for(keyToUse = key._tilekey; 
            keyToUse.valid(); 
            keyToUse = keyToUse.createParentKey())
        {
            populated = layersToSample.populateHeightField(
                hf.get(),
                resolutions,
                keyToUse,
                map->getProfileNoVDatum(), // convertToHAE,
                map->getElevationInterpolation(),
                NULL ); // TODO: progress callback

            if (populated==true || acceptLowerRes==false)
                break;
        }

        if (populated)
        {
            result = new ElevationTexture(
                GeoHeightField(hf.get(), keyToUse.getExtent()),
                resolutions);
        }
        else
        {
            return NULL;
        }
    }

    // update WorkingSet:
    if (ws)
    {
        ws->_lru.insert(key, result.get());
    }

    // update if L2 cache, but ONLY if the user did not supply
    // their own working set. We don't want to "pollute" the 
    // shared L2 cache with localized data.
    else if (_L2)
    {
        _L2->_lru.insert(key, result.get());
    }

    // update system weak-LUT:
    if (!fromLUT)
    {
        _globalLUT.lock();
        _globalLUT[key] = result.get();
        _globalLUT.unlock();
    }

    return result;
}

namespace
{
    typedef vector_map<
        Internal::RevElevationKey,
        osg::ref_ptr<ElevationTexture> > QuickCache;

    struct QuickSampleVars {
        double sizeS, sizeT;
        double s, t;
        double s0, s1, smix;
        double t0, t1, tmix;
        osg::Vec4f UL, UR, LL, LR, TOP, BOT;
    };

    inline void quickSample(
        const ImageUtils::PixelReader& reader, 
        double u, double v, 
        osg::Vec4f& out,
        QuickSampleVars& a)
    {
        double sizeS = (double)(reader.s()-1);
        double sizeT = (double)(reader.t()-1);

        // u, v => [0..1]
        double s = u * sizeS;
        double t = v * sizeT;

        double s0 = osg::maximum(floor(s), 0.0);
        double s1 = osg::minimum(s0 + 1.0, sizeS);
        double smix = s0 < s1 ? (s - s0) / (s1 - s0) : 0.0;

        double t0 = osg::maximum(floor(t), 0.0);
        double t1 = osg::minimum(t0 + 1.0, sizeT);
        double tmix = t0 < t1 ? (t - t0) / (t1 - t0) : 0.0;

        reader(a.UL, (int)s0, (int)t0, 0, 0); // upper left
        reader(a.UR, (int)s1, (int)t0, 0, 0); // upper right
        reader(a.LL, (int)s0, (int)t1, 0, 0); // lower left
        reader(a.LR, (int)s1, (int)t1, 0, 0); // lower right

        a.TOP = a.UL * (1.0f - smix) + a.UR * smix;
        a.BOT = a.LL * (1.0f - smix) + a.LR * smix;
        out = a.TOP * (1.0f - tmix) + a.BOT * tmix;
    }
}

int
ElevationPool::sampleMapCoords(
    std::vector<osg::Vec4d>& points,
    WorkingSet* ws)
{
    OE_PROFILING_ZONE;

    if (points.empty())
        return -1;

    osg::ref_ptr<const Map> map;
    if (_map.lock(map) == false || map->getProfile() == NULL)
        return -1;

    sync(map.get(), ws);
    ScopedAtomicCounter counter(_workers);

    Internal::RevElevationKey key;
    key._revision = getElevationRevision(map.get());

    osg::ref_ptr<ElevationTexture> raster;
    osg::Vec4 elev;
    double u, v;

    const Profile* profile = map->getProfile();
    double pw = profile->getExtent().width();
    double ph = profile->getExtent().height();
    double pxmin = profile->getExtent().xMin();
    double pymin = profile->getExtent().yMin();

    int count = 0;

    QuickCache quickCache;
    QuickSampleVars qvars;

    //TODO: TESTING..?
    //ws = NULL;

    unsigned tw, th;
    double rx, ry;
    int tx, ty;
    int tx_prev = INT_MAX, ty_prev = INT_MAX;
    float lastRes = -1.0f;
    unsigned lod;
    unsigned lod_prev = INT_MAX;
    const Units& units = map->getSRS()->getUnits();
    Distance pointRes(0.0, units);

    for(auto& p : points)
    {
        {
            OE_PROFILING_ZONE_NAMED("createTileKey");

            if (p.w() >= 0.0f && p.w() != lastRes)
            {
                pointRes.set(p.w(), units);

                double resolutionInMapUnits = pointRes.asDistance(units, p.y());

                unsigned maxLOD = profile->getLevelOfDetailForHorizResolution(
                    resolutionInMapUnits,
                    ELEVATION_TILE_SIZE);

                lod = osg::minimum( getLOD(p.x(), p.y()), maxLOD );

                profile->getNumTiles(lod, tw, th);

                lastRes = p.w();
            }

            rx = (p.x()-pxmin)/pw, ry = (p.y()-pymin)/ph;
            tx = osg::clampBelow((unsigned)(rx * (double)tw), tw-1u ); // TODO: wrap around for geo
            ty = osg::clampBelow((unsigned)((1.0-ry) * (double)th), th-1u );

            if (lod != lod_prev || tx != tx_prev || ty != ty_prev)
            {
                key._tilekey = TileKey(lod, tx, ty, profile);
                lod_prev = lod;
                tx_prev = tx;
                ty_prev = ty;
            }
        }
        
        if (key._tilekey.valid())
        {
            auto& iter = quickCache.find(key);

            if (iter == quickCache.end())
            {
                raster = getOrCreateRaster(
                    key,   // key to query
                    map.get(), // map to query
                    true,  // for the cache
                    true,  // fall back on lower resolution data if necessary
                    ws);   // user's workingset

                quickCache[key] = raster.get();
            }
            else
            {
                raster = iter->second;
            }

            {
                OE_PROFILING_ZONE_NAMED("sample");
                if (raster.valid())
                {
                    u = (p.x() - raster->getExtent().xMin()) /  raster->getExtent().width();
                    v = (p.y() - raster->getExtent().yMin()) /  raster->getExtent().height();

                    // Note: This can happen on the map edges..
                    // TODO: consider looping around for geo and clamping for projected
                    u = osg::clampBetween(u, 0.0, 1.0);
                    v = osg::clampBetween(v, 0.0, 1.0);

                    quickSample(raster->reader(), u, v, elev, qvars);
                    p.z() = elev.r();
                }
                else
                {
                    p.z() = NO_DATA_VALUE;
                }
            }
        }
        else
        {
            p.z() = NO_DATA_VALUE;
        }

        if (p.z() != NO_DATA_VALUE)
            ++count;
    }

    return count;
}

int
ElevationPool::sampleMapCoords(
    std::vector<osg::Vec3d>& points,
    const Distance& resolution,
    WorkingSet* ws)
{
    OE_PROFILING_ZONE;

    if (points.empty())
        return -1;

    osg::ref_ptr<const Map> map;
    if (_map.lock(map) == false || map->getProfile() == NULL)
        return -1;

    sync(map.get(), ws);
    ScopedAtomicCounter counter(_workers);

    Internal::RevElevationKey key;
    key._revision = getElevationRevision(map.get());

    osg::ref_ptr<ElevationTexture> raster;
    osg::Vec4 elev;
    double u, v;

    const Profile* profile = map->getProfile();
    double pw = profile->getExtent().width();
    double ph = profile->getExtent().height();
    double pxmin = profile->getExtent().xMin();
    double pymin = profile->getExtent().yMin();

    int count = 0;

    QuickCache quickCache;
    QuickSampleVars qvars;

    //TODO: TESTING..?
    //ws = NULL;

    unsigned tw, th;
    double rx, ry;
    int tx, ty;
    int tx_prev = INT_MAX, ty_prev = INT_MAX;
    float lastRes = -1.0f;
    unsigned lod;
    unsigned lod_prev = INT_MAX;
    const Units& units = map->getSRS()->getUnits();
    Distance pointRes(0.0, units);

    double resolutionInMapUnits = resolution.asDistance(units, points[0].y());

    unsigned maxLOD = profile->getLevelOfDetailForHorizResolution(
        resolutionInMapUnits,
        ELEVATION_TILE_SIZE);

    lod = osg::minimum( getLOD(points[0].x(), points[0].y()), maxLOD );

    profile->getNumTiles(lod, tw, th);

    for(auto& p : points)
    {
        {
            OE_PROFILING_ZONE_NAMED("createTileKey");

            rx = (p.x()-pxmin)/pw, ry = (p.y()-pymin)/ph;
            tx = osg::clampBelow((unsigned)(rx * (double)tw), tw-1u ); // TODO: wrap around for geo
            ty = osg::clampBelow((unsigned)((1.0-ry) * (double)th), th-1u );

            if (lod != lod_prev || tx != tx_prev || ty != ty_prev)
            {
                key._tilekey = TileKey(lod, tx, ty, profile);
                lod_prev = lod;
                tx_prev = tx;
                ty_prev = ty;
            }
        }

        if (key._tilekey.valid())
        {
            auto& iter = quickCache.find(key);

            if (iter == quickCache.end())
            {
                raster = getOrCreateRaster(
                    key,   // key to query
                    map.get(), // map to query
                    true,  // for the cache
                    true,  // fall back on lower resolution data if necessary
                    ws);   // user's workingset

                quickCache[key] = raster.get();
            }
            else
            {
                raster = iter->second;
            }

            {
                OE_PROFILING_ZONE_NAMED("sample");
                if (raster.valid())
                {
                    u = (p.x() - raster->getExtent().xMin()) /  raster->getExtent().width();
                    v = (p.y() - raster->getExtent().yMin()) /  raster->getExtent().height();

                    // Note: This can happen on the map edges..
                    // TODO: consider looping around for geo and clamping for projected
                    u = osg::clampBetween(u, 0.0, 1.0);
                    v = osg::clampBetween(v, 0.0, 1.0);

                    quickSample(raster->reader(), u, v, elev, qvars);
                    p.z() = elev.r();
                }
                else
                {
                    p.z() = NO_DATA_VALUE;
                }
            }
        }
        else
        {
            p.z() = NO_DATA_VALUE;
        }

        if (p.z() != NO_DATA_VALUE)
            ++count;
    }

    return count;
}

ElevationSample
ElevationPool::getSample(
    const GeoPoint& p, 
    unsigned maxLOD, 
    const Map* map, 
    WorkingSet* ws)
{
    // ensure the Pool is in sync with the map
    sync(map, ws);

    ScopedAtomicCounter counter(_workers);

    Internal::RevElevationKey key;
    unsigned lod = osg::minimum( getLOD(p.x(), p.y()), maxLOD );
    key._tilekey = map->getProfile()->createTileKey(p.x(), p.y(), lod);
    key._revision = getElevationRevision(map);

    osg::ref_ptr<ElevationTexture> raster = getOrCreateRaster(
        key,   // key to query
        map,   // map to query
        false, // no normal maps
        true,  // fall back on lower resolution data if necessary
        ws);   // user's workingset

    if (raster.valid())
    {
        return raster->getElevation(p.x(), p.y());
    }

    return ElevationSample();
}

ElevationSample
ElevationPool::getSample(const GeoPoint& p, WorkingSet* ws)
{
    if (!p.isValid())
        return ElevationSample();

    osg::ref_ptr<const Map> map = _map.get();
    if (!map.valid() || !map->getProfile())
        return ElevationSample();

    if (!p.getSRS()->isHorizEquivalentTo(map->getProfile()->getSRS()))
    {
        GeoPoint xp(p);
        xp.transformInPlace(map->getProfile()->getSRS());
        return getSample(xp, ~0, map.get(), ws);
    }
    else
    {
        return getSample(p, ~0, map.get(), ws);
    }
}

ElevationSample
ElevationPool::getSample(const GeoPoint& p, const Distance& resolution, WorkingSet* ws)
{
    if (!p.isValid())
        return ElevationSample();

    osg::ref_ptr<const Map> map;
    if (_map.lock(map) == false || map->getProfile() == NULL)
        return ElevationSample();

    // mostly right. :)
    double resolutionInMapUnits = SpatialReference::transformUnits(
        resolution,
        map->getSRS(),
        p.y());

    unsigned maxLOD = map->getProfile()->getLevelOfDetailForHorizResolution(
        resolutionInMapUnits,
        ELEVATION_TILE_SIZE);

    if (!p.getSRS()->isHorizEquivalentTo(map->getProfile()->getSRS()))
    {
        GeoPoint xp(p);
        xp.transformInPlace(map->getProfile()->getSRS());
        return getSample(xp, maxLOD, map.get(), ws);
    }
    else
    {
        return getSample(p, maxLOD, map.get(), ws);
    }
}

bool
ElevationPool::getTile(
    const TileKey& tilekey, 
    bool getResolutions, 
    bool acceptLowerRes,
    osg::ref_ptr<ElevationTexture>& out_tex,
    WorkingSet* ws)
{
    osg::ref_ptr<const Map> map;
    if (!_map.lock(map))
        return false;

    // ensure we are in sync with the map
    sync(map.get(), ws);

    ScopedAtomicCounter counter(_workers);

    Internal::RevElevationKey key;
    key._tilekey = tilekey;
    key._revision = getElevationRevision(map.get());

    out_tex = getOrCreateRaster(
        key, 
        _map.get(), 
        getResolutions, 
        acceptLowerRes,
        ws);

    return true;
}

//...................................................................

namespace osgEarth { namespace Internal
{
    struct SampleElevationOp : public osg::Operation
    {
        osg::observer_ptr<const Map> _map;
        GeoPoint _p;
        Distance _res;
        ElevationPool::WorkingSet* _ws;
        Promise<RefElevationSample> _promise;

        SampleElevationOp(osg::observer_ptr<const Map> map, const GeoPoint& p, const Distance& res, ElevationPool::WorkingSet* ws) :
            _map(map), _p(p), _res(res), _ws(ws) { }

        void operator()(osg::Object*)
        {
            if (!_promise.isAbandoned())
            {
                osg::ref_ptr<const Map> map;
                if (_map.lock(map))
                {
                    ElevationSample sample = map->getElevationPool()->getSample(_p, _res, _ws);
                    _promise.resolve(new RefElevationSample(sample.elevation(), sample.resolution()));
                    return;
                }
            }

            _promise.resolve(NULL);
        }
    };
}}

AsyncElevationSampler::AsyncElevationSampler(
    const Map* map,
    unsigned numThreads) :

    _map(map)
{
    _threadPool = new ThreadPool(numThreads);
}

Future<RefElevationSample>
AsyncElevationSampler::getSample(const GeoPoint& p)
{
    return getSample(p, Distance(0, p.getXYUnits()));
}

Future<RefElevationSample>
AsyncElevationSampler::getSample(
    const GeoPoint& p,
    const Distance& resolution)
{
    Internal::SampleElevationOp* op = new Internal::SampleElevationOp(_map, p, resolution, &_ws);
    Future<RefElevationSample> result = op->_promise.getFuture();
    _threadPool->getQueue()->add(op);
    return result;
}
