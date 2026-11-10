// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_UPGRADEINDICATOR_H
#define BITCOIN_QT_UPGRADEINDICATOR_H

#include <node/signalling.h>
#include <warnings.h>

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QStringList>

#include <set>

/** The status bar icon for network upgrades and the warnings that go with
 *  them. Kept apart from the window so tests can check the mapping. */
namespace UpgradeIndicator {

/** What the icon shows, in increasing order of urgency. */
enum class Level {
    NONE,     //!< Nothing to report: no icon.
    UPGRADE,  //!< An up arrow in the theme colour: a deployment this release knows is being signalled or is locked in.
    WARNING,  //!< An amber warning sign: a rule change this release doesn't know is being signalled, or a fork warning is up.
    CRITICAL, //!< A red warning sign: a rule change this release doesn't know is locked in or in effect.
};

struct Status {
    Level level{Level::NONE};
    /** One line per reason, most urgent first. */
    QStringList reasons;
};

/** Map the node's signalling state and warnings to the icon's state. */
Status Evaluate(const SignallingInfo& info, const std::set<WarningKind>& kinds);

/** The icon's colour. An upgrade follows the theme like the other status
 *  bar icons; the warnings keep fixed colours, so they stand out on every
 *  theme. Invalid for NONE. */
QColor IconColor(Level level, const QColor& theme_color);
QString ToolTip(const Status& status);
/** The icon for a level, or a null pixmap for NONE. */
QPixmap Icon(Level level, const QColor& theme_color, int size, qreal device_pixel_ratio);

} // namespace UpgradeIndicator

#endif // BITCOIN_QT_UPGRADEINDICATOR_H
