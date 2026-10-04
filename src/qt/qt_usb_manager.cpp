/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Host USB devices and the virtual machine.  See qt_usb_manager.hpp.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_usb_manager.hpp"

#include <QAction>
#include <QCheckBox>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QWidget>

extern "C" {
#include <86box/86box.h>
#include <86box/ini.h>
#include <86box/config.h>
}

#define POLL_MS 2000

UsbManager::UsbManager(QWidget *parent, QMenuBar *menubar, QAction *menuBefore)
    : QObject(parent)
    , parentWidget(parent)
{
    menu = new QMenu(tr("&USB"), parent);
    menubar->insertMenu(menuBefore, menu);
    connect(menu, &QMenu::aboutToShow, this, &UsbManager::buildMenu);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &UsbManager::poll);
    refresh();
}

QString
UsbManager::key(const usbn_host_info_t &d)
{
    QString path;
    for (int i = 0; i < d.port_depth; i++)
        path += QString(".%1").arg(d.port_path[i]);
    return QString("%1:%2@%3%4").arg(d.vid, 4, 16, QChar('0')).arg(d.pid, 4, 16, QChar('0')).arg(d.bus).arg(path);
}

QString
UsbManager::label(const usbn_host_info_t &d)
{
    return QString("%1 (%2:%3)").arg(QString::fromUtf8(d.desc),
                                     QString("%1").arg(d.vid, 4, 16, QChar('0')).toUpper(),
                                     QString("%1").arg(d.pid, 4, 16, QChar('0')).toUpper());
}

QString
UsbManager::rememberKey(uint16_t vid, uint16_t pid)
{
    return QString("%1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
}

/* Remembered choices: [USB] remember = vvvv:pppp=vm,vvvv:pppp=host,... */
static QString
remembered(const QString &id)
{
    const QString all = QString::fromUtf8(config_get_string((char *) "USB", (char *) "remember", (char *) ""));
    for (const QString &e : all.split(',', Qt::SkipEmptyParts)) {
        if (e.section('=', 0, 0) == id)
            return e.section('=', 1);
    }
    return QString();
}

static void
rememberChoice(const QString &id, const QString &choice)
{
    QStringList out;
    const QString all = QString::fromUtf8(config_get_string((char *) "USB", (char *) "remember", (char *) ""));
    for (const QString &e : all.split(',', Qt::SkipEmptyParts))
        if (e.section('=', 0, 0) != id)
            out << e;
    out << id + "=" + choice;
    config_set_string((char *) "USB", (char *) "remember", (char *) out.join(',').toUtf8().constData());
}

int
UsbManager::portOf(uint16_t vid, uint16_t pid) const
{
    const QString id = QString("%1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
    for (int p = 0; p < USBN_PORTS; p++)
        if (QString(usbn_port_cfg[p]).compare(id, Qt::CaseInsensitive) == 0)
            return p;
    return -1;
}

void
UsbManager::refresh()
{
    const bool on = (usb_card_type > 0);

    menu->menuAction()->setVisible(on);
    if (on && usbn_host_available()) {
        if (!timer->isActive()) {
            firstPoll = true;
            timer->start(POLL_MS);
            poll();
        }
    } else
        timer->stop();
}

/* What is plugged into the host now; what is new since last time is offered
   to the VM, and what the VM had and is gone from the host is unplugged. */
void
UsbManager::poll()
{
    if (prompting)
        return;

    usbn_host_info_t list[64];
    int              n = usbn_host_list(list, 64);
    QSet<QString>    now;
    QVector<usbn_host_info_t> arrived;

    devices.clear();
    for (int i = 0; i < n; i++) {
        devices.append(list[i]);
        const QString k = key(list[i]);
        now.insert(k);
        if (!firstPoll && !known.contains(k))
            arrived.append(list[i]);
    }

    for (int p = 0; p < USBN_PORTS; p++) {
        unsigned vid, pid;
        if (sscanf(usbn_port_cfg[p], "%x:%x", &vid, &pid) != 2)
            continue;
        bool present = false;
        for (const auto &d : devices)
            present |= (d.vid == vid) && (d.pid == pid);
        if (!present && !firstPoll) {
            usbn_detach(p);
            config_save();
        }
    }

    known     = now;
    firstPoll = false;

    if (!usbn_present())
        return;
    for (const auto &d : arrived) {
        const QString choice = remembered(rememberKey(d.vid, d.pid));
        if (choice == "vm")
            connectToVm(d, true);
        else if ((choice != "host") && config_get_int((char *) "USB", (char *) "ask_on_plug", 1))
            ask(d);
    }
}

void
UsbManager::ask(const usbn_host_info_t &d)
{
    prompting = true;

    QMessageBox box(QMessageBox::Question, tr("USB device connected"),
                    tr("A USB device was plugged into the host:\n\n%1\n\nConnect it to the virtual machine, or keep it on the host? "
                       "While the virtual machine has it, the host cannot use it.").arg(label(d)),
                    QMessageBox::NoButton, parentWidget);
    auto *vm   = box.addButton(tr("Connect to the &virtual machine"), QMessageBox::AcceptRole);
    auto *host = box.addButton(tr("Keep on the &host"), QMessageBox::RejectRole);
    box.setDefaultButton(host);
    auto *remember = new QCheckBox(tr("&Remember my choice for this device"));
    box.setCheckBox(remember);
    box.exec();

    const bool toVm = (box.clickedButton() == vm);
    if (remember->isChecked()) {
        rememberChoice(rememberKey(d.vid, d.pid), toVm ? "vm" : "host");
        config_save();
    }
    if (toVm)
        connectToVm(d);

    prompting = false;
}

bool
UsbManager::connectToVm(const usbn_host_info_t &d, bool quiet)
{
    int  port = -1;
    char err[512];

    if (!usbn_present())
        return false;
    for (int p = 0; p < USBN_PORTS; p++) {
        if (!usbn_port_busy(p)) {
            port = p;
            break;
        }
    }
    if (port < 0) {
        if (!quiet)
            QMessageBox::warning(parentWidget, tr("USB"), tr("Both USB ports of the virtual machine are in use. Disconnect a device from the USB menu first."));
        return false;
    }

    usbn_device_t *dev = usbn_host_open(d.vid, d.pid, err, sizeof(err));
    if (!dev) {
        QMessageBox::warning(parentWidget, tr("USB"), tr("%1 could not be connected to the virtual machine: %2.").arg(label(d), QString::fromUtf8(err)));
        return false;
    }
    if (!usbn_attach(port, dev)) {
        dev->destroy(dev);
        return false;
    }
    snprintf(usbn_port_cfg[port], sizeof(usbn_port_cfg[port]), "%04x:%04x", d.vid, d.pid);
    config_save();
    return true;
}

void
UsbManager::disconnectFromVm(uint16_t vid, uint16_t pid)
{
    const int p = portOf(vid, pid);
    if (p < 0)
        return;
    usbn_detach(p);
    config_save();
}

void
UsbManager::buildMenu()
{
    menu->clear();

    if (!usbn_present()) {
        auto *a = menu->addAction(tr("The USB controller starts at the next hard reset"));
        a->setEnabled(false);
        return;
    }
    if (!usbn_host_available()) {
        auto *a = menu->addAction(tr("This build has no USB passthrough"));
        a->setEnabled(false);
        return;
    }

    poll();
    auto *hdr = menu->addAction(tr("Host devices (checked = connected to the virtual machine)"));
    hdr->setEnabled(false);
    if (devices.isEmpty())
        menu->addAction(tr("(none found)"))->setEnabled(false);

    for (const auto &d : devices) {
        auto *a = menu->addAction(label(d));
        a->setCheckable(true);
        a->setChecked(portOf(d.vid, d.pid) >= 0);
        if ((d.speed == USBN_SPEED_HIGH) && !usbn_bus_high_speed())
            a->setToolTip(tr("A high-speed (USB 2.0) device: on this USB 1.1 controller it runs at full speed, as it would on a real USB 1.1 port."));
        const usbn_host_info_t info = d;
        connect(a, &QAction::triggered, this, [this, info](bool on) {
            if (on)
                connectToVm(info);
            else
                disconnectFromVm(info.vid, info.pid);
        });
    }
    menu->setToolTipsVisible(true);

#ifdef _WIN32
    menu->addSeparator();
    if (usbn_host_uses_usbdk())
        menu->addAction(tr("Capturing devices through UsbDk"))->setEnabled(false);
    else {
        auto *hint = menu->addAction(tr("Only devices with the WinUSB driver can be connected. Install UsbDk to connect any device..."));
        connect(hint, &QAction::triggered, this, [this]() {
            QMessageBox::information(parentWidget, tr("USB passthrough"),
                                     tr("To connect any USB device to the virtual machine the way VirtualBox and VMware do, install UsbDk "
                                        "(a signed driver from Red Hat / Daynix) once, then restart 86Box-Next:\n\n"
                                        "https://github.com/daynix/UsbDk/releases\n\n"
                                        "Without it, only devices given the WinUSB driver (for example with Zadig) can be connected."));
        });
    }
#endif

    menu->addSeparator();
    auto *askAct = menu->addAction(tr("&Ask when a device is plugged into the host"));
    askAct->setCheckable(true);
    askAct->setChecked(config_get_int((char *) "USB", (char *) "ask_on_plug", 1));
    connect(askAct, &QAction::toggled, this, [](bool on) {
        if (on)
            config_delete_var((char *) "USB", (char *) "ask_on_plug");
        else
            config_set_int((char *) "USB", (char *) "ask_on_plug", 0);
        config_save();
    });
    auto *forget = menu->addAction(tr("&Forget remembered choices"));
    forget->setEnabled(config_get_string((char *) "USB", (char *) "remember", (char *) "")[0] != ' ');
    connect(forget, &QAction::triggered, this, []() {
        config_delete_var((char *) "USB", (char *) "remember");
        config_save();
    });
}
