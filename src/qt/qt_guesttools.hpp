/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             86Box Guest Tools, the UI side: the host clipboard for the
 *             guest agent, and leaving pointer integration's mouse alone.
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

class GuestTools : public QObject {
    Q_OBJECT

public:
    explicit GuestTools(QObject *parent = nullptr);
    ~GuestTools() override;

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
