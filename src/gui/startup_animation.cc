// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QEvent>
#include <QPainter>

#include "startup_animation.hh"

namespace vds::gui {

namespace {

constexpr double kLogoSize = 256;
constexpr double kLargestShareOfWindow = 0.8;
constexpr int kFadeMs = 250;

} // namespace

StartupAnimation::StartupAnimation(LottieAnimation animation,
                                   std::function<void()> on_done,
                                   QWidget *parent)
    : QWidget(parent), animation_(std::move(animation)) {
  setGeometry(parent->rect());
  // Taking the keyboard focus instead would hand it to some button of the
  // parent afterwards, which then looks selected.
  QCoreApplication::instance()->installEventFilter(this);

  frames_.setStartValue(0.0);
  frames_.setEndValue(animation_.last_frame());
  frames_.setDuration(animation_.duration_ms());
  connect(&frames_, &QVariantAnimation::valueChanged, this,
          [this] { update(); });
  connect(&frames_, &QVariantAnimation::finished, this,
          [this] { fade_out(); });

  fade_.setStartValue(1.0);
  fade_.setEndValue(0.0);
  fade_.setDuration(kFadeMs);
  connect(&fade_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &opacity) {
            opacity_ = opacity.toDouble();
            update();
          });
  connect(&fade_, &QVariantAnimation::finished, this,
          [this, on_done = std::move(on_done)] {
            hide();
            deleteLater();
            on_done();
          });

  raise();
  frames_.start();
}

bool StartupAnimation::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::KeyPress) {
    fade_out();
    return true;
  }
  if (watched == parent() && event->type() == QEvent::Resize) {
    setGeometry(parentWidget()->rect());
  }
  return false;
}

void StartupAnimation::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setOpacity(opacity_);
  painter.fillRect(rect(), palette().color(QPalette::Window));
  const double side = std::min({kLogoSize, width() * kLargestShareOfWindow,
                                height() * kLargestShareOfWindow});
  painter.translate((width() - side) / 2, (height() - side) / 2);
  painter.scale(side / animation_.size().width(),
                side / animation_.size().height());
  animation_.draw(painter, frames_.currentValue().toDouble());
}

void StartupAnimation::mousePressEvent(QMouseEvent *) { fade_out(); }

void StartupAnimation::fade_out() {
  if (fade_.state() != QAbstractAnimation::Stopped || opacity_ == 0) {
    return;
  }
  frames_.stop();
  fade_.start();
}

} // namespace vds::gui
