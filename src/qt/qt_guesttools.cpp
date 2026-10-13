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
#include <QMimeData>
#include <QTimer>

#include <cstdlib>
#include <cstring>

extern "C" {
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

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &GuestTools::poll);
    timer->start(100);
}

GuestTools::~GuestTools()
{
    guest_tools_set_clipboard_handlers(nullptr, nullptr);
    instance = nullptr;
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

/* Pointer integration took over: let go of a mouse captured before it did. */
void
GuestTools::poll()
{
    bool active = guest_tools_pointer_active();

    if (active && !pointerActive && mouse_capture)
        plat_mouse_capture(0);
    pointerActive = active;
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
