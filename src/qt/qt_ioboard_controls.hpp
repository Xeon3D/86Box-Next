/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The cabinet controls of the fitted arcade I/O board: an
 *             "I/O Board" menu and toolbar buttons for its coin, note and
 *             operator lines, shown only while that board is in the machine.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_IOBOARD_CONTROLS_HPP
#define QT_IOBOARD_CONTROLS_HPP

#include <QObject>
#include <QList>

class QAction;
class QMenu;
class QMenuBar;
class QToolBar;
class QWidget;

class IOBoardControls : public QObject {
    Q_OBJECT

public:
    /* The menu goes into `menubar` before `menuBefore`; the toolbar buttons
       into `toolbar` before `toolbarBefore`. */
    IOBoardControls(QWidget *parent, QMenuBar *menubar, QAction *menuBefore,
                    QToolBar *toolbar, QAction *toolbarBefore);

    /* Show the controls of the board that is fitted now, and only those. */
    void refresh();

private:
    struct Control {
        QAction *action;
        int      board;     /* IO_BOARD_*, or -1 for every Merit board */
        bool     toolbar;
    };

    QWidget        *parentWidget;
    QMenu          *menu;
    QAction        *toolbarSeparator;
    QAction        *keyHeader;
    QList<Control>  controls;

    void addLine(int board, const char *icon, const QString &text, const QString &tip,
                 int line, QToolBar *toolbar, QAction *toolbarBefore);
    void addSeparator(int board);
    void chooseKey();
    void removeKey();
    void refreshKey();
};

#endif
