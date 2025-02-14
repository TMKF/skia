
/*
 * Copyright 2010 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrAtlas.h"
#include "GrContext.h"
#include "GrGpu.h"
#include "GrRectanizer.h"
#include "GrTracing.h"

///////////////////////////////////////////////////////////////////////////////

// for testing
#define FONT_CACHE_STATS 0
#if FONT_CACHE_STATS
static int g_UploadCount = 0;
#endif

GrPlot::GrPlot() 
    : fID(-1)
    , fTexture(nullptr)
    , fRects(nullptr)
    , fAtlas(nullptr)
    , fBytesPerPixel(1)
    , fDirty(false)
    , fBatchUploads(false)
{
    fOffset.set(0, 0);
}

GrPlot::~GrPlot() {
    delete[] fPlotData;
    fPlotData = nullptr;
    delete fRects;
}

void GrPlot::init(GrAtlas* atlas, int id, int offX, int offY, int width, int height, size_t bpp,
                  bool batchUploads) {
    fID = id;
    fRects = GrRectanizer::Factory(width, height);
    fAtlas = atlas;
    fOffset.set(offX * width, offY * height);
    fBytesPerPixel = bpp;
    fPlotData = nullptr;
    fDirtyRect.setEmpty();
    fDirty = false;
    fBatchUploads = batchUploads;
}

static inline void adjust_for_offset(SkIPoint16* loc, const SkIPoint16& offset) {
    loc->fX += offset.fX;
    loc->fY += offset.fY;
}

bool GrPlot::addSubImage(int width, int height, const void* image, SkIPoint16* loc) {
    float percentFull = fRects->percentFull();
    if (!fRects->addRect(width, height, loc)) {
        return false;
    }

    // if batching uploads, create backing memory on first use
    // once the plot is nearly full we will revert to uploading each subimage individually
    int plotWidth = fRects->width();
    int plotHeight = fRects->height();
    if (fBatchUploads && nullptr == fPlotData && 0.0f == percentFull) {
        fPlotData = new unsigned char[fBytesPerPixel * plotWidth * plotHeight];
        memset(fPlotData, 0, fBytesPerPixel*plotWidth*plotHeight);
    }

    // if we have backing memory, copy to the memory and set for future upload
    if (fPlotData) {
        const unsigned char* imagePtr = (const unsigned char*) image;
        // point ourselves at the right starting spot
        unsigned char* dataPtr = fPlotData;
        dataPtr += fBytesPerPixel*plotWidth*loc->fY;
        dataPtr += fBytesPerPixel*loc->fX;
        // copy into the data buffer
        for (int i = 0; i < height; ++i) {
            memcpy(dataPtr, imagePtr, fBytesPerPixel*width);
            dataPtr += fBytesPerPixel*plotWidth;
            imagePtr += fBytesPerPixel*width;
        }

        fDirtyRect.join(loc->fX, loc->fY, loc->fX + width, loc->fY + height);
        adjust_for_offset(loc, fOffset);
        fDirty = true;
    // otherwise, just upload the image directly
    } else if (image) {
        adjust_for_offset(loc, fOffset);
        TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("skia.gpu"), "GrPlot::uploadToTexture");
        fTexture->writePixels(loc->fX, loc->fY, width, height, fTexture->config(), image, 0,
                              GrContext::kDontFlush_PixelOpsFlag);
    } else {
        adjust_for_offset(loc, fOffset);
    }

#if FONT_CACHE_STATS
    ++g_UploadCount;
#endif

    return true;
}

void GrPlot::resetRects() {
    SkASSERT(fRects);
    fRects->reset();
}

///////////////////////////////////////////////////////////////////////////////

GrAtlas::GrAtlas(GrGpu* gpu, GrPixelConfig config, GrSurfaceFlags flags,
                 const SkISize& backingTextureSize,
                 int numPlotsX, int numPlotsY, bool batchUploads) {
    fGpu = SkRef(gpu);
    fPixelConfig = config;
    fFlags = flags;
    fBackingTextureSize = backingTextureSize;
    fNumPlotsX = numPlotsX;
    fNumPlotsY = numPlotsY;
    fBatchUploads = batchUploads;
    fTexture = nullptr;

    int textureWidth = fBackingTextureSize.width();
    int textureHeight = fBackingTextureSize.height();

    int plotWidth = textureWidth / fNumPlotsX;
    int plotHeight = textureHeight / fNumPlotsY;

    SkASSERT(plotWidth * fNumPlotsX == textureWidth);
    SkASSERT(plotHeight * fNumPlotsY == textureHeight);

    // We currently do not support compressed atlases...
    SkASSERT(!GrPixelConfigIsCompressed(config));

    // set up allocated plots
    size_t bpp = GrBytesPerPixel(fPixelConfig);
    fPlotArray = new GrPlot[(fNumPlotsX * fNumPlotsY)];

    GrPlot* currPlot = fPlotArray;
    for (int y = numPlotsY-1; y >= 0; --y) {
        for (int x = numPlotsX-1; x >= 0; --x) {
            currPlot->init(this, y*numPlotsX+x, x, y, plotWidth, plotHeight, bpp, batchUploads);

            // build LRU list
            fPlotList.addToHead(currPlot);
            ++currPlot;
        }
    }
}

GrAtlas::~GrAtlas() {
    SkSafeUnref(fTexture);
    delete[] fPlotArray;

    fGpu->unref();
#if FONT_CACHE_STATS
      SkDebugf("Num uploads: %d\n", g_UploadCount);
#endif
}

void GrAtlas::makeMRU(GrPlot* plot) {
    if (fPlotList.head() == plot) {
        return;
    }

    fPlotList.remove(plot);
    fPlotList.addToHead(plot);
};

GrPlot* GrAtlas::addToAtlas(ClientPlotUsage* usage,
                            int width, int height, const void* image,
                            SkIPoint16* loc) {
    // iterate through entire plot list for this atlas, see if we can find a hole
    // last one was most recently added and probably most empty
    for (int i = usage->fPlots.count()-1; i >= 0; --i) {
        GrPlot* plot = usage->fPlots[i];
        // client may have plots from more than one atlas, must check for ours before adding
        if (this == plot->fAtlas && plot->addSubImage(width, height, image, loc)) {
            this->makeMRU(plot);
            return plot;
        }
    }

    // before we get a new plot, make sure we have a backing texture
    if (nullptr == fTexture) {
        // TODO: Update this to use the cache rather than directly creating a texture.
        GrSurfaceDesc desc;
        desc.fFlags = fFlags;
        desc.fWidth = fBackingTextureSize.width();
        desc.fHeight = fBackingTextureSize.height();
        desc.fConfig = fPixelConfig;

        fTexture = fGpu->createTexture(desc, true, nullptr, 0);
        if (nullptr == fTexture) {
            return nullptr;
        }
    }

    // now look through all allocated plots for one we can share, in MRU order
    GrPlotList::Iter plotIter;
    plotIter.init(fPlotList, GrPlotList::Iter::kHead_IterStart);
    GrPlot* plot;
    while ((plot = plotIter.get())) {
        // make sure texture is set for quick lookup
        plot->fTexture = fTexture;
        if (plot->addSubImage(width, height, image, loc)) {
            this->makeMRU(plot);
            // new plot for atlas, put at end of array
            SkASSERT(!usage->fPlots.contains(plot));
            *(usage->fPlots.append()) = plot;
            return plot;
        }
        plotIter.next();
    }

    // If the above fails, then the current plot list has no room
    return nullptr;
}

void GrAtlas::RemovePlot(ClientPlotUsage* usage, const GrPlot* plot) {
    int index = usage->fPlots.find(const_cast<GrPlot*>(plot));
    if (index >= 0) {
        usage->fPlots.remove(index);
    }
}
