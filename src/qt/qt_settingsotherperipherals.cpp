/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Other peripherals configuration UI module.
 *
 * Authors: Joakim L. Gilje <jgilje@jgilje.net>
 *          Jasmine Iwanek <jriwanek@gmail.com>
 *
 *          Copyright 2021 Joakim L. Gilje
 *          Copyright 2025 Jasmine Iwanek
 */
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/mcamem.h>
#include <86box/isamem.h>
#include <86box/isarom.h>
#include <86box/isartc.h>
#include <86box/io_board.h>
#include <86box/modem_card.h>
#include <86box/usb_next.h>
#include <86box/unittester.h>
#include <86box/softpower.h>
#include <86box/novell_cardkey.h>
}

#include "qt_settings_completer.hpp"

#include "qt_settingsotherperipherals.hpp"
#include "qt_settingspcmcia.hpp"
#include "ui_qt_settingsotherperipherals.h"

#include "qt_deviceconfig.hpp"
#include "qt_models_common.hpp"

#include "qt_defs.hpp"

static bool
hasIsaOrSidecarBus(int machineId)
{
    return machine_has_bus(machineId, MACHINE_BUS_ISA | MACHINE_BUS_SIDECAR) > 0;
}

SettingsOtherPeripherals::SettingsOtherPeripherals(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingsOtherPeripherals)
{
    ui->setupUi(this);

    /* 86Box-Next: the PC Card controller and its sockets, a tab of its own. */
    pcmcia = new SettingsPcmcia(this);
    ui->tabWidgetSound->addTab(pcmcia, QIcon(":/settings/qt/icons/pcmcia.ico"), tr("PCMCIA"));

    /* Memory expansion cards: one shared set of four slots, fed from the
       ISA board list (isamem) on ISA machines and from the MCA board list
       (mcamem) on MCA machines. */
    for (uint8_t i = 0; i < ISAMEM_MAX; ++i) {
        scMemExpCard[i] = new SettingsCompleter(findChild<QComboBox *>(QString("comboBoxMemExpCard%1").arg(i + 1)), nullptr);
        memexp_cfg_changed[i] = 0;
    }

    for (uint8_t i = 0; i < ISAROM_MAX; ++i) {
        scIsaRomCard[i] = new SettingsCompleter(findChild<QComboBox *>(QString("comboBoxIsaRomCard%1").arg(i + 1)), nullptr);
        isarom_cfg_changed[i] = 0;
    }

    scRTC           = new SettingsCompleter(ui->comboBoxRTC, nullptr);
    isartc_cfg_changed         = 0;

    unittester_cfg_changed     = 0;
    softpower_cfg_changed      = 0;
    softpower_card_enabled     = softpower_enabled > 0;
    novell_keycard_cfg_changed = 0;

    onCurrentMachineChanged(machine);
}

SettingsOtherPeripherals::~SettingsOtherPeripherals()
{
    for (uint8_t i = 0; i < ISAMEM_MAX; ++i)
        delete scMemExpCard[i];

    for (uint8_t i = 0; i < ISAROM_MAX; ++i)
        delete scIsaRomCard[i];

    delete scRTC;

    delete ui;
}

void
SettingsOtherPeripherals::onCurrentMachineChanged(int machineId)
{
    this->machineId = machineId;
    pcmcia->onCurrentMachineChanged(machineId);

    bool machineHasIsa          = (machine_has_bus(machineId, MACHINE_BUS_ISA) > 0);
    bool machineHasIsaOrSidecar = hasIsaOrSidecarBus(machineId);
    const bool nativeSoftPower = machines[machineId].init == machine_ibm5140_init;

    ui->pushButtonConfigureRTC->setEnabled(machineHasIsaOrSidecar);
    ui->comboBoxRTC->setEnabled(machineHasIsaOrSidecar);
    ui->checkBoxISABugger->setEnabled(machineHasIsa);
    ui->pushButtonConfigureUT->setEnabled(unittester_enabled > 0);
    ui->checkBoxKeyCard->setEnabled(machineHasIsa);
    ui->pushButtonConfigureKeyCard->setEnabled(novell_keycard_enabled > 0);
    ui->checkBoxSoftPower->setEnabled(machineHasIsa && !nativeSoftPower);
    ui->pushButtonConfigureSoftPower->setEnabled(machineHasIsa && !nativeSoftPower && softpower_card_enabled);

    ui->checkBoxISABugger->setChecked((machineHasIsa && (bugger_enabled > 0)) ? true : false);
    ui->checkBoxPOSTCard->setChecked(postcard_enabled > 0 ? true : false);
    ui->checkBoxUnitTester->setChecked(unittester_enabled > 0 ? true : false);
    ui->checkBoxKeyCard->setChecked((machineHasIsa && (novell_keycard_enabled > 0)) ? true : false);
    /* Native power hardware must not overwrite the optional card preference. */
    ui->checkBoxSoftPower->setChecked(nativeSoftPower || (machineHasIsa && softpower_card_enabled));

    scRTC->removeRows();
    ui->comboBoxRTC->clear();

    for (uint8_t i = 0; i < ISAMEM_MAX; ++i) {
        scMemExpCard[i]->removeRows();
        if (auto *cb = findChild<QComboBox *>(QString("comboBoxMemExpCard%1").arg(i + 1)))
            cb->clear();
    }

    for (uint8_t i = 0; i < ISAROM_MAX; ++i) {
        scIsaRomCard[i]->removeRows();
        if (auto *cb = findChild<QComboBox *>(QString("comboBoxIsaRomCard%1").arg(i + 1)))
            cb->clear();
    }

    int selectedRow = 0;

    // ISA RTC Cards
    auto *model = ui->comboBoxRTC->model();
    Models::Batch rtcRows(model);
    for (const auto &rtc : Models::Devices(isartc_get_device, isartc_get_internal_name, nullptr, 0)) {
        if (!device_is_valid(rtc.dev, machineId))
            continue;

        int row = rtcRows.add(rtc.name, rtc.id);
        scRTC->addDevice(rtc.dev, rtc.name);
        if (rtc.id == isartc_type)
            selectedRow = row;
    }
    rtcRows.commit();
    ui->comboBoxRTC->setCurrentIndex(selectedRow);
    ui->pushButtonConfigureRTC->setEnabled((isartc_type != 0) && isartc_has_config(isartc_type) && machineHasIsaOrSidecar);

    // Arcade I/O board: one per machine, so a single selection.
    ui->comboBoxIOBoard->clear();
    int ioRow = 0;
    for (int i = 0; i < IO_BOARD_COUNT; i++) {
        const device_t *dev = io_board_get_device(i);
        if ((i != IO_BOARD_NONE) && !device_is_valid(dev, machineId))
            continue;
        ui->comboBoxIOBoard->addItem((i == IO_BOARD_NONE) ? tr("None") : DeviceConfig::DeviceName(dev, io_board_get_internal_name(i), 0), i);
        ui->comboBoxIOBoard->setItemData(ui->comboBoxIOBoard->count() - 1, ioBoardDescription(i), Qt::ToolTipRole);
        if (i == io_board_type)
            ioRow = ui->comboBoxIOBoard->count() - 1;
    }
    ui->comboBoxIOBoard->setCurrentIndex(ioRow);
    ui->comboBoxIOBoard->setEnabled(machineHasIsaOrSidecar);
    ui->pushButtonConfigureIOBoard->setEnabled((io_board_type != IO_BOARD_NONE) && io_board_has_config(io_board_type) && machineHasIsaOrSidecar);
    updateIOBoardHint();

    // 86Box-Next: the internal modem, one ISA card per machine.
    ui->comboBoxModemCard->clear();
    int modemRow = 0;
    for (int i = 0; i < MODEM_CARD_COUNT; i++) {
        const device_t *dev = modem_card_get_device(i);
        if ((i != MODEM_CARD_NONE) && !device_is_valid(dev, machineId))
            continue;
        ui->comboBoxModemCard->addItem((i == MODEM_CARD_NONE) ? tr("None") : DeviceConfig::DeviceName(dev, modem_card_get_internal_name(i), 0), i);
        if (i == modem_card_type)
            modemRow = ui->comboBoxModemCard->count() - 1;
    }
    ui->comboBoxModemCard->setCurrentIndex(modemRow);
    ui->comboBoxModemCard->setEnabled(machineHasIsaOrSidecar);
    ui->pushButtonConfigureModemCard->setEnabled((modem_card_type != MODEM_CARD_NONE) && modem_card_has_config(modem_card_type) && machineHasIsaOrSidecar);

    // USB controller card: a PCI card.
    ui->comboBoxUSB->clear();
    for (int i = 0; i < usb_card_count(); i++)
        ui->comboBoxUSB->addItem((i == 0) ? tr("None") : QString(usb_card_get_name(i)), i);
    ui->comboBoxUSB->setCurrentIndex(machine_has_bus(machineId, MACHINE_BUS_PCI) ? usb_card_type : 0);
    ui->comboBoxUSB->setEnabled(machine_has_bus(machineId, MACHINE_BUS_PCI) > 0);
    updateUSBHint();

    // Memory Expansion Cards (shared UI: ISA or MCA boards depending on
    // the machine bus).  The isamem/mcamem databases and their config
    // globals remain separate; only the four dropdown slots are shared.
    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    const bool isa_bus = !mca_bus && hasIsaOrSidecarBus(machineId);

    QComboBox          *mem_cbox[ISAMEM_MAX]         = { 0 };
    QAbstractItemModel *mem_models[ISAMEM_MAX]       = { 0 };
    int                 mem_removeRows_[ISAMEM_MAX]  = { 0 };
    int                 mem_selectedRows[ISAMEM_MAX] = { 0 };

    for (uint8_t i = 0; i < ISAMEM_MAX; ++i) {
        mem_cbox[i]        = findChild<QComboBox *>(QString("comboBoxMemExpCard%1").arg(i + 1));
        mem_models[i]      = mem_cbox[i]->model();
        mem_removeRows_[i] = mem_models[i]->rowCount();
    }

    static const QVector<Models::Device> none;
    const auto &mem_cards = mca_bus ? Models::Devices(mcamem_get_device, mcamem_get_internal_name,
                                                      [](int c) -> int { return device_available(mcamem_get_device(c)); }, 0)
                          : isa_bus ? Models::Devices(isamem_get_device, isamem_get_internal_name,
                                                      [](int c) -> int { return device_available(isamem_get_device(c)); }, 0)
                                    : none;

    std::vector<Models::Batch> mem_rows(mem_models, mem_models + ISAMEM_MAX);
    for (const auto &card : mem_cards) {
        if (card.available && device_is_valid(card.dev, machineId)) {
            for (uint8_t i = 0; i < ISAMEM_MAX; ++i) {
                int cur = mca_bus ? mcamem_type[i] : isamem_type[i];
                int row = mem_rows[i].add(card.name, card.id);
                scMemExpCard[i]->addDevice(card.dev, card.name);

                if (card.id == cur)
                    mem_selectedRows[i] = row - mem_removeRows_[i];
            }
        }
    }

    for (uint8_t i = 0; i < ISAMEM_MAX; ++i) {
        mem_rows[i].commit();
        const device_t *seldev = mca_bus ? mcamem_get_device(mcamem_type[i]) : isamem_get_device(isamem_type[i]);
        bool            hascfg = mca_bus ? (mcamem_has_config(mcamem_type[i]) != 0) : (isamem_has_config(isamem_type[i]) != 0);

        mem_models[i]->removeRows(0, mem_removeRows_[i]);
        mem_cbox[i]->setEnabled(mem_models[i]->rowCount() > 1);
        mem_cbox[i]->setCurrentIndex(-1);
        mem_cbox[i]->setCurrentIndex(mem_selectedRows[i]);
        findChild<QPushButton *>(QString("pushButtonConfigureMemExpCard%1").arg(i + 1))->setEnabled(device_is_valid(seldev, machineId) && hascfg);
    }

    // ISA ROM Expansion Cards
    QComboBox          *isarom_cbox[ISAROM_MAX]         = { 0 };
    QAbstractItemModel *isarom_models[ISAROM_MAX]       = { 0 };
    int                 isarom_removeRows_[ISAROM_MAX]  = { 0 };
    int                 isarom_selectedRows[ISAROM_MAX] = { 0 };

    for (uint8_t i = 0; i < ISAROM_MAX; ++i) {
        isarom_cbox[i]        = findChild<QComboBox *>(QString("comboBoxIsaRomCard%1").arg(i + 1));
        isarom_models[i]      = isarom_cbox[i]->model();
        isarom_removeRows_[i] = isarom_models[i]->rowCount();
    }

    std::vector<Models::Batch> isarom_rows(isarom_models, isarom_models + ISAROM_MAX);
    for (const auto &card : Models::Devices(isarom_get_device, isarom_get_internal_name, nullptr, 0)) {
        if (device_is_valid(card.dev, machineId)) {
            for (uint8_t i = 0; i < ISAROM_MAX; ++i) {
                int row = isarom_rows[i].add(card.name, card.id);
                scIsaRomCard[i]->addDevice(card.dev, card.name);

                if (card.id == isarom_type[i])
                    isarom_selectedRows[i] = row - isarom_removeRows_[i];
            }
        }
    }

    for (uint8_t i = 0; i < ISAROM_MAX; ++i) {
        isarom_rows[i].commit();
        isarom_models[i]->removeRows(0, isarom_removeRows_[i]);
        isarom_cbox[i]->setEnabled(isarom_models[i]->rowCount() > 1);
        isarom_cbox[i]->setCurrentIndex(-1);
        isarom_cbox[i]->setCurrentIndex(isarom_selectedRows[i]);
        findChild<QPushButton *>(QString("pushButtonConfigureIsaRomCard%1").arg(i + 1))->setEnabled((isarom_type[i] != 0) && isarom_has_config(isarom_type[i]) && machineHasIsaOrSidecar);
    }
}

int
SettingsOtherPeripherals::changed()
{
    int has_changed = 0;

    has_changed |= (isartc_type            != ui->comboBoxRTC->currentData().toInt());
    has_changed |= isartc_cfg_changed;
    has_changed |= (io_board_type          != ui->comboBoxIOBoard->currentData().toInt());
    has_changed |= io_board_cfg_changed;
    has_changed |= (modem_card_type        != ui->comboBoxModemCard->currentData().toInt());
    has_changed |= modem_card_cfg_changed;
    has_changed |= (usb_card_type          != ui->comboBoxUSB->currentData().toInt());
    has_changed |= (bugger_enabled         != (ui->checkBoxISABugger->isChecked() ? 1 : 0));
    has_changed |= (postcard_enabled       != (ui->checkBoxPOSTCard->isChecked() ? 1 : 0));
    has_changed |= (unittester_enabled     != (ui->checkBoxUnitTester->isChecked() ? 1 : 0));
    has_changed |= unittester_cfg_changed;
    has_changed |= (softpower_enabled      != (softpower_card_enabled ? 1 : 0));
    has_changed |= softpower_cfg_changed;
    has_changed |= (novell_keycard_enabled != (ui->checkBoxKeyCard->isChecked() ? 1 : 0));
    has_changed |= novell_keycard_cfg_changed;

    /* Memory expansion boards (shared slots; active family by machine). */
    {
        const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);

        if (mca_bus || hasIsaOrSidecarBus(machineId)) {
            for (int i = 0; i < ISAMEM_MAX; i++) {
                auto *cbox    = findChild<QComboBox *>(QString("comboBoxMemExpCard%1").arg(i + 1));
                int   cur     = mca_bus ? mcamem_type[i] : isamem_type[i];
                has_changed  |= (cur != cbox->currentData().toInt());
                has_changed  |= memexp_cfg_changed[i];
            }
        }
    }

    /* ISA ROM boards. */
    for (int i = 0; i < ISAROM_MAX; i++) {
        auto *cbox     = findChild<QComboBox *>(QString("comboBoxIsaRomCard%1").arg(i + 1));
        has_changed |= (isarom_type[i]         != cbox->currentData().toInt());
        has_changed |= isarom_cfg_changed[i];
    }

    return (has_changed ? (SETTINGS_CHANGED | SETTINGS_REQUIRE_HARD_RESET) : 0) | pcmcia->changed();
}

void
SettingsOtherPeripherals::restore()
{
}

void
SettingsOtherPeripherals::save(int soft)
{
    pcmcia->save(soft);
    if (soft)
        return;

    /* Other peripherals category */
    isartc_type            = ui->comboBoxRTC->currentData().toInt();
    io_board_type          = hasIsaOrSidecarBus(machineId) ? ui->comboBoxIOBoard->currentData().toInt() : IO_BOARD_NONE;
    modem_card_type        = hasIsaOrSidecarBus(machineId) ? ui->comboBoxModemCard->currentData().toInt() : MODEM_CARD_NONE;
    usb_card_type          = machine_has_bus(machineId, MACHINE_BUS_PCI) ? ui->comboBoxUSB->currentData().toInt() : 0;
    bugger_enabled         = ui->checkBoxISABugger->isChecked() ? 1 : 0;
    postcard_enabled       = ui->checkBoxPOSTCard->isChecked() ? 1 : 0;
    unittester_enabled     = ui->checkBoxUnitTester->isChecked() ? 1 : 0;
    softpower_enabled      = softpower_card_enabled ? 1 : 0;
    novell_keycard_enabled = ui->checkBoxKeyCard->isChecked() ? 1 : 0;

    /* Memory expansion boards (shared slots; write the active family and
       clear the inactive one, matching the single-UI layout). */
    {
        const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
        const bool isa_bus = !mca_bus && hasIsaOrSidecarBus(machineId);

        for (int i = 0; i < ISAMEM_MAX; i++) {
            int val = (mca_bus || isa_bus) ? findChild<QComboBox *>(QString("comboBoxMemExpCard%1").arg(i + 1))->currentData().toInt() : 0;

            if (mca_bus) {
                mcamem_type[i] = val;
                isamem_type[i] = 0;
            } else {
                isamem_type[i] = val;
                mcamem_type[i] = 0;
            }
        }
    }

    /* ISA ROM boards. */
    for (int i = 0; i < ISAROM_MAX; i++) {
        auto *cbox     = findChild<QComboBox *>(QString("comboBoxIsaRomCard%1").arg(i + 1));
        isarom_type[i] = cbox->currentData().toInt();
    }
}

void
SettingsOtherPeripherals::on_comboBoxRTC_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    ui->pushButtonConfigureRTC->setEnabled((index != 0) && isartc_has_config(index) && hasIsaOrSidecarBus(machineId));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureRTC_clicked()
{
    isartc_cfg_changed |= DeviceConfig::ConfigureDevice(isartc_get_device(ui->comboBoxRTC->currentData().toInt()));
}

void
SettingsOtherPeripherals::on_comboBoxIOBoard_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    const int board = ui->comboBoxIOBoard->currentData().toInt();
    ui->pushButtonConfigureIOBoard->setEnabled((board != IO_BOARD_NONE) && io_board_has_config(board) && hasIsaOrSidecarBus(machineId));
    updateIOBoardHint();
}

/* The USB controller row: why it is greyed out, or what it gives. */
void
SettingsOtherPeripherals::updateUSBHint()
{
    QString text;

    if (!machine_has_bus(machineId, MACHINE_BUS_PCI))
        text = tr("The USB controller is a PCI card, and this machine has no PCI slot. Choose a machine with PCI slots on the Machine page to fit one.");
    else if (!usbn_host_available())
        text = tr("This build has no USB passthrough, so devices on the host cannot be connected.");

    ui->labelUSBHint->setVisible(!text.isEmpty());
    ui->labelUSBHint->setText(QString("<small>&#9432; %1</small>").arg(text.toHtmlEscaped()));
    const QString tip = text.isEmpty() ? tr("The USB 1.1 card (UHCI) takes full- and low-speed devices; the USB 2.0 card (EHCI with a UHCI companion) takes any. Both have two ports. Host USB devices are connected from the USB menu; a device connected to the virtual machine is disconnected from the host.") : text;
    ui->comboBoxUSB->setToolTip(tip);
    ui->labelUSB->setToolTip(tip);
}

/* What each board is, for its dropdown entry and the Configure button. */
QString
SettingsOtherPeripherals::ioBoardDescription(int board)
{
    switch (board) {
        case IO_BOARD_FUNWORLD:
            return tr("funworld Photo Play / I.G.O. I/O card (8255): six coins, four notes and the two door buttons. Configure sets its I/O address.");
        case IO_BOARD_MERIT_XL:
            return tr("Merit Megatouch XL I/O board (CRT-500 Zeus): four coins, Operator Setup, Calibrate, the security key, the CS4231A codec and the U12 ROM window. Configure picks the key dump, board ROM, battery RAM and DIP switches.");
        case IO_BOARD_MERIT_MAXX:
            return tr("Merit Megatouch MAXX I/O board (Millennium): four coins, Operator Setup, Calibrate, the security key and the PC Card slots. Configure picks the key dump and DIP switches.");
        default:
            return tr("No arcade I/O board.");
    }
}

/* Whatever of the I/O board row is greyed out, say why and what to do. */
void
SettingsOtherPeripherals::updateIOBoardHint()
{
    QString why;
    QString fix;

    if (!hasIsaOrSidecarBus(machineId)) {
        why = tr("The arcade I/O boards are ISA cards, and this machine has no ISA slot.");
        fix = tr("Choose a machine with ISA slots on the Machine page to fit one.");
    } else if (ui->comboBoxIOBoard->currentData().toInt() == IO_BOARD_NONE) {
        why = tr("Configure is unavailable because no board is fitted.");
        fix = tr("Select a board above, then configure it.");
    } else if (!io_board_has_config(ui->comboBoxIOBoard->currentData().toInt())) {
        why = tr("This board has no settings to configure.");
    }

    const int board = ui->comboBoxIOBoard->currentData().toInt();
    const QString tip = why.isEmpty() ? tr("Configure the %1.").arg(ui->comboBoxIOBoard->currentText()) + "\n\n" + ioBoardDescription(board)
                                      : (fix.isEmpty() ? why : why + "\n" + fix);
    ui->pushButtonConfigureIOBoard->setToolTip(tip);
    ui->comboBoxIOBoard->setToolTip(hasIsaOrSidecarBus(machineId) ? ioBoardDescription(board) : why + "\n" + fix);

    ui->labelIOBoardHint->setVisible(!why.isEmpty());
    if (!why.isEmpty())
        ui->labelIOBoardHint->setText(QString("<small>&#9432; %1%2</small>").arg(why.toHtmlEscaped(), fix.isEmpty() ? QString() : QString(" ") + fix.toHtmlEscaped()));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureIOBoard_clicked()
{
    io_board_cfg_changed |= DeviceConfig::ConfigureDevice(io_board_get_device(ui->comboBoxIOBoard->currentData().toInt()));
}

void
SettingsOtherPeripherals::on_comboBoxModemCard_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    const int card = ui->comboBoxModemCard->currentData().toInt();
    ui->pushButtonConfigureModemCard->setEnabled((card != MODEM_CARD_NONE) && modem_card_has_config(card) && hasIsaOrSidecarBus(machineId));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureModemCard_clicked()
{
    modem_card_cfg_changed |= DeviceConfig::ConfigureDevice(modem_card_get_device(ui->comboBoxModemCard->currentData().toInt()));
}

/* Shared memory-expansion slot helpers: resolve the board device and its
   config capability from the machine's active bus family (MCA or ISA). */
static const device_t *
memexp_slot_device(int idx, bool mca_bus)
{
    return mca_bus ? mcamem_get_device(idx) : isamem_get_device(idx);
}

static int
memexp_slot_has_config(int idx, bool mca_bus)
{
    return mca_bus ? mcamem_has_config(idx) : isamem_has_config(idx);
}

void
SettingsOtherPeripherals::on_comboBoxMemExpCard1_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    ui->pushButtonConfigureMemExpCard1->setEnabled(device_is_valid(memexp_slot_device(index, mca_bus), machineId) && memexp_slot_has_config(index, mca_bus));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureMemExpCard1_clicked()
{
    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    memexp_cfg_changed[0] |= DeviceConfig::ConfigureDevice(memexp_slot_device(ui->comboBoxMemExpCard1->currentData().toInt(), mca_bus), 1);
}

void
SettingsOtherPeripherals::on_comboBoxMemExpCard2_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    ui->pushButtonConfigureMemExpCard2->setEnabled(device_is_valid(memexp_slot_device(index, mca_bus), machineId) && memexp_slot_has_config(index, mca_bus));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureMemExpCard2_clicked()
{
    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    memexp_cfg_changed[1] |= DeviceConfig::ConfigureDevice(memexp_slot_device(ui->comboBoxMemExpCard2->currentData().toInt(), mca_bus), 2);
}

void
SettingsOtherPeripherals::on_comboBoxMemExpCard3_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    ui->pushButtonConfigureMemExpCard3->setEnabled(device_is_valid(memexp_slot_device(index, mca_bus), machineId) && memexp_slot_has_config(index, mca_bus));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureMemExpCard3_clicked()
{
    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    memexp_cfg_changed[2] |= DeviceConfig::ConfigureDevice(memexp_slot_device(ui->comboBoxMemExpCard3->currentData().toInt(), mca_bus), 3);
}

void
SettingsOtherPeripherals::on_comboBoxMemExpCard4_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    ui->pushButtonConfigureMemExpCard4->setEnabled(device_is_valid(memexp_slot_device(index, mca_bus), machineId) && memexp_slot_has_config(index, mca_bus));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureMemExpCard4_clicked()
{
    const bool mca_bus = (machine_has_bus(machineId, MACHINE_BUS_MCA) > 0);
    memexp_cfg_changed[3] |= DeviceConfig::ConfigureDevice(memexp_slot_device(ui->comboBoxMemExpCard4->currentData().toInt(), mca_bus), 4);
}

void
SettingsOtherPeripherals::on_comboBoxIsaRomCard1_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    ui->pushButtonConfigureIsaRomCard1->setEnabled((index != 0) && isarom_has_config(index) && hasIsaOrSidecarBus(machineId));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureIsaRomCard1_clicked()
{
    isarom_cfg_changed[0] |= DeviceConfig::ConfigureDevice(isarom_get_device(ui->comboBoxIsaRomCard1->currentData().toInt()), 1);
}

void
SettingsOtherPeripherals::on_comboBoxIsaRomCard2_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    ui->pushButtonConfigureIsaRomCard2->setEnabled((index != 0) && isarom_has_config(index) && hasIsaOrSidecarBus(machineId));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureIsaRomCard2_clicked()
{
    isarom_cfg_changed[1] |= DeviceConfig::ConfigureDevice(isarom_get_device(ui->comboBoxIsaRomCard2->currentData().toInt()), 2);
}

void
SettingsOtherPeripherals::on_comboBoxIsaRomCard3_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    ui->pushButtonConfigureIsaRomCard3->setEnabled((index != 0) && isarom_has_config(index) && hasIsaOrSidecarBus(machineId));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureIsaRomCard3_clicked()
{
    isarom_cfg_changed[2] |= DeviceConfig::ConfigureDevice(isarom_get_device(ui->comboBoxIsaRomCard3->currentData().toInt()), 3);
}

void
SettingsOtherPeripherals::on_comboBoxIsaRomCard4_currentIndexChanged(int index)
{
    if (index < 0)
        return;

    ui->pushButtonConfigureIsaRomCard4->setEnabled((index != 0) && isarom_has_config(index) && hasIsaOrSidecarBus(machineId));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureIsaRomCard4_clicked()
{
    isarom_cfg_changed[3] |= DeviceConfig::ConfigureDevice(isarom_get_device(ui->comboBoxIsaRomCard4->currentData().toInt()), 4);
}

void
SettingsOtherPeripherals::on_checkBoxUnitTester_stateChanged(int arg1)
{
    ui->pushButtonConfigureUT->setEnabled(arg1 != 0);
}

void
SettingsOtherPeripherals::on_pushButtonConfigureUT_clicked()
{
    unittester_cfg_changed |= DeviceConfig::ConfigureDevice(&unittester_device);
}

void
SettingsOtherPeripherals::on_checkBoxSoftPower_stateChanged(int arg1)
{
    const bool optionalCard = (machines[machineId].init != machine_ibm5140_init) &&
                              (machine_has_bus(machineId, MACHINE_BUS_ISA) > 0);
    if (optionalCard)
        softpower_card_enabled = arg1 != 0;
    ui->pushButtonConfigureSoftPower->setEnabled(optionalCard && (arg1 != 0));
}

void
SettingsOtherPeripherals::on_pushButtonConfigureSoftPower_clicked()
{
    softpower_cfg_changed |= DeviceConfig::ConfigureDevice(&softpower_device);
}

void
SettingsOtherPeripherals::on_checkBoxKeyCard_stateChanged(int arg1)
{
    ui->pushButtonConfigureKeyCard->setEnabled(arg1 != 0);
}

void
SettingsOtherPeripherals::on_pushButtonConfigureKeyCard_clicked()
{
    novell_keycard_cfg_changed |= DeviceConfig::ConfigureDevice(&novell_keycard_device);
}

int
SettingsOtherPeripherals::pcmciaCard(int s) const
{
    return pcmcia->socketCard(s);
}
