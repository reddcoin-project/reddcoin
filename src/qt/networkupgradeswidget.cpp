// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkupgradeswidget.h>

#include <qt/guiutil.h>
#include <warnings.h>

#include <algorithm>
#include <cassert>

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
#include <QToolTip>
#include <QVBoxLayout>

#include <utility>

namespace NetworkUpgrades {

static QString Percent(int count, int blocks)
{
    return QString::number(blocks > 0 ? 100.0 * count / blocks : 0.0, 'f', 1);
}

static QString When(int64_t time)
{
    return GUIUtil::dateTimeStr(time);
}

QString HeaderText(const SignallingInfo& info)
{
    if (info.height < 0 || info.period <= 0) return QObject::tr("No blocks yet.");
    // The window that the next block belongs to. Windows start at multiples of
    // the period.
    const int window = (info.height + 1) / info.period;
    return QObject::tr("Window %1: %2 of %3 blocks, ends at block %4 (about %5)")
        .arg(window)
        .arg(info.elapsed)
        .arg(info.period)
        .arg(info.period_end_height)
        .arg(When(info.period_end_time));
}

QString StateText(const DeploymentSignalling& deployment)
{
    if (!deployment.scheduled) return QObject::tr("Not scheduled");
    switch (deployment.state) {
    case ThresholdState::DEFINED: return QObject::tr("Defined");
    case ThresholdState::STARTED: return QObject::tr("Started");
    case ThresholdState::LOCKED_IN: return QObject::tr("Locked in");
    case ThresholdState::ACTIVE: return QObject::tr("Active");
    case ThresholdState::FAILED: return QObject::tr("Failed");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString DetailText(const DeploymentSignalling& deployment, const SignallingInfo& info)
{
    if (!deployment.scheduled) return QObject::tr("This release has no start date for this deployment.");
    switch (deployment.state) {
    case ThresholdState::DEFINED:
        return QObject::tr("Signalling can start after %1.").arg(When(deployment.start_time));
    case ThresholdState::STARTED: {
        QString text = QObject::tr("%1 of %2 blocks signalled this window (%3%).")
                           .arg(deployment.count)
                           .arg(info.elapsed)
                           .arg(Percent(deployment.count, info.elapsed));
        if (deployment.count >= info.threshold) {
            text += " " + QObject::tr("Threshold reached: locks in at the end of this window.");
        } else {
            text += " " + QObject::tr("Needs %1: %2 more in the %3 blocks left.")
                              .arg(info.threshold)
                              .arg(info.threshold - deployment.count)
                              .arg(info.period - info.elapsed);
            text += " " + (deployment.possible ? QObject::tr("Still possible.") : QObject::tr("Can no longer lock in this window."));
        }
        return text;
    }
    case ThresholdState::LOCKED_IN:
        return QObject::tr("Locked in. The new rules take effect at block %1 (about %2).")
            .arg(deployment.activation_height)
            .arg(When(deployment.activation_time));
    case ThresholdState::ACTIVE:
        if (deployment.since_time > 0) {
            return QObject::tr("In effect since block %1 (%2).").arg(deployment.since).arg(When(deployment.since_time));
        }
        return QObject::tr("In effect since block %1.").arg(deployment.since);
    case ThresholdState::FAILED:
        return QObject::tr("Not locked in before its timeout. It will not activate.");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString UnknownBitText(const UnknownBitSignalling& bit, const SignallingInfo& info)
{
    const QString url = QString::fromUtf8(UPGRADE_URL);
    return QObject::tr("%1 of %2 blocks signalled this window (%3%). This version does not know this rule. Upgrade Reddcoin Core: %4")
        .arg(bit.count)
        .arg(info.elapsed)
        .arg(Percent(bit.count, info.elapsed))
        .arg(QString("<a href=\"%1\">%1</a>").arg(url.toHtmlEscaped()));
}

bool ShowsBar(const DeploymentSignalling& deployment)
{
    return deployment.scheduled &&
           (deployment.state == ThresholdState::STARTED || deployment.state == ThresholdState::LOCKED_IN);
}

QString StateToolTip(const DeploymentSignalling& deployment)
{
    if (!deployment.scheduled) {
        return QObject::tr("This release has no start date for this upgrade, so blocks can't vote for it yet.");
    }
    switch (deployment.state) {
    case ThresholdState::DEFINED:
        return QObject::tr("The upgrade is defined but its start date hasn't been reached. Blocks can't vote for it yet.");
    case ThresholdState::STARTED:
        return QObject::tr("Voting is open. Each new block can signal support by setting this upgrade's bit in its version. "
                           "If enough blocks in one window signal, the upgrade locks in.");
    case ThresholdState::LOCKED_IN:
        return QObject::tr("Enough blocks signalled support, so the upgrade is certain to happen. "
                           "Its new rules take effect at the start of the next window. "
                           "Nodes that don't know the new rules should upgrade before then.");
    case ThresholdState::ACTIVE:
        return QObject::tr("The upgrade's new rules are in effect and every block must follow them.");
    case ThresholdState::FAILED:
        return QObject::tr("The upgrade didn't get enough support before its timeout and will not activate.");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString BarToolTip(int count, int elapsed, int period, int threshold)
{
    const int without = std::max(0, elapsed - count);
    const int to_come = std::max(0, period - elapsed);
    return QObject::tr(
               "<p>Blocks are counted in windows of %1. Each block can <b>signal</b> support for an upgrade "
               "by setting the upgrade's bit in its version.</p>"
               "<p><b>Coloured:</b> %2 blocks so far signalled.<br>"
               "<b>Grey:</b> %3 blocks so far did not.<br>"
               "<b>Empty:</b> %4 blocks still to come in this window.</p>"
               "<p>The <b>line</b> marks the <b>threshold</b>: %5 of the %1 blocks (%6%) must signal within one window "
               "for the upgrade to <b>lock in</b>. Once locked in, the new rules take effect one window later. "
               "If the window ends short of the threshold, counting starts again from zero in the next one.</p>")
        .arg(period)
        .arg(count)
        .arg(without)
        .arg(to_come)
        .arg(threshold)
        .arg(Percent(threshold, period));
}

QString HeaderToolTip(const SignallingInfo& info)
{
    return QObject::tr("Network upgrades (soft forks) are voted on by the blocks themselves, counted in windows of %1 blocks. "
                       "Windows start at block heights that are multiples of %1. "
                       "An upgrade locks in when %2 blocks of one window signal for it.")
        .arg(info.period)
        .arg(info.threshold);
}

QString HistoryToolTip(int windows, int period, int threshold)
{
    // No %n plural: the English forms only come from a translation file.
    return QObject::tr("Completed windows: %1, oldest on the left. Each bar is one window of %2 blocks: "
                       "the coloured part is the share of its blocks that signalled, and the line marks the "
                       "threshold of %3 blocks (%4%). Bars that grow towards the line mean support is growing.")
        .arg(windows)
        .arg(period)
        .arg(threshold)
        .arg(Percent(threshold, period));
}

QString HistoryWindowToolTip(int count, int start_height, int period, int threshold)
{
    QString text = QObject::tr("Window %1, blocks %2 to %3: %4 of %5 blocks signalled (%6%).")
                       .arg(start_height / period)
                       .arg(start_height)
                       .arg(start_height + period - 1)
                       .arg(count)
                       .arg(period)
                       .arg(Percent(count, period));
    text += ' ';
    if (count >= threshold) {
        text += QObject::tr("It reached the threshold of %1.").arg(threshold);
    } else {
        text += QObject::tr("It was %1 short of the threshold of %2.").arg(threshold - count).arg(threshold);
    }
    return text;
}

} // namespace NetworkUpgrades

HistoryStrip::HistoryStrip(QWidget* parent)
    : QWidget(parent), m_fill(palette().color(QPalette::Highlight))
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void HistoryStrip::setValues(std::vector<int> counts, int first_height, int period, int threshold)
{
    m_counts = std::move(counts);
    m_first_height = first_height;
    m_period = period;
    m_threshold = threshold;
    setToolTip(NetworkUpgrades::HistoryToolTip(int(m_counts.size()), period, threshold));
    updateGeometry();
    update();
}

void HistoryStrip::setFillColor(const QColor& color)
{
    m_fill = color;
    update();
}

int HistoryStrip::windowAt(int x) const
{
    if (x < 0) return -1;
    const int index = x / (BAR_WIDTH + BAR_GAP);
    if (index >= int(m_counts.size()) || x % (BAR_WIDTH + BAR_GAP) >= BAR_WIDTH) return -1;
    return index;
}

QSize HistoryStrip::sizeHint() const
{
    const int bars = m_counts.size();
    return QSize(bars > 0 ? bars * BAR_WIDTH + (bars - 1) * BAR_GAP : 0, HEIGHT);
}

QSize HistoryStrip::minimumSizeHint() const
{
    return sizeHint();
}

bool HistoryStrip::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const int index = windowAt(help->pos().x());
        if (index >= 0 && m_period > 0) {
            QToolTip::showText(help->globalPos(),
                               NetworkUpgrades::HistoryWindowToolTip(m_counts[index], m_first_height + index * m_period, m_period, m_threshold),
                               this, QRect(index * (BAR_WIDTH + BAR_GAP), 0, BAR_WIDTH, height()));
            return true;
        }
    }
    return QWidget::event(event);
}

void HistoryStrip::paintEvent(QPaintEvent*)
{
    if (m_period <= 0) return;
    QPainter painter(this);
    const int bottom = height() - 1;
    const auto y = [&](int blocks) {
        return bottom - static_cast<int>(int64_t{bottom} * std::clamp(blocks, 0, m_period) / m_period);
    };
    for (size_t i = 0; i < m_counts.size(); ++i) {
        const QRect bar(int(i) * (BAR_WIDTH + BAR_GAP), 0, BAR_WIDTH - 1, bottom);
        painter.fillRect(bar, palette().color(QPalette::Base));
        painter.fillRect(QRect(QPoint(bar.left(), y(m_counts[i])), bar.bottomRight()), m_fill);
        painter.setPen(palette().color(QPalette::Mid));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bar);
    }
    painter.setPen(QPen(palette().color(QPalette::WindowText), 1));
    painter.drawLine(0, y(m_threshold), width() - 1, y(m_threshold));
}

SignallingBar::SignallingBar(QWidget* parent)
    : QWidget(parent), m_fill(palette().color(QPalette::Highlight))
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SignallingBar::setValues(int count, int elapsed, int period, int threshold)
{
    m_count = count;
    m_elapsed = elapsed;
    m_period = period;
    m_threshold = threshold;
    setToolTip(NetworkUpgrades::BarToolTip(count, elapsed, period, threshold));
    update();
}

void SignallingBar::setFillColor(const QColor& color)
{
    m_fill = color;
    update();
}

QSize SignallingBar::sizeHint() const
{
    return QSize(320, 16);
}

QSize SignallingBar::minimumSizeHint() const
{
    return QSize(80, 16);
}

void SignallingBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const QRect bar = rect().adjusted(0, 3, -1, -3);
    painter.fillRect(bar, palette().color(QPalette::Base));
    if (m_period > 0) {
        const auto x = [&](int blocks) {
            return bar.left() + static_cast<int>(int64_t{bar.width()} * std::clamp(blocks, 0, m_period) / m_period);
        };
        // Elapsed blocks without the bit, then the blocks that set it.
        painter.fillRect(QRect(QPoint(bar.left(), bar.top()), QPoint(x(m_elapsed), bar.bottom())), palette().color(QPalette::Mid));
        painter.fillRect(QRect(QPoint(bar.left(), bar.top()), QPoint(x(m_count), bar.bottom())), m_fill);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 2));
        painter.drawLine(x(m_threshold), rect().top(), x(m_threshold), rect().bottom());
    }
    painter.setPen(palette().color(QPalette::Mid));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bar);
}

NetworkUpgradesWidget::NetworkUpgradesWidget(QWidget* parent)
    : QWidget(parent),
      m_header(new QLabel(this)),
      m_unknown_box(new QGroupBox(tr("Unknown to this version"), this))
{
    auto* outer = new QVBoxLayout(this);

    m_header->setObjectName("header");
    m_header->setWordWrap(true);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_header);

    auto* known_box = new QGroupBox(tr("Deployments"), this);
    m_deployments = new QGridLayout(known_box);
    m_deployments->setColumnStretch(3, 1);
    outer->addWidget(known_box);

    m_unknown_box->setObjectName("unknownBits");
    m_unknown_box->setToolTip(tr("Bits that blocks are setting in this window but that this release does not know about."));
    m_unknown = new QGridLayout(m_unknown_box);
    m_unknown->setColumnStretch(3, 1);
    m_unknown_box->hide();
    outer->addWidget(m_unknown_box);

    outer->addStretch(1);

    setInfo(SignallingInfo{});
}

QColor NetworkUpgradesWidget::warningColor()
{
    return QColor(0xd9, 0x7a, 0x00);
}

QWidget* NetworkUpgradesWidget::HistoryRow(const QString& key, const std::vector<int>& history, const SignallingInfo& info, const QColor& fill)
{
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(tr("Past windows:"));
    auto* strip = new HistoryStrip;
    strip->setObjectName("history_" + key);
    strip->setFillColor(fill);
    strip->setValues(history, info.history_start, info.period, info.threshold);
    label->setToolTip(strip->toolTip());
    layout->addWidget(label);
    layout->addWidget(strip);
    layout->addStretch(1);
    return row;
}

void NetworkUpgradesWidget::clearLayout(QGridLayout* layout)
{
    // Delete at once rather than deleteLater, so the old rows are gone as
    // soon as setInfo() returns.
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void NetworkUpgradesWidget::setInfo(const SignallingInfo& info)
{
    using namespace NetworkUpgrades;

    m_header->setText(HeaderText(info));
    m_header->setToolTip(info.period > 0 ? HeaderToolTip(info) : QString());

    clearLayout(m_deployments);
    int row = 0;
    for (const DeploymentSignalling& d : info.deployments) {
        const QString key = QString::fromStdString(d.name);
        auto* name = new QLabel(key);
        auto* bit = new QLabel(QString::number(d.bit));
        auto* state = new QLabel(StateText(d));
        state->setObjectName("state_" + key);
        state->setToolTip(StateToolTip(d));

        auto* progress = new QWidget;
        auto* column = new QVBoxLayout(progress);
        column->setContentsMargins(0, 0, 0, 0);
        if (ShowsBar(d)) {
            auto* bar = new SignallingBar;
            bar->setObjectName("bar_" + key);
            bar->setValues(d.count, info.elapsed, info.period, info.threshold);
            column->addWidget(bar);
            if (!d.history.empty()) column->addWidget(HistoryRow(key, d.history, info, palette().color(QPalette::Highlight)));
        }
        auto* detail = new QLabel(DetailText(d, info));
        detail->setObjectName("detail_" + key);
        detail->setWordWrap(true);
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->addWidget(detail);

        m_deployments->addWidget(name, row, 0, Qt::AlignTop);
        m_deployments->addWidget(bit, row, 1, Qt::AlignTop);
        m_deployments->addWidget(state, row, 2, Qt::AlignTop);
        m_deployments->addWidget(progress, row, 3);
        ++row;
    }

    // Only bits set in the current window: a bit seen only in the previous
    // window has stopped and is not worth a warning here.
    clearLayout(m_unknown);
    row = 0;
    const QString warning_style = QString("QLabel { color: %1; }").arg(warningColor().name());
    for (const UnknownBitSignalling& u : info.unknown_bits) {
        if (u.count == 0) continue;
        const QString key = QString("bit%1").arg(u.bit);
        auto* name = new QLabel(tr("Unknown rule"));
        name->setStyleSheet(warning_style);
        auto* bit = new QLabel(QString::number(u.bit));
        bit->setStyleSheet(warning_style);
        auto* state = new QLabel(tr("Signalling"));
        state->setObjectName("state_" + key);
        state->setStyleSheet(warning_style);
        state->setToolTip(tr("Blocks are setting a bit that no upgrade known to this release uses. "
                             "Other software on the network may be voting for new rules that this release cannot check. "
                             "If those rules lock in and take effect, this release may stop following the network."));

        auto* progress = new QWidget;
        auto* column = new QVBoxLayout(progress);
        column->setContentsMargins(0, 0, 0, 0);
        auto* bar = new SignallingBar;
        bar->setObjectName("bar_" + key);
        bar->setFillColor(warningColor());
        bar->setValues(u.count, info.elapsed, info.period, info.threshold);
        column->addWidget(bar);
        if (!u.history.empty()) column->addWidget(HistoryRow(key, u.history, info, warningColor()));
        auto* detail = new QLabel(UnknownBitText(u, info));
        detail->setObjectName("detail_" + key);
        detail->setWordWrap(true);
        detail->setTextFormat(Qt::RichText);
        detail->setTextInteractionFlags(Qt::TextBrowserInteraction);
        detail->setOpenExternalLinks(true);
        column->addWidget(detail);

        m_unknown->addWidget(name, row, 0, Qt::AlignTop);
        m_unknown->addWidget(bit, row, 1, Qt::AlignTop);
        m_unknown->addWidget(state, row, 2, Qt::AlignTop);
        m_unknown->addWidget(progress, row, 3);
        ++row;
    }
    m_unknown_box->setVisible(row > 0);
}
