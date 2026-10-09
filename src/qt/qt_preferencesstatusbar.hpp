/*
 * 86Box-Next: Preferences > Status bar -- the order of the icon groups at the bottom left of the
 * main window.
 */
#ifndef QT_PREFERENCESSTATUSBAR_HPP
#define QT_PREFERENCESSTATUSBAR_HPP

#include <QWidget>

class QListWidget;

class PreferencesStatusBar : public QWidget {
    Q_OBJECT

public:
    explicit PreferencesStatusBar(QWidget *parent = nullptr);

    void save();

private:
    void fillIconOrder(const QStringList &order);

    QListWidget *iconOrder;
};

#endif // QT_PREFERENCESSTATUSBAR_HPP
