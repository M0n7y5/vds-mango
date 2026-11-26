// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>
#pragma once

#include <functional>

#include <QVariantAnimation>
#include <QWidget>

#include "lottie.hh"

namespace vds::gui {

/// Covers its parent with the logo animation, then fades away and deletes
/// itself. A click or a key press skips to the fade.
class StartupAnimation : public QWidget {
public:
  /// on_done runs once the parent is in full view again.
  StartupAnimation(LottieAnimation animation, std::function<void()> on_done,
                   QWidget *parent);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;

private:
  void fade_out();

  LottieAnimation animation_;
  QVariantAnimation frames_;
  QVariantAnimation fade_;
  double opacity_ = 1;
};

} // namespace vds::gui
