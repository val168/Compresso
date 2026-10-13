#include "compresso/theme_manager.h"

#include <QApplication>
#include <QColor>
#include <QSettings>
#include <QStyleHints>
#include <QtGlobal>

namespace compresso
{
namespace
{

ThemePreference parsePreference(const QString& value)
{
    if (value == QStringLiteral("light")) return ThemePreference::light;
    if (value == QStringLiteral("dark")) return ThemePreference::dark;
    return ThemePreference::system;
}

QString preferenceName(ThemePreference preference)
{
    switch (preference) {
    case ThemePreference::light: return QStringLiteral("light");
    case ThemePreference::dark: return QStringLiteral("dark");
    case ThemePreference::system: return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

} // namespace

ThemeManager::ThemeManager(QApplication& application, QObject* parent)
    : QObject(parent), application_(application), nativePalette_(application.palette())
{
    QSettings settings;
    preference_ = parsePreference(settings.value(QStringLiteral("appearance/theme"),
                                                 QStringLiteral("dark")).toString());
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(application_.styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this](Qt::ColorScheme) {
                if (preference_ == ThemePreference::system) applyPreference();
            });
#endif
    applyPreference();
}

void ThemeManager::setPreference(ThemePreference preference)
{
    if (preference_ == preference) return;
    preference_ = preference;
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/theme"), preferenceName(preference));
    applyPreference();
    emit preferenceChanged(preference_);
}

void ThemeManager::applyPreference()
{
    QPalette palette;
    if (preference_ == ThemePreference::dark) {
        palette = darkPalette();
    } else if (preference_ == ThemePreference::light) {
        palette = lightPalette();
    } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (application_.styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
            palette = darkPalette();
        } else if (application_.styleHints()->colorScheme() == Qt::ColorScheme::Light) {
            palette = lightPalette();
        } else {
            palette = nativePalette_;
        }
#else
        palette = nativePalette_;
#endif
    }
    application_.setPalette(palette);
    emit paletteApplied(palette);
}

QPalette ThemeManager::lightPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(243, 243, 243));
    palette.setColor(QPalette::WindowText, QColor(28, 28, 28));
    palette.setColor(QPalette::Base, QColor(255, 255, 255));
    palette.setColor(QPalette::AlternateBase, QColor(247, 247, 247));
    palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 225));
    palette.setColor(QPalette::ToolTipText, QColor(28, 28, 28));
    palette.setColor(QPalette::Text, QColor(28, 28, 28));
    palette.setColor(QPalette::Button, QColor(243, 243, 243));
    palette.setColor(QPalette::ButtonText, QColor(28, 28, 28));
    palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
    palette.setColor(QPalette::Highlight, QColor(0, 103, 192));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Link, QColor(0, 90, 158));
    palette.setColor(QPalette::Mid, QColor(205, 205, 205));
    palette.setColor(QPalette::Midlight, QColor(232, 232, 232));
    palette.setColor(QPalette::Dark, QColor(160, 160, 160));
    palette.setColor(QPalette::Shadow, QColor(120, 120, 120));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(125, 125, 125));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(125, 125, 125));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(125, 125, 125));
    return palette;
}

QPalette ThemeManager::darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(32, 32, 32));
    palette.setColor(QPalette::WindowText, QColor(240, 240, 240));
    palette.setColor(QPalette::Base, QColor(25, 25, 25));
    palette.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
    palette.setColor(QPalette::ToolTipBase, QColor(45, 45, 45));
    palette.setColor(QPalette::ToolTipText, QColor(240, 240, 240));
    palette.setColor(QPalette::Text, QColor(240, 240, 240));
    palette.setColor(QPalette::Button, QColor(45, 45, 45));
    palette.setColor(QPalette::ButtonText, QColor(240, 240, 240));
    palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
    palette.setColor(QPalette::Highlight, QColor(0, 103, 192));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Link, QColor(112, 181, 235));
    palette.setColor(QPalette::Mid, QColor(75, 75, 75));
    palette.setColor(QPalette::Midlight, QColor(60, 60, 60));
    palette.setColor(QPalette::Dark, QColor(18, 18, 18));
    palette.setColor(QPalette::Shadow, QColor(0, 0, 0));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(145, 145, 145));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(145, 145, 145));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(145, 145, 145));
    return palette;
}

} // namespace compresso
