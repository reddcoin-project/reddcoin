// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_NETWORKUPGRADESWIDGET_H
#define BITCOIN_QT_NETWORKUPGRADESWIDGET_H

#include <node/signalling.h>

#include <QColor>
#include <QString>
#include <QWidget>

#include <vector>

QT_BEGIN_NAMESPACE
class QGridLayout;
class QGroupBox;
class QLabel;
QT_END_NAMESPACE

/** Texts shown on the Network Upgrades tab, kept apart from the widgets so
 *  tests can check them directly. */
namespace NetworkUpgrades {
/** "Window 12: 7416 of 14400 blocks, ends at block 172799 (about ...)". */
QString HeaderText(const SignallingInfo& info);
/** Short state name: "Started", "Locked in", "Not scheduled", ... */
QString StateText(const DeploymentSignalling& deployment);
/** The line under a deployment: progress towards the threshold, the
 *  activation height or the start date, depending on the state. */
QString DetailText(const DeploymentSignalling& deployment, const SignallingInfo& info);
/** The line under an unknown bit, with the upgrade link as HTML. */
QString UnknownBitText(const UnknownBitSignalling& bit, const SignallingInfo& info);
/** Whether a deployment in this state gets a bar for the current window. */
bool ShowsBar(const DeploymentSignalling& deployment);
/** What a state means, for the state column's tooltip. */
QString StateToolTip(const DeploymentSignalling& deployment);
/** How to read a bar: what the filled, grey and empty parts and the
 *  threshold line stand for, with this window's numbers. */
QString BarToolTip(int count, int elapsed, int period, int threshold);
/** What a window is, for the header's tooltip. */
QString HeaderToolTip(const SignallingInfo& info);
/** What the history strip shows, for the space between its bars. */
QString HistoryToolTip(int windows, int period, int threshold);
/** One completed window of the history strip: its blocks and how many signalled. */
QString HistoryWindowToolTip(int count, int start_height, int period, int threshold);
} // namespace NetworkUpgrades

/** A bar for one window: signalled blocks, blocks elapsed without the bit,
 *  blocks still to come, and a mark at the threshold. */
class SignallingBar : public QWidget
{
    Q_OBJECT

public:
    explicit SignallingBar(QWidget* parent = nullptr);
    void setValues(int count, int elapsed, int period, int threshold);
    void setFillColor(const QColor& color);

    int count() const { return m_count; }
    int elapsed() const { return m_elapsed; }
    int period() const { return m_period; }
    int threshold() const { return m_threshold; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_count{0};
    int m_elapsed{0};
    int m_period{0};
    int m_threshold{0};
    QColor m_fill;
};

/** One small bar per completed window, oldest on the left: the share of its
 *  blocks that signalled, against a line at the threshold. Each bar has its
 *  own tooltip. */
class HistoryStrip : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryStrip(QWidget* parent = nullptr);
    void setValues(std::vector<int> counts, int first_height, int period, int threshold);
    void setFillColor(const QColor& color);

    const std::vector<int>& counts() const { return m_counts; }
    int firstHeight() const { return m_first_height; }
    /** The window drawn at x, as an index into counts(), or -1 between bars. */
    int windowAt(int x) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    static constexpr int BAR_WIDTH{10};
    static constexpr int BAR_GAP{3};
    static constexpr int HEIGHT{24};

    std::vector<int> m_counts;
    int m_first_height{0};
    int m_period{0};
    int m_threshold{0};
    QColor m_fill;
};

/** The "Network Upgrades" tab of the debug window: versionbits signalling for
 *  every deployment this release knows, plus bits it doesn't know. It only
 *  displays a SignallingInfo; the caller fetches it from the node. */
class NetworkUpgradesWidget : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkUpgradesWidget(QWidget* parent = nullptr);

    void setInfo(const SignallingInfo& info);

    /** Colour of unknown-rule rows. */
    static QColor warningColor();

private:
    QLabel* m_header;
    QGridLayout* m_deployments;
    QGroupBox* m_unknown_box;
    QGridLayout* m_unknown;

    static void clearLayout(QGridLayout* layout);
    /** "Last N windows:" and a strip of them, for one deployment or bit. */
    QWidget* HistoryRow(const QString& key, const std::vector<int>& history, const SignallingInfo& info, const QColor& fill);
};

#endif // BITCOIN_QT_NETWORKUPGRADESWIDGET_H
