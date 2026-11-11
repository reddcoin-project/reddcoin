// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/upgradeindicator.h>

#include <QImage>
#include <QObject>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cassert>

namespace UpgradeIndicator {

Status Evaluate(const SignallingInfo& info, const std::set<WarningKind>& kinds)
{
    Status status;
    QStringList critical, warning, upgrade;

    if (kinds.count(WarningKind::UNKNOWN_RULE_ACTIVE)) {
        critical << QObject::tr("A network rule change that this version does not know about is in effect. This version may not follow the network. Upgrade now.");
    }
    if (kinds.count(WarningKind::UNKNOWN_RULE_LOCKED_IN)) {
        critical << QObject::tr("A network rule change that this version does not know about is locked in. Upgrade before it takes effect.");
    }
    if (kinds.count(WarningKind::UNKNOWN_RULE_SIGNALLING)) {
        warning << QObject::tr("Most recent blocks signal a network rule change that this version does not know about. Upgrade soon.");
    }
    if (kinds.count(WarningKind::UNKNOWN_BLOCK_VERSION)) {
        warning << QObject::tr("Most recent blocks use a block version that this version does not recognise. The network may be changing its rules.");
    }
    if (kinds.count(WarningKind::INVALID_CHAIN)) {
        warning << QObject::tr("A chain with more work than this node's is invalid to this node. This node or other nodes may need to upgrade.");
    }
    if (kinds.count(WarningKind::HEAVIER_CHAIN)) {
        warning << QObject::tr("A chain with more work than this node's is not being followed. This node may be on a minority fork.");
    }

    for (const DeploymentSignalling& deployment : info.deployments) {
        if (!deployment.scheduled) continue;
        const QString name = QString::fromStdString(deployment.name);
        if (deployment.state == ThresholdState::STARTED) {
            upgrade << QObject::tr("%1 is being signalled: %2 of %3 blocks so far this window, %4 needed.")
                           .arg(name)
                           .arg(deployment.count)
                           .arg(info.elapsed)
                           .arg(info.threshold);
        } else if (deployment.state == ThresholdState::LOCKED_IN) {
            upgrade << QObject::tr("%1 is locked in and takes effect at block %2.")
                           .arg(name)
                           .arg(deployment.activation_height);
        }
    }

    if (!critical.isEmpty()) {
        status.level = Level::CRITICAL;
    } else if (!warning.isEmpty()) {
        status.level = Level::WARNING;
    } else if (!upgrade.isEmpty()) {
        status.level = Level::UPGRADE;
    }
    status.reasons << critical << warning << upgrade;
    return status;
}

QColor IconColor(Level level, const QColor& theme_color)
{
    switch (level) {
    case Level::NONE: return {};
    case Level::UPGRADE: return theme_color;
    case Level::WARNING: return QColor(0xd9, 0x7a, 0x00); // NetworkUpgradesWidget::warningColor()
    case Level::CRITICAL: return QColor(0xd3, 0x2f, 0x2f);
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString ToolTip(const Status& status)
{
    if (status.reasons.isEmpty()) {
        return QObject::tr("No network upgrade in progress.");
    }
    QString text;
    for (const QString& reason : status.reasons) {
        text += QStringLiteral("<p>") + reason.toHtmlEscaped() + QStringLiteral("</p>");
    }
    text += QStringLiteral("<p>") + QObject::tr("Click for the Network Upgrades tab.").toHtmlEscaped() + QStringLiteral("</p>");
    return text;
}

QPixmap Icon(Level level, const QColor& theme_color, int size, qreal device_pixel_ratio)
{
    if (level == Level::NONE) return {};
    const QColor color = IconColor(level, theme_color);
    const qreal ratio = std::max<qreal>(1.0, device_pixel_ratio);
    const int pixels = qRound(size * ratio);

    QImage image;
    if (level == Level::UPGRADE) {
        // An up arrow: a head and a stem.
        image = QImage(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal s = pixels;
        QPainterPath arrow;
        arrow.moveTo(s * 0.50, s * 0.05);
        arrow.lineTo(s * 0.95, s * 0.50);
        arrow.lineTo(s * 0.65, s * 0.50);
        arrow.lineTo(s * 0.65, s * 0.95);
        arrow.lineTo(s * 0.35, s * 0.95);
        arrow.lineTo(s * 0.35, s * 0.50);
        arrow.lineTo(s * 0.05, s * 0.50);
        arrow.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(arrow);
    } else {
        // The warning sign the other status bar warnings use, in a fixed colour.
        image = QImage(QStringLiteral(":/icons/warning"))
                    .scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixel(x, y, qRgba(color.red(), color.green(), color.blue(), qAlpha(image.pixel(x, y))));
            }
        }
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

} // namespace UpgradeIndicator
