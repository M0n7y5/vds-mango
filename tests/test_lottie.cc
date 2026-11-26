// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include "vds_check.hh"

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QPainter>

#include "lottie.hh"

namespace vds::test {

namespace {

using vds::gui::LottieAnimation;

// A 100 by 100 animation of 60 frames.
QByteArray animation(const QByteArray &layers) {
  return R"({"v":"5.7.4","fr":60,"ip":0,"op":60,"w":100,"h":100,"layers":[)" +
         layers + "]}";
}

QByteArray still(const QByteArray &value) {
  return R"({"a":0,"k":)" + value + "}";
}

// A red 10 by 10 square around the origin of its layer.
const QByteArray kSquare =
    R"({"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[10,10]},)"
    R"("r":{"a":0,"k":0}},)"
    R"({"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},"r":1})";

QByteArray shape_layer(const QByteArray &position, const QByteArray &shapes,
                       const QByteArray &more = "") {
  return R"({"ind":1,"ty":4,"ip":0,"op":60,"ks":{"p":)" + position +
         R"(},"shapes":[)" + shapes + "]" + more + "}";
}

QImage drawn(const QByteArray &json, double frame) {
  const LottieAnimation animation(json);
  QImage image(animation.size().toSize(),
               QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  animation.draw(painter, frame);
  return image;
}

bool painted(const QImage &image, int x, int y) {
  return qAlpha(image.pixel(x, y)) > 127;
}

} // namespace

void test_shipped_startup_animation_ends_on_a_drawn_logo() {
  QFile file(VDS_STARTUP_ANIMATION);
  VDS_CHECK(file.open(QIODevice::ReadOnly));
  const LottieAnimation animation(file.readAll());
  QImage image(animation.size().toSize(),
               QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  animation.draw(painter, animation.last_frame());
  VDS_CHECK(painted(image, image.width() / 2, image.height() / 2));
}

void test_image_layer_is_refused() {
  const QByteArray json =
      animation(R"({"ind":1,"ty":2,"ip":0,"op":60,"ks":{}})");
  VDS_CHECK_THROWS(LottieAnimation(json));
}

void test_expression_is_refused() {
  const QByteArray json = animation(
      shape_layer(R"({"a":0,"k":[50,50],"x":"time * 2"})", kSquare));
  VDS_CHECK_THROWS(LottieAnimation(json));
}

void test_square_is_halfway_between_two_keyframes_without_easing() {
  const QImage image = drawn(
      animation(shape_layer(
          R"({"a":1,"k":[{"t":0,"s":[10,50]},{"t":20,"s":[90,50]}]})",
          kSquare)),
      10);
  VDS_CHECK(painted(image, 50, 50) && !painted(image, 10, 50));
}

void test_square_stays_where_its_last_keyframe_put_it() {
  const QImage image = drawn(
      animation(shape_layer(
          R"({"a":1,"k":[{"t":0,"s":[10,50]},{"t":20,"s":[90,50]}]})",
          kSquare)),
      40);
  VDS_CHECK(painted(image, 90, 50));
}

void test_easing_past_the_end_value_overshoots_it() {
  // Halfway through, a curve with handles (0.34, 1.56) and (0.64, 1) is at
  // 1.09 of the way: x = 10 + 40 * 1.09 = 53.5, so the square covers 57,
  // which lies right of where it comes to rest (45 to 55).
  const QImage image = drawn(
      animation(shape_layer(
          R"({"a":1,"k":[{"t":0,"s":[10,50],"o":{"x":[0.34],"y":[1.56]},)"
          R"("i":{"x":[0.64],"y":[1]}},{"t":20,"s":[50,50]}]})",
          kSquare)),
      10);
  VDS_CHECK(painted(image, 57, 50));
}

void test_layer_moves_with_its_parent() {
  const QImage image = drawn(
      animation(R"({"ind":1,"ty":4,"parent":2,"ip":0,"op":60,)"
                R"("ks":{"p":{"a":0,"k":[20,50]}},"shapes":[)" +
                kSquare +
                R"(]},{"ind":2,"ty":3,"ip":0,"op":60,)"
                R"("ks":{"p":{"a":0,"k":[30,0]}}})"),
      0);
  VDS_CHECK(painted(image, 50, 50) && !painted(image, 20, 50));
}

void test_mask_cuts_the_layer_off() {
  // The square spans 30 to 70 and the mask keeps what lies left of 50.
  const QByteArray big_square =
      R"({"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]}},)"
      R"({"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},"r":1})";
  const QImage image = drawn(
      animation(shape_layer(
          still("[50,50]"), big_square,
          R"(,"masksProperties":[{"mode":"a","inv":false,"pt":{"a":0,"k":)"
          R"({"c":true,"v":[[-50,-50],[0,-50],[0,50],[-50,50]],)"
          R"("i":[[0,0],[0,0],[0,0],[0,0]],)"
          R"("o":[[0,0],[0,0],[0,0],[0,0]]}}}])")),
      0);
  VDS_CHECK(painted(image, 40, 50) && !painted(image, 60, 50));
}

void test_trim_draws_the_start_of_a_stroke() {
  // A line from 10 to 90, of which the first half is kept.
  const QByteArray line =
      R"({"ty":"sh","ks":{"a":0,"k":{"c":false,"v":[[10,50],[90,50]],)"
      R"("i":[[0,0],[0,0]],"o":[[0,0],[0,0]]}}},)"
      R"({"ty":"tm","s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0}},)"
      R"({"ty":"st","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},)"
      R"("w":{"a":0,"k":6},"lc":1,"lj":1})";
  const QImage image =
      drawn(animation(shape_layer(still("[0,0]"), line)), 0);
  VDS_CHECK(painted(image, 30, 50) && !painted(image, 70, 50));
}

void test_half_transparent_layer_is_drawn_half_transparent() {
  const QImage image = drawn(
      animation(R"({"ind":1,"ty":4,"ip":0,"op":60,"ks":{"p":)" +
                still("[50,50]") + R"(,"o":)" + still("50") +
                R"(},"shapes":[)" + kSquare + "]}"),
      0);
  const int alpha = qAlpha(image.pixel(50, 50));
  VDS_CHECK(alpha > 120 && alpha < 135);
}

void run_tests() {
  test_shipped_startup_animation_ends_on_a_drawn_logo();
  test_image_layer_is_refused();
  test_expression_is_refused();
  test_square_is_halfway_between_two_keyframes_without_easing();
  test_square_stays_where_its_last_keyframe_put_it();
  test_easing_past_the_end_value_overshoots_it();
  test_layer_moves_with_its_parent();
  test_mask_cuts_the_layer_off();
  test_trim_draws_the_start_of_a_stroke();
  test_half_transparent_layer_is_drawn_half_transparent();
}

} // namespace vds::test
