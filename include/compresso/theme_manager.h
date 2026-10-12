#pragma once

#include <QObject>
#include <QPalette>

class QApplication;

namespace compresso
{

enum class ThemePreference
{
    system,
    light,
    dark
};

class ThemeManager final : public QObject
{
    Q_OBJECT

public:
    explicit ThemeManager(QApplication& application, QObject* parent = nullptr);

    [[nodiscard]] ThemePreference preference() const noexcept { return preference_; }
    void setPreference(ThemePreference preference);

signals:
    void preferenceChanged(compresso::ThemePreference preference);
    void paletteApplied(const QPalette& palette);

private:
    void applyPreference();
    [[nodiscard]] static QPalette lightPalette();
    [[nodiscard]] static QPalette darkPalette();

    QApplication& application_;
    QPalette nativePalette_;
    ThemePreference preference_ = ThemePreference::system;
};

} // namespace compresso

Q_DECLARE_METATYPE(compresso::ThemePreference)
