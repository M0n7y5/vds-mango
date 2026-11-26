// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSizeF>

class QByteArray;
class QPainter;

namespace vds::gui {

/// Draws a Lottie animation with QPainter. Qt's own player is QML only and
/// no other one is packaged for Arch, so this knows the part of the format
/// the startup animation is made of. Layers: null and shape layers with
/// parents and static masks. Shapes: groups of rectangles, ellipses and
/// static paths with fills, strokes and a trim. Keyframes: cubic easing.
class LottieAnimation {
public:
  /// Throws std::runtime_error when the animation uses anything else, so a
  /// replaced file is refused instead of drawn wrong.
  explicit LottieAnimation(const QByteArray &json);

  QSizeF size() const { return size_; }
  /// frame may run from 0 to this.
  double last_frame() const { return last_frame_; }
  int duration_ms() const;
  /// Draws into a size() box at the painter's origin. frame may lie between
  /// two frames.
  void draw(QPainter &painter, double frame) const;

private:
  QJsonObject parent_of(const QJsonObject &layer) const;

  QJsonArray layers_;
  /// Where the layer with a given "ind" sits in layers_.
  QHash<int, qsizetype> layer_positions_;
  QSizeF size_;
  double frame_rate_ = 0;
  double first_frame_ = 0;
  double last_frame_ = 0;
};

} // namespace vds::gui
