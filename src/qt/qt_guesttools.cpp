/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             86Box Guest Tools, the UI side.  See qt_guesttools.hpp.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_guesttools.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QMimeData>
#include <QTimer>
#include <QWidget>

#include "qt_mediamenu.hpp"

#include <cstdlib>
#include <cstring>

#ifdef Q_OS_WINDOWS
#    include <windows.h>
#endif

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/timer.h>
#include <86box/cdrom.h>
#include <86box/fdd.h>
#include <86box/plat.h>
#include <86box/guest_tools.h>
}

GuestTools *GuestTools::instance = nullptr;

/* The guest sees LF line ends only. */
static QString
lineFeeds(QString text)
{
    return text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

GuestTools::GuestTools(QObject *parent)
    : QObject(parent)
{
    instance = this;

    const QMimeData *data = QApplication::clipboard()->mimeData();
    hostHasText           = data && data->hasText();
    hostText              = hostHasText ? lineFeeds(data->text()) : QString();
    guest_tools_host_clipboard_changed();

    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &GuestTools::hostClipboardChanged);
    guest_tools_set_clipboard_handlers(setHostHandler, getHostHandler);
    guest_tools_insert_media_hook = [] {
        if (instance != nullptr)
            QMetaObject::invokeMethod(instance, [] {
                insertMedia(qobject_cast<QWidget *>(instance->parent()));
            }, Qt::QueuedConnection);
    };

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &GuestTools::poll);
    timer->start(100);
}

GuestTools::~GuestTools()
{
    guest_tools_set_clipboard_handlers(nullptr, nullptr);
    guest_tools_insert_media_hook = nullptr;
    instance                      = nullptr;
}

void
GuestTools::hostClipboardChanged()
{
    const QMimeData *data = QApplication::clipboard()->mimeData();
    bool             has  = data && data->hasText();
    QString          text = has ? lineFeeds(data->text()) : QString();

    {
        QMutexLocker lock(&mutex);
        /* Our own copy of the guest's text coming back: nothing new. */
        if ((has == hostHasText) && (text == hostText))
            return;
        hostHasText = has;
        hostText    = text;
    }
    guest_tools_host_clipboard_changed();
}

void
GuestTools::setHostClipboard(const QString &text)
{
    {
        QMutexLocker lock(&mutex);
        if (hostHasText && (text == hostText))
            return;
        hostHasText = true;
        hostText    = text;
    }
    QApplication::clipboard()->setText(text);
}

#ifdef Q_OS_WINDOWS
/* The executable's file name of a process, lower case; empty if unknown. */
static QString
processExeName(DWORD pid)
{
    wchar_t path[MAX_PATH];
    DWORD   size = MAX_PATH;
    HANDLE  proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    QString name;

    if (proc == nullptr)
        return name;
    if (QueryFullProcessImageNameW(proc, 0, path, &size))
        name = QString::fromWCharArray(path, static_cast<int>(size)).section(QLatin1Char('\\'), -1).toLower();
    CloseHandle(proc);
    return name;
}

/* Integrated, the keyboard follows the pointer from one machine's window to
   another's: while this machine has the focus and the pointer is over
   another 86Box-Next window on top, that window gets the focus. (Only
   between machines: other programs still take the focus on a click.) */
static void
handFocusToMachineUnderPointer()
{
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    POINT pt;
    HWND  target;
    DWORD target_pid = 0;

    if ((fg == nullptr) || !GetCursorPos(&pt))
        return;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId())
        return;
    /* Not while dragging something. */
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000)
        return;

    target = WindowFromPoint(pt);
    if (target == nullptr)
        return;
    target = GetAncestor(target, GA_ROOT);
    GetWindowThreadProcessId(target, &target_pid);
    if ((target_pid == 0) || (target_pid == pid))
        return;
    if (processExeName(target_pid) != processExeName(pid))
        return;

    SetForegroundWindow(target);
}
#endif

/* Pointer integration took over: let go of a mouse captured before it did. */
void
GuestTools::poll()
{
    bool active = guest_tools_pointer_active();

    if (active && !pointerActive && mouse_capture)
        plat_mouse_capture(0);
    pointerActive = active;

#ifdef Q_OS_WINDOWS
    if (guest_tools_keyboard_outside())
        handFocusToMachineUnderPointer();
#endif
}

/* Emulation thread. */
void
GuestTools::setHostHandler(const uint16_t *text, size_t chars)
{
    if (instance == nullptr)
        return;

    QString copy = QString::fromUtf16(reinterpret_cast<const char16_t *>(text), static_cast<int>(chars));
    QMetaObject::invokeMethod(instance, [copy] {
        if (instance != nullptr)
            instance->setHostClipboard(copy);
    }, Qt::QueuedConnection);
}

/* Emulation thread. */
uint16_t *
GuestTools::getHostHandler(size_t *chars)
{
    *chars = 0;
    if (instance == nullptr)
        return nullptr;

    QMutexLocker lock(&instance->mutex);
    if (!instance->hostHasText)
        return nullptr;

    const QString &text = instance->hostText;
    auto          *copy = static_cast<uint16_t *>(malloc((text.size() + 1) * sizeof(uint16_t)));
    if (copy == nullptr)
        return nullptr;
    memcpy(copy, text.utf16(), text.size() * sizeof(uint16_t));
    copy[text.size()] = 0;
    *chars            = text.size();
    return copy;
}

/* ---- Install media --------------------------------------------------------- */

/* The built-in image, written once to the global data directory (its name
   carries a hash of the contents, so a newer build never overwrites an
   image a running machine has open). Empty on failure. */
static void
init_resources()
{
    static bool done = false;
    if (!done) {
        /* A static library's resources only link in when referenced. */
        Q_INIT_RESOURCE(guesttools);
        done = true;
    }
}

static QString
mediaFile(const QString &ext)
{
    init_resources();

    QFile res(QStringLiteral(":/guesttools/86box-guest-tools.") + ext);
    char  dir[1024];

    if (!res.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray data = res.readAll();
    const QString    hash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex().left(8));

    plat_get_global_data_dir(dir, sizeof(dir));
    QDir base(QString::fromUtf8(dir));
    if (!base.mkpath(QStringLiteral("guest-tools")))
        return QString();
    const QString path = base.filePath(QStringLiteral("guest-tools/86box-guest-tools-%1.%2").arg(hash, ext));

    QFile out(path);
    if (out.exists() && (out.size() == data.size()))
        return path;
    if (!out.open(QIODevice::WriteOnly) || (out.write(data) != data.size()))
        return QString();
    return path;
}

void
GuestTools::insertMedia(QWidget *parent)
{
    const QString title = tr("86Box Guest Tools");
    int           cd    = -1;
    int           fd    = -1;
    bool          reset = false;
    QString       text;

    for (int i = 0; (i < CDROM_NUM) && (cd < 0); i++) {
        if (cdrom[i].bus_type != CDROM_BUS_DISABLED)
            cd = i;
    }
    for (int i = 0; (i < FDD_NUM) && (fd < 0); i++) {
        if (fdd_get_type(&drives[i]) != 0)
            fd = i;
    }
    if ((cd < 0) && (fd < 0)) {
        QMessageBox::warning(parent, title, tr("This machine has no CD-ROM or floppy drive to put the Guest Tools in."));
        return;
    }

    if (!guest_tools_enabled) {
        if (QMessageBox::question(parent, title,
                                  tr("86Box Guest Tools are off for this machine. Turn them on?\n\n"
                                     "The machine restarts (as after changing its settings)."))
            != QMessageBox::Yes)
            return;
        const int was_paused = dopause;
        plat_pause(1);
        guest_tools_enabled = 1;
        config_changed      = 2;
        pc_reset_hard();
        config_save();
        plat_pause(was_paused);
        reset = true;
    }

    const QString path = mediaFile((cd >= 0) ? QStringLiteral("iso") : QStringLiteral("img"));
    if (path.isEmpty()) {
        QMessageBox::warning(parent, title, tr("Could not write the Guest Tools image to the data directory."));
        return;
    }

    if (cd >= 0) {
        MediaMenu::ptr->cdromMount(cd, path);
        text = tr("The 86Box Guest Tools CD is in the machine's CD-ROM drive.\n\n"
                  "Windows 95 and later start the installer by themselves%1. "
                  "If not, run 86BOXGT.EXE from the CD; on Windows 3.x, GTWIN.EXE "
                  "(Program Manager: File > Run).")
                   .arg(reset ? tr(" (Windows 95/98: when Windows starts)") : QString());
    } else {
        MediaMenu::ptr->floppyMount(fd, path, true);
        text = tr("The 86Box Guest Tools floppy is in drive %1:.\n\n"
                  "Run %1:\\86BOXGT.EXE (Windows 95 and later) or %1:\\GTWIN.EXE "
                  "(Windows 3.x: Program Manager, File > Run).")
                   .arg(QChar('A' + fd));
    }

    /* Not modal: the machine keeps running while it shows. */
    auto *box = new QMessageBox(QMessageBox::Information, title, text, QMessageBox::Ok, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}
