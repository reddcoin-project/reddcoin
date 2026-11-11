// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/upgradeindicatortests.h>

#include <node/signalling.h>
#include <qt/upgradeindicator.h>
#include <warnings.h>

#include <QImage>

using UpgradeIndicator::Level;

namespace {
//! Regtest-sized window: 144 blocks, threshold 108 (75%), 100 blocks in.
SignallingInfo Window()
{
    SignallingInfo info;
    info.height = 387;
    info.period = 144;
    info.threshold = 108;
    info.elapsed = 100;
    info.period_end_height = 431;
    return info;
}

DeploymentSignalling Deployment(const std::string& name, ThresholdState state, bool scheduled = true)
{
    DeploymentSignalling d;
    d.name = name;
    d.scheduled = scheduled;
    d.state = state;
    return d;
}
} // namespace

// The warning sign is a resource of the GUI library.
static void InitResources() { Q_INIT_RESOURCE(bitcoin); }

void UpgradeIndicatorTests::initTestCase()
{
    InitResources();
}

void UpgradeIndicatorTests::hiddenWhenNothingToReport()
{
    // Mainnet today: two rules in effect, the rest not scheduled, no warnings.
    SignallingInfo info = Window();
    info.deployments.push_back(Deployment("heightincb", ThresholdState::ACTIVE));
    info.deployments.push_back(Deployment("csv", ThresholdState::DEFINED));
    info.deployments.push_back(Deployment("segwit", ThresholdState::STARTED, /*scheduled=*/false));
    info.deployments.push_back(Deployment("taproot", ThresholdState::FAILED));
    // A stray unknown bit below the warning threshold is the tab's business.
    info.unknown_bits.push_back({/*bit=*/27, /*count=*/3, /*previous_count=*/0});

    const auto status = UpgradeIndicator::Evaluate(info, {});
    QCOMPARE(status.level, Level::NONE);
    QVERIFY(status.reasons.isEmpty());
    QVERIFY(UpgradeIndicator::Icon(status.level, Qt::black, 16, 1.0).isNull());

    // No blocks yet.
    QCOMPARE(UpgradeIndicator::Evaluate(SignallingInfo{}, {}).level, Level::NONE);
}

void UpgradeIndicatorTests::upgradeWhileAKnownDeploymentIsSignalledOrLockedIn()
{
    SignallingInfo info = Window();
    DeploymentSignalling csv = Deployment("csv", ThresholdState::STARTED);
    csv.count = 90;
    info.deployments.push_back(csv);

    auto status = UpgradeIndicator::Evaluate(info, {});
    QCOMPARE(status.level, Level::UPGRADE);
    QCOMPARE(status.reasons.size(), 1);
    QCOMPARE(status.reasons[0], QString("csv is being signalled: 90 of 100 blocks so far this window, 108 needed."));

    DeploymentSignalling segwit = Deployment("segwit", ThresholdState::LOCKED_IN);
    segwit.activation_height = 576;
    info.deployments.push_back(segwit);
    status = UpgradeIndicator::Evaluate(info, {});
    QCOMPARE(status.level, Level::UPGRADE);
    QCOMPARE(status.reasons.size(), 2);
    QCOMPARE(status.reasons[1], QString("segwit is locked in and takes effect at block 576."));

    const QString tooltip = UpgradeIndicator::ToolTip(status);
    QVERIFY(tooltip.contains("csv is being signalled"));
    QVERIFY(tooltip.contains("segwit is locked in"));
    QVERIFY(tooltip.contains("Network Upgrades tab"));
}

void UpgradeIndicatorTests::amberForUnknownSignallingBlockVersionsAndInvalidChains()
{
    SignallingInfo info = Window();
    info.deployments.push_back(Deployment("csv", ThresholdState::STARTED));

    for (const WarningKind kind : {WarningKind::UNKNOWN_RULE_SIGNALLING, WarningKind::UNKNOWN_BLOCK_VERSION, WarningKind::INVALID_CHAIN, WarningKind::HEAVIER_CHAIN, WarningKind::DEEP_REORG}) {
        const auto status = UpgradeIndicator::Evaluate(info, {kind});
        QCOMPARE(status.level, Level::WARNING);
        // The warning comes first, the known deployment's progress after it.
        QCOMPARE(status.reasons.size(), 2);
        QVERIFY(status.reasons[1].startsWith("csv is being signalled"));
    }
    QVERIFY(UpgradeIndicator::Evaluate(info, {WarningKind::UNKNOWN_RULE_SIGNALLING}).reasons[0].contains("does not know about"));
    QVERIFY(UpgradeIndicator::Evaluate(info, {WarningKind::UNKNOWN_BLOCK_VERSION}).reasons[0].contains("block version"));
    QVERIFY(UpgradeIndicator::Evaluate(info, {WarningKind::INVALID_CHAIN}).reasons[0].contains("invalid"));
    QVERIFY(UpgradeIndicator::Evaluate(info, {WarningKind::HEAVIER_CHAIN}).reasons[0].contains("minority fork"));
    QVERIFY(UpgradeIndicator::Evaluate(info, {WarningKind::DEEP_REORG}).reasons[0].contains("replaced"));

    const auto both = UpgradeIndicator::Evaluate(info, {WarningKind::UNKNOWN_RULE_SIGNALLING, WarningKind::UNKNOWN_BLOCK_VERSION});
    QCOMPARE(both.level, Level::WARNING);
    QCOMPARE(both.reasons.size(), 3);
}

void UpgradeIndicatorTests::redOnceAnUnknownRuleIsLockedInOrActive()
{
    const SignallingInfo info = Window();
    for (const WarningKind kind : {WarningKind::UNKNOWN_RULE_LOCKED_IN, WarningKind::UNKNOWN_RULE_ACTIVE}) {
        const auto status = UpgradeIndicator::Evaluate(info, {kind, WarningKind::UNKNOWN_BLOCK_VERSION});
        QCOMPARE(status.level, Level::CRITICAL);
        QCOMPARE(status.reasons.size(), 2);
        QVERIFY(status.reasons[0].contains("Upgrade"));
        QVERIFY(status.reasons[1].contains("block version"));
    }
    // In effect is the more urgent of the two, so it comes first.
    const auto status = UpgradeIndicator::Evaluate(info, {WarningKind::UNKNOWN_RULE_LOCKED_IN, WarningKind::UNKNOWN_RULE_ACTIVE});
    QVERIFY(status.reasons[0].contains("is in effect"));
    QVERIFY(status.reasons[1].contains("is locked in"));
}

void UpgradeIndicatorTests::upgradeIconFollowsTheThemeAndWarningsDoNot()
{
    const QColor orange(0xff, 0x93, 0x33);
    const QColor blue(0x2a, 0x82, 0xda);

    // The up arrow is drawn in whatever colour the theme gives.
    for (const QColor& theme : {orange, blue}) {
        const QPixmap pixmap = UpgradeIndicator::Icon(Level::UPGRADE, theme, 16, 2.0);
        QCOMPARE(pixmap.size(), QSize(32, 32));
        QCOMPARE(pixmap.devicePixelRatio(), 2.0);
        const QImage image = pixmap.toImage();
        QCOMPARE(image.pixelColor(16, 24).rgb(), theme.rgb()); // the stem
        QCOMPARE(image.pixelColor(4, 28).alpha(), 0);          // beside it
    }

    // The warning signs keep their colours on any theme, and differ.
    for (const Level level : {Level::WARNING, Level::CRITICAL}) {
        const QColor expected = UpgradeIndicator::IconColor(level, orange);
        QCOMPARE(UpgradeIndicator::IconColor(level, blue), expected);
        const QImage image = UpgradeIndicator::Icon(level, orange, 16, 1.0).toImage();
        QVERIFY(!image.isNull());
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 255) continue;
                ++opaque;
                QCOMPARE(pixel.rgb(), expected.rgb());
            }
        }
        QVERIFY(opaque > 0);
    }
    QVERIFY(UpgradeIndicator::IconColor(Level::WARNING, orange) != UpgradeIndicator::IconColor(Level::CRITICAL, orange));
    QCOMPARE(UpgradeIndicator::IconColor(Level::UPGRADE, blue), blue);
}
