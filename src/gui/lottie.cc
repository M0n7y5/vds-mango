// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <QByteArray>
#include <QColor>
#include <QEasingCurve>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTransform>

#include "lottie.hh"

namespace vds::gui {

namespace {

constexpr int kNullLayer = 3;
constexpr int kShapeLayer = 4;
constexpr int kTrimSteps = 48;

[[noreturn]] void unsupported(const QString &what) {
  throw std::runtime_error(
      ("the animation uses " + what + ", which cannot be drawn")
          .toStdString());
}

bool animated(const QJsonObject &property) {
  return property.value("a").toInt() == 1;
}

QList<double> numbers(const QJsonValue &value) {
  if (!value.isArray()) {
    return {value.toDouble()};
  }
  QList<double> list;
  const QJsonArray array = value.toArray();
  for (const auto &number : array) {
    list.append(number.toDouble());
  }
  return list;
}

/* How far a keyframe has moved towards the next one, which may be past it. */
double eased(const QJsonObject &keyframe, double progress) {
  if (!keyframe.contains("o") || !keyframe.contains("i")) {
    return progress;
  }
  const auto handle = [&](const char *name) {
    const QJsonObject point = keyframe.value(name).toObject();
    return QPointF(numbers(point.value("x")).value(0),
                   numbers(point.value("y")).value(0));
  };
  QEasingCurve curve(QEasingCurve::BezierSpline);
  curve.addCubicBezierSegment(handle("o"), handle("i"), QPointF(1, 1));
  return curve.valueForProgress(progress);
}

/* One number per dimension of the property. */
QList<double> value_at(const QJsonObject &property, double frame) {
  const QJsonValue held = property.value("k");
  if (!animated(property)) {
    return numbers(held);
  }
  const QJsonArray keyframes = held.toArray();
  qsizetype next = 0;
  while (next < keyframes.size() &&
         keyframes.at(next).toObject().value("t").toDouble() <= frame) {
    ++next;
  }
  if (next == 0) {
    return numbers(keyframes.first().toObject().value("s"));
  }
  const QJsonObject from = keyframes.at(next - 1).toObject();
  QList<double> value = numbers(from.value("s"));
  if (next == keyframes.size() || from.value("h").toInt() == 1) {
    return value;
  }
  const QJsonObject to = keyframes.at(next).toObject();
  const double start = from.value("t").toDouble();
  const double progress =
      eased(from, (frame - start) / (to.value("t").toDouble() - start));
  const QList<double> target = numbers(to.value("s"));
  for (qsizetype i = 0; i < std::min(value.size(), target.size()); ++i) {
    value[i] += (target.at(i) - value.at(i)) * progress;
  }
  return value;
}

struct Placement {
  QTransform transform;
  double opacity = 1;
};

/* The transform of a layer ("ks") or of a group ("tr"). */
Placement placement(const QJsonObject &transform, double frame) {
  const auto at = [&](const char *key, qsizetype dimension, double fallback) {
    const QJsonValue property = transform.value(key);
    if (!property.isObject()) {
      return fallback;
    }
    return value_at(property.toObject(), frame).value(dimension, fallback);
  };
  Placement result;
  result.transform.translate(at("p", 0, 0), at("p", 1, 0));
  result.transform.rotate(at("r", 0, 0));
  result.transform.scale(at("s", 0, 100) / 100, at("s", 1, 100) / 100);
  result.transform.translate(-at("a", 0, 0), -at("a", 1, 0));
  result.opacity = at("o", 0, 100) / 100;
  return result;
}

/* A path as Lottie stores it: vertices with tangents relative to them. */
QPainterPath bezier_path(const QJsonObject &bezier) {
  const QJsonArray vertices = bezier.value("v").toArray();
  const QJsonArray in = bezier.value("i").toArray();
  const QJsonArray out = bezier.value("o").toArray();
  const auto point = [](const QJsonArray &points, qsizetype index) {
    const QJsonArray pair = points.at(index).toArray();
    return QPointF(pair.at(0).toDouble(), pair.at(1).toDouble());
  };
  QPainterPath path;
  if (vertices.isEmpty()) {
    return path;
  }
  const bool closed = bezier.value("c").toBool();
  path.moveTo(point(vertices, 0));
  const qsizetype segments = closed ? vertices.size() : vertices.size() - 1;
  for (qsizetype from = 0; from < segments; ++from) {
    const qsizetype to = (from + 1) % vertices.size();
    path.cubicTo(point(vertices, from) + point(out, from),
                 point(vertices, to) + point(in, to), point(vertices, to));
  }
  if (closed) {
    path.closeSubpath();
  }
  return path;
}

/* The stretch of a path between two fractions of its length. */
QPainterPath trimmed(const QPainterPath &path, double start, double end) {
  QPainterPath part;
  if (end <= start) {
    return part;
  }
  part.moveTo(path.pointAtPercent(start));
  for (int step = 1; step <= kTrimSteps; ++step) {
    part.lineTo(
        path.pointAtPercent(start + (end - start) * step / kTrimSteps));
  }
  return part;
}

QColor color_of(const QJsonObject &paint, double frame) {
  const QList<double> channels = value_at(paint.value("c").toObject(), frame);
  return QColor::fromRgbF(static_cast<float>(channels.value(0)),
                          static_cast<float>(channels.value(1)),
                          static_cast<float>(channels.value(2)));
}

QPen pen_of(const QJsonObject &stroke, double frame) {
  static constexpr Qt::PenCapStyle kCaps[] = {Qt::FlatCap, Qt::RoundCap,
                                              Qt::SquareCap};
  static constexpr Qt::PenJoinStyle kJoins[] = {Qt::MiterJoin, Qt::RoundJoin,
                                                Qt::BevelJoin};
  return QPen(color_of(stroke, frame),
              value_at(stroke.value("w").toObject(), frame).first(),
              Qt::SolidLine,
              kCaps[std::clamp(stroke.value("lc").toInt(1), 1, 3) - 1],
              kJoins[std::clamp(stroke.value("lj").toInt(1), 1, 3) - 1]);
}

/*
 * Draws a group, or the top level of a shape layer. Every fill and stroke
 * paints all shapes of its group, and what comes first lies on top.
 */
void draw_items(QPainter &painter, const QJsonArray &items, double frame,
                double opacity) {
  painter.save();
  QPainterPath shape;
  for (const auto &entry : items) {
    const QJsonObject item = entry.toObject();
    const QString type = item.value("ty").toString();
    const auto at = [&](const char *key) {
      return value_at(item.value(key).toObject(), frame);
    };
    if (type == "tr") {
      const Placement own = placement(item, frame);
      painter.setTransform(own.transform, true);
      opacity *= own.opacity;
    } else if (type == "el") {
      const QList<double> center = at("p");
      const QList<double> size = at("s");
      shape.addEllipse(QPointF(center.value(0), center.value(1)),
                       size.value(0) / 2, size.value(1) / 2);
    } else if (type == "rc") {
      const QList<double> center = at("p");
      const QList<double> size = at("s");
      const double radius = std::min(
          {at("r").first(), size.value(0) / 2, size.value(1) / 2});
      shape.addRoundedRect(QRectF(center.value(0) - size.value(0) / 2,
                                  center.value(1) - size.value(1) / 2,
                                  size.value(0), size.value(1)),
                           radius, radius);
    } else if (type == "sh") {
      shape.addPath(bezier_path(
          item.value("ks").toObject().value("k").toObject()));
    }
  }
  for (const auto &entry : items) {
    const QJsonObject item = entry.toObject();
    if (item.value("ty").toString() == "tm") {
      shape = trimmed(
          shape, value_at(item.value("s").toObject(), frame).first() / 100,
          value_at(item.value("e").toObject(), frame).first() / 100);
    }
  }
  for (qsizetype index = items.size(); index-- > 0;) {
    const QJsonObject item = items.at(index).toObject();
    const QString type = item.value("ty").toString();
    if (type == "gr") {
      draw_items(painter, item.value("it").toArray(), frame, opacity);
      continue;
    }
    if (type != "fl" && type != "st") {
      continue;
    }
    painter.setOpacity(
        opacity * value_at(item.value("o").toObject(), frame).first() / 100);
    if (type == "fl") {
      shape.setFillRule(item.value("r").toInt() == 2 ? Qt::OddEvenFill
                                                     : Qt::WindingFill);
      painter.fillPath(shape, color_of(item, frame));
    } else {
      painter.strokePath(shape, pen_of(item, frame));
    }
  }
  painter.restore();
}

/*
 * Expressions need a script engine, spatial tangents bend the way between
 * two keyframes, and an easing per axis needs one curve each.
 */
void refuse_what_keyframes_cannot_do(const QJsonValue &value) {
  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const auto &entry : array) {
      refuse_what_keyframes_cannot_do(entry);
    }
    return;
  }
  if (!value.isObject()) {
    return;
  }
  const QJsonObject object = value.toObject();
  if (object.value("x").isString()) {
    unsupported("expressions");
  }
  if (object.contains("to") || object.contains("ti")) {
    unsupported("curved motion paths");
  }
  for (const char *handle : {"o", "i"}) {
    const QJsonValue point = object.value(handle);
    if (point.isObject() &&
        numbers(point.toObject().value("x")).size() > 1) {
      unsupported("an easing per axis");
    }
  }
  for (const auto &member : object) {
    refuse_what_keyframes_cannot_do(member);
  }
}

void check_items(const QJsonArray &items) {
  static const QStringList kTypes = {"gr", "el", "rc", "sh",
                                     "fl", "st", "tm", "tr"};
  for (const auto &entry : items) {
    const QJsonObject item = entry.toObject();
    const QString type = item.value("ty").toString();
    if (!kTypes.contains(type)) {
      unsupported("the shape item \"" + type + "\"");
    }
    if (type == "gr") {
      check_items(item.value("it").toArray());
    } else if (type == "sh" && animated(item.value("ks").toObject())) {
      unsupported("an animated path");
    } else if (type == "st" && item.contains("d")) {
      unsupported("a dashed stroke");
    } else if (type == "tm") {
      const QJsonObject offset = item.value("o").toObject();
      if (animated(offset) || offset.value("k").toDouble() != 0) {
        unsupported("a trim offset");
      }
    }
  }
}

void check_layer(const QJsonObject &layer) {
  const int type = layer.value("ty").toInt(-1);
  if (type != kNullLayer && type != kShapeLayer) {
    unsupported("a layer of type " + QString::number(type));
  }
  for (const char *key : {"tt", "td", "tm"}) {
    if (layer.contains(key)) {
      unsupported("a matte or time remapping");
    }
  }
  if (!layer.value("ef").toArray().isEmpty()) {
    unsupported("layer effects");
  }
  if (layer.value("sr").toDouble(1) != 1 ||
      layer.value("st").toDouble() != 0) {
    unsupported("a stretched or shifted layer");
  }
  const QJsonObject transform = layer.value("ks").toObject();
  for (const QString &key : transform.keys()) {
    if (!QStringList{"a", "p", "s", "r", "o"}.contains(key)) {
      unsupported("the layer transform \"" + key + "\"");
    }
  }
  if (transform.value("p").toObject().contains("s")) {
    unsupported("a position split into axes");
  }
  const QJsonArray masks = layer.value("masksProperties").toArray();
  for (const auto &entry : masks) {
    const QJsonObject mask = entry.toObject();
    if (mask.value("mode").toString() != "a" ||
        mask.value("inv").toBool() ||
        animated(mask.value("pt").toObject())) {
      unsupported("a mask that is not a plain static cut-out");
    }
  }
  check_items(layer.value("shapes").toArray());
}

} // namespace

LottieAnimation::LottieAnimation(const QByteArray &json) {
  const QJsonObject root = QJsonDocument::fromJson(json).object();
  layers_ = root.value("layers").toArray();
  size_ = QSizeF(root.value("w").toDouble(), root.value("h").toDouble());
  frame_rate_ = root.value("fr").toDouble();
  first_frame_ = root.value("ip").toDouble();
  // The frame at "op" is the first one that is no longer part of it.
  last_frame_ = root.value("op").toDouble() - first_frame_ - 1;
  if (layers_.isEmpty() || size_.isEmpty() || frame_rate_ <= 0 ||
      last_frame_ <= 0) {
    unsupported("no layers, size or duration");
  }
  if (!root.value("assets").toArray().isEmpty()) {
    unsupported("images or nested compositions");
  }
  refuse_what_keyframes_cannot_do(root);
  for (qsizetype position = 0; position < layers_.size(); ++position) {
    const QJsonObject layer = layers_.at(position).toObject();
    check_layer(layer);
    layer_positions_.insert(layer.value("ind").toInt(), position);
  }
  for (const auto &entry : std::as_const(layers_)) {
    QJsonObject layer = entry.toObject();
    for (qsizetype depth = 0; !layer.isEmpty(); ++depth) {
      if (depth > layers_.size()) {
        unsupported("layers that are their own parents");
      }
      layer = parent_of(layer);
    }
  }
}

int LottieAnimation::duration_ms() const {
  return static_cast<int>(std::lround(last_frame_ * 1000 / frame_rate_));
}

QJsonObject LottieAnimation::parent_of(const QJsonObject &layer) const {
  const auto position = layer_positions_.constFind(
      layer.value("parent").toInt(-1));
  return position == layer_positions_.constEnd()
             ? QJsonObject()
             : layers_.at(*position).toObject();
}

void LottieAnimation::draw(QPainter &painter, double frame) const {
  frame += first_frame_;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  const double opacity = painter.opacity();
  // The first layer lies on top.
  for (qsizetype position = layers_.size(); position-- > 0;) {
    const QJsonObject layer = layers_.at(position).toObject();
    if (layer.value("ty").toInt() != kShapeLayer ||
        frame < layer.value("ip").toDouble() ||
        frame >= layer.value("op").toDouble()) {
      continue;
    }
    const Placement own = placement(layer.value("ks").toObject(), frame);
    QTransform transform = own.transform;
    for (QJsonObject parent = parent_of(layer); !parent.isEmpty();
         parent = parent_of(parent)) {
      transform *= placement(parent.value("ks").toObject(), frame).transform;
    }
    painter.save();
    painter.setTransform(transform, true);
    const QJsonArray masks = layer.value("masksProperties").toArray();
    if (!masks.isEmpty()) {
      QPainterPath cut_out;
      for (const auto &mask : masks) {
        cut_out.addPath(bezier_path(mask.toObject()
                                        .value("pt")
                                        .toObject()
                                        .value("k")
                                        .toObject()));
      }
      painter.setClipPath(cut_out, Qt::IntersectClip);
    }
    // A layer does not pass its opacity on to the layers it is parent of.
    draw_items(painter, layer.value("shapes").toArray(), frame,
               opacity * own.opacity);
    painter.restore();
  }
  painter.restore();
}

} // namespace vds::gui
