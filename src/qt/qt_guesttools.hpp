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
#include <QStringList>

#include <cstddef>
#include <cstdint>

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

    /* guest_tools_file_ops_t, on the emulation thread. */
    static int filesList();
    static int filesEntry(int index, uint32_t *flags, uint64_t *size, const uint16_t **name, size_t *chars);
    static int filesRead(int index, uint64_t offset, void *buf, uint32_t bytes);
    static int filesBegin();
    static int filesAdd(uint32_t flags, uint64_t size, const uint16_t *name, size_t chars);
    static int filesWrite(int index, uint64_t offset, const void *buf, uint32_t bytes);
    static int filesEnd(int action);
    static void filesDebug(const char *paths);

private:
    void hostClipboardChanged();
    void setHostClipboard(const QString &text);
    void setHostFiles(const QStringList &files);
    void poll();

    static void      setHostHandler(const uint16_t *text, size_t chars);
    static uint16_t *getHostHandler(size_t *chars);

    static GuestTools *instance;

    QMutex  mutex;
    QString     hostText;  /* the host clipboard as the guest last saw or set it */
    QStringList hostFiles; /* the host clipboard's local files, if it holds files */
    QStringList ownFiles;  /* the files the guest last put there */
    bool    hostHasText   = false;
    bool    pointerActive = false;
    QTimer *timer         = nullptr;
};

#endif
