/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The PC Card menu: take the card in each socket out, and put it
 *             back, as with a real laptop's eject buttons.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_PCCARD_MENU_HPP
#define QT_PCCARD_MENU_HPP

#include <QObject>

class QAction;
class QMenu;
class QMenuBar;
class QWidget;

class PcCardMenu : public QObject {
    Q_OBJECT

public:
    PcCardMenu(QWidget *parent, QMenuBar *menubar, QAction *menuBefore);

    /* The menu shows while a PC Card controller is fitted. */
    void refresh();

private:
    void buildMenu();

    QMenu *menu;
};

#endif // QT_PCCARD_MENU_HPP
