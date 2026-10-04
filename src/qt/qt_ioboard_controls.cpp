/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The cabinet controls of the fitted arcade I/O board.  The coin
 *             mechanisms and the operator buttons are wires on the board, not
 *             keys, so they are driven here rather than typed into the guest.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_ioboard_controls.hpp"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QWidget>

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/device.h>
#include <86box/io_board.h>
#include <86box/funworld_io.h>
#include <86box/merit_io.h>
}

/* Any Merit board: XL and MAXX share their controls. */
static constexpr int MERIT_ANY = -1;

static bool
board_matches(int board)
{
    if (board == MERIT_ANY)
        return (io_board_type == IO_BOARD_MERIT_XL) || (io_board_type == IO_BOARD_MERIT_MAXX);
    return io_board_type == board;
}

static void
pulse(int board, int line)
{
    if (board == IO_BOARD_FUNWORLD)
        funworld_io_pulse(line);
    else
        merit_io_pulse(line, 0);
}

IOBoardControls::IOBoardControls(QWidget *parent, QMenuBar *menubar, QAction *menuBefore,
                                 QToolBar *toolbar, QAction *toolbarBefore)
    : QObject(parent)
    , parentWidget(parent)
{
    menu = new QMenu(tr("&I/O Board"), parent);
    menubar->insertMenu(menuBefore, menu);

    toolbarSeparator = toolbar->insertSeparator(toolbarBefore);

    /* funworld Photo Play / I.G.O. (PeepeeBox): six coins on the Coin Controls
       C120, four notes on the bill validator, and the two door buttons. */
    const int fw = IO_BOARD_FUNWORLD;
    addLine(fw, "coin_1", tr("Coin &1 (0.10 EUR)"), tr("Coin 1 - 0.10 EUR"), FWIO_LINE_COIN1, toolbar, toolbarBefore);
    addLine(fw, "coin_2", tr("Coin &2 (0.20 EUR)"), tr("Coin 2 - 0.20 EUR"), FWIO_LINE_COIN2, toolbar, toolbarBefore);
    addLine(fw, "coin_3", tr("Coin &3 (0.50 EUR)"), tr("Coin 3 - 0.50 EUR"), FWIO_LINE_COIN3, toolbar, toolbarBefore);
    addLine(fw, "coin_4", tr("Coin &4 (1.00 EUR)"), tr("Coin 4 - 1.00 EUR"), FWIO_LINE_COIN4, toolbar, toolbarBefore);
    addLine(fw, "coin_5", tr("Coin &5 (2.00 EUR)"), tr("Coin 5 - 2.00 EUR"), FWIO_LINE_COIN5, toolbar, toolbarBefore);
    addLine(fw, "coin_6", tr("Coin &6 (TOKEN 10)"), tr("Coin 6 - TOKEN 10"), FWIO_LINE_COIN6, toolbar, toolbarBefore);
    addSeparator(fw);
    addLine(fw, "note_1", tr("Note 5 EUR"), tr("Note - 5 EUR"), FWIO_LINE_NOTE1, toolbar, toolbarBefore);
    addLine(fw, "note_2", tr("Note 10 EUR"), tr("Note - 10 EUR"), FWIO_LINE_NOTE2, toolbar, toolbarBefore);
    addLine(fw, "note_3", tr("Note 20 EUR"), tr("Note - 20 EUR"), FWIO_LINE_NOTE3, toolbar, toolbarBefore);
    addLine(fw, "note_4", tr("Note 50 EUR"), tr("Note - 50 EUR"), FWIO_LINE_NOTE4, toolbar, toolbarBefore);
    addSeparator(fw);
    addLine(fw, "operator_setup", tr("&Operator Setup"), tr("Operator setup button (inside the door)"), FWIO_LINE_SETUP, toolbar, toolbarBefore);
    addLine(fw, "calibrate", tr("Second &door button"), tr("Second door button (A1)"), FWIO_LINE_DOOR2, toolbar, toolbarBefore);

    /* Merit Megatouch XL / MAXX (MegaPPBox): four coins, the two operator
       buttons, and the security key socket. */
    addLine(MERIT_ANY, "coin_1", tr("Coin &1"), tr("Coin 1"), MERIT_LINE_COIN1, toolbar, toolbarBefore);
    addLine(MERIT_ANY, "coin_2", tr("Coin &2"), tr("Coin 2"), MERIT_LINE_COIN2, toolbar, toolbarBefore);
    addLine(MERIT_ANY, "coin_3", tr("Coin &3"), tr("Coin 3"), MERIT_LINE_COIN3, toolbar, toolbarBefore);
    addLine(MERIT_ANY, "coin_4", tr("Coin &4"), tr("Coin 4"), MERIT_LINE_COIN4, toolbar, toolbarBefore);
    addSeparator(MERIT_ANY);
    addLine(MERIT_ANY, "operator_setup", tr("&Operator Setup"), tr("Operator Setup button (inside the cabinet)"), MERIT_LINE_SETUP, toolbar, toolbarBefore);
    addLine(MERIT_ANY, "calibrate", tr("&Calibrate"), tr("Calibrate button: touch-screen calibration"), MERIT_LINE_CALIBRATE, toolbar, toolbarBefore);
    addSeparator(MERIT_ANY);

    keyHeader = menu->addAction(QString());
    keyHeader->setEnabled(false);
    controls.append({ keyHeader, MERIT_ANY, false });
    auto *chooseAct = menu->addAction(tr("Fit security &key..."));
    controls.append({ chooseAct, MERIT_ANY, false });
    connect(chooseAct, &QAction::triggered, this, &IOBoardControls::chooseKey);
    auto *removeAct = menu->addAction(tr("&Remove security key"));
    controls.append({ removeAct, MERIT_ANY, false });
    connect(removeAct, &QAction::triggered, this, &IOBoardControls::removeKey);
    connect(menu, &QMenu::aboutToShow, this, &IOBoardControls::refreshKey);

    refresh();
}

void
IOBoardControls::addLine(int board, const char *icon, const QString &text, const QString &tip,
                         int line, QToolBar *toolbar, QAction *toolbarBefore)
{
    auto *act = new QAction(QIcon(QString(":/menuicons/qt/icons/%1.ico").arg(icon)), text, this);
    act->setToolTip(tip);
    connect(act, &QAction::triggered, this, [board, line]() { pulse((board == MERIT_ANY) ? IO_BOARD_MERIT_XL : board, line); });
    menu->addAction(act);
    toolbar->insertAction(toolbarBefore, act);
    controls.append({ act, board, true });
}

void
IOBoardControls::addSeparator(int board)
{
    controls.append({ menu->addSeparator(), board, false });
}

void
IOBoardControls::refresh()
{
    bool any = false;

    for (const auto &c : controls) {
        const bool on = board_matches(c.board);
        /* A toolbar action is the same QAction as its menu entry. */
        c.action->setVisible(on);
        any |= on;
    }
    menu->menuAction()->setVisible(any);
    toolbarSeparator->setVisible(any);
    refreshKey();
}

void
IOBoardControls::refreshKey()
{
    if (!board_matches(MERIT_ANY))
        return;

    const char *fn = merit_io_key_file();
    if (fn && fn[0])
        keyHeader->setText(tr("Security key: %1").arg(QFileInfo(QString::fromUtf8(fn)).fileName()));
    else
        keyHeader->setText(tr("Security key: none"));
}

void
IOBoardControls::chooseKey()
{
    const QString fn = QFileDialog::getOpenFileName(parentWidget, tr("Fit security key"), QString::fromUtf8(usr_path),
                                                    tr("Key dumps (*.bin *.key);;All files (*)"));
    if (fn.isEmpty())
        return;
    merit_io_set_key(fn.toUtf8().constData());
    refreshKey();
}

void
IOBoardControls::removeKey()
{
    merit_io_set_key("");
    refreshKey();
}
