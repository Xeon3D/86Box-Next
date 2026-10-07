/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The modem menu, behind the modem icon in the status bar: what
 *             each COM port modem's telephone line reaches -- nothing, a TCP
 *             host, isp-server's telephone network and its number there --
 *             changed while the machine runs.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_MODEM_MENU_HPP
#define QT_MODEM_MENU_HPP

#include <QObject>
#include <QString>

class QMenu;
class QWidget;

class ModemMenu : public QObject {
    Q_OBJECT

public:
    explicit ModemMenu(QWidget *parent);

    QMenu  *menu() const { return m_menu; }
    QString toolTip() const;   /* each modem, its line and what it is doing */
    static bool any();         /* a modem on some COM port, or carried by a device */
    static bool carried();     /* a modem another device carries (a PC Card's) */
    static bool busy();        /* ...and one of them is in a call */
    void        buildMenu();   /* also run before showing, to know its size */

signals:
    void changed();

private:
    void editHost(int com);
    void editPhone(int com);
    void callNumber(int com);

    QWidget *m_parent;
    QMenu   *m_menu;
};

#endif // QT_MODEM_MENU_HPP
