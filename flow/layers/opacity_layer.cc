// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/opacity_layer.h"

#include "flutter/fml/trace_event.h"
#include "third_party/skia/include/core/SkPaint.h"

namespace flutter {

OpacityLayer::OpacityLayer(SkAlpha alpha, const SkPoint& offset)
    : alpha_(alpha), offset_(offset), children_can_accept_opacity_(false) {
  // We can always inhert opacity even if we cannot pass it along to
  // our children as we can accumulate the inherited opacity into our
  // own opacity value before we recurse.
  set_layer_can_inherit_opacity(true);
}

void OpacityLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  auto* prev = static_cast<const OpacityLayer*>(old_layer);
  if (!context->IsSubtreeDirty()) {
    FML_DCHECK(prev);
    if (alpha_ != prev->alpha_ || offset_ != prev->offset_) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(old_layer));
    }
  }
  context->PushTransform(SkMatrix::Translate(offset_.fX, offset_.fY));
#ifndef SUPPORT_FRACTIONAL_TRANSLATION
  context->SetTransform(
      RasterCache::GetIntegralTransCTM(context->GetTransform()));
#endif
  DiffChildren(context, prev);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void OpacityLayer::Preroll(PrerollContext* context, const SkMatrix& matrix) {
  TRACE_EVENT0("flutter", "OpacityLayer::Preroll");
  FML_DCHECK(!layers().empty());  // We can't be a leaf.

  SkMatrix child_matrix = matrix;
  child_matrix.preTranslate(offset_.fX, offset_.fY);

  // Similar to what's done in TransformLayer::Preroll, we have to apply the
  // reverse transformation to the cull rect to properly cull child layers.
  context->cull_rect = context->cull_rect.makeOffset(-offset_.fX, -offset_.fY);

  context->mutators_stack.PushTransform(
      SkMatrix::Translate(offset_.fX, offset_.fY));
  context->mutators_stack.PushOpacity(alpha_);
  Layer::AutoPrerollSaveLayerState save =
      Layer::AutoPrerollSaveLayerState::Create(context);
  ContainerLayer::Preroll(context, child_matrix);
  context->mutators_stack.Pop();
  context->mutators_stack.Pop();

  set_children_can_accept_opacity(context->subtree_can_inherit_opacity);

  set_paint_bounds(paint_bounds().makeOffset(offset_.fX, offset_.fY));

  if (!children_can_accept_opacity()) {
#ifndef SUPPORT_FRACTIONAL_TRANSLATION
    child_matrix = RasterCache::GetIntegralTransCTM(child_matrix);
#endif
    TryToPrepareRasterCache(context, this, child_matrix,
                            RasterCacheLayerStrategy::kLayerChildren);
  }

  // Restore cull_rect
  context->cull_rect = context->cull_rect.makeOffset(offset_.fX, offset_.fY);
}

void OpacityLayer::Paint(PaintContext& context) const {
  TRACE_EVENT0("flutter", "OpacityLayer::Paint");
  FML_DCHECK(needs_painting(context));

  SkAutoCanvasRestore save(context.internal_nodes_canvas, true);
  context.internal_nodes_canvas->translate(offset_.fX, offset_.fY);

#ifndef SUPPORT_FRACTIONAL_TRANSLATION
  context.internal_nodes_canvas->setMatrix(RasterCache::GetIntegralTransCTM(
      context.leaf_nodes_canvas->getTotalMatrix()));
#endif

  SkScalar inherited_opacity = context.inherited_opacity;
  SkScalar subtree_opacity = opacity() * inherited_opacity;

  if (children_can_accept_opacity()) {
    context.inherited_opacity = subtree_opacity;
    PaintChildren(context);
    context.inherited_opacity = inherited_opacity;
    return;
  }

  SkPaint paint;
  paint.setAlphaf(subtree_opacity);

  if (context.raster_cache &&
      context.raster_cache->Draw(this, *context.leaf_nodes_canvas,
                                 RasterCacheLayerStrategy::kLayerChildren,
                                 &paint)) {
    return;
  }

  // Skia may clip the content with saveLayerBounds (although it's not a
  // guaranteed clip). So we have to provide a big enough saveLayerBounds. To do
  // so, we first remove the offset from paint bounds since it's already in the
  // matrix. Then we round out the bounds.
  //
  // Note that the following lines are only accessible when the raster cache is
  // not available (e.g., when we're using the software backend in golden
  // tests).
  SkRect saveLayerBounds;
  paint_bounds()
      .makeOffset(-offset_.fX, -offset_.fY)
      .roundOut(&saveLayerBounds);

  Layer::AutoSaveLayer save_layer =
      Layer::AutoSaveLayer::Create(context, saveLayerBounds, &paint);
  PaintChildren(context);
}

}  // namespace flutter
