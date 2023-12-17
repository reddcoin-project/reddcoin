// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/uritests.h>

#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <QUrl>

void URITests::uriTests()
{
    SendCoinsRecipient rv;
    QUrl uri;
    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?req-dontexist="));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?dontexist="));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?label=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.label == QString("Wikipedia Example Address"));
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?amount=0.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100000);

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?amount=1.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100100000);

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?amount=100&label=Wikipedia Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?message=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.label == QString());

    QVERIFY(GUIUtil::parseBitcoinURI("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?message=Wikipedia Example Address", &rv));
    QVERIFY(rv.address == QString("RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV"));
    QVERIFY(rv.label == QString());

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?req-message=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?amount=1,000&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("reddcoin:RtXwY8BB61A38iYgSSynhdAPyQ2azUT9gV?amount=1,000.0&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));
}
