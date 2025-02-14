/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrGLPrimitiveProcessor_DEFINED
#define GrGLPrimitiveProcessor_DEFINED

#include "GrPrimitiveProcessor.h"
#include "GrGLProcessor.h"

class GrBatchTracker;
class GrPrimitiveProcessor;
class GrGLGPBuilder;

class GrGLPrimitiveProcessor {
public:
    virtual ~GrGLPrimitiveProcessor() {}

    typedef GrGLProgramDataManager::UniformHandle UniformHandle;
    typedef GrGLProcessor::TextureSamplerArray TextureSamplerArray;

    typedef SkSTArray<2, const GrCoordTransform*, true> ProcCoords;
    typedef SkSTArray<8, ProcCoords> TransformsIn;
    typedef SkSTArray<8, GrGLProcessor::TransformedCoordsArray> TransformsOut;

    struct EmitArgs {
        EmitArgs(GrGLGPBuilder* pb,
                 const GrPrimitiveProcessor& gp,
                 const char* outputColor,
                 const char* outputCoverage,
                 const TextureSamplerArray& samplers,
                 const TransformsIn& transformsIn,
                 TransformsOut* transformsOut)
            : fPB(pb)
            , fGP(gp)
            , fOutputColor(outputColor)
            , fOutputCoverage(outputCoverage)
            , fSamplers(samplers)
            , fTransformsIn(transformsIn)
            , fTransformsOut(transformsOut) {}
        GrGLGPBuilder* fPB;
        const GrPrimitiveProcessor& fGP;
        const char* fOutputColor;
        const char* fOutputCoverage;
        const TextureSamplerArray& fSamplers;
        const TransformsIn& fTransformsIn;
        TransformsOut* fTransformsOut;
    };

    /**
     * This is similar to emitCode() in the base class, except it takes a full shader builder.
     * This allows the effect subclass to emit vertex code.
     */
    virtual void emitCode(EmitArgs&) = 0;


    /** A GrGLPrimitiveProcessor instance can be reused with any GrGLPrimitiveProcessor that
        produces the same stage key; this function reads data from a GrGLPrimitiveProcessor and
        uploads any uniform variables required  by the shaders created in emitCode(). The
        GrPrimitiveProcessor parameter is guaranteed to be of the same type that created this
        GrGLPrimitiveProcessor and to have an identical processor key as the one that created this
        GrGLPrimitiveProcessor.  */
    virtual void setData(const GrGLProgramDataManager&, const GrPrimitiveProcessor&) = 0;

    static SkMatrix GetTransformMatrix(const SkMatrix& localMatrix, const GrCoordTransform&);

    virtual void setTransformData(const GrPrimitiveProcessor&,
                                  const GrGLProgramDataManager& pdman,
                                  int index,
                                  const SkTArray<const GrCoordTransform*, true>& transforms) = 0;

protected:
    void setupUniformColor(GrGLGPBuilder* pb, const char* outputName, UniformHandle* colorUniform);

    struct Transform {
        Transform() : fType(kVoid_GrSLType) { fCurrentValue = SkMatrix::InvalidMatrix(); }
        GrGLProgramDataManager::UniformHandle fHandle;
        SkMatrix                              fCurrentValue;
        GrSLType                              fType;
    };

    SkSTArray<8, SkSTArray<2, Transform, true> > fInstalledTransforms;
};

#endif
