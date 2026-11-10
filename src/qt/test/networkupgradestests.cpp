// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/networkupgradestests.h>

#include <node/signalling.h>
#include <qt/networkupgradeswidget.h>
#include <warnings.h>

#include <QGroupBox>
#include <QLabel>

namespace {
//! Regtest-sized window: 144 blocks, threshold 108 (75%).
SignallingInfo Window(int height, int elapsed)
{
    SignallingInfo info;
    info.height = height;
    info.hash = std::string(64, '0');
    info.period = 144;
    info.threshold = 108;
    info.elapsed = elapsed;
    info.previous_elapsed = 144;
    info.period_end_height = height + (144 - elapsed);
    info.period_end_time = 1700000000;
    return info;
}

DeploymentSignalling Deployment(const std::string& name, int bit, ThresholdState state)
{
    DeploymentSignalling d;
    d.name = name;
    d.bit = bit;
    d.scheduled = true;
    d.state = state;
    return d;
}

QString Text(const QWidget& widget, const QString& name)
{
    const auto* label = widget.findChild<QLabel*>(name);
    if (!label) {
        QTest::qFail(qPrintable("no label " + name), __FILE__, __LINE__);
        return {};
    }
    return label->text();
}
} // namespace

void NetworkUpgradesTests::headerNamesTheWindowOfTheNextBlock()
{
    NetworkUpgradesWidget widget;
    QCOMPARE(Text(widget, "header"), QObject::tr("No blocks yet."));

    // Tip 299: the next block, 300, is in the window starting at 288, which
    // is window 2 (0-143, 144-287, 288-431). 12 blocks of it are in.
    widget.setInfo(Window(299, 12));
    const QString header = Text(widget, "header");
    QVERIFY2(header.startsWith("Window 2: 12 of 144 blocks, ends at block 431"), qPrintable(header));

    // Tip 287 closes window 1, so the next block opens window 2 with none in.
    widget.setInfo(Window(287, 0));
    QVERIFY(Text(widget, "header").startsWith("Window 2: 0 of 144 blocks, ends at block 431"));
}

void NetworkUpgradesTests::startedDeploymentShowsProgressAgainstTheThreshold()
{
    SignallingInfo info = Window(299, 100);
    DeploymentSignalling csv = Deployment("csv", 0, ThresholdState::STARTED);
    csv.count = 80;
    csv.possible = true;
    info.deployments.push_back(csv);

    NetworkUpgradesWidget widget;
    widget.setInfo(info);
    QCOMPARE(Text(widget, "state_csv"), QObject::tr("Started"));
    const QString detail = Text(widget, "detail_csv");
    QVERIFY2(detail.contains("80 of 100 blocks signalled this window (80.0%)"), qPrintable(detail));
    QVERIFY2(detail.contains("Needs 108: 28 more in the 44 blocks left. Still possible."), qPrintable(detail));

    const auto* bar = widget.findChild<SignallingBar*>("bar_csv");
    QVERIFY(bar);
    QCOMPARE(bar->count(), 80);
    QCOMPARE(bar->elapsed(), 100);
    QCOMPARE(bar->period(), 144);
    QCOMPARE(bar->threshold(), 108);
    // The tooltip explains the bar with this window's numbers: 80 signalled,
    // 20 without the bit, 44 to come, and a threshold of 108 of 144 (75.0%).
    const QString tip = bar->toolTip();
    QVERIFY2(tip.contains("<b>Coloured:</b> 80 blocks"), qPrintable(tip));
    QVERIFY2(tip.contains("<b>Grey:</b> 20 blocks"), qPrintable(tip));
    QVERIFY2(tip.contains("<b>Empty:</b> 44 blocks"), qPrintable(tip));
    QVERIFY2(tip.contains("108 of the 144 blocks (75.0%)"), qPrintable(tip));
    QVERIFY(!widget.findChild<QLabel*>("state_csv")->toolTip().isEmpty());
    QVERIFY(widget.findChild<QLabel*>("header")->toolTip().contains("windows of 144 blocks"));

    // 37 blocks without the bit leave at most 107 of 144: out of reach.
    info.deployments[0].count = 63;
    info.deployments[0].possible = false;
    widget.setInfo(info);
    QVERIFY(Text(widget, "detail_csv").endsWith("Can no longer lock in this window."));

    // Threshold met before the window ends.
    info.deployments[0].count = 108;
    info.deployments[0].possible = true;
    widget.setInfo(info);
    QVERIFY(Text(widget, "detail_csv").contains("Threshold reached: locks in at the end of this window."));
}

void NetworkUpgradesTests::lockedInActiveAndUnscheduledRows()
{
    SignallingInfo info = Window(299, 12);
    DeploymentSignalling locked = Deployment("csv", 0, ThresholdState::LOCKED_IN);
    locked.activation_height = 432;
    locked.activation_time = 1700000000;
    locked.count = 10;
    info.deployments.push_back(locked);
    DeploymentSignalling active = Deployment("segwit", 1, ThresholdState::ACTIVE);
    active.since = 144;
    active.since_time = 1600000000;
    info.deployments.push_back(active);
    DeploymentSignalling off = Deployment("taproot", 2, ThresholdState::DEFINED);
    off.scheduled = false;
    info.deployments.push_back(off);

    NetworkUpgradesWidget widget;
    widget.setInfo(info);

    QCOMPARE(Text(widget, "state_csv"), QObject::tr("Locked in"));
    QVERIFY(Text(widget, "detail_csv").startsWith("Locked in. The new rules take effect at block 432"));
    QVERIFY(widget.findChild<SignallingBar*>("bar_csv"));

    QCOMPARE(Text(widget, "state_segwit"), QObject::tr("Active"));
    QVERIFY(Text(widget, "detail_segwit").startsWith("In effect since block 144 ("));
    QVERIFY(!widget.findChild<SignallingBar*>("bar_segwit"));

    QCOMPARE(Text(widget, "state_taproot"), QObject::tr("Not scheduled"));
    QVERIFY(!widget.findChild<SignallingBar*>("bar_taproot"));
}

void NetworkUpgradesTests::unknownBitsShownOnlyWhileSignalledThisWindow()
{
    SignallingInfo info = Window(299, 100);
    // Bit 7 was set in the previous window only.
    info.unknown_bits.push_back({7, 0, 30});

    NetworkUpgradesWidget widget;
    widget.setInfo(info);
    auto* box = widget.findChild<QGroupBox*>("unknownBits");
    QVERIFY(box);
    QVERIFY(box->isHidden());
    QVERIFY(!widget.findChild<QLabel*>("state_bit7"));

    info.unknown_bits[0].count = 55;
    widget.setInfo(info);
    QVERIFY(!box->isHidden());
    QCOMPARE(Text(widget, "state_bit7"), QObject::tr("Signalling"));
    const QString detail = Text(widget, "detail_bit7");
    QVERIFY2(detail.startsWith("55 of 100 blocks signalled this window (55.0%)"), qPrintable(detail));
    QVERIFY2(detail.contains(QString("href=\"%1\"").arg(UPGRADE_URL)), qPrintable(detail));
    const auto* bar = widget.findChild<SignallingBar*>("bar_bit7");
    QVERIFY(bar);
    QCOMPARE(bar->count(), 55);

    // It stops: the section goes away again.
    info.unknown_bits[0].count = 0;
    widget.setInfo(info);
    QVERIFY(box->isHidden());
}

void NetworkUpgradesTests::updateReplacesTheRows()
{
    SignallingInfo info = Window(299, 100);
    info.deployments.push_back(Deployment("csv", 0, ThresholdState::STARTED));
    info.deployments.push_back(Deployment("segwit", 1, ThresholdState::STARTED));
    info.unknown_bits.push_back({7, 5, 0});

    NetworkUpgradesWidget widget;
    for (int i = 0; i < 3; ++i) widget.setInfo(info);
    QCOMPARE(widget.findChildren<SignallingBar*>().size(), 3);
    QCOMPARE(widget.findChildren<QLabel*>("state_csv").size(), 1);

    info.deployments.pop_back();
    widget.setInfo(info);
    QVERIFY(!widget.findChild<QLabel*>("state_segwit"));
    QCOMPARE(widget.findChildren<SignallingBar*>().size(), 2);
}

void NetworkUpgradesTests::historyStripsForSignalledDeploymentsAndUnknownBits()
{
    // Tip 899: the next block opens window 6 (864-1007) with 36 in. Three
    // completed windows of history: 3, 4 and 5, from block 432.
    SignallingInfo info = Window(899, 36);
    info.history_start = 432;
    DeploymentSignalling csv = Deployment("csv", 0, ThresholdState::STARTED);
    csv.history = {20, 70, 107};
    info.deployments.push_back(csv);
    DeploymentSignalling segwit = Deployment("segwit", 1, ThresholdState::LOCKED_IN);
    segwit.history = {50, 100, 120};
    info.deployments.push_back(segwit);
    DeploymentSignalling taproot = Deployment("taproot", 2, ThresholdState::ACTIVE);
    taproot.history = {144, 144, 144};
    info.deployments.push_back(taproot);
    info.unknown_bits.push_back({7, 30, 90, {0, 40, 90}});

    NetworkUpgradesWidget widget;
    widget.setInfo(info);

    const auto* strip = widget.findChild<HistoryStrip*>("history_csv");
    QVERIFY(strip);
    QCOMPARE(strip->counts(), (std::vector<int>{20, 70, 107}));
    QCOMPARE(strip->firstHeight(), 432);
    QVERIFY2(strip->toolTip().startsWith("Completed windows: 3, oldest on the left."), qPrintable(strip->toolTip()));
    QVERIFY2(strip->toolTip().contains("threshold of 108 blocks (75.0%)"), qPrintable(strip->toolTip()));
    // One bar per window, oldest on the left, gaps between them.
    QCOMPARE(strip->sizeHint().width(), 3 * 10 + 2 * 3);
    QCOMPARE(strip->windowAt(0), 0);
    QCOMPARE(strip->windowAt(9), 0);
    QCOMPARE(strip->windowAt(11), -1);
    QCOMPARE(strip->windowAt(13), 1);
    QCOMPARE(strip->windowAt(26), 2);
    QCOMPARE(strip->windowAt(40), -1);

    // Each window's own tooltip.
    QCOMPARE(NetworkUpgrades::HistoryWindowToolTip(107, 720, 144, 108),
             QString("Window 5, blocks 720 to 863: 107 of 144 blocks signalled (74.3%). It was 1 short of the threshold of 108."));
    QCOMPARE(NetworkUpgrades::HistoryWindowToolTip(120, 720, 144, 108),
             QString("Window 5, blocks 720 to 863: 120 of 144 blocks signalled (83.3%). It reached the threshold of 108."));

    // Locked in keeps its strip, to show how it got there; active doesn't.
    QVERIFY(widget.findChild<HistoryStrip*>("history_segwit"));
    QVERIFY(!widget.findChild<HistoryStrip*>("history_taproot"));

    // The unknown bit gets one too, in the warning colour.
    const auto* unknown = widget.findChild<HistoryStrip*>("history_bit7");
    QVERIFY(unknown);
    QCOMPARE(unknown->counts(), (std::vector<int>{0, 40, 90}));

    // No completed window yet: no strip.
    SignallingInfo first = Window(100, 101);
    DeploymentSignalling early = Deployment("csv", 0, ThresholdState::STARTED);
    first.deployments.push_back(early);
    widget.setInfo(first);
    QVERIFY(!widget.findChild<HistoryStrip*>("history_csv"));
}
