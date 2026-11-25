// SPDX-License-Identifier: MIT
// Copyright (C) 2026 M0n7y5 <17201053+M0n7y5@users.noreply.github.com>

#include <algorithm>
#include <utility>

#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include "controller_card.hh"

namespace vds::gui {

namespace {

constexpr int kCardMinimumWidth = 380;
constexpr int kAddCardHeight = 72;
constexpr qreal kCardRadius = 12;
constexpr qreal kStatusRadius = 11;
constexpr qreal kChipRadius = 6;
constexpr QSize kPictureSize(112, 78);
constexpr QSize kStateIconSize(20, 20);
constexpr unsigned kLowBatteryPercent = 15;

enum class Tone { Plain, Muted, Positive, Warning, Negative };

QColor tone_color(Tone tone, const QPalette &palette) {
  switch (tone) {
  case Tone::Positive:
    return QColor(0x27, 0xae, 0x60);
  case Tone::Warning:
    return QColor(0xf6, 0x74, 0x00);
  case Tone::Negative:
    return QColor(0xda, 0x44, 0x53);
  case Tone::Muted:
    return palette.color(QPalette::PlaceholderText);
  case Tone::Plain:
    break;
  }
  return palette.color(QPalette::WindowText);
}

// QPalette::Mid nearly vanishes against the card on dark colour schemes.
QColor outline_color(const QPalette &palette) {
  QColor color = palette.color(QPalette::WindowText);
  color.setAlphaF(0.3);
  return color;
}

void make_secondary(QLabel *label) {
  QFont font = label->font();
  font.setPointSizeF(font.pointSizeF() * 0.9);
  label->setFont(font);
  QPalette colors = label->palette();
  colors.setColor(QPalette::WindowText,
                  colors.color(QPalette::PlaceholderText));
  label->setPalette(colors);
}

QPixmap tinted_picture(const QString &resource, QSize size,
                       const QColor &color, qreal pixel_ratio) {
  QPixmap picture = QIcon(resource).pixmap(size, pixel_ratio);
  QPainter painter(&picture);
  painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
  painter.fillRect(picture.rect(), color);
  return picture;
}

QString delay_name(unsigned audio_buffer) {
  switch (audio_buffer) {
  case 64:
    return "Standard";
  case 32:
    return "Reduced";
  case 24:
    return "Low";
  case 16:
    return "Lowest";
  }
  return QString::number(audio_buffer);
}

QToolButton *action_button(const QString &text, const char *icon,
                           QWidget *parent) {
  auto *button = new QToolButton(parent);
  button->setText(text);
  button->setIcon(QIcon::fromTheme(icon));
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setAutoRaise(true);
  return button;
}

} // namespace

QString profile_label(ControllerProfile profile) {
  switch (profile) {
  case ControllerProfile::Ds5:
    return "DualSense";
  case ControllerProfile::Dse:
    return "DualSense Edge";
  case ControllerProfile::Unspecified:
    break;
  }
  return "Automatic";
}

QString ports_label(std::span<const unsigned> ports) {
  if (ports.empty()) {
    return "Any";
  }
  QStringList labels;
  for (const unsigned port : ports) {
    labels.append(QString::number(port));
  }
  return labels.join(", ");
}

class ControllerCard::Pill : public QLabel {
public:
  Pill(qreal radius, QWidget *parent) : QLabel(parent), radius_(radius) {
    setContentsMargins(10, 3, 10, 3);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  }

  void show_text(Tone tone, const QString &text) {
    tone_ = tone;
    QPalette colors = parentWidget()->palette();
    colors.setColor(QPalette::WindowText, tone_color(tone, colors));
    setPalette(colors);
    setText(text);
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    {
      QPainter painter(this);
      painter.setRenderHint(QPainter::Antialiasing);
      const bool quiet = tone_ == Tone::Plain || tone_ == Tone::Muted;
      painter.setPen(QPen(quiet ? outline_color(palette())
                                : tone_color(tone_, palette()),
                          1));
      painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                              radius_, radius_);
    }
    QLabel::paintEvent(event);
  }

private:
  qreal radius_;
  Tone tone_ = Tone::Plain;
};

class ControllerCard::Battery : public QWidget {
public:
  explicit Battery(QWidget *parent) : QWidget(parent) { setFixedSize(30, 14); }

  void show_level(unsigned percent, bool charging) {
    percent_ = std::min(percent, 100u);
    charging_ = charging;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor outline = palette().color(QPalette::WindowText);
    painter.setPen(QPen(outline, 1));
    painter.drawRoundedRect(QRectF(0.5, 0.5, 25, 13), 3, 3);
    painter.fillRect(QRectF(26.5, 4, 2.5, 6), outline);
    const Tone tone = charging_                         ? Tone::Positive
                      : percent_ <= kLowBatteryPercent ? Tone::Negative
                                                       : Tone::Plain;
    painter.fillRect(QRectF(2.5, 2.5, 21.0 * percent_ / 100.0, 9),
                     tone_color(tone, palette()));
  }

private:
  unsigned percent_ = 0;
  bool charging_ = false;
};

ControllerCard::ControllerCard(std::function<void()> on_edit,
                               std::function<void()> on_remove,
                               QWidget *parent)
    : QFrame(parent) {
  setMinimumWidth(kCardMinimumWidth);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  picture_ = new QLabel(this);
  picture_->setFixedSize(kPictureSize);
  name_ = new QLabel(this);
  QFont bold = name_->font();
  bold.setBold(true);
  name_->setFont(bold);
  address_ = new QLabel(this);
  address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  make_secondary(address_);
  status_ = new Pill(kStatusRadius, this);
  battery_ = new Battery(this);
  battery_text_ = new QLabel(this);

  auto *state_row = new QHBoxLayout;
  state_row->addWidget(status_);
  state_row->addSpacing(6);
  state_row->addWidget(battery_);
  state_row->addWidget(battery_text_);
  state_row->addStretch(1);

  auto *identity = new QVBoxLayout;
  identity->addWidget(name_);
  identity->addWidget(address_);
  identity->addSpacing(4);
  identity->addLayout(state_row);
  identity->addStretch(1);

  auto *top = new QHBoxLayout;
  top->addWidget(picture_);
  top->addSpacing(8);
  top->addLayout(identity, 1);

  profile_ = new Pill(kChipRadius, this);
  port_ = new Pill(kChipRadius, this);
  delay_ = new Pill(kChipRadius, this);
  auto *chips = new QHBoxLayout;
  chips->addWidget(profile_);
  chips->addWidget(port_);
  chips->addWidget(delay_);
  chips->addStretch(1);

  headset_ = new QLabel(this);
  headset_->setFixedSize(kStateIconSize);
  mic_ = new QLabel(this);
  mic_->setFixedSize(kStateIconSize);
  detail_ = new QLabel(this);
  detail_->setWordWrap(true);
  make_secondary(detail_);
  auto *edit = action_button("Edit", "document-edit", this);
  auto *remove = action_button("Remove", "list-remove", this);
  connect(edit, &QToolButton::clicked, this, std::move(on_edit));
  connect(remove, &QToolButton::clicked, this, std::move(on_remove));
  auto *actions = new QHBoxLayout;
  actions->addWidget(headset_);
  actions->addWidget(mic_);
  actions->addWidget(detail_, 1);
  actions->addWidget(edit, 0, Qt::AlignBottom);
  actions->addWidget(remove, 0, Qt::AlignBottom);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 12, 10);
  layout->setSpacing(10);
  layout->addLayout(top);
  layout->addLayout(chips);
  layout->addLayout(actions);
}

void ControllerCard::show_controller(const ControllerStatus &controller,
                                     const QString &name) {
  connected_ = controller.connected;
  name_->setText(name);
  address_->setText(controller.address);

  const bool edge = name.contains("Edge");
  const int picture_state = (edge ? 2 : 0) | (connected_ ? 1 : 0);
  if (picture_state != picture_state_) {
    picture_state_ = picture_state;
    picture_->setPixmap(tinted_picture(
        edge ? ":/icons/dualsense-edge.svg" : ":/icons/dualsense.svg",
        kPictureSize,
        tone_color(connected_ ? Tone::Plain : Tone::Muted, palette()),
        devicePixelRatioF()));
  }

  headset_->setVisible(connected_);
  mic_->setVisible(connected_);
  const int state_icons =
      (controller.headset ? 1 : 0) | (controller.mic_muted ? 2 : 0);
  if (connected_ && state_icons != state_icons_) {
    state_icons_ = state_icons;
    headset_->setPixmap(tinted_picture(
        ":/icons/headset.svg", kStateIconSize,
        tone_color(controller.headset ? Tone::Plain : Tone::Muted, palette()),
        devicePixelRatioF()));
    headset_->setToolTip(controller.headset ? "Headset plugged in"
                                            : "No headset plugged in");
    mic_->setPixmap(tinted_picture(
        controller.mic_muted ? ":/icons/microphone-muted.svg"
                             : ":/icons/microphone.svg",
        kStateIconSize,
        tone_color(controller.mic_muted ? Tone::Warning : Tone::Plain,
                   palette()),
        devicePixelRatioF()));
    mic_->setToolTip(controller.mic_muted ? "Microphone muted"
                                          : "Microphone not muted");
  }

  QString detail;
  if (controller.connected) {
    status_->show_text(Tone::Positive, "Connected");
  } else if (kernel_fix_missing(controller)) {
    status_->show_text(Tone::Warning, "Needs a newer kernel");
    detail = kKernelRequirement;
  } else if (!controller.error.isEmpty()) {
    status_->show_text(Tone::Negative, "Failed");
    detail = controller.error;
  } else {
    status_->show_text(Tone::Muted, "Press PS to connect");
  }
  detail_->setText(detail);
  detail_->setToolTip(detail);

  battery_->setVisible(controller.battery.has_value());
  battery_text_->setVisible(controller.battery.has_value());
  if (controller.battery) {
    battery_->show_level(*controller.battery, controller.charging);
    battery_text_->setText(QString::number(*controller.battery) +
                           (controller.charging ? "%, charging" : "%"));
  }

  profile_->show_text(Tone::Plain,
                      controller.profile == ControllerProfile::Unspecified
                          ? QString("Appears as itself")
                          : "Appears as " + profile_label(controller.profile));
  if (controller.connected) {
    port_->show_text(Tone::Plain, "Port " + controller.path);
  } else if (controller.ports.empty()) {
    port_->show_text(Tone::Plain, "Any port");
  } else {
    port_->show_text(Tone::Plain,
                     (controller.ports.size() == 1 ? "Port " : "Ports ") +
                         ports_label(controller.ports));
  }
  delay_->show_text(Tone::Plain,
                    "Delay: " + delay_name(controller.audio_buffer));
  update();
}

void ControllerCard::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(connected_ ? QPen(palette().color(QPalette::Highlight), 1.5)
                            : QPen(outline_color(palette()), 1));
  painter.setBrush(palette().color(QPalette::Base));
  painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), kCardRadius,
                          kCardRadius);
}

AddControllerCard::AddControllerCard(QWidget *parent)
    : QAbstractButton(parent) {
  setText("+  Add controller");
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover);
  setMinimumWidth(kCardMinimumWidth);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize AddControllerCard::sizeHint() const {
  return {kCardMinimumWidth, kAddCardHeight};
}

void AddControllerCard::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QColor color = underMouse() || hasFocus()
                           ? palette().color(QPalette::Highlight)
                           : outline_color(palette());
  painter.setPen(QPen(color, 1.5, Qt::DashLine));
  painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), kCardRadius,
                          kCardRadius);
  painter.setPen(palette().color(QPalette::WindowText));
  painter.drawText(rect(), Qt::AlignCenter, text());
}

} // namespace vds::gui
