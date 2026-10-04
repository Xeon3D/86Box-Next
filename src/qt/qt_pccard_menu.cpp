/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The PC Card menu: take the card in each socket out, and put it
 *             back, as with a real laptop's eject buttons.  The guest's
 *             socket services see the card go and come (pcic_pd6722.c).
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_pccard_menu.hpp"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QWidget>

extern "C" {
#include <86box/pcmcia.h>
}

PcCardMenu::PcCardMenu(QWidget *parent, QMenuBar *menubar, QAction *menuBefore)
    : QObject(parent)
{
    menu = new QMenu(tr("PC &Card"), parent);
    menubar->insertMenu(menuBefore, menu);
    connect(menu, &QMenu::aboutToShow, this, &PcCardMenu::buildMenu);
    refresh();
}

void
PcCardMenu::refresh()
{
    menu->menuAction()->setVisible(pcmcia_enabled || pcmcia_controller_present());
}

void
PcCardMenu::buildMenu()
{
    menu->clear();
    for (int s = 0; s < PCMCIA_SOCKETS; s++) {
        const QChar  letter = QChar('A' + s);
        const char  *name   = pcmcia_socket_card_name(s);
        QAction     *a;

        if (!name) {
            a = menu->addAction(tr("Socket %1: empty").arg(letter));
            a->setEnabled(false);
            continue;
        }
        const bool out = pcmcia_ejected(s);
        a = menu->addAction(QIcon(":/settings/qt/icons/pcmcia.ico"),
                            out ? tr("Insert %1 into socket %2").arg(QString::fromUtf8(name), letter)
                                : tr("Eject %1 from socket %2").arg(QString::fromUtf8(name), letter));
        connect(a, &QAction::triggered, this, [s, out]() { pcmcia_eject(s, !out); });
    }
    if (!pcmcia_controller_present()) {
        menu->addSeparator();
        menu->addAction(tr("(the controller is not running)"))->setEnabled(false);
    }
}
