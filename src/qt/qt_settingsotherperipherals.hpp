#ifndef QT_SETTINGSOTHERPERIPHERALS_HPP
#define QT_SETTINGSOTHERPERIPHERALS_HPP

#include <QWidget>

class SettingsPcmcia;

namespace Ui {
class SettingsOtherPeripherals;
}

class SettingsOtherPeripherals : public QWidget {
    Q_OBJECT

public:
    explicit SettingsOtherPeripherals(QWidget *parent = nullptr);
    ~SettingsOtherPeripherals();

    int  changed();
    int  pcmciaCard(int s) const;   /* the PC Card chosen for socket s now */

    void restore();
    void save(int soft);

public slots:
    void onCurrentMachineChanged(int machineId);

private slots:
    void on_comboBoxRTC_currentIndexChanged(int index);
    void on_pushButtonConfigureRTC_clicked();
    void on_comboBoxIOBoard_currentIndexChanged(int index);
    void on_pushButtonConfigureIOBoard_clicked();
    void on_comboBoxModemCard_currentIndexChanged(int index);
    void on_pushButtonConfigureModemCard_clicked();
    void updateIOBoardHint();
    QString ioBoardDescription(int board);
    void updateUSBHint();

    void on_comboBoxMemExpCard1_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard1_clicked();
    void on_comboBoxMemExpCard2_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard2_clicked();
    void on_comboBoxMemExpCard3_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard3_clicked();
    void on_comboBoxMemExpCard4_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard4_clicked();

    void on_comboBoxIsaRomCard1_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard1_clicked();
    void on_comboBoxIsaRomCard2_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard2_clicked();
    void on_comboBoxIsaRomCard3_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard3_clicked();
    void on_comboBoxIsaRomCard4_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard4_clicked();

    void on_checkBoxUnitTester_stateChanged(int arg1);
    void on_pushButtonConfigureUT_clicked();

    void on_checkBoxKeyCard_stateChanged(int arg1);
    void on_pushButtonConfigureKeyCard_clicked();

    void on_checkBoxSoftPower_stateChanged(int arg1);
    void on_pushButtonConfigureSoftPower_clicked();

private:
    Ui::SettingsOtherPeripherals *ui;
    int                           machineId { 0 };

    int                           memexp_cfg_changed[4]      = { 0, 0, 0, 0 };
    int                           isarom_cfg_changed[4]      = { 0, 0, 0, 0 };
    int                           isartc_cfg_changed         = 0;
    int                           unittester_cfg_changed     = 0;
    int                           novell_keycard_cfg_changed = 0;
    int                           softpower_cfg_changed      = 0;
    bool                          softpower_card_enabled     = false;

    SettingsCompleter            *scRTC;
    SettingsPcmcia               *pcmcia;   /* the PCMCIA tab */
    int                           io_board_cfg_changed = 0;
    int                           modem_card_cfg_changed = 0;

    SettingsCompleter            *scMemExpCard[4];
    SettingsCompleter            *scIsaRomCard[4];
};

#endif // QT_SETTINGSOTHERPERIPHERALS_HPP
