/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCanvas.h"
#include "SkColorPriv.h"
#include "SkMathPriv.h"
#include "SkRegion.h"
#include "SkSurface.h"
#include "Test.h"

#if SK_SUPPORT_GPU
#include "GrContextFactory.h"
#include "SkGpuDevice.h"
#include "SkGr.h"
#endif

static const int DEV_W = 100, DEV_H = 100;
static const SkIRect DEV_RECT = SkIRect::MakeWH(DEV_W, DEV_H);
static const SkRect DEV_RECT_S = SkRect::MakeWH(DEV_W * SK_Scalar1,
                                                DEV_H * SK_Scalar1);

static SkPMColor get_src_color(int x, int y) {
    SkASSERT(x >= 0 && x < DEV_W);
    SkASSERT(y >= 0 && y < DEV_H);

    U8CPU r = x;
    U8CPU g = y;
    U8CPU b = 0xc;

    U8CPU a = 0xff;
    switch ((x+y) % 5) {
        case 0:
            a = 0xff;
            break;
        case 1:
            a = 0x80;
            break;
        case 2:
            a = 0xCC;
            break;
        case 4:
            a = 0x01;
            break;
        case 3:
            a = 0x00;
            break;
    }
    return SkPremultiplyARGBInline(a, r, g, b);
}
    
static SkPMColor get_dst_bmp_init_color(int x, int y, int w) {
    int n = y * w + x;

    U8CPU b = n & 0xff;
    U8CPU g = (n >> 8) & 0xff;
    U8CPU r = (n >> 16) & 0xff;
    return SkPackARGB32(0xff, r, g , b);
}

static SkPMColor convert_to_pmcolor(SkColorType ct, SkAlphaType at, const uint32_t* addr,
                                    bool* doUnpremul) {
    *doUnpremul = (kUnpremul_SkAlphaType == at);

    const uint8_t* c = reinterpret_cast<const uint8_t*>(addr);
    U8CPU a,r,g,b;
    switch (ct) {
        case kBGRA_8888_SkColorType:
            b = static_cast<U8CPU>(c[0]);
            g = static_cast<U8CPU>(c[1]);
            r = static_cast<U8CPU>(c[2]);
            a = static_cast<U8CPU>(c[3]);
            break;
        case kRGBA_8888_SkColorType:
            r = static_cast<U8CPU>(c[0]);
            g = static_cast<U8CPU>(c[1]);
            b = static_cast<U8CPU>(c[2]);
            a = static_cast<U8CPU>(c[3]);
            break;
        default:
            SkDEBUGFAIL("Unexpected colortype");
            return 0;
    }

    if (*doUnpremul) {
        r = SkMulDiv255Ceiling(r, a);
        g = SkMulDiv255Ceiling(g, a);
        b = SkMulDiv255Ceiling(b, a);
    }
    return SkPackARGB32(a, r, g, b);
}

static SkBitmap make_src_bitmap() {
    static SkBitmap bmp;
    if (bmp.isNull()) {
        bmp.allocN32Pixels(DEV_W, DEV_H);
        intptr_t pixels = reinterpret_cast<intptr_t>(bmp.getPixels());
        for (int y = 0; y < DEV_H; ++y) {
            for (int x = 0; x < DEV_W; ++x) {
                SkPMColor* pixel = reinterpret_cast<SkPMColor*>(pixels + y * bmp.rowBytes() + x * bmp.bytesPerPixel());
                *pixel = get_src_color(x, y);
            }
        }
    }
    return bmp;
}

static void fill_src_canvas(SkCanvas* canvas) {
    canvas->save();
    canvas->setMatrix(SkMatrix::I());
    canvas->clipRect(DEV_RECT_S, SkRegion::kReplace_Op);
    SkPaint paint;
    paint.setXfermodeMode(SkXfermode::kSrc_Mode);
    canvas->drawBitmap(make_src_bitmap(), 0, 0, &paint);
    canvas->restore();
}

#if SK_SUPPORT_GPU
static void fill_src_texture(GrTexture* texture) {
    SkBitmap bmp = make_src_bitmap();
    bmp.lockPixels();
    texture->writePixels(0, 0, DEV_W, DEV_H, kSkia8888_GrPixelConfig, bmp.getPixels(),
                         bmp.rowBytes());
    bmp.unlockPixels();
}
#endif

static void fill_dst_bmp_with_init_data(SkBitmap* bitmap) {
    SkASSERT(bitmap->lockPixelsAreWritable());
    SkAutoLockPixels alp(*bitmap);
    int w = bitmap->width();
    int h = bitmap->height();
    intptr_t pixels = reinterpret_cast<intptr_t>(bitmap->getPixels());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            SkPMColor* pixel = reinterpret_cast<SkPMColor*>(pixels + y * bitmap->rowBytes() + x * bitmap->bytesPerPixel());
            *pixel = get_dst_bmp_init_color(x, y, w);
        }
    }
}

static bool check_read_pixel(SkPMColor a, SkPMColor b, bool didPremulConversion) {
    if (!didPremulConversion) {
        return a == b;
    }
    int32_t aA = static_cast<int32_t>(SkGetPackedA32(a));
    int32_t aR = static_cast<int32_t>(SkGetPackedR32(a));
    int32_t aG = static_cast<int32_t>(SkGetPackedG32(a));
    int32_t aB = SkGetPackedB32(a);

    int32_t bA = static_cast<int32_t>(SkGetPackedA32(b));
    int32_t bR = static_cast<int32_t>(SkGetPackedR32(b));
    int32_t bG = static_cast<int32_t>(SkGetPackedG32(b));
    int32_t bB = static_cast<int32_t>(SkGetPackedB32(b));

    return aA == bA &&
           SkAbs32(aR - bR) <= 1 &&
           SkAbs32(aG - bG) <= 1 &&
           SkAbs32(aB - bB) <= 1;
}

// checks the bitmap contains correct pixels after the readPixels
// if the bitmap was prefilled with pixels it checks that these weren't
// overwritten in the area outside the readPixels.
static bool check_read(skiatest::Reporter* reporter,
                       const SkBitmap& bitmap,
                       int x, int y,
                       bool checkCanvasPixels,
                       bool checkBitmapPixels) {
    SkASSERT(4 == bitmap.bytesPerPixel());
    SkASSERT(!bitmap.isNull());
    SkASSERT(checkCanvasPixels || checkBitmapPixels);

    const SkColorType ct = bitmap.colorType();
    const SkAlphaType at = bitmap.alphaType();

    int bw = bitmap.width();
    int bh = bitmap.height();

    SkIRect srcRect = SkIRect::MakeXYWH(x, y, bw, bh);
    SkIRect clippedSrcRect = DEV_RECT;
    if (!clippedSrcRect.intersect(srcRect)) {
        clippedSrcRect.setEmpty();
    }
    SkAutoLockPixels alp(bitmap);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            int devx = bx + srcRect.fLeft;
            int devy = by + srcRect.fTop;

            const uint32_t* pixel = bitmap.getAddr32(bx, by);

            if (clippedSrcRect.contains(devx, devy)) {
                if (checkCanvasPixels) {
                    SkPMColor canvasPixel = get_src_color(devx, devy);
                    bool didPremul;
                    SkPMColor pmPixel = convert_to_pmcolor(ct, at, pixel, &didPremul);
                    if (!check_read_pixel(pmPixel, canvasPixel, didPremul)) {
                        ERRORF(reporter, "Expected readback pixel value 0x%08x, got 0x%08x. "
                               "Readback was unpremul: %d", canvasPixel, pmPixel, didPremul);
                        return false;
                    }
                }
            } else if (checkBitmapPixels) {
                uint32_t origDstPixel = get_dst_bmp_init_color(bx, by, bw);
                if (origDstPixel != *pixel) {
                    ERRORF(reporter, "Expected clipped out area of readback to be unchanged. "
                           "Expected 0x%08x, got 0x%08x", origDstPixel, *pixel);
                    return false;
                }
            }
        }
    }
    return true;
}

enum BitmapInit {
    kFirstBitmapInit = 0,

    kNoPixels_BitmapInit = kFirstBitmapInit,
    kTight_BitmapInit,
    kRowBytes_BitmapInit,

    kBitmapInitCnt
};

static BitmapInit nextBMI(BitmapInit bmi) {
    int x = bmi;
    return static_cast<BitmapInit>(++x);
}

static void init_bitmap(SkBitmap* bitmap, const SkIRect& rect, BitmapInit init, SkColorType ct,
                        SkAlphaType at) {
    SkImageInfo info = SkImageInfo::Make(rect.width(), rect.height(), ct, at);
    size_t rowBytes = 0;
    bool alloc = true;
    switch (init) {
        case kNoPixels_BitmapInit:
            alloc = false;
        case kTight_BitmapInit:
            break;
        case kRowBytes_BitmapInit:
            rowBytes = (info.width() + 16) * sizeof(SkPMColor);
            break;
        default:
            SkASSERT(0);
            break;
    }

    if (alloc) {
        bitmap->allocPixels(info);
    } else {
        bitmap->setInfo(info, rowBytes);
    }
}

DEF_GPUTEST(ReadPixels, reporter, factory) {
    const SkIRect testRects[] = {
        // entire thing
        DEV_RECT,
        // larger on all sides
        SkIRect::MakeLTRB(-10, -10, DEV_W + 10, DEV_H + 10),
        // fully contained
        SkIRect::MakeLTRB(DEV_W / 4, DEV_H / 4, 3 * DEV_W / 4, 3 * DEV_H / 4),
        // outside top left
        SkIRect::MakeLTRB(-10, -10, -1, -1),
        // touching top left corner
        SkIRect::MakeLTRB(-10, -10, 0, 0),
        // overlapping top left corner
        SkIRect::MakeLTRB(-10, -10, DEV_W / 4, DEV_H / 4),
        // overlapping top left and top right corners
        SkIRect::MakeLTRB(-10, -10, DEV_W  + 10, DEV_H / 4),
        // touching entire top edge
        SkIRect::MakeLTRB(-10, -10, DEV_W  + 10, 0),
        // overlapping top right corner
        SkIRect::MakeLTRB(3 * DEV_W / 4, -10, DEV_W  + 10, DEV_H / 4),
        // contained in x, overlapping top edge
        SkIRect::MakeLTRB(DEV_W / 4, -10, 3 * DEV_W  / 4, DEV_H / 4),
        // outside top right corner
        SkIRect::MakeLTRB(DEV_W + 1, -10, DEV_W + 10, -1),
        // touching top right corner
        SkIRect::MakeLTRB(DEV_W, -10, DEV_W + 10, 0),
        // overlapping top left and bottom left corners
        SkIRect::MakeLTRB(-10, -10, DEV_W / 4, DEV_H + 10),
        // touching entire left edge
        SkIRect::MakeLTRB(-10, -10, 0, DEV_H + 10),
        // overlapping bottom left corner
        SkIRect::MakeLTRB(-10, 3 * DEV_H / 4, DEV_W / 4, DEV_H + 10),
        // contained in y, overlapping left edge
        SkIRect::MakeLTRB(-10, DEV_H / 4, DEV_W / 4, 3 * DEV_H / 4),
        // outside bottom left corner
        SkIRect::MakeLTRB(-10, DEV_H + 1, -1, DEV_H + 10),
        // touching bottom left corner
        SkIRect::MakeLTRB(-10, DEV_H, 0, DEV_H + 10),
        // overlapping bottom left and bottom right corners
        SkIRect::MakeLTRB(-10, 3 * DEV_H / 4, DEV_W + 10, DEV_H + 10),
        // touching entire left edge
        SkIRect::MakeLTRB(0, DEV_H, DEV_W, DEV_H + 10),
        // overlapping bottom right corner
        SkIRect::MakeLTRB(3 * DEV_W / 4, 3 * DEV_H / 4, DEV_W + 10, DEV_H + 10),
        // overlapping top right and bottom right corners
        SkIRect::MakeLTRB(3 * DEV_W / 4, -10, DEV_W + 10, DEV_H + 10),
    };

    for (int dtype = 0; dtype < 3; ++dtype) {
        int glCtxTypeCnt = 1;
#if SK_SUPPORT_GPU
        // On the GPU we will also try reading back from a non-renderable texture.
        SkAutoTUnref<GrTexture> texture;

        if (0 != dtype)  {
            glCtxTypeCnt = GrContextFactory::kGLContextTypeCnt;
        }
#endif
        const SkImageInfo info = SkImageInfo::MakeN32Premul(DEV_W, DEV_H);
        for (int glCtxType = 0; glCtxType < glCtxTypeCnt; ++glCtxType) {
            SkAutoTUnref<SkSurface> surface;
            if (0 == dtype) {
                surface.reset(SkSurface::NewRaster(info));
            } else {
#if SK_SUPPORT_GPU
                GrContextFactory::GLContextType type =
                    static_cast<GrContextFactory::GLContextType>(glCtxType);
                if (!GrContextFactory::IsRenderingGLContext(type)) {
                    continue;
                }
                GrContext* context = factory->get(type);
                if (nullptr == context) {
                    continue;
                }
                GrSurfaceDesc desc;
                desc.fFlags = kRenderTarget_GrSurfaceFlag;
                desc.fWidth = DEV_W;
                desc.fHeight = DEV_H;
                desc.fConfig = kSkia8888_GrPixelConfig;
                desc.fOrigin = 1 == dtype ? kBottomLeft_GrSurfaceOrigin : kTopLeft_GrSurfaceOrigin;
                SkAutoTUnref<GrTexture> surfaceTexture(
                    context->textureProvider()->createTexture(desc, false));
                surface.reset(SkSurface::NewRenderTargetDirect(surfaceTexture->asRenderTarget()));
                desc.fFlags = kNone_GrSurfaceFlags;

                texture.reset(context->textureProvider()->createTexture(desc, false));
#else
                continue;
#endif
            }
            SkCanvas& canvas = *surface->getCanvas();
            fill_src_canvas(&canvas);

#if SK_SUPPORT_GPU
            if (texture) {
                fill_src_texture(texture);
            }
#endif

            static const struct {
                SkColorType fColorType;
                SkAlphaType fAlphaType;
            } gReadConfigs[] = {
                { kRGBA_8888_SkColorType,   kPremul_SkAlphaType },
                { kRGBA_8888_SkColorType,   kUnpremul_SkAlphaType },
                { kBGRA_8888_SkColorType,   kPremul_SkAlphaType },
                { kBGRA_8888_SkColorType,   kUnpremul_SkAlphaType },
            };
            for (size_t rect = 0; rect < SK_ARRAY_COUNT(testRects); ++rect) {
                const SkIRect& srcRect = testRects[rect];
                for (BitmapInit bmi = kFirstBitmapInit; bmi < kBitmapInitCnt; bmi = nextBMI(bmi)) {
                    for (size_t c = 0; c < SK_ARRAY_COUNT(gReadConfigs); ++c) {
                        SkBitmap bmp;
                        init_bitmap(&bmp, srcRect, bmi,
                                    gReadConfigs[c].fColorType, gReadConfigs[c].fAlphaType);

                        // if the bitmap has pixels allocated before the readPixels,
                        // note that and fill them with pattern
                        bool startsWithPixels = !bmp.isNull();
                        if (startsWithPixels) {
                            fill_dst_bmp_with_init_data(&bmp);
                        }
                        uint32_t idBefore = surface->generationID();
                        bool success = canvas.readPixels(&bmp, srcRect.fLeft, srcRect.fTop);
                        uint32_t idAfter = surface->generationID();

                        // we expect to succeed when the read isn't fully clipped
                        // out.
                        bool expectSuccess = SkIRect::Intersects(srcRect, DEV_RECT);
                        // determine whether we expected the read to succeed.
                        REPORTER_ASSERT(reporter, success == expectSuccess);
                        // read pixels should never change the gen id
                        REPORTER_ASSERT(reporter, idBefore == idAfter);

                        if (success || startsWithPixels) {
                            check_read(reporter, bmp, srcRect.fLeft, srcRect.fTop,
                                       success, startsWithPixels);
                        } else {
                            // if we had no pixels beforehand and the readPixels
                            // failed then our bitmap should still not have pixels
                            REPORTER_ASSERT(reporter, bmp.isNull());
                        }
#if SK_SUPPORT_GPU
                        // Try doing the read directly from a non-renderable texture
                        if (texture && startsWithPixels) {
                            fill_dst_bmp_with_init_data(&bmp);
                            GrPixelConfig dstConfig =
                                SkImageInfo2GrPixelConfig(gReadConfigs[c].fColorType,
                                                          gReadConfigs[c].fAlphaType,
                                                          kLinear_SkColorProfileType);
                            uint32_t flags = 0;
                            if (gReadConfigs[c].fAlphaType == kUnpremul_SkAlphaType) {
                                flags = GrContext::kUnpremul_PixelOpsFlag;
                            }
                            bmp.lockPixels();
                            success = texture->readPixels(srcRect.fLeft, srcRect.fTop, bmp.width(),
                                                bmp.height(), dstConfig, bmp.getPixels(),
                                                bmp.rowBytes(), flags);
                            bmp.unlockPixels();
                            check_read(reporter, bmp, srcRect.fLeft, srcRect.fTop,
                                       success, true);
                        }
#endif
                    }
                    // check the old webkit version of readPixels that clips the
                    // bitmap size
                    SkBitmap wkbmp;
                    bool success = canvas.readPixels(srcRect, &wkbmp);
                    SkIRect clippedRect = DEV_RECT;
                    if (clippedRect.intersect(srcRect)) {
                        REPORTER_ASSERT(reporter, success);
                        REPORTER_ASSERT(reporter, kN32_SkColorType == wkbmp.colorType());
                        REPORTER_ASSERT(reporter, kPremul_SkAlphaType == wkbmp.alphaType());
                        check_read(reporter, wkbmp, clippedRect.fLeft,
                                   clippedRect.fTop, true, false);
                    } else {
                        REPORTER_ASSERT(reporter, !success);
                    }
                }
            }
        }
    }
}

/////////////////////
#if SK_SUPPORT_GPU

// make_ringed_bitmap was lifted from gm/bleed.cpp, as that GM was what showed the following
// bug when a change was made to SkImage_Raster.cpp. It is possible that other test bitmaps
// would also tickle skbug.com/4351 but this one is know to do it, so I've pasted the code
// here so we have a dependable repro case.

// Create a black&white checked texture with 2 1-pixel rings
// around the outside edge. The inner ring is red and the outer ring is blue.
static void make_ringed_bitmap(SkBitmap* result, int width, int height) {
    SkASSERT(0 == width % 2 && 0 == height % 2);

    static const SkPMColor kRed = SkPreMultiplyColor(SK_ColorRED);
    static const SkPMColor kBlue = SkPreMultiplyColor(SK_ColorBLUE);
    static const SkPMColor kBlack = SkPreMultiplyColor(SK_ColorBLACK);
    static const SkPMColor kWhite = SkPreMultiplyColor(SK_ColorWHITE);

    result->allocN32Pixels(width, height, true);

    SkPMColor* scanline = result->getAddr32(0, 0);
    for (int x = 0; x < width; ++x) {
        scanline[x] = kBlue;
    }
    scanline = result->getAddr32(0, 1);
    scanline[0] = kBlue;
    for (int x = 1; x < width - 1; ++x) {
        scanline[x] = kRed;
    }
    scanline[width-1] = kBlue;

    for (int y = 2; y < height/2; ++y) {
        scanline = result->getAddr32(0, y);
        scanline[0] = kBlue;
        scanline[1] = kRed;
        for (int x = 2; x < width/2; ++x) {
            scanline[x] = kBlack;
        }
        for (int x = width/2; x < width-2; ++x) {
            scanline[x] = kWhite;
        }
        scanline[width-2] = kRed;
        scanline[width-1] = kBlue;
    }

    for (int y = height/2; y < height-2; ++y) {
        scanline = result->getAddr32(0, y);
        scanline[0] = kBlue;
        scanline[1] = kRed;
        for (int x = 2; x < width/2; ++x) {
            scanline[x] = kWhite;
        }
        for (int x = width/2; x < width-2; ++x) {
            scanline[x] = kBlack;
        }
        scanline[width-2] = kRed;
        scanline[width-1] = kBlue;
    }

    scanline = result->getAddr32(0, height-2);
    scanline[0] = kBlue;
    for (int x = 1; x < width - 1; ++x) {
        scanline[x] = kRed;
    }
    scanline[width-1] = kBlue;

    scanline = result->getAddr32(0, height-1);
    for (int x = 0; x < width; ++x) {
        scanline[x] = kBlue;
    }
    result->setImmutable();
}

static void compare_textures(skiatest::Reporter* reporter, GrTexture* txa, GrTexture* txb) {
    REPORTER_ASSERT(reporter, txa->width() == 2);
    REPORTER_ASSERT(reporter, txa->height() == 2);
    REPORTER_ASSERT(reporter, txb->width() == 2);
    REPORTER_ASSERT(reporter, txb->height() == 2);
    REPORTER_ASSERT(reporter, txa->config() == txb->config());

    SkPMColor pixelsA[4], pixelsB[4];
    REPORTER_ASSERT(reporter, txa->readPixels(0, 0, 2, 2, txa->config(), pixelsA));
    REPORTER_ASSERT(reporter, txb->readPixels(0, 0, 2, 2, txa->config(), pixelsB));
    REPORTER_ASSERT(reporter, 0 == memcmp(pixelsA, pixelsB, sizeof(pixelsA)));
}

static SkData* draw_into_surface(SkSurface* surf, const SkBitmap& bm, SkFilterQuality quality) {
    SkCanvas* canvas = surf->getCanvas();
    canvas->clear(SK_ColorBLUE);

    SkPaint paint;
    paint.setFilterQuality(quality);

    canvas->translate(40, 100);
    canvas->rotate(30);
    canvas->scale(20, 30);
    canvas->translate(-SkScalarHalf(bm.width()), -SkScalarHalf(bm.height()));
    canvas->drawBitmap(bm, 0, 0, &paint);

    SkAutoTUnref<SkImage> image(surf->newImageSnapshot());
    return image->encode();
}

#include "SkStream.h"
static void dump_to_file(const char name[], SkData* data) {
    SkFILEWStream file(name);
    file.write(data->data(), data->size());
}

/*
 *  Test two different ways to turn a subset of a bitmap into a texture
 *  - subset and then upload to a texture
 *  - upload to a texture and then subset
 *
 *  These two techniques result in the same pixels (ala readPixels)
 *  but when we draw them (rotated+scaled) we don't always get the same results.
 *
 *  skbug.com/4351
 */
DEF_GPUTEST(ReadPixels_Subset_Gpu, reporter, factory) {
    GrContext* ctx = factory->get(GrContextFactory::kNative_GLContextType);
    if (!ctx) {
        REPORTER_ASSERT(reporter, false);
        return;
    }

    SkBitmap bitmap;
    make_ringed_bitmap(&bitmap, 6, 6);
    const SkIRect subset = SkIRect::MakeLTRB(2, 2, 4, 4);

    // make two textures...
    SkBitmap bm_subset, tx_subset;

    // ... one from a texture-subset
    SkAutoTUnref<GrTexture> fullTx(GrRefCachedBitmapTexture(ctx, bitmap,
                                                            kUntiled_SkImageUsageType));
    SkBitmap tx_full;
    GrWrapTextureInBitmap(fullTx, bitmap.width(), bitmap.height(), true, &tx_full);
    tx_full.extractSubset(&tx_subset, subset);

    // ... one from a bitmap-subset
    SkBitmap tmp_subset;
    bitmap.extractSubset(&tmp_subset, subset);
    SkAutoTUnref<GrTexture> subsetTx(GrRefCachedBitmapTexture(ctx, tmp_subset,
                                                              kUntiled_SkImageUsageType));
    GrWrapTextureInBitmap(subsetTx, tmp_subset.width(), tmp_subset.height(), true, &bm_subset);

    // did we get the same subset?
    compare_textures(reporter, bm_subset.getTexture(), tx_subset.getTexture());

    // do they draw the same?
    const SkImageInfo info = SkImageInfo::MakeN32Premul(128, 128);
    SkAutoTUnref<SkSurface> surfA(SkSurface::NewRenderTarget(ctx, SkSurface::kNo_Budgeted, info, 0));
    SkAutoTUnref<SkSurface> surfB(SkSurface::NewRenderTarget(ctx, SkSurface::kNo_Budgeted, info, 0));

    if (false) {
        //
        //  BUG: depending on the driver, if we calls this with various quality settings, it
        //       may fail.
        //
        SkFilterQuality quality = kLow_SkFilterQuality;

        SkAutoTUnref<SkData> dataA(draw_into_surface(surfA, bm_subset, quality));
        SkAutoTUnref<SkData> dataB(draw_into_surface(surfB, tx_subset, quality));

        REPORTER_ASSERT(reporter, dataA->equals(dataB));
        if (false) {
            dump_to_file("test_image_A.png", dataA);
            dump_to_file("test_image_B.png", dataB);
        }
    }
}
#endif
