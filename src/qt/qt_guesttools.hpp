/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             86Box Guest Tools, the UI side: the host clipboard for the
 *             guest agent, leaving pointer integration's mouse alone, and
 *             the install media built into the emulator.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_GUESTTOOLS_HPP
#define QT_GUESTTOOLS_HPP

#include <QMutex>
#include <QObject>
#include <QString>

class QTimer;
class QWidget;

class GuestTools : public QObject {
    Q_OBJECT

public:
    explicit GuestTools(QObject *parent = nullptr);
    ~GuestTools() override;

    /* Tools > Install 86Box Guest Tools: put the built-in CD (or floppy)
       into the machine, turning the tools on first if need be. */
    static void insertMedia(QWidget *parent);

private:
    void hostClipboardChanged();
    void setHostClipboard(const QString &text);
    void poll();

    static void      setHostHandler(const uint16_t *text, size_t chars);
    static uint16_t *getHostHandler(size_t *chars);

    static GuestTools *instance;

    QMutex  mutex;
    QString hostText; /* the host clipboard as the guest last saw or set it */
    bool    hostHasText   = false;
    bool    pointerActive = false;
    QTimer *timer         = nullptr;
};

#endif
