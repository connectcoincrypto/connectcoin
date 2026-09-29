// Copyright (c) 2014-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkstyle.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>

#include <tinyformat.h>
#include <util/chaintype.h>

#include <QApplication>

#include <future>
#include <utility>

static const struct {
    const ChainType networkId;
    const char *appName;
    const int iconColorHueShift;
    const int iconColorSaturationReduction;
} network_styles[] = {
    {ChainType::MAIN, QAPP_APP_NAME_DEFAULT, 0, 0},
    {ChainType::TESTNET, QAPP_APP_NAME_TESTNET, 70, 30},
    {ChainType::TESTNET4, QAPP_APP_NAME_TESTNET4, 70, 30},
    {ChainType::SIGNET, QAPP_APP_NAME_SIGNET, 35, 15},
    {ChainType::REGTEST, QAPP_APP_NAME_REGTEST, 160, 30},
};

namespace {
std::pair<QImage, QImage> PrepareNetworkImages(int iconColorHueShift, int iconColorSaturationReduction)
{
    // Decode/recolor/resize the raster resource without any GUI-owned objects.
    // Match the premultiplied storage previously supplied by QPixmap::toImage
    // so the existing network colors and alpha edges remain unchanged.
    QImage img{QStringLiteral(":/icons/bitcoin")};
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if(iconColorHueShift != 0 && iconColorSaturationReduction != 0)
    {
        int h,s,l,a;

        // traverse though lines
        for(int y=0;y<img.height();y++)
        {
            QRgb *scL = reinterpret_cast< QRgb *>( img.scanLine( y ) );

            // loop through pixels
            for(int x=0;x<img.width();x++)
            {
                // preserve alpha because QColor::getHsl doesn't return the alpha value
                a = qAlpha(scL[x]);
                QColor col(scL[x]);

                // get hue value
                col.getHsl(&h,&s,&l);

                // rotate color on RGB color circle
                // 70° should end up with the typical "testnet" green
                h+=iconColorHueShift;

                // change saturation value
                if(s>iconColorSaturationReduction)
                {
                    s -= iconColorSaturationReduction;
                }
                col.setHsl(h,s,l,a);

                // set the pixel
                scL[x] = col.rgba();
            }
        }

    }
    auto tray = img.scaled(QSize(256, 256));
    return {std::move(img), std::move(tray)};
}
} // namespace

NetworkStyle::NetworkStyle(const QString& _appName, const QString& _titleAddText, QImage appImage, QImage trayImage) :
    appName(_appName),
    appIcon(QPixmap::fromImage(std::move(appImage))),
    trayAndWindowIcon(QPixmap::fromImage(std::move(trayImage))),
    titleAddText(_titleAddText)
{
}

const NetworkStyle* NetworkStyle::instantiate(const ChainType networkId)
{
    std::string titleAddText = networkId == ChainType::MAIN ? "" : strprintf("[%s]", ChainTypeToString(networkId));
    for (const auto& network_style : network_styles) {
        if (networkId == network_style.networkId) {
            // Wait at the explicit startup factory boundary, before creating
            // the style. Only QPixmap/QIcon construction stays on the GUI.
            auto images = GUIUtil::WaitForBackendTask(std::async(std::launch::async,
                [hue = network_style.iconColorHueShift, saturation = network_style.iconColorSaturationReduction] {
                    return PrepareNetworkImages(hue, saturation);
                }));
            return new NetworkStyle(
                    network_style.appName,
                    qApp->translate("SplashScreen", titleAddText.c_str()),
                    std::move(images.first), std::move(images.second));
        }
    }
    return nullptr;
}
