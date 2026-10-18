/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             86Box Guest Tools, the UI side.  See qt_guesttools.hpp.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_guesttools.hpp"

#include <QAbstractNativeEventFilter>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>
#include <QMessageBox>
#include <QMimeData>
#include <QTimer>
#include <QWidget>

#include "qt_mediamenu.hpp"

#include <cstdlib>
#include <cstring>
#include <atomic>
#include <vector>

#ifdef Q_OS_WINDOWS
#    include <windows.h>

static void installFocusFilter(bool on);
#endif

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/timer.h>
#include <86box/cdrom.h>
#include <86box/fdd.h>
#include <86box/plat.h>
#include <86box/mouse.h>
#include <86box/guest_tools.h>
#include <86box/guest_tools_version.h>
}

GuestTools *GuestTools::instance = nullptr;

/* The guest sees LF line ends only. */
static QString
lineFeeds(QString text)
{
    return text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

/* The local files and folders on the clipboard, if that is what it holds. */
static QStringList
localFiles(const QMimeData *data)
{
    QStringList files;

    if ((data == nullptr) || !data->hasUrls())
        return files;
    for (const QUrl &url : data->urls()) {
        if (!url.isLocalFile())
            return QStringList();
        files << QDir::cleanPath(url.toLocalFile());
    }
    return files;
}

/* ---- Files ------------------------------------------------------------------ */

/* Emulation thread only: the host files as the guest reads them, one
   snapshot per source (the clipboard, a drag), and the files coming from
   the guest. */
namespace {
struct FileEntry {
    QString  path; /* on the host */
    QString  name; /* as the guest sees it, '\' between the parts */
    uint32_t flags;
    uint64_t size;
};

std::vector<FileEntry> snapshot[2];
QFile                  readFile;
int                    readSource = -1;
int                    readIndex  = -1;

QString              uploadDir;
std::vector<QString> uploadPaths;
QFile                writeFile;
int                  writeIndex = -1;
int                  uploadCount = 0;

/* debug_cmd "gt files": the host clipboard is left alone. */
std::atomic<bool> debugFiles { false };
}

/* <global data dir>/guest-tools/clipboard: one folder per set from a guest,
   named <process id>-<n>. */
static QString
stagingBase()
{
    char dir[1024];

    plat_get_global_data_dir(dir, sizeof(dir));
    return QDir(QString::fromUtf8(dir)).filePath(QStringLiteral("guest-tools/clipboard"));
}

static QString
stagingPrefix()
{
    return QString::number(QCoreApplication::applicationPid()) + QLatin1Char('-');
}

static void
addEntries(std::vector<FileEntry> &list, const QString &path, const QString &name)
{
    if (list.size() >= GT_FILES_MAX_ENTRIES)
        return;

    QFileInfo info(path);
    if (info.isDir()) {
        list.push_back({ path, name, GT_FILE_DIR, 0 });
        if (info.isSymLink())
            return;
        const QFileInfoList children = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                                QDir::Name | QDir::DirsFirst);
        for (const QFileInfo &child : children)
            addEntries(list, child.filePath(), name + QLatin1Char('\\') + child.fileName());
    } else if (info.isFile())
        list.push_back({ path, name, 0, static_cast<uint64_t>(info.size()) });
}

int
GuestTools::filesList(int source)
{
    QStringList files;

    if ((instance == nullptr) || (source < 0) || (source > 1))
        return -1;
    {
        QMutexLocker lock(&instance->mutex);
        files = (source == GT_SOURCE_DRAG) ? instance->dragFiles : instance->hostFiles;
    }
    if (files.isEmpty())
        return -1;

    snapshot[source].clear();
    if (readSource == source) {
        readFile.close();
        readSource = -1;
        readIndex  = -1;
    }
    for (const QString &path : files)
        addEntries(snapshot[source], path, QFileInfo(path).fileName());
    return static_cast<int>(snapshot[source].size());
}

int
GuestTools::filesEntry(int source, int index, uint32_t *flags, uint64_t *size, const uint16_t **name, size_t *chars)
{
    if ((source < 0) || (source > 1) || (index < 0) || (index >= static_cast<int>(snapshot[source].size())))
        return -1;

    const FileEntry &e = snapshot[source][index];
    *flags             = e.flags;
    *size              = e.size;
    *name              = reinterpret_cast<const uint16_t *>(e.name.utf16());
    *chars             = e.name.size();
    return 0;
}

int
GuestTools::filesRead(int source, int index, uint64_t offset, void *buf, uint32_t bytes)
{
    if ((source < 0) || (source > 1) || (index < 0) || (index >= static_cast<int>(snapshot[source].size())) ||
        (snapshot[source][index].flags & GT_FILE_DIR))
        return -1;
    if ((source != readSource) || (index != readIndex)) {
        readFile.close();
        readFile.setFileName(snapshot[source][index].path);
        if (!readFile.open(QIODevice::ReadOnly)) {
            readSource = -1;
            readIndex  = -1;
            return -1;
        }
        readSource = source;
        readIndex  = index;
    }
    if (!readFile.seek(static_cast<qint64>(offset)))
        return -1;
    const qint64 got = readFile.read(static_cast<char *>(buf), bytes);
    return (got < 0) ? -1 : static_cast<int>(got);
}

/* A guest's name for something, as a path under the set's folder; empty
   if it would leave the folder or has no name. */
static QString
safeName(const QString &name)
{
    static const QString bad = QStringLiteral("<>:\"/|?*");
    QStringList          parts;

    for (QString part : name.split(QLatin1Char('\\'))) {
        QString clean;

        for (const QChar c : part) {
            if ((c.unicode() >= 0x20) && !bad.contains(c))
                clean += c;
        }
        /* Windows drops trailing dots and spaces. */
        while (clean.endsWith(QLatin1Char('.')) || clean.endsWith(QLatin1Char(' ')))
            clean.chop(1);
        if (clean.isEmpty())
            return QString();
        parts << clean;
    }
    return parts.join(QLatin1Char('/'));
}

/* Drop the sets this process made, but the one on the clipboard. */
static void
removeStaging(const QString &keep)
{
    QDir base(stagingBase());

    for (const QString &dir : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString path = base.filePath(dir);
        if (dir.startsWith(stagingPrefix()) && (path != keep))
            QDir(path).removeRecursively();
    }
}

int
GuestTools::filesBegin()
{
    QString keep;

    if (instance == nullptr)
        return -1;
    {
        QMutexLocker lock(&instance->mutex);
        if (!instance->ownFiles.isEmpty() && (instance->ownFiles == instance->hostFiles))
            keep = QFileInfo(instance->ownFiles.first()).path();
    }
    filesEnd(GT_FILES_CANCEL);
    removeStaging(keep);
    {
        QMutexLocker lock(&instance->mutex);
        instance->dragOutDone = false;
        instance->dragOutFiles.clear();
    }

    uploadDir = QDir(stagingBase()).filePath(stagingPrefix() + QString::number(++uploadCount));
    QDir(uploadDir).removeRecursively();
    if (!QDir().mkpath(uploadDir)) {
        uploadDir.clear();
        return -1;
    }
    return 0;
}

int
GuestTools::filesAdd(uint32_t flags, uint64_t size, const uint16_t *name, size_t chars)
{
    (void) size;
    if (uploadDir.isEmpty())
        return -1;

    const QString rel = safeName(QString::fromUtf16(reinterpret_cast<const char16_t *>(name), static_cast<int>(chars)));
    if (rel.isEmpty())
        return -1;
    const QString path = QDir(uploadDir).filePath(rel);

    if (flags & GT_FILE_DIR) {
        if (!QDir().mkpath(path))
            return -1;
    } else {
        QFile file(path);

        QDir().mkpath(QFileInfo(path).path());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return -1;
    }
    uploadPaths.push_back(path);
    return static_cast<int>(uploadPaths.size()) - 1;
}

int
GuestTools::filesWrite(int index, uint64_t offset, const void *buf, uint32_t bytes)
{
    if ((index < 0) || (index >= static_cast<int>(uploadPaths.size())))
        return -1;
    if (index != writeIndex) {
        writeFile.close();
        writeFile.setFileName(uploadPaths[index]);
        writeIndex = writeFile.open(QIODevice::ReadWrite) ? index : -1;
        if (writeIndex < 0)
            return -1;
    }
    if (!writeFile.seek(static_cast<qint64>(offset)))
        return -1;
    return (writeFile.write(static_cast<const char *>(buf), bytes) == bytes) ? 0 : -1;
}

int
GuestTools::filesEnd(int action)
{
    QStringList top;

    writeFile.close();
    writeIndex = -1;
    if (uploadDir.isEmpty())
        return (action == GT_FILES_CANCEL) ? 0 : -1;

    if ((action == GT_FILES_CLIPBOARD) || (action == GT_FILES_DRAG)) {
        const QDir dir(uploadDir);
        for (const QString &path : uploadPaths) {
            if (QFileInfo(path).path() == dir.path())
                top << QDir::cleanPath(path);
        }
    }
    if (top.isEmpty())
        QDir(uploadDir).removeRecursively();
    uploadDir.clear();
    uploadPaths.clear();

    if (action == GT_FILES_DRAG) {
        /* The host drag carrying them waits for this. */
        if (debugFiles) {
            for (const QString &path : top)
                pclog("Guest Tools: guest file for the host drag: %s\n", path.toUtf8().constData());
        }
        if (instance != nullptr) {
            QMutexLocker lock(&instance->mutex);
            instance->dragOutFiles = top;
            instance->dragOutDone  = true;
        }
        return 0;
    }
    if (!top.isEmpty() && debugFiles) {
        for (const QString &path : top)
            pclog("Guest Tools: guest file for the host clipboard: %s\n", path.toUtf8().constData());
        return 0;
    }
    if (!top.isEmpty() && (instance != nullptr))
        QMetaObject::invokeMethod(instance, [top] {
            if (instance != nullptr)
                instance->setHostFiles(top);
        }, Qt::QueuedConnection);
    return 0;
}

void
GuestTools::filesDebug(const char *paths)
{
    if (instance == nullptr)
        return;
    debugFiles = true;

    QStringList files;
    for (const QString &path : QString::fromUtf8(paths).split(QLatin1Char('|'), Qt::SkipEmptyParts))
        files << QDir::cleanPath(path.trimmed());
    QMutexLocker lock(&instance->mutex);
    instance->hostFiles = files;
    instance->dragFiles = files;
}

/* ---- Drag and drop ---------------------------------------------------------- */

void
GuestTools::hostDragEnter(const QStringList &files)
{
    if (instance == nullptr)
        return;
    QMutexLocker lock(&instance->mutex);
    instance->dragFiles = files;
}

/* The files of the guest drag the host carries on, once the guest has sent
   them all (empty if it never did). Runs the event loop while it waits. */
QStringList
GuestTools::waitDragOutFiles()
{
    QElapsedTimer waited;
    bool          cursor = false;

    waited.start();
    while (instance != nullptr) {
        {
            QMutexLocker lock(&instance->mutex);
            if (instance->dragOutDone) {
                if (cursor)
                    QApplication::restoreOverrideCursor();
                pclog("Guest Tools: guest drag files ready (%d) after %d ms\n",
                      static_cast<int>(instance->dragOutFiles.size()), static_cast<int>(waited.elapsed()));
                return instance->dragOutFiles;
            }
        }
        /* The agent went away, or the guest stopped sending. */
        if (!guest_tools_pointer_active() && (waited.elapsed() > 5000))
            break;
        if (waited.elapsed() > (30 * 60 * 1000))
            break;
        if (!cursor && (waited.elapsed() > 300)) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            cursor = true;
        }
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (cursor)
        QApplication::restoreOverrideCursor();
    pclog("Guest Tools: guest drag files never came\n");
    return QStringList();
}

namespace {
/* What the host drag of a guest's files carries: their paths, which only
   exist once the guest has sent them, so asking waits for them. */
class GuestDragData : public QMimeData {
public:
    QStringList formats() const override
    {
        return { QStringLiteral("text/uri-list") };
    }

    bool hasFormat(const QString &mimeType) const override
    {
        return mimeType == QStringLiteral("text/uri-list");
    }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVariant retrieveData(const QString &mimeType, QMetaType) const override
#else
    QVariant retrieveData(const QString &mimeType, QVariant::Type) const override
#endif
    {
        QVariantList urls;

        if (mimeType != QStringLiteral("text/uri-list"))
            return QVariant();
        if (files.isEmpty())
            files = GuestTools::waitDragOutFiles();
        for (const QString &path : files)
            urls << QUrl::fromLocalFile(path);
        return urls;
    }

private:
    mutable QStringList files;
};
}

void
GuestTools::startHostDrag()
{
    auto *drag = new QDrag(this);

    drag->setMimeData(new GuestDragData);
    hostDragging = true;
    pclog("Guest Tools: host drag of the guest's files starts\n");
    const Qt::DropAction action = drag->exec(Qt::CopyAction, Qt::CopyAction);
    pclog("Guest Tools: host drag of the guest's files ended, action %d\n", static_cast<int>(action));
    hostDragging = false;
}

static const guest_tools_file_ops_t fileOps = {
    GuestTools::filesList,
    GuestTools::filesEntry,
    GuestTools::filesRead,
    GuestTools::filesBegin,
    GuestTools::filesAdd,
    GuestTools::filesWrite,
    GuestTools::filesEnd,
    GuestTools::filesDebug,
};

/* The guest's files on the host clipboard, as a file manager's copy. */
void
GuestTools::setHostFiles(const QStringList &files)
{
    QList<QUrl> urls;
    auto       *data = new QMimeData;

    for (const QString &path : files)
        urls << QUrl::fromLocalFile(path);
    data->setUrls(urls);
#ifdef Q_OS_WINDOWS
    /* Explorer: a copy, not a move. */
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\""),
                  QByteArray("\x01\x00\x00\x00", 4));
#endif
    {
        QMutexLocker lock(&mutex);
        ownFiles = files;
    }
    QApplication::clipboard()->setMimeData(data);
}

GuestTools::GuestTools(QObject *parent)
    : QObject(parent)
{
    instance = this;

    const QMimeData *data = QApplication::clipboard()->mimeData();
    hostHasText           = data && data->hasText();
    hostText              = hostHasText ? lineFeeds(data->text()) : QString();
    hostFiles             = localFiles(data);
    guest_tools_host_clipboard_changed();

    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &GuestTools::hostClipboardChanged);
    guest_tools_set_clipboard_handlers(setHostHandler, getHostHandler);
    guest_tools_set_file_ops(&fileOps);
    guest_tools_insert_media_hook = [] {
        if (instance != nullptr)
            QMetaObject::invokeMethod(instance, [] {
                insertMedia(qobject_cast<QWidget *>(instance->parent()));
            }, Qt::QueuedConnection);
    };

#ifdef Q_OS_WINDOWS
    installFocusFilter(true);
#endif

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &GuestTools::poll);
    timer->start(50);
}

GuestTools::~GuestTools()
{
#ifdef Q_OS_WINDOWS
    installFocusFilter(false);
#endif
    guest_tools_set_clipboard_handlers(nullptr, nullptr);
    guest_tools_set_file_ops(nullptr);
    {
        /* Our files stay while the clipboard still offers them. */
        QMutexLocker lock(&mutex);
        removeStaging((!ownFiles.isEmpty() && (ownFiles == hostFiles)) ? QFileInfo(ownFiles.first()).path() : QString());
    }
    guest_tools_insert_media_hook = nullptr;
    instance                      = nullptr;
}

void
GuestTools::hostClipboardChanged()
{
    const QMimeData *data = QApplication::clipboard()->mimeData();
    bool             has   = data && data->hasText();
    QString          text  = has ? lineFeeds(data->text()) : QString();
    QStringList      files = localFiles(data);

    if (debugFiles)
        return;
    {
        QMutexLocker lock(&mutex);
        /* Our own copy of the guest's text coming back: nothing new. */
        if ((has == hostHasText) && (text == hostText) && (files == hostFiles))
            return;
        hostHasText = has;
        hostText    = text;
        hostFiles   = files;
        /* The guest's own files: it has them already. */
        if (!files.isEmpty() && (files == ownFiles))
            return;
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
        hostFiles.clear();
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

/* Asks another machine's window to take the focus without coming to the
   front. */
static UINT
focusMessage()
{
    static UINT msg = RegisterWindowMessageW(L"86Box-Next.GuestTools.TakeFocus");
    return msg;
}

/* Until then (GetTickCount), the machine's own window takes the focus
   without changing its place in the Z order. */
static DWORD noRaiseUntil = 0;

/* The machine's side of focusMessage(): the activation that follows would
   bring the window to the top, so WM_WINDOWPOSCHANGING keeps its Z order. */
class FocusFilter : public QAbstractNativeEventFilter {
public:
#    if QT_VERSION_MAJOR >= 6
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override
#    else
    bool nativeEventFilter(const QByteArray &eventType, void *message, long *) override
#    endif
    {
        if (eventType != "windows_generic_MSG")
            return false;

        MSG *msg = static_cast<MSG *>(message);
        if ((msg->message == focusMessage()) && (focusMessage() != 0)) {
            noRaiseUntil = GetTickCount() + 250;
            SetForegroundWindow(msg->hwnd);
            return true;
        }
        if ((msg->message == WM_WINDOWPOSCHANGING) && (static_cast<LONG>(noRaiseUntil - GetTickCount()) > 0))
            reinterpret_cast<WINDOWPOS *>(msg->lParam)->flags |= SWP_NOZORDER;
        return false;
    }
};

static void
installFocusFilter(bool on)
{
    static FocusFilter filter;

    if (on)
        qApp->installNativeEventFilter(&filter);
    else
        qApp->removeNativeEventFilter(&filter);
}

/* Integrated, the keyboard follows the pointer from one machine's window to
   another's: while this machine has the focus and the pointer is over
   another 86Box-Next window, that window takes the focus where it is,
   covered or not -- it is not raised. (Only between machines: other
   programs still take the focus on a click.) */
static void
handFocusToMachineUnderPointer()
{
    static HWND  lastTarget = nullptr;
    static DWORD lastAsked  = 0;

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

    /* Asked already: the poll comes every 100 ms, the answer a little later. */
    if ((target == lastTarget) && ((GetTickCount() - lastAsked) < 500))
        return;
    lastTarget = target;
    lastAsked  = GetTickCount();

    /* The foreground process lets the other one take the focus itself. */
    AllowSetForegroundWindow(target_pid);
    PostMessageW(target, focusMessage(), 0, 0);
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

    /* The agent caught a guest drag of files leaving the display: the guest
       drops it on the agent (let go of the guest's button) and the host
       carries it on with the host's button, still down. */
    /* Tell the window which tools the guest runs (Tools menu). */
    const unsigned int version = guest_tools_agent_version();
    if (version != agentVersion) {
        agentVersion = version;
        emit agentVersionChanged(version);
    }

    const bool out = guest_tools_drag_out();
    if (out && !dragOut) {
        bool held = QApplication::mouseButtons() & Qt::LeftButton;

#ifdef Q_OS_WINDOWS
        /* Qt only knows about buttons pressed over its own windows. */
        held = held || (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000);
#endif
        dragOut = true;
        mouse_set_buttons_ex(mouse_get_buttons_ex() & ~1);
        pclog("Guest Tools: guest drag out, host button %s%s\n", held ? "held" : "up",
              debugFiles ? " (debug: no host drag)" : (hostDragging ? " (host drag running)" : ""));
        if (!debugFiles && !hostDragging && held)
            QTimer::singleShot(0, this, &GuestTools::startHostDrag);
    } else if (!out && !hostDragging)
        dragOut = false;
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

QString
GuestTools::versionString(unsigned int version)
{
    if (version <= 1)
        return QStringLiteral("1.0");
    return QStringLiteral("%1.%2.%3").arg(version >> 24).arg((version >> 16) & 0xff).arg(version & 0xffff);
}

/* The Tools menu item says which tools the guest runs, and whether the
   built-in ones are newer. */
void
GuestTools::updateInstallAction(QAction *action, unsigned int version)
{
    const QString builtIn = QStringLiteral(GT_TOOLS_VERSION_STR);

    if ((version != 0) && (version < GT_TOOLS_VERSION)) {
        action->setText(tr("Update 86Box &Guest Tools (%1 \u2192 %2)\u2026").arg(versionString(version), builtIn));
        action->setToolTip(tr("The guest runs 86Box Guest Tools %1; this build has %2. "
                              "Inserts the CD (or floppy) with them.").arg(versionString(version), builtIn));
    } else {
        action->setText(tr("Install 86Box &Guest Tools %1\u2026").arg(builtIn));
        action->setToolTip((version != 0) ? tr("The guest runs 86Box Guest Tools %1. Inserts the CD (or floppy) with %2.")
                                                .arg(versionString(version), builtIn)
                                          : tr("Insert the 86Box Guest Tools %1 CD (or floppy) into the machine").arg(builtIn));
    }
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
