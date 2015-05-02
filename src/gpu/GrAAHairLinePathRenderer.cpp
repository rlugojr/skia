/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrAAHairLinePathRenderer.h"

#include "GrBatch.h"
#include "GrBatchTarget.h"
#include "GrBatchTest.h"
#include "GrBufferAllocPool.h"
#include "GrContext.h"
#include "GrDefaultGeoProcFactory.h"
#include "GrDrawTargetCaps.h"
#include "GrGpu.h"
#include "GrIndexBuffer.h"
#include "GrPathUtils.h"
#include "GrPipelineBuilder.h"
#include "GrProcessor.h"
#include "GrVertexBuffer.h"
#include "SkGeometry.h"
#include "SkStroke.h"
#include "SkTemplates.h"

#include "effects/GrBezierEffect.h"

// quadratics are rendered as 5-sided polys in order to bound the
// AA stroke around the center-curve. See comments in push_quad_index_buffer and
// bloat_quad. Quadratics and conics share an index buffer

// lines are rendered as:
//      *______________*
//      |\ -_______   /|
//      | \        \ / |
//      |  *--------*  |
//      | /  ______/ \ |
//      */_-__________\*
// For: 6 vertices and 18 indices (for 6 triangles)

// Each quadratic is rendered as a five sided polygon. This poly bounds
// the quadratic's bounding triangle but has been expanded so that the
// 1-pixel wide area around the curve is inside the poly.
// If a,b,c are the original control points then the poly a0,b0,c0,c1,a1
// that is rendered would look like this:
//              b0
//              b
//
//     a0              c0
//      a            c
//       a1       c1
// Each is drawn as three triangles ((a0,a1,b0), (b0,c1,c0), (a1,c1,b0))
// specified by these 9 indices:
static const uint16_t kQuadIdxBufPattern[] = {
    0, 1, 2,
    2, 4, 3,
    1, 4, 2
};

static const int kIdxsPerQuad = SK_ARRAY_COUNT(kQuadIdxBufPattern);
static const int kQuadNumVertices = 5;
static const int kQuadsNumInIdxBuffer = 256;


// Each line segment is rendered as two quads and two triangles.
// p0 and p1 have alpha = 1 while all other points have alpha = 0.
// The four external points are offset 1 pixel perpendicular to the
// line and half a pixel parallel to the line.
//
// p4                  p5
//      p0         p1
// p2                  p3
//
// Each is drawn as six triangles specified by these 18 indices:

static const uint16_t kLineSegIdxBufPattern[] = {
    0, 1, 3,
    0, 3, 2,
    0, 4, 5,
    0, 5, 1,
    0, 2, 4,
    1, 5, 3
};

static const int kIdxsPerLineSeg = SK_ARRAY_COUNT(kLineSegIdxBufPattern);
static const int kLineSegNumVertices = 6;
static const int kLineSegsNumInIdxBuffer = 256;

GrPathRenderer* GrAAHairLinePathRenderer::Create(GrContext* context) {
    GrGpu* gpu = context->getGpu();
    GrIndexBuffer* qIdxBuf = gpu->createInstancedIndexBuffer(kQuadIdxBufPattern,
                                                             kIdxsPerQuad,
                                                             kQuadsNumInIdxBuffer,
                                                             kQuadNumVertices);
    SkAutoTUnref<GrIndexBuffer> qIdxBuffer(qIdxBuf);
    GrIndexBuffer* lIdxBuf = gpu->createInstancedIndexBuffer(kLineSegIdxBufPattern,
                                                             kIdxsPerLineSeg,
                                                             kLineSegsNumInIdxBuffer,
                                                             kLineSegNumVertices);
    SkAutoTUnref<GrIndexBuffer> lIdxBuffer(lIdxBuf);
    return SkNEW_ARGS(GrAAHairLinePathRenderer,
                      (context, lIdxBuf, qIdxBuf));
}

GrAAHairLinePathRenderer::GrAAHairLinePathRenderer(
                                        const GrContext* context,
                                        const GrIndexBuffer* linesIndexBuffer,
                                        const GrIndexBuffer* quadsIndexBuffer) {
    fLinesIndexBuffer = linesIndexBuffer;
    linesIndexBuffer->ref();
    fQuadsIndexBuffer = quadsIndexBuffer;
    quadsIndexBuffer->ref();
}

GrAAHairLinePathRenderer::~GrAAHairLinePathRenderer() {
    fLinesIndexBuffer->unref();
    fQuadsIndexBuffer->unref();
}

namespace {

#define PREALLOC_PTARRAY(N) SkSTArray<(N),SkPoint, true>

// Takes 178th time of logf on Z600 / VC2010
int get_float_exp(float x) {
    GR_STATIC_ASSERT(sizeof(int) == sizeof(float));
#ifdef SK_DEBUG
    static bool tested;
    if (!tested) {
        tested = true;
        SkASSERT(get_float_exp(0.25f) == -2);
        SkASSERT(get_float_exp(0.3f) == -2);
        SkASSERT(get_float_exp(0.5f) == -1);
        SkASSERT(get_float_exp(1.f) == 0);
        SkASSERT(get_float_exp(2.f) == 1);
        SkASSERT(get_float_exp(2.5f) == 1);
        SkASSERT(get_float_exp(8.f) == 3);
        SkASSERT(get_float_exp(100.f) == 6);
        SkASSERT(get_float_exp(1000.f) == 9);
        SkASSERT(get_float_exp(1024.f) == 10);
        SkASSERT(get_float_exp(3000000.f) == 21);
    }
#endif
    const int* iptr = (const int*)&x;
    return (((*iptr) & 0x7f800000) >> 23) - 127;
}

// Uses the max curvature function for quads to estimate
// where to chop the conic. If the max curvature is not
// found along the curve segment it will return 1 and
// dst[0] is the original conic. If it returns 2 the dst[0]
// and dst[1] are the two new conics.
int split_conic(const SkPoint src[3], SkConic dst[2], const SkScalar weight) {
    SkScalar t = SkFindQuadMaxCurvature(src);
    if (t == 0) {
        if (dst) {
            dst[0].set(src, weight);
        }
        return 1;
    } else {
        if (dst) {
            SkConic conic;
            conic.set(src, weight);
            conic.chopAt(t, dst);
        }
        return 2;
    }
}

// Calls split_conic on the entire conic and then once more on each subsection.
// Most cases will result in either 1 conic (chop point is not within t range)
// or 3 points (split once and then one subsection is split again).
int chop_conic(const SkPoint src[3], SkConic dst[4], const SkScalar weight) {
    SkConic dstTemp[2];
    int conicCnt = split_conic(src, dstTemp, weight);
    if (2 == conicCnt) {
        int conicCnt2 = split_conic(dstTemp[0].fPts, dst, dstTemp[0].fW);
        conicCnt = conicCnt2 + split_conic(dstTemp[1].fPts, &dst[conicCnt2], dstTemp[1].fW);
    } else {
        dst[0] = dstTemp[0];
    }
    return conicCnt;
}

// returns 0 if quad/conic is degen or close to it
// in this case approx the path with lines
// otherwise returns 1
int is_degen_quad_or_conic(const SkPoint p[3], SkScalar* dsqd) {
    static const SkScalar gDegenerateToLineTol = SK_Scalar1;
    static const SkScalar gDegenerateToLineTolSqd =
        SkScalarMul(gDegenerateToLineTol, gDegenerateToLineTol);

    if (p[0].distanceToSqd(p[1]) < gDegenerateToLineTolSqd ||
        p[1].distanceToSqd(p[2]) < gDegenerateToLineTolSqd) {
        return 1;
    }

    *dsqd = p[1].distanceToLineBetweenSqd(p[0], p[2]);
    if (*dsqd < gDegenerateToLineTolSqd) {
        return 1;
    }

    if (p[2].distanceToLineBetweenSqd(p[1], p[0]) < gDegenerateToLineTolSqd) {
        return 1;
    }
    return 0;
}

int is_degen_quad_or_conic(const SkPoint p[3]) {
    SkScalar dsqd;
    return is_degen_quad_or_conic(p, &dsqd);
}

// we subdivide the quads to avoid huge overfill
// if it returns -1 then should be drawn as lines
int num_quad_subdivs(const SkPoint p[3]) {
    SkScalar dsqd;
    if (is_degen_quad_or_conic(p, &dsqd)) {
        return -1;
    }

    // tolerance of triangle height in pixels
    // tuned on windows  Quadro FX 380 / Z600
    // trade off of fill vs cpu time on verts
    // maybe different when do this using gpu (geo or tess shaders)
    static const SkScalar gSubdivTol = 175 * SK_Scalar1;

    if (dsqd <= SkScalarMul(gSubdivTol, gSubdivTol)) {
        return 0;
    } else {
        static const int kMaxSub = 4;
        // subdividing the quad reduces d by 4. so we want x = log4(d/tol)
        // = log4(d*d/tol*tol)/2
        // = log2(d*d/tol*tol)

        // +1 since we're ignoring the mantissa contribution.
        int log = get_float_exp(dsqd/(gSubdivTol*gSubdivTol)) + 1;
        log = SkTMin(SkTMax(0, log), kMaxSub);
        return log;
    }
}

/**
 * Generates the lines and quads to be rendered. Lines are always recorded in
 * device space. We will do a device space bloat to account for the 1pixel
 * thickness.
 * Quads are recorded in device space unless m contains
 * perspective, then in they are in src space. We do this because we will
 * subdivide large quads to reduce over-fill. This subdivision has to be
 * performed before applying the perspective matrix.
 */
int gather_lines_and_quads(const SkPath& path,
                           const SkMatrix& m,
                           const SkIRect& devClipBounds,
                           GrAAHairLinePathRenderer::PtArray* lines,
                           GrAAHairLinePathRenderer::PtArray* quads,
                           GrAAHairLinePathRenderer::PtArray* conics,
                           GrAAHairLinePathRenderer::IntArray* quadSubdivCnts,
                           GrAAHairLinePathRenderer::FloatArray* conicWeights) {
    SkPath::Iter iter(path, false);

    int totalQuadCount = 0;
    SkRect bounds;
    SkIRect ibounds;

    bool persp = m.hasPerspective();

    for (;;) {
        SkPoint pathPts[4];
        SkPoint devPts[4];
        SkPath::Verb verb = iter.next(pathPts);
        switch (verb) {
            case SkPath::kConic_Verb: {
                SkConic dst[4];
                // We chop the conics to create tighter clipping to hide error
                // that appears near max curvature of very thin conics. Thin
                // hyperbolas with high weight still show error.
                int conicCnt = chop_conic(pathPts, dst, iter.conicWeight());
                for (int i = 0; i < conicCnt; ++i) {
                    SkPoint* chopPnts = dst[i].fPts;
                    m.mapPoints(devPts, chopPnts, 3);
                    bounds.setBounds(devPts, 3);
                    bounds.outset(SK_Scalar1, SK_Scalar1);
                    bounds.roundOut(&ibounds);
                    if (SkIRect::Intersects(devClipBounds, ibounds)) {
                        if (is_degen_quad_or_conic(devPts)) {
                            SkPoint* pts = lines->push_back_n(4);
                            pts[0] = devPts[0];
                            pts[1] = devPts[1];
                            pts[2] = devPts[1];
                            pts[3] = devPts[2];
                        } else {
                            // when in perspective keep conics in src space
                            SkPoint* cPts = persp ? chopPnts : devPts;
                            SkPoint* pts = conics->push_back_n(3);
                            pts[0] = cPts[0];
                            pts[1] = cPts[1];
                            pts[2] = cPts[2];
                            conicWeights->push_back() = dst[i].fW;
                        }
                    }
                }
                break;
            }
            case SkPath::kMove_Verb:
                break;
            case SkPath::kLine_Verb:
                m.mapPoints(devPts, pathPts, 2);
                bounds.setBounds(devPts, 2);
                bounds.outset(SK_Scalar1, SK_Scalar1);
                bounds.roundOut(&ibounds);
                if (SkIRect::Intersects(devClipBounds, ibounds)) {
                    SkPoint* pts = lines->push_back_n(2);
                    pts[0] = devPts[0];
                    pts[1] = devPts[1];
                }
                break;
            case SkPath::kQuad_Verb: {
                SkPoint choppedPts[5];
                // Chopping the quad helps when the quad is either degenerate or nearly degenerate.
                // When it is degenerate it allows the approximation with lines to work since the
                // chop point (if there is one) will be at the parabola's vertex. In the nearly
                // degenerate the QuadUVMatrix computed for the points is almost singular which
                // can cause rendering artifacts.
                int n = SkChopQuadAtMaxCurvature(pathPts, choppedPts);
                for (int i = 0; i < n; ++i) {
                    SkPoint* quadPts = choppedPts + i * 2;
                    m.mapPoints(devPts, quadPts, 3);
                    bounds.setBounds(devPts, 3);
                    bounds.outset(SK_Scalar1, SK_Scalar1);
                    bounds.roundOut(&ibounds);

                    if (SkIRect::Intersects(devClipBounds, ibounds)) {
                        int subdiv = num_quad_subdivs(devPts);
                        SkASSERT(subdiv >= -1);
                        if (-1 == subdiv) {
                            SkPoint* pts = lines->push_back_n(4);
                            pts[0] = devPts[0];
                            pts[1] = devPts[1];
                            pts[2] = devPts[1];
                            pts[3] = devPts[2];
                        } else {
                            // when in perspective keep quads in src space
                            SkPoint* qPts = persp ? quadPts : devPts;
                            SkPoint* pts = quads->push_back_n(3);
                            pts[0] = qPts[0];
                            pts[1] = qPts[1];
                            pts[2] = qPts[2];
                            quadSubdivCnts->push_back() = subdiv;
                            totalQuadCount += 1 << subdiv;
                        }
                    }
                }
                break;
            }
            case SkPath::kCubic_Verb:
                m.mapPoints(devPts, pathPts, 4);
                bounds.setBounds(devPts, 4);
                bounds.outset(SK_Scalar1, SK_Scalar1);
                bounds.roundOut(&ibounds);
                if (SkIRect::Intersects(devClipBounds, ibounds)) {
                    PREALLOC_PTARRAY(32) q;
                    // we don't need a direction if we aren't constraining the subdivision
                    static const SkPath::Direction kDummyDir = SkPath::kCCW_Direction;
                    // We convert cubics to quadratics (for now).
                    // In perspective have to do conversion in src space.
                    if (persp) {
                        SkScalar tolScale =
                            GrPathUtils::scaleToleranceToSrc(SK_Scalar1, m,
                                                             path.getBounds());
                        GrPathUtils::convertCubicToQuads(pathPts, tolScale, false, kDummyDir, &q);
                    } else {
                        GrPathUtils::convertCubicToQuads(devPts, SK_Scalar1, false, kDummyDir, &q);
                    }
                    for (int i = 0; i < q.count(); i += 3) {
                        SkPoint* qInDevSpace;
                        // bounds has to be calculated in device space, but q is
                        // in src space when there is perspective.
                        if (persp) {
                            m.mapPoints(devPts, &q[i], 3);
                            bounds.setBounds(devPts, 3);
                            qInDevSpace = devPts;
                        } else {
                            bounds.setBounds(&q[i], 3);
                            qInDevSpace = &q[i];
                        }
                        bounds.outset(SK_Scalar1, SK_Scalar1);
                        bounds.roundOut(&ibounds);
                        if (SkIRect::Intersects(devClipBounds, ibounds)) {
                            int subdiv = num_quad_subdivs(qInDevSpace);
                            SkASSERT(subdiv >= -1);
                            if (-1 == subdiv) {
                                SkPoint* pts = lines->push_back_n(4);
                                // lines should always be in device coords
                                pts[0] = qInDevSpace[0];
                                pts[1] = qInDevSpace[1];
                                pts[2] = qInDevSpace[1];
                                pts[3] = qInDevSpace[2];
                            } else {
                                SkPoint* pts = quads->push_back_n(3);
                                // q is already in src space when there is no
                                // perspective and dev coords otherwise.
                                pts[0] = q[0 + i];
                                pts[1] = q[1 + i];
                                pts[2] = q[2 + i];
                                quadSubdivCnts->push_back() = subdiv;
                                totalQuadCount += 1 << subdiv;
                            }
                        }
                    }
                }
                break;
            case SkPath::kClose_Verb:
                break;
            case SkPath::kDone_Verb:
                return totalQuadCount;
        }
    }
}

struct LineVertex {
    SkPoint fPos;
    float fCoverage;
};

struct BezierVertex {
    SkPoint fPos;
    union {
        struct {
            SkScalar fK;
            SkScalar fL;
            SkScalar fM;
        } fConic;
        SkVector   fQuadCoord;
        struct {
            SkScalar fBogus[4];
        };
    };
};

GR_STATIC_ASSERT(sizeof(BezierVertex) == 3 * sizeof(SkPoint));

void intersect_lines(const SkPoint& ptA, const SkVector& normA,
                     const SkPoint& ptB, const SkVector& normB,
                     SkPoint* result) {

    SkScalar lineAW = -normA.dot(ptA);
    SkScalar lineBW = -normB.dot(ptB);

    SkScalar wInv = SkScalarMul(normA.fX, normB.fY) -
        SkScalarMul(normA.fY, normB.fX);
    wInv = SkScalarInvert(wInv);

    result->fX = SkScalarMul(normA.fY, lineBW) - SkScalarMul(lineAW, normB.fY);
    result->fX = SkScalarMul(result->fX, wInv);

    result->fY = SkScalarMul(lineAW, normB.fX) - SkScalarMul(normA.fX, lineBW);
    result->fY = SkScalarMul(result->fY, wInv);
}

void set_uv_quad(const SkPoint qpts[3], BezierVertex verts[kQuadNumVertices]) {
    // this should be in the src space, not dev coords, when we have perspective
    GrPathUtils::QuadUVMatrix DevToUV(qpts);
    DevToUV.apply<kQuadNumVertices, sizeof(BezierVertex), sizeof(SkPoint)>(verts);
}

void bloat_quad(const SkPoint qpts[3], const SkMatrix* toDevice,
                const SkMatrix* toSrc, BezierVertex verts[kQuadNumVertices]) {
    SkASSERT(!toDevice == !toSrc);
    // original quad is specified by tri a,b,c
    SkPoint a = qpts[0];
    SkPoint b = qpts[1];
    SkPoint c = qpts[2];

    if (toDevice) {
        toDevice->mapPoints(&a, 1);
        toDevice->mapPoints(&b, 1);
        toDevice->mapPoints(&c, 1);
    }
    // make a new poly where we replace a and c by a 1-pixel wide edges orthog
    // to edges ab and bc:
    //
    //   before       |        after
    //                |              b0
    //         b      |
    //                |
    //                |     a0            c0
    // a         c    |        a1       c1
    //
    // edges a0->b0 and b0->c0 are parallel to original edges a->b and b->c,
    // respectively.
    BezierVertex& a0 = verts[0];
    BezierVertex& a1 = verts[1];
    BezierVertex& b0 = verts[2];
    BezierVertex& c0 = verts[3];
    BezierVertex& c1 = verts[4];

    SkVector ab = b;
    ab -= a;
    SkVector ac = c;
    ac -= a;
    SkVector cb = b;
    cb -= c;

    // We should have already handled degenerates
    SkASSERT(ab.length() > 0 && cb.length() > 0);

    ab.normalize();
    SkVector abN;
    abN.setOrthog(ab, SkVector::kLeft_Side);
    if (abN.dot(ac) > 0) {
        abN.negate();
    }

    cb.normalize();
    SkVector cbN;
    cbN.setOrthog(cb, SkVector::kLeft_Side);
    if (cbN.dot(ac) < 0) {
        cbN.negate();
    }

    a0.fPos = a;
    a0.fPos += abN;
    a1.fPos = a;
    a1.fPos -= abN;

    c0.fPos = c;
    c0.fPos += cbN;
    c1.fPos = c;
    c1.fPos -= cbN;

    intersect_lines(a0.fPos, abN, c0.fPos, cbN, &b0.fPos);

    if (toSrc) {
        toSrc->mapPointsWithStride(&verts[0].fPos, sizeof(BezierVertex), kQuadNumVertices);
    }
}

// Equations based off of Loop-Blinn Quadratic GPU Rendering
// Input Parametric:
// P(t) = (P0*(1-t)^2 + 2*w*P1*t*(1-t) + P2*t^2) / (1-t)^2 + 2*w*t*(1-t) + t^2)
// Output Implicit:
// f(x, y, w) = f(P) = K^2 - LM
// K = dot(k, P), L = dot(l, P), M = dot(m, P)
// k, l, m are calculated in function GrPathUtils::getConicKLM
void set_conic_coeffs(const SkPoint p[3], BezierVertex verts[kQuadNumVertices],
                      const SkScalar weight) {
    SkScalar klm[9];

    GrPathUtils::getConicKLM(p, weight, klm);

    for (int i = 0; i < kQuadNumVertices; ++i) {
        const SkPoint pnt = verts[i].fPos;
        verts[i].fConic.fK = pnt.fX * klm[0] + pnt.fY * klm[1] + klm[2];
        verts[i].fConic.fL = pnt.fX * klm[3] + pnt.fY * klm[4] + klm[5];
        verts[i].fConic.fM = pnt.fX * klm[6] + pnt.fY * klm[7] + klm[8];
    }
}

void add_conics(const SkPoint p[3],
                const SkScalar weight,
                const SkMatrix* toDevice,
                const SkMatrix* toSrc,
                BezierVertex** vert) {
    bloat_quad(p, toDevice, toSrc, *vert);
    set_conic_coeffs(p, *vert, weight);
    *vert += kQuadNumVertices;
}

void add_quads(const SkPoint p[3],
               int subdiv,
               const SkMatrix* toDevice,
               const SkMatrix* toSrc,
               BezierVertex** vert) {
    SkASSERT(subdiv >= 0);
    if (subdiv) {
        SkPoint newP[5];
        SkChopQuadAtHalf(p, newP);
        add_quads(newP + 0, subdiv-1, toDevice, toSrc, vert);
        add_quads(newP + 2, subdiv-1, toDevice, toSrc, vert);
    } else {
        bloat_quad(p, toDevice, toSrc, *vert);
        set_uv_quad(p, *vert);
        *vert += kQuadNumVertices;
    }
}

void add_line(const SkPoint p[2],
              const SkMatrix* toSrc,
              uint8_t coverage,
              LineVertex** vert) {
    const SkPoint& a = p[0];
    const SkPoint& b = p[1];

    SkVector ortho, vec = b;
    vec -= a;

    if (vec.setLength(SK_ScalarHalf)) {
        // Create a vector orthogonal to 'vec' and of unit length
        ortho.fX = 2.0f * vec.fY;
        ortho.fY = -2.0f * vec.fX;

        float floatCoverage = GrNormalizeByteToFloat(coverage);

        (*vert)[0].fPos = a;
        (*vert)[0].fCoverage = floatCoverage;
        (*vert)[1].fPos = b;
        (*vert)[1].fCoverage = floatCoverage;
        (*vert)[2].fPos = a - vec + ortho;
        (*vert)[2].fCoverage = 0;
        (*vert)[3].fPos = b + vec + ortho;
        (*vert)[3].fCoverage = 0;
        (*vert)[4].fPos = a - vec - ortho;
        (*vert)[4].fCoverage = 0;
        (*vert)[5].fPos = b + vec - ortho;
        (*vert)[5].fCoverage = 0;

        if (toSrc) {
            toSrc->mapPointsWithStride(&(*vert)->fPos,
                                       sizeof(LineVertex),
                                       kLineSegNumVertices);
        }
    } else {
        // just make it degenerate and likely offscreen
        for (int i = 0; i < kLineSegNumVertices; ++i) {
            (*vert)[i].fPos.set(SK_ScalarMax, SK_ScalarMax);
        }
    }

    *vert += kLineSegNumVertices;
}

}

///////////////////////////////////////////////////////////////////////////////

bool GrAAHairLinePathRenderer::canDrawPath(const GrDrawTarget* target,
                                           const GrPipelineBuilder* pipelineBuilder,
                                           const SkMatrix& viewMatrix,
                                           const SkPath& path,
                                           const GrStrokeInfo& stroke,
                                           bool antiAlias) const {
    if (!antiAlias) {
        return false;
    }

    if (!IsStrokeHairlineOrEquivalent(stroke, viewMatrix, NULL)) {
        return false;
    }

    if (SkPath::kLine_SegmentMask == path.getSegmentMasks() ||
        target->caps()->shaderCaps()->shaderDerivativeSupport()) {
        return true;
    }
    return false;
}

template <class VertexType>
bool check_bounds(const SkMatrix& viewMatrix, const SkRect& devBounds, void* vertices, int vCount)
{
    SkRect tolDevBounds = devBounds;
    // The bounds ought to be tight, but in perspective the below code runs the verts
    // through the view matrix to get back to dev coords, which can introduce imprecision.
    if (viewMatrix.hasPerspective()) {
        tolDevBounds.outset(SK_Scalar1 / 1000, SK_Scalar1 / 1000);
    } else {
        // Non-persp matrices cause this path renderer to draw in device space.
        SkASSERT(viewMatrix.isIdentity());
    }
    SkRect actualBounds;

    VertexType* verts = reinterpret_cast<VertexType*>(vertices);
    bool first = true;
    for (int i = 0; i < vCount; ++i) {
        SkPoint pos = verts[i].fPos;
        // This is a hack to workaround the fact that we move some degenerate segments offscreen.
        if (SK_ScalarMax == pos.fX) {
            continue;
        }
        viewMatrix.mapPoints(&pos, 1);
        if (first) {
            actualBounds.set(pos.fX, pos.fY, pos.fX, pos.fY);
            first = false;
        } else {
            actualBounds.growToInclude(pos.fX, pos.fY);
        }
    }
    if (!first) {
        return tolDevBounds.contains(actualBounds);
    }

    return true;
}

class AAHairlineBatch : public GrBatch {
public:
    struct Geometry {
        GrColor fColor;
        uint8_t fCoverage;
        SkMatrix fViewMatrix;
        SkPath fPath;
        SkIRect fDevClipBounds;
    };

    // TODO Batch itself should not hold on to index buffers.  Instead, these should live in the
    // cache.
    static GrBatch* Create(const Geometry& geometry, const GrIndexBuffer* linesIndexBuffer,
                           const GrIndexBuffer* quadsIndexBuffer) {
        return SkNEW_ARGS(AAHairlineBatch, (geometry, linesIndexBuffer, quadsIndexBuffer));
    }

    const char* name() const override { return "AAHairlineBatch"; }

    void getInvariantOutputColor(GrInitInvariantOutput* out) const override {
        // When this is called on a batch, there is only one geometry bundle
        out->setKnownFourComponents(fGeoData[0].fColor);
    }
    void getInvariantOutputCoverage(GrInitInvariantOutput* out) const override {
        out->setUnknownSingleComponent();
    }

    void initBatchTracker(const GrPipelineInfo& init) override {
        // Handle any color overrides
        if (init.fColorIgnored) {
            fGeoData[0].fColor = GrColor_ILLEGAL;
        } else if (GrColor_ILLEGAL != init.fOverrideColor) {
            fGeoData[0].fColor = init.fOverrideColor;
        }

        // setup batch properties
        fBatch.fColorIgnored = init.fColorIgnored;
        fBatch.fColor = fGeoData[0].fColor;
        fBatch.fUsesLocalCoords = init.fUsesLocalCoords;
        fBatch.fCoverageIgnored = init.fCoverageIgnored;
        fBatch.fCoverage = fGeoData[0].fCoverage;
    }

    void generateGeometry(GrBatchTarget* batchTarget, const GrPipeline* pipeline) override;

    SkSTArray<1, Geometry, true>* geoData() { return &fGeoData; }

private:
    typedef SkTArray<SkPoint, true> PtArray;
    typedef SkTArray<int, true> IntArray;
    typedef SkTArray<float, true> FloatArray;

    AAHairlineBatch(const Geometry& geometry, const GrIndexBuffer* linesIndexBuffer,
                    const GrIndexBuffer* quadsIndexBuffer)
        : fLinesIndexBuffer(linesIndexBuffer)
        , fQuadsIndexBuffer(quadsIndexBuffer) {
        SkASSERT(linesIndexBuffer && quadsIndexBuffer);
        this->initClassID<AAHairlineBatch>();
        fGeoData.push_back(geometry);

        // compute bounds
        fBounds = geometry.fPath.getBounds();
        geometry.fViewMatrix.mapRect(&fBounds);
    }

    bool onCombineIfPossible(GrBatch* t) override {
        AAHairlineBatch* that = t->cast<AAHairlineBatch>();

        if (this->viewMatrix().hasPerspective() != that->viewMatrix().hasPerspective()) {
            return false;
        }

        // We go to identity if we don't have perspective
        if (this->viewMatrix().hasPerspective() &&
            !this->viewMatrix().cheapEqualTo(that->viewMatrix())) {
            return false;
        }

        // TODO we can actually batch hairlines if they are the same color in a kind of bulk method
        // but we haven't implemented this yet
        // TODO investigate going to vertex color and coverage?
        if (this->coverage() != that->coverage()) {
            return false;
        }

        if (this->color() != that->color()) {
            return false;
        }

        SkASSERT(this->usesLocalCoords() == that->usesLocalCoords());
        if (this->usesLocalCoords() && !this->viewMatrix().cheapEqualTo(that->viewMatrix())) {
            return false;
        }

        fGeoData.push_back_n(that->geoData()->count(), that->geoData()->begin());
        this->joinBounds(that->bounds());
        return true;
    }

    GrColor color() const { return fBatch.fColor; }
    uint8_t coverage() const { return fBatch.fCoverage; }
    bool usesLocalCoords() const { return fBatch.fUsesLocalCoords; }
    const SkMatrix& viewMatrix() const { return fGeoData[0].fViewMatrix; }

    struct BatchTracker {
        GrColor fColor;
        uint8_t fCoverage;
        SkRect fDevBounds;
        bool fUsesLocalCoords;
        bool fColorIgnored;
        bool fCoverageIgnored;
    };

    BatchTracker fBatch;
    SkSTArray<1, Geometry, true> fGeoData;
    const GrIndexBuffer* fLinesIndexBuffer;
    const GrIndexBuffer* fQuadsIndexBuffer;
};

void AAHairlineBatch::generateGeometry(GrBatchTarget* batchTarget, const GrPipeline* pipeline) {
    // Setup the viewmatrix and localmatrix for the GrGeometryProcessor.
    SkMatrix invert;
    if (!this->viewMatrix().invert(&invert)) {
        return;
    }

    // we will transform to identity space if the viewmatrix does not have perspective
    bool hasPerspective = this->viewMatrix().hasPerspective();
    const SkMatrix* geometryProcessorViewM = &SkMatrix::I();
    const SkMatrix* geometryProcessorLocalM = &invert;
    const SkMatrix* toDevice = NULL;
    const SkMatrix* toSrc = NULL;
    if (hasPerspective) {
        geometryProcessorViewM = &this->viewMatrix();
        geometryProcessorLocalM = &SkMatrix::I();
        toDevice = &this->viewMatrix();
        toSrc = &invert;
    }

    // Setup geometry processors for worst case
    uint32_t gpFlags = GrDefaultGeoProcFactory::kPosition_GPType |
                       GrDefaultGeoProcFactory::kCoverage_GPType;

    SkAutoTUnref<const GrGeometryProcessor> lineGP(
            GrDefaultGeoProcFactory::Create(gpFlags,
                                            this->color(),
                                            *geometryProcessorViewM,
                                            *geometryProcessorLocalM,
                                            false,
                                            this->coverage()));

    SkAutoTUnref<const GrGeometryProcessor> quadGP(
            GrQuadEffect::Create(this->color(),
                                 *geometryProcessorViewM,
                                 kHairlineAA_GrProcessorEdgeType,
                                 batchTarget->caps(),
                                 *geometryProcessorLocalM,
                                 this->coverage()));

    SkAutoTUnref<const GrGeometryProcessor> conicGP(
            GrConicEffect::Create(this->color(),
                                  *geometryProcessorViewM,
                                  kHairlineAA_GrProcessorEdgeType,
                                  batchTarget->caps(),
                                  *geometryProcessorLocalM,
                                  this->coverage()));

    // This is hand inlined for maximum performance.
    PREALLOC_PTARRAY(128) lines;
    PREALLOC_PTARRAY(128) quads;
    PREALLOC_PTARRAY(128) conics;
    IntArray qSubdivs;
    FloatArray cWeights;
    int quadCount = 0;

    int instanceCount = fGeoData.count();
    for (int i = 0; i < instanceCount; i++) {
        const Geometry& args = fGeoData[i];
        quadCount += gather_lines_and_quads(args.fPath, args.fViewMatrix, args.fDevClipBounds,
                                            &lines, &quads, &conics, &qSubdivs, &cWeights);
    }

    int lineCount = lines.count() / 2;
    int conicCount = conics.count() / 3;

    // do lines first
    if (lineCount) {
        batchTarget->initDraw(lineGP, pipeline);

        // TODO remove this when batch is everywhere
        GrPipelineInfo init;
        init.fColorIgnored = fBatch.fColorIgnored;
        init.fOverrideColor = GrColor_ILLEGAL;
        init.fCoverageIgnored = fBatch.fCoverageIgnored;
        init.fUsesLocalCoords = this->usesLocalCoords();
        lineGP->initBatchTracker(batchTarget->currentBatchTracker(), init);

        const GrVertexBuffer* vertexBuffer;
        int firstVertex;

        size_t vertexStride = lineGP->getVertexStride();
        int vertexCount = kLineSegNumVertices * lineCount;
        void *vertices = batchTarget->vertexPool()->makeSpace(vertexStride,
                                                              vertexCount,
                                                              &vertexBuffer,
                                                              &firstVertex);

        if (!vertices) {
            SkDebugf("Could not allocate vertices\n");
            return;
        }

        SkASSERT(lineGP->getVertexStride() == sizeof(LineVertex));

        LineVertex* verts = reinterpret_cast<LineVertex*>(vertices);
        for (int i = 0; i < lineCount; ++i) {
            add_line(&lines[2*i], toSrc, this->coverage(), &verts);
        }

        {
            GrDrawTarget::DrawInfo info;
            info.setVertexBuffer(vertexBuffer);
            info.setIndexBuffer(fLinesIndexBuffer);
            info.setPrimitiveType(kTriangles_GrPrimitiveType);
            info.setStartIndex(0);

            int lines = 0;
            while (lines < lineCount) {
                int n = SkTMin(lineCount - lines, kLineSegsNumInIdxBuffer);

                info.setStartVertex(kLineSegNumVertices*lines + firstVertex);
                info.setVertexCount(kLineSegNumVertices*n);
                info.setIndexCount(kIdxsPerLineSeg*n);
                batchTarget->draw(info);

                lines += n;
            }
        }
    }

    if (quadCount || conicCount) {
        const GrVertexBuffer* vertexBuffer;
        int firstVertex;

        size_t vertexStride = sizeof(BezierVertex);
        int vertexCount = kQuadNumVertices * quadCount + kQuadNumVertices * conicCount;
        void *vertices = batchTarget->vertexPool()->makeSpace(vertexStride,
                                                              vertexCount,
                                                              &vertexBuffer,
                                                              &firstVertex);

        if (!vertices) {
            SkDebugf("Could not allocate vertices\n");
            return;
        }

        // Setup vertices
        BezierVertex* verts = reinterpret_cast<BezierVertex*>(vertices);

        int unsubdivQuadCnt = quads.count() / 3;
        for (int i = 0; i < unsubdivQuadCnt; ++i) {
            SkASSERT(qSubdivs[i] >= 0);
            add_quads(&quads[3*i], qSubdivs[i], toDevice, toSrc, &verts);
        }

        // Start Conics
        for (int i = 0; i < conicCount; ++i) {
            add_conics(&conics[3*i], cWeights[i], toDevice, toSrc, &verts);
        }

        if (quadCount > 0) {
            batchTarget->initDraw(quadGP, pipeline);

            // TODO remove this when batch is everywhere
            GrPipelineInfo init;
            init.fColorIgnored = fBatch.fColorIgnored;
            init.fOverrideColor = GrColor_ILLEGAL;
            init.fCoverageIgnored = fBatch.fCoverageIgnored;
            init.fUsesLocalCoords = this->usesLocalCoords();
            quadGP->initBatchTracker(batchTarget->currentBatchTracker(), init);

            {
               GrDrawTarget::DrawInfo info;
               info.setVertexBuffer(vertexBuffer);
               info.setIndexBuffer(fQuadsIndexBuffer);
               info.setPrimitiveType(kTriangles_GrPrimitiveType);
               info.setStartIndex(0);

               int quads = 0;
               while (quads < quadCount) {
                   int n = SkTMin(quadCount - quads, kQuadsNumInIdxBuffer);

                   info.setStartVertex(kQuadNumVertices*quads + firstVertex);
                   info.setVertexCount(kQuadNumVertices*n);
                   info.setIndexCount(kIdxsPerQuad*n);
                   batchTarget->draw(info);

                   quads += n;
               }
           }
        }

        if (conicCount > 0) {
            batchTarget->initDraw(conicGP, pipeline);

            // TODO remove this when batch is everywhere
            GrPipelineInfo init;
            init.fColorIgnored = fBatch.fColorIgnored;
            init.fOverrideColor = GrColor_ILLEGAL;
            init.fCoverageIgnored = fBatch.fCoverageIgnored;
            init.fUsesLocalCoords = this->usesLocalCoords();
            conicGP->initBatchTracker(batchTarget->currentBatchTracker(), init);

            {
                GrDrawTarget::DrawInfo info;
                info.setVertexBuffer(vertexBuffer);
                info.setIndexBuffer(fQuadsIndexBuffer);
                info.setPrimitiveType(kTriangles_GrPrimitiveType);
                info.setStartIndex(0);

                int conics = 0;
                while (conics < conicCount) {
                    int n = SkTMin(conicCount - conics, kQuadsNumInIdxBuffer);

                    info.setStartVertex(kQuadNumVertices*(quadCount + conics) + firstVertex);
                    info.setVertexCount(kQuadNumVertices*n);
                    info.setIndexCount(kIdxsPerQuad*n);
                    batchTarget->draw(info);

                    conics += n;
                }
            }
        }
    }
}

static GrBatch* create_hairline_batch(GrColor color,
                                      const SkMatrix& viewMatrix,
                                      const SkPath& path,
                                      const GrStrokeInfo& stroke,
                                      const SkIRect& devClipBounds,
                                      const GrIndexBuffer* linesIndexBuffer,
                                      const GrIndexBuffer* quadsIndexBuffer) {
    SkScalar hairlineCoverage;
    uint8_t newCoverage = 0xff;
    if (GrPathRenderer::IsStrokeHairlineOrEquivalent(stroke, viewMatrix, &hairlineCoverage)) {
        newCoverage = SkScalarRoundToInt(hairlineCoverage * 0xff);
    }

    AAHairlineBatch::Geometry geometry;
    geometry.fColor = color;
    geometry.fCoverage = newCoverage;
    geometry.fViewMatrix = viewMatrix;
    geometry.fPath = path;
    geometry.fDevClipBounds = devClipBounds;

    return AAHairlineBatch::Create(geometry, linesIndexBuffer, quadsIndexBuffer);
}

bool GrAAHairLinePathRenderer::onDrawPath(GrDrawTarget* target,
                                          GrPipelineBuilder* pipelineBuilder,
                                          GrColor color,
                                          const SkMatrix& viewMatrix,
                                          const SkPath& path,
                                          const GrStrokeInfo& stroke,
                                          bool) {
    if (!fLinesIndexBuffer || !fQuadsIndexBuffer) {
        SkDebugf("unable to allocate indices\n");
        return false;
    }

    SkIRect devClipBounds;
    pipelineBuilder->clip().getConservativeBounds(pipelineBuilder->getRenderTarget(),
                                                  &devClipBounds);

    SkAutoTUnref<GrBatch> batch(create_hairline_batch(color, viewMatrix, path, stroke,
                                                      devClipBounds, fLinesIndexBuffer,
                                                      fQuadsIndexBuffer));
    target->drawBatch(pipelineBuilder, batch);

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef GR_TEST_UTILS

BATCH_TEST_DEFINE(AAHairlineBatch) {
    // TODO put these in the cache
    static GrIndexBuffer* gQuadIndexBuffer;
    static GrIndexBuffer* gLineIndexBuffer;
    if (!gQuadIndexBuffer) {
        gQuadIndexBuffer = context->getGpu()->createInstancedIndexBuffer(kQuadIdxBufPattern,
                                                                         kIdxsPerQuad,
                                                                         kQuadsNumInIdxBuffer,
                                                                         kQuadNumVertices);
        gLineIndexBuffer = context->getGpu()->createInstancedIndexBuffer(kLineSegIdxBufPattern,
                                                                         kIdxsPerLineSeg,
                                                                         kLineSegsNumInIdxBuffer,
                                                                         kLineSegNumVertices);
    }

    GrColor color = GrRandomColor(random);
    SkMatrix viewMatrix = GrTest::TestMatrix(random);
    GrStrokeInfo stroke(SkStrokeRec::kHairline_InitStyle);
    SkPath path = GrTest::TestPath(random);
    SkIRect devClipBounds;
    devClipBounds.setEmpty();
    return create_hairline_batch(color, viewMatrix, path, stroke, devClipBounds, gLineIndexBuffer,
                                 gQuadIndexBuffer);
}

#endif
