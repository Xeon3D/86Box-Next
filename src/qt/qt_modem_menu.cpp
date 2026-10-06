/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The modem menu, behind the modem icon in the status bar.  For
 *             each COM port with a modem on it, and each PC Card with a modem
 *             of its own (the 3C562D) (char_modem.c): leave its telephone
 *             line unplugged, have dialling reach a TCP/IP host, or plug it
 *             into the telephone network of isp-server: a number of its own,
 *             other modems' numbers, and the ISP -- the Internet -- on any
 *             other.  The
 *             change is made at once, without a reset -- a call in progress
 *             ends with NO CARRIER -- and kept in the modem's configuration
 *             (a PC Card's: the card's), where Settings shows it too.
 *
 *             On the telephone network a voice modem also has a phone beside
 *             it, which is the host's speaker and microphone: pick it up to
 *             answer a call ringing or to call a number, as a voice call.
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
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
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

/* The line points at isp-server where it listens by default. */
static bool
isIsp(int line, const QString &host, int port)
{
    return (line == CHAR_MODEM_LINE_TCP) && (host == QStringLiteral(CHAR_MODEM_ISP_HOST)) &&
           (port == CHAR_MODEM_ISP_PORT);
}

/* "5550101" -> "555-0101" */
static QString
phoneText(const QString &digits)
{
    if ((digits.size() == 7) && (digits.toULongLong() || (digits == QStringLiteral("0000000"))))
        return digits.left(3) + QStringLiteral("-") + digits.mid(3);
    return digits;
}

static QString
lineText(int com, int line, const QString &host, int port)
{
    if (line == CHAR_MODEM_LINE_PHONE) {
        char number[24] = "";

        if (char_modem_get_phone(com, nullptr, 0, nullptr, 0, number, sizeof(number)) == 1)
            return QObject::tr("telephone %1").arg(phoneText(QString::fromUtf8(number)));
        return QObject::tr("telephone network, not reached (is isp-server running?)");
    }
    if (isIsp(line, host, port))
        return QObject::tr("dials the ISP (isp-server)");
    if ((line == CHAR_MODEM_LINE_TCP) && !host.isEmpty())
        return QObject::tr("dials %1:%2").arg(host).arg(port);
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
        case CHAR_MODEM_VOICE:
            return QObject::tr("in a voice call");
        case CHAR_MODEM_HANDSET:
            return QObject::tr("on the phone");
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
        tip += "\n" + tr("%1: %2, %3, %4").arg(slotText(i), QString::fromUtf8(name), lineText(i, line, QString::fromUtf8(host), port), stateText(char_modem_get_state(i)));
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

        /* isp-server's telephone network (src/network/isp/): a number of the
           modem's own, other modems' numbers, and the ISP on any other.  It
           has to be running; the modem keeps trying until it is. */
        QAction *phone = sub->addAction(tr("Telephone network (isp-server)"));
        phone->setCheckable(true);
        phone->setChecked(line == CHAR_MODEM_LINE_PHONE);
        grp->addAction(phone);
        connect(phone, &QAction::triggered, this, [this, i]() {
            char h[128] = "";
            int  p      = 23;
            if (char_modem_get_line(i, h, sizeof(h), &p) < 0)
                return;
            char_modem_set_line(i, CHAR_MODEM_LINE_PHONE, h, p);
            config_save();
            emit changed();
        });

        sub->addSeparator();
        QAction *edit = sub->addAction(tr("Set TCP/IP host..."));
        connect(edit, &QAction::triggered, this, [this, i]() { editHost(i); });
        QAction *num = sub->addAction(tr("Set phone number..."));
        connect(num, &QAction::triggered, this, [this, i]() { editPhone(i); });

        /* The phone beside the modem: the host's speaker and microphone. */
        const int hs = char_modem_handset_state(i);
        if ((hs >= 0) && (hs & 4)) {
            sub->addSeparator();
            if (hs & 1) {
                QAction *down = sub->addAction(tr("Hang up the phone"));
                connect(down, &QAction::triggered, this, [this, i]() {
                    char_modem_handset(i, CHAR_MODEM_HANDSET_HANGUP, nullptr);
                    emit changed();
                });
            } else {
                QAction *up = sub->addAction((hs & 2) ? tr("Answer the phone (ringing)") : tr("Pick up the phone"));
                connect(up, &QAction::triggered, this, [this, i]() {
                    char_modem_handset(i, CHAR_MODEM_HANDSET_PICKUP, nullptr);
                    emit changed();
                });
            }
            if (!(hs & 2) || (hs & 1)) {
                QAction *call = sub->addAction(tr("Call a number on the phone..."));
                connect(call, &QAction::triggered, this, [this, i]() { callNumber(i); });
            }
        }

        sub->addSeparator();
        QAction *st = sub->addAction(tr("Status: %1").arg(stateText(char_modem_get_state(i))));
        st->setEnabled(false);
        if (line == CHAR_MODEM_LINE_PHONE) {
            QAction *n = sub->addAction(lineText(i, line, qhost, port));
            n->setEnabled(false);
        }
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

/* A voice call from the phone beside the modem: off hook, then the number. */
void
ModemMenu::callNumber(int com)
{
    QDialog dlg(m_parent);
    dlg.setWindowTitle(tr("Call from the phone on %1").arg(slotText(com)));
    auto *form = new QFormLayout(&dlg);
    auto *numE = new QLineEdit(&dlg);
    numE->setPlaceholderText(tr("e.g. 555-0102"));
    numE->setMinimumWidth(200);
    form->addRow(tr("Number:"), numE);
    form->addRow(new QLabel(tr("You talk on the host's microphone and hear the call on its speakers."), &dlg));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Call"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if ((dlg.exec() != QDialog::Accepted) || numE->text().trimmed().isEmpty())
        return;
    char_modem_handset(com, CHAR_MODEM_HANDSET_DIAL, numE->text().trimmed().toUtf8().constData());
    emit changed();
}

/* The number this modem asks for (blank: the exchange gives one) and where
   the exchange is. */
void
ModemMenu::editPhone(int com)
{
    char want[24]      = "";
    char exchange[160] = "";
    char number[24]    = "";

    if (char_modem_get_phone(com, want, sizeof(want), exchange, sizeof(exchange), number, sizeof(number)) < 0)
        return;

    QDialog dlg(m_parent);
    dlg.setWindowTitle(tr("Telephone line of %1").arg(slotText(com)));
    auto *form  = new QFormLayout(&dlg);
    auto *wantE = new QLineEdit(QString::fromUtf8(want), &dlg);
    wantE->setPlaceholderText(tr("blank: the exchange gives one"));
    wantE->setMinimumWidth(240);
    auto *exchE = new QLineEdit(QString::fromUtf8(exchange), &dlg);
    exchE->setPlaceholderText(QStringLiteral(CHAR_MODEM_EXCHANGE));
    form->addRow(tr("Phone number:"), wantE);
    form->addRow(tr("Exchange (isp-server):"), exchE);
    form->addRow(new QLabel(number[0] ? tr("The exchange has given this line %1.").arg(phoneText(QString::fromUtf8(number)))
                                      : tr("Not registered with the exchange now."),
                            &dlg));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    char_modem_set_phone(com, wantE->text().trimmed().toUtf8().constData(),
                         exchE->text().trimmed().toUtf8().constData());
    config_save();
    emit changed();
}
