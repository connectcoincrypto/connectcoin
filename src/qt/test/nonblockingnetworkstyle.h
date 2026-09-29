// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TEST_NONBLOCKINGNETWORKSTYLE_H
#define CONNECTCOIN_QT_TEST_NONBLOCKINGNETWORKSTYLE_H

#include <qt/networkstyle.h>
#include <util/chaintype.h>

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QTest>

#include <array>
#include <memory>

// Compare the worker-prepared assets with the previous GUI QPixmap pipeline.
// In particular, changing where decoding happens must not change alpha edges,
// network tint or the fast-scaled 256-pixel tray/window icon.
inline void CheckNetworkStyleImages()
{
    const QPixmap source{QStringLiteral(":/icons/bitcoin")};
    QVERIFY(!source.isNull());
    struct Style {
        ChainType chain;
        int hue;
        int saturation;
    };
    const std::array<Style, 4> styles{{
        {ChainType::MAIN, 0, 0},
        {ChainType::TESTNET4, 70, 30},
        {ChainType::SIGNET, 35, 15},
        {ChainType::REGTEST, 160, 30},
    }};
    for (const auto& style : styles) {
        QPixmap expected = source;
        if (style.hue && style.saturation) {
            QImage pixels = source.toImage();
            for (int y = 0; y < pixels.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(pixels.scanLine(y));
                for (int x = 0; x < pixels.width(); ++x) {
                    const int alpha = qAlpha(row[x]);
                    QColor color{row[x]};
                    int hue, saturation, lightness;
                    color.getHsl(&hue, &saturation, &lightness);
                    hue += style.hue;
                    if (saturation > style.saturation) saturation -= style.saturation;
                    color.setHsl(hue, saturation, lightness, alpha);
                    row[x] = color.rgba();
                }
            }
            expected.convertFromImage(pixels);
        }
        const std::unique_ptr<const NetworkStyle> prepared{NetworkStyle::instantiate(style.chain)};
        QVERIFY(prepared);
        QVERIFY(!prepared->getAppIcon().isNull());
        QVERIFY(!prepared->getTrayAndWindowIcon().isNull());
        const auto argb_image = [](const QPixmap& pixmap) { return pixmap.toImage().convertToFormat(QImage::Format_ARGB32); };
        QCOMPARE(argb_image(prepared->getAppIcon().pixmap(source.size())), argb_image(expected));
        QCOMPARE(argb_image(prepared->getTrayAndWindowIcon().pixmap(QSize{256, 256})), argb_image(expected.scaled(QSize{256, 256})));
        QCOMPARE(prepared->getTitleAddText().isEmpty(), style.chain == ChainType::MAIN);
    }
}

#endif // CONNECTCOIN_QT_TEST_NONBLOCKINGNETWORKSTYLE_H
