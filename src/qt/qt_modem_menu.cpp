/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The modem menu, behind the modem icon in the status bar.  For
 *             each COM port with a modem on it, and each PC Card with a modem
 *             of its own (the 3C562D) (char_modem.c): leave its telephone
 *             line unplugged, have dialling reach a TCP/IP host, or have it
 *             reach the built-in ISP and through it the Internet.  The
 *             change is made at once, without a reset -- a call in progress
 *             ends with NO CARRIER -- and kept in the modem's configuration
 *             (a PC Card's: the card's), where Settings shows it too.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_modem_menu.hpp"

#include <QAction>
#include <QActionGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QSpinBox>
#include <QWidget>

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/char.h>
#include <86box/serial.h>
#include <86box/char_modem.h>
#include <86box/pcmcia.h>
}

/* "COM1", or "PC Card A" for a PC Card's own modem. */
static QString
slotText(int slot)
{
    char buf[32];
    char_modem_slot_label(slot, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

static QString
lineText(int line, const QString &host, int port)
{
    if ((line == CHAR_MODEM_LINE_TCP) && !host.isEmpty())
        return QObject::tr("dials %1:%2").arg(host).arg(port);
    if (line == CHAR_MODEM_LINE_ISP)
        return QObject::tr("dials the Internet");
    return QObject::tr("line not connected");
}

static QString
stateText(int state)
{
    switch (state) {
        case CHAR_MODEM_CALLING:
            return QObject::tr("calling");
        case CHAR_MODEM_ONLINE:
            return QObject::tr("on line");
        default:
            return QObject::tr("on hook");
    }
}

ModemMenu::ModemMenu(QWidget *parent)
    : QObject(parent)
    , m_parent(parent)
{
    m_menu = new QMenu(parent);
    connect(m_menu, &QMenu::aboutToShow, this, &ModemMenu::buildMenu);
}

/* From the configuration rather than from the modems themselves: the status
   bar is built before the machine's devices are, at start-up. */
bool
ModemMenu::any()
{
    for (int i = 0; i < SERIAL_MAX; i++) {
        if (!com_ports[i].enabled)
            continue;
        const device_t *d = char_get_device(com_ports[i].device);
        if ((d == &char_modem_supra_com_device) || (d == &char_modem_elsa_com_device))
            return true;
    }
    /* Or a PC Card with a modem of its own in a socket. */
    if (pcmcia_enabled) {
        for (int s = 0; s < PCMCIA_SOCKETS; s++) {
            if (pcmcia_card_has_modem(pcmcia_card_type[s]))
                return true;
        }
    }
    return false;
}

bool
ModemMenu::busy()
{
    for (int i = 0; i < char_modem_slots(); i++) {
        if (char_modem_get_state(i) > CHAR_MODEM_IDLE)
            return true;
    }
    return false;
}

QString
ModemMenu::toolTip() const
{
    QString tip = tr("Modem");
    for (int i = 0; i < char_modem_slots(); i++) {
        char        host[128] = "";
        int         port      = 0;
        const int   line      = char_modem_get_line(i, host, sizeof(host), &port);
        const char *name      = char_modem_name(i);

        if ((line < 0) || (name == nullptr))
            continue;
        tip += "\n" + tr("%1: %2, %3, %4").arg(slotText(i), QString::fromUtf8(name), lineText(line, QString::fromUtf8(host), port), stateText(char_modem_get_state(i)));
    }
    return tip;
}

void
ModemMenu::buildMenu()
{
    m_menu->clear();
    for (int i = 0; i < char_modem_slots(); i++) {
        char        host[128] = "";
        int         port      = 0;
        const int   line      = char_modem_get_line(i, host, sizeof(host), &port);
        const char *name      = char_modem_name(i);

        if ((line < 0) || (name == nullptr))
            continue;

        const QString qhost = QString::fromUtf8(host);
        QMenu        *sub   = m_menu->addMenu(QIcon(":/settings/qt/icons/modem.ico"),
                                              tr("%1: %2").arg(slotText(i), QString::fromUtf8(name)));
        auto         *grp   = new QActionGroup(sub);

        QAction *dead = sub->addAction(tr("Line not connected"));
        dead->setCheckable(true);
        dead->setChecked((line == CHAR_MODEM_LINE_DEAD) || ((line == CHAR_MODEM_LINE_TCP) && qhost.isEmpty()));
        grp->addAction(dead);
        connect(dead, &QAction::triggered, this, [this, i]() {
            char h[128] = "";
            int  p      = 23;
            if (char_modem_get_line(i, h, sizeof(h), &p) < 0)
                return;
            char_modem_set_line(i, CHAR_MODEM_LINE_DEAD, h, p);
            config_save();
            emit changed();
        });

        QAction *tcp = sub->addAction(qhost.isEmpty() ? tr("Dial out to a TCP/IP host...")
                                                      : tr("Dial out to %1:%2").arg(qhost).arg(port));
        tcp->setCheckable(true);
        tcp->setChecked((line == CHAR_MODEM_LINE_TCP) && !qhost.isEmpty());
        grp->addAction(tcp);
        connect(tcp, &QAction::triggered, this, [this, i, qhost, port]() {
            if (qhost.isEmpty()) {
                editHost(i);
                return;
            }
            char_modem_set_line(i, CHAR_MODEM_LINE_TCP, qhost.toUtf8().constData(), port);
            config_save();
            emit changed();
        });

        /* Any number reaches the built-in ISP: PPP, an address, the host's
           Internet.  The TCP host is kept for when it is chosen again. */
        QAction *isp = sub->addAction(tr("Internet (built-in ISP)"));
        isp->setCheckable(true);
        isp->setChecked(line == CHAR_MODEM_LINE_ISP);
        grp->addAction(isp);
        connect(isp, &QAction::triggered, this, [this, i]() {
            char h[128] = "";
            int  p      = 23;
            if (char_modem_get_line(i, h, sizeof(h), &p) < 0)
                return;
            char_modem_set_line(i, CHAR_MODEM_LINE_ISP, h, p);
            config_save();
            emit changed();
        });

        sub->addSeparator();
        QAction *edit = sub->addAction(tr("Set TCP/IP host..."));
        connect(edit, &QAction::triggered, this, [this, i]() { editHost(i); });

        sub->addSeparator();
        QAction *st = sub->addAction(tr("Status: %1").arg(stateText(char_modem_get_state(i))));
        st->setEnabled(false);
    }

    if (m_menu->isEmpty()) {
        QAction *none = m_menu->addAction(tr("No modem attached"));
        none->setEnabled(false);
    }
}

void
ModemMenu::editHost(int com)
{
    char host[128] = "";
    int  port      = 23;

    if (char_modem_get_line(com, host, sizeof(host), &port) < 0)
        return;

    QDialog dlg(m_parent);
    dlg.setWindowTitle(tr("Modem on %1").arg(slotText(com)));
    auto *form  = new QFormLayout(&dlg);
    auto *hostE = new QLineEdit(QString::fromUtf8(host), &dlg);
    hostE->setPlaceholderText(tr("host name or address"));
    hostE->setMinimumWidth(240);
    auto *portE = new QSpinBox(&dlg);
    portE->setRange(1, 32767); /* the Settings spinner stops here too */
    portE->setValue((port >= 1) ? port : 23);
    form->addRow(tr("Host:"), hostE);
    form->addRow(tr("Port:"), portE);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString h = hostE->text().trimmed();
    char_modem_set_line(com, h.isEmpty() ? CHAR_MODEM_LINE_DEAD : CHAR_MODEM_LINE_TCP,
                        h.toUtf8().constData(), portE->value());
    config_save();
    emit changed();
}
