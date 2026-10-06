/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The Roland Sound Canvas MIDI device's windows, see
 *             qt_soundcanvas.hpp. The front panel is 88emuPlayer's
 *             (gearmulator, GPLv3): its artwork without the playlist, and
 *             the controls, lamps and key bindings of its skin
 *             (emu88Player.rml/.rcss, Emu88EditorBindings.h), laid out in
 *             the skin's 612 x 187 dp.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include "qt_soundcanvas.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QScreen>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

extern "C" {
#include <86box/86box.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/device.h>
#include <86box/midi.h>
#include <86box/mem.h>
#include <86box/path.h>
#include <86box/rom.h>
}

#include "emu88_host.h"

static void
init_resources()
{
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(soundcanvas);
        done = true;
    }
}

/* ---- Configuration ------------------------------------------------------ */

static const char *
config_section()
{
    static device_context_t ctx;
    device_set_context(&ctx, &soundcanvas_device, 0);
    return ctx.name;
}

static QString
config_string(const char *name, const char *def)
{
    return QString::fromUtf8(config_get_string(const_cast<char *>(config_section()), const_cast<char *>(name), const_cast<char *>(def)));
}

static int
config_int(const char *name, int def)
{
    return config_get_int(const_cast<char *>(config_section()), const_cast<char *>(name), def);
}

static void
config_put_int(const char *name, int val)
{
    config_set_int(const_cast<char *>(config_section()), const_cast<char *>(name), val);
}

/* A panel lamp, lit in colour or dark. */
static QIcon
led_icon(const QColor &colour, bool lit)
{
    const int dpr = 2, size = 14;
    QPixmap   pm(size * dpr, size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF    r(1.5, 1.5, size - 3, size - 3);
    QRadialGradient g(r.center() - QPointF(1.5, 1.5), r.width() * 0.75);
    if (lit) {
        g.setColorAt(0.0, colour.lighter(170));
        g.setColorAt(0.5, colour);
        g.setColorAt(1.0, colour.darker(160));
    } else {
        g.setColorAt(0.0, QColor(0x70, 0x70, 0x70));
        g.setColorAt(1.0, QColor(0x30, 0x30, 0x30));
    }
    p.setBrush(g);
    p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1.0));
    p.drawEllipse(r);
    return QIcon(pm);
}

static QString
size_text(uint32_t size)
{
    if (size >= (1u << 20) && !(size & ((1u << 20) - 1)))
        return QString("%1 MiB").arg(size >> 20);
    return QString("%1 KiB").arg(size >> 10);
}

SoundCanvasConfigDialog::SoundCanvasConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Roland Sound Canvas Device Configuration"));

    auto *layout = new QVBoxLayout(this);
    auto *lists  = new QHBoxLayout;
    layout->addLayout(lists, 1);

    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(tr("Synthesizer:")));
    models = new QListWidget;
    models->setMinimumWidth(220);
    left->addWidget(models, 1);
    lists->addLayout(left);

    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(tr("ROM images:")));
    roms = new QTreeWidget;
    roms->setRootIsDecorated(false);
    roms->setHeaderLabels({ tr("ROM"), tr("File in use"), tr("MD5"), tr("Accepted names"), tr("Size") });
    roms->setMinimumWidth(900);
    roms->header()->setStretchLastSection(false);
    roms->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    right->addWidget(roms, 1);
    status = new QLabel;
    status->setWordWrap(true);
    right->addWidget(status);
    lists->addLayout(right, 1);

    auto *legend = new QLabel;
    legend->setText(tr("Green: found, a known dump. Yellow: found by name and size, but not a known dump. "
                       "Red: missing. Unlit: one of a pair of chips that can stand in for a missing image."));
    legend->setWordWrap(true);
    layout->addWidget(legend);

    auto *folderRow = new QHBoxLayout;
    folder          = new QLabel;
    folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folderRow->addWidget(folder, 1);
    auto *open = new QPushButton(tr("Open folder"));
    folderRow->addWidget(open);
    auto *again = new QPushButton(tr("Rescan"));
    folderRow->addWidget(again);
    layout->addLayout(folderRow);

    auto *options = new QHBoxLayout;
    factoryReset  = new QCheckBox(tr("Factory reset at power-on"));
    factoryReset->setToolTip(tr("Run the firmware's factory initialization on a fresh board, as a unit with a charged backup battery would be set up."));
    fastBoot = new QCheckBox(tr("Fast boot (skip the intro)"));
    fastBoot->setToolTip(tr("Run the board past its power-on intro before the machine starts."));
    options->addWidget(factoryReset);
    options->addWidget(fastBoot);
    options->addStretch(1);
    layout->addLayout(options);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    okButton      = buttons->button(QDialogButtonBox::Ok);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(models, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            showModel(models->item(row)->data(Qt::UserRole).toInt());
    });
    connect(again, &QPushButton::clicked, this, &SoundCanvasConfigDialog::rescan);
    connect(open, &QPushButton::clicked, this, [] {
        char path[1024];
        path_append_filename(path, rom_paths.path, "soundcanvas");
        QDir().mkpath(QString::fromUtf8(path));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(path)));
    });

    char path[1024];
    path_append_filename(path, rom_paths.path, "soundcanvas");
    folder->setText(tr("ROM folder: %1").arg(QDir::toNativeSeparators(QString::fromUtf8(path))));

    factoryReset->setChecked(config_int("factory_reset", 1));
    fastBoot->setChecked(config_int("fast_boot", 0));

    QApplication::setOverrideCursor(Qt::WaitCursor);
    soundcanvas_set_rom_dirs();
    QApplication::restoreOverrideCursor();
    fillModels(soundcanvas_config_model(config_string("model", "sc55mk2").toUtf8().constData()));
}

void
SoundCanvasConfigDialog::fillModels(int select)
{
    models->clear();
    for (int i = 0; i < emu88h_model_count(); i++) {
        const int model = emu88h_model_at(i);
        const bool ok   = emu88h_model_available(model);
        auto *item      = new QListWidgetItem(ok ? QString::fromUtf8(emu88h_model_name(model))
                                                 : tr("%1 (unavailable)").arg(QString::fromUtf8(emu88h_model_name(model))));
        item->setData(Qt::UserRole, model);
        item->setIcon(led_icon(QColor(0x30, 0xd0, 0x40), ok));
        if (!ok)
            item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        models->addItem(item);
        if (model == select)
            models->setCurrentItem(item);
    }
    if (!models->currentItem() && models->count())
        models->setCurrentRow(0);
}

void
SoundCanvasConfigDialog::showModel(int model)
{
    emu88h_rom_t list[48];
    const int    n = std::min(emu88h_model_roms(model, list, 48), 48);

    roms->clear();
    for (int i = 0; i < n; i++) {
        const auto &r    = list[i];
        auto       *item = new QTreeWidgetItem(roms);
        item->setText(0, QString::fromUtf8(r.label));
        switch (r.status) {
            case EMU88H_ROM_OK:
                item->setIcon(0, led_icon(QColor(0x30, 0xd0, 0x40), true));
                item->setText(1, QString::fromUtf8(r.file));
                break;
            case EMU88H_ROM_UNVERIFIED:
                item->setIcon(0, led_icon(QColor(0xf0, 0xc0, 0x20), true));
                item->setText(1, tr("%1 (not a known dump)").arg(QString::fromUtf8(r.file)));
                break;
            case EMU88H_ROM_DERIVED:
                item->setIcon(0, led_icon(QColor(0x30, 0xd0, 0x40), true));
                item->setText(1, tr("(supplied by another image)"));
                break;
            case EMU88H_ROM_ALTERNATIVE:
                item->setIcon(0, led_icon(Qt::gray, false));
                item->setText(1, tr("(optional: a chip of the image above)"));
                break;
            default:
                item->setIcon(0, led_icon(QColor(0xe0, 0x20, 0x20), true));
                item->setText(1, tr("missing"));
                break;
        }

        /* The checksum: the file's own, or for an image not found the known dumps' (all of them in
           the tooltip, as a board's firmware revisions each have one). */
        const QStringList known = QString::fromUtf8(r.known).split('\n', Qt::SkipEmptyParts);
        QString           md5   = QString::fromUtf8(r.md5);
        if (md5.isEmpty() && !known.isEmpty()) {
            md5 = known.first().section(' ', 1, 1);
            if (known.size() > 1)
                md5 += tr(" (or %n other(s))", nullptr, known.size() - 1);
            item->setForeground(2, palette().color(QPalette::Disabled, QPalette::Text));
        }
        item->setText(2, md5);
        item->setFont(2, QFontDatabase::systemFont(QFontDatabase::FixedFont));
        item->setToolTip(2, known.isEmpty() ? tr("No reference checksum is registered for this image: its name and size identify it.")
                                            : tr("Known dumps:\n%1").arg(known.join('\n')));
        item->setText(3, QString::fromUtf8(r.names));
        item->setText(4, size_text(r.size));
    }

    const bool ok = emu88h_model_available(model);
    char       note[512];
    emu88h_model_note(model, note, sizeof(note));
    if (ok)
        status->setText(tr("All ROM images found: the %1 is ready.").arg(QString::fromUtf8(emu88h_model_name(model))));
    else if (note[0])
        status->setText(QString::fromUtf8(note));
    else
        status->setText(tr("Unavailable: put the missing images in the ROM folder (any file name; known dumps are recognized by their contents), then Rescan."));
    okButton->setEnabled(ok);
}

void
SoundCanvasConfigDialog::rescan()
{
    const int model = models->currentItem() ? models->currentItem()->data(Qt::UserRole).toInt() : -1;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    emu88h_rescan();
    QApplication::restoreOverrideCursor();
    fillModels(model);
}

bool
SoundCanvasConfigDialog::configure(QWidget *parent)
{
    SoundCanvasConfigDialog dialog(parent);
    if (dialog.exec() != QDialog::Accepted || !dialog.models->currentItem())
        return false;

    const QByteArray key     = emu88h_model_key(dialog.models->currentItem()->data(Qt::UserRole).toInt());
    bool             changed = false;
    if (config_string("model", "sc55mk2") != QString::fromUtf8(key)) {
        config_set_string(const_cast<char *>(config_section()), const_cast<char *>("model"), const_cast<char *>(key.constData()));
        changed = true;
    }
    if (config_int("factory_reset", 1) != (dialog.factoryReset->isChecked() ? 1 : 0)) {
        config_put_int("factory_reset", dialog.factoryReset->isChecked() ? 1 : 0);
        changed = true;
    }
    if (config_int("fast_boot", 0) != (dialog.fastBoot->isChecked() ? 1 : 0)) {
        config_put_int("fast_boot", dialog.fastBoot->isChecked() ? 1 : 0);
        changed = true;
    }
    return changed;
}

/* ---- The front panel ------------------------------------------------------ */

static constexpr double PANEL_W = 612.0;
static constexpr double PANEL_H = 187.0;

enum Face {
    FACE_NONE,
    FACE_ALL,          /* SC-88 mode switches: a lit face when their lamp is on */
    FACE_ROUND,
    FACE_PREVIEW,
    FACE_PLAIN,
    FACE_LEFT,
    FACE_RIGHT,
    FACE_PART_LEFT,
    FACE_PART_RIGHT,
    FACE_8850,         /* SC-8850 switches */
    FACE_8850_LED,
    FACE_8850_PREVIEW,
    LAMP_SELECT,       /* lamps without a switch */
    LAMP_EFX,
    LAMP_CM_MIDI
};

struct SoundCanvasPanel::Control {
    QRectF  rect; /* dp */
    int     face;
    int     bit;  /* switch, -1 for a lamp */
    int     led;  /* lamp bit, -1 for none */
    QString label;
};

/* The faces, cropped from 88emu's sprites. */
struct Sprites {
    QPixmap all, round, ledOn, preview, plain, left, right, partLeft, partRight;
    QPixmap selOff, selOn, efxOff, efxOn, efxGreen, efxAmber, cmLed;
    QPixmap b8850, b8850LedOff, b8850LedOn, b8850Preview;
    QPixmap knob, valueKnob;

    Sprites()
    {
        const QString p = ":/soundcanvas/";
        all       = QPixmap(p + "button_all.png");
        round     = QPixmap(p + "button_round.png");
        ledOn     = QPixmap(p + "button_led_on.png");
        preview   = QPixmap(p + "preview.png");
        plain     = QPixmap(p + "button_plain.png");
        left      = QPixmap(p + "button_left.png");
        right     = QPixmap(p + "button_right.png");
        partLeft  = QPixmap(p + "button_part_left.png");
        partRight = QPixmap(p + "button_part_right.png");
        selOff    = QPixmap(p + "selector_led_off.png");
        selOn     = QPixmap(p + "selector_led_on.png");
        efxOff    = QPixmap(p + "efx_led_off.png");
        efxOn     = QPixmap(p + "efx_led_on.png");
        efxGreen  = QPixmap(p + "efx_led_green.png");
        efxAmber  = QPixmap(p + "efx_led_amber.png");
        cmLed     = QPixmap(p + "cm_led_on.png");
        const QPixmap sheet(p + "sc8850_assets.png");
        b8850        = sheet.copy(108, 66, 50, 51);
        b8850LedOff  = sheet.copy(14, 159, 51, 49);
        b8850LedOn   = sheet.copy(96, 149, 63, 64);
        b8850Preview = sheet.copy(0, 54, 78, 79);
        knob         = QPixmap(p + "knob.png");
        valueKnob    = QPixmap(p + "value_knob.png");
    }
};

static void
blit(QPainter &p, const QRectF &target, const QPixmap &pm)
{
    p.drawPixmap(target, pm, QRectF(pm.rect()));
}

static const Sprites &
sprites()
{
    static Sprites s;
    return s;
}

/* Keys, 88emuPlayer's (Emu88EditorBindings.h). */
struct KeyBinding {
    int key;
    int bit;
};

static const KeyBinding sc88_keys[] = {
    { Qt::Key_W, 6 }, { Qt::Key_E, 5 }, { Qt::Key_1, 2 }, { Qt::Key_2, 1 }, { Qt::Key_3, 24 }, { Qt::Key_4, 25 },
    { Qt::Key_Tab, 7 }, { Qt::Key_R, 22 }, { Qt::Key_T, 14 }, { Qt::Key_Y, 3 }, { Qt::Key_U, 4 },
    { Qt::Key_I, 16 }, { Qt::Key_O, 17 }, { Qt::Key_P, 20 }, { Qt::Key_BracketLeft, 21 },
    { Qt::Key_A, 8 }, { Qt::Key_S, 9 }, { Qt::Key_D, 12 }, { Qt::Key_F, 13 }, { Qt::Key_G, 18 }, { Qt::Key_H, 19 },
    { Qt::Key_J, 10 }, { Qt::Key_K, 11 }, { Qt::Key_Z, 26 }, { Qt::Key_X, 27 }, { Qt::Key_C, 28 }, { Qt::Key_V, 29 },
    { Qt::Key_B, 30 }, { Qt::Key_N, 31 }
};

static const KeyBinding sc8850_keys[] = {
    { Qt::Key_E, 0 }, { Qt::Key_Left, 1 }, { Qt::Key_Right, 2 }, { Qt::Key_D, 3 }, { Qt::Key_V, 4 }, { Qt::Key_N, 5 },
    { Qt::Key_X, 6 }, { Qt::Key_Backspace, 7 }, { Qt::Key_Return, 8 }, { Qt::Key_Enter, 8 }, { Qt::Key_Shift, 9 },
    { Qt::Key_S, 10 }, { Qt::Key_M, 11 }, { Qt::Key_Minus, 12 }, { Qt::Key_Equal, 13 }, { Qt::Key_Plus, 13 },
    { Qt::Key_F1, 14 }, { Qt::Key_F2, 15 }, { Qt::Key_F3, 16 }, { Qt::Key_F4, 17 }, { Qt::Key_I, 18 },
    { Qt::Key_Space, 19 }, { Qt::Key_P, 20 }
};

static const KeyBinding la_keys[] = {
    { Qt::Key_1, 0 }, { Qt::Key_2, 1 }, { Qt::Key_3, 2 }, { Qt::Key_4, 8 }, { Qt::Key_5, 9 }, { Qt::Key_R, 10 },
    { Qt::Key_G, 3 }, { Qt::Key_S, 11 }, { Qt::Key_V, 4 }, { Qt::Key_M, 12 }
};

static bool
la_buttons(int model)
{
    return model == EMU88H_MT32_OLD || model == EMU88H_MT32_NEW || model == EMU88H_CM32L || model == EMU88H_CM32LN || model == EMU88H_CM64;
}

static QRectF
lcd_rect(int model, unsigned screen)
{
    if (screen)
        return QRectF(162, 81, 218, 20);
    switch (model) {
        case EMU88H_SC8850:
            return QRectF(165, 28, 175, 70);
        case EMU88H_CM32P:
        case EMU88H_CM64:
            return QRectF(162, 23, 218, 41);
        case EMU88H_CM32L:
        case EMU88H_CM32LN:
        case EMU88H_MT32_OLD:
        case EMU88H_MT32_NEW:
            return QRectF(162, 34, 218, 20);
        case EMU88H_SC8820:
            return QRectF(164, 28, 180, 71);
        default:
            return QRectF(156, 22, 209, 76);
    }
}

SoundCanvasPanel::SoundCanvasPanel(emu88h *board, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , board_(board)
{
    init_resources();
    model = emu88h_model(board_);
    panel = emu88h_model_panel(model);
    gain  = emu88h_gain(board_);

    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setWindowTitle(tr("Roland Sound Canvas - %1").arg(QString::fromUtf8(emu88h_model_name(model))));

    artwork = QPixmap(QString(":/soundcanvas/%1_panel.png").arg(QString::fromUtf8(emu88h_model_artwork(model))));
    buildControls();

    for (unsigned s = 0; s < 2; s++) {
        int w, h;
        if ((s == 0 || emu88h_model_has_second_lcd(model)) && emu88h_lcd_size(model, s, &w, &h))
            lcd[s] = QImage(w, h, QImage::Format_ARGB32);
    }

    const int width = config_int("panel_width", 1040);
    resize(width, qRound(width * PANEL_H / PANEL_W));
    setMinimumSize(306, 94);
    const int x = config_int("panel_x", INT_MIN), y = config_int("panel_y", INT_MIN);
    if (x != INT_MIN && y != INT_MIN && QGuiApplication::screenAt(QPoint(x + 40, y + 20)))
        move(x, y);
    else if (parent)
        move(parent->frameGeometry().bottomLeft() + QPoint(0, 8));

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &SoundCanvasPanel::poll);
    timer->start(33);
    poll();
}

SoundCanvasPanel::~SoundCanvasPanel()
{
    emu88h_set_buttons(board_, 0);
    emu88h_release(board_);
}

void
SoundCanvasPanel::buildControls()
{
    const uint32_t mask     = emu88h_model_button_mask(model);
    const bool     extended = model == EMU88H_SC88 || model == EMU88H_SC88PRO;
    const auto     add      = [this, mask](double x, double y, double w, double h, int face, int bit, int led, const QString &label) {
        if (bit >= 0 && !(mask & (1u << bit)))
            return;
        controls.push_back({ QRectF(x, y, w, h), face, bit, led, label });
    };

    if (panel == EMU88H_PANEL_SC88) {
        const bool sc55 = !extended && model != EMU88H_SC88VL;
        add(420, 14, 15, 15, FACE_ALL, 6, 0, "ALL (W)");
        add(420, 40, 15, 15, FACE_ROUND, 5, 1, "MUTE (E)");
        if (!sc55) {
            add(420, 66, 15, 15, FACE_ROUND, 2, 2, "SC-55 MAP (1)");
            add(420, 92, 15, 15, FACE_ROUND, 1, 3, model == EMU88H_SC88PRO ? "SC-88 MAP (2)" : "EQ (2)");
            add(116, 78, 20, 20, FACE_PREVIEW, 7, -1, "PREVIEW (Tab)");
        }
        if (extended) {
            add(311, 166, 27, 9, FACE_PLAIN, 24, -1, "USER INST (3)");
            add(339, 166, 27, 9, FACE_PLAIN, 25, -1, "SELECT (4)");
        }
        add(472, 14, 13, 13, FACE_PART_LEFT, 22, -1, "PART left (R)");
        add(500, 14, 13, 13, FACE_PART_RIGHT, 14, -1, "PART right (T)");
        add(538, 17, 27, 9, FACE_LEFT, 3, -1, "INSTRUMENT left (Y)");
        add(566, 17, 27, 9, FACE_RIGHT, 4, -1, "INSTRUMENT right (U)");
        add(465, 43, 27, 9, FACE_LEFT, 20, -1, "LEVEL left (P)");
        add(493, 43, 27, 9, FACE_RIGHT, 21, -1, "LEVEL right ([)");
        add(538, 43, 27, 9, FACE_LEFT, 12, -1, "PAN left (D)");
        add(566, 43, 27, 9, FACE_RIGHT, 13, -1, "PAN right (F)");
        add(465, 69, 27, 9, FACE_LEFT, 18, -1, "REVERB left (G)");
        add(493, 69, 27, 9, FACE_RIGHT, 19, -1, "REVERB right (H)");
        add(538, 69, 27, 9, FACE_LEFT, 10, -1, "CHORUS left (J)");
        add(566, 69, 27, 9, FACE_RIGHT, 11, -1, "CHORUS right (K)");
        add(465, 95, 27, 9, FACE_LEFT, 16, -1, "KEY SHIFT left (I)");
        add(493, 95, 27, 9, FACE_RIGHT, 17, -1, "KEY SHIFT right (O)");
        add(538, 95, 27, 9, FACE_LEFT, 8, -1, "MIDI CH left (A)");
        add(566, 95, 27, 9, FACE_RIGHT, 9, -1, "MIDI CH right (S)");
        if (extended) {
            add(392, 166, 27, 9, FACE_LEFT, 26, -1, "VIB RATE / EFX TYPE left (Z)");
            add(420, 166, 27, 9, FACE_RIGHT, 27, -1, "VIB RATE / EFX TYPE right (X)");
            add(465, 166, 27, 9, FACE_LEFT, 28, -1, "VIB DEPTH / EFX PARAM left (C)");
            add(493, 166, 27, 9, FACE_RIGHT, 29, -1, "VIB DEPTH / EFX PARAM right (V)");
            add(538, 166, 27, 9, FACE_LEFT, 30, -1, "VIB DELAY / EFX VALUE left (B)");
            add(566, 166, 27, 9, FACE_RIGHT, 31, -1, "VIB DELAY / EFX VALUE right (N)");
            add(378.25, 132, 8.3467, 8, LAMP_SELECT, -1, 4, QString());
            add(378.25, 143, 8.3467, 8, LAMP_SELECT, -1, 5, QString());
            add(378.25, 154, 8.3467, 8, LAMP_SELECT, -1, 6, QString());
            add(287.5, 165.5, 9, 9, LAMP_EFX, -1, 7, QString());
        }
    } else if (panel == EMU88H_PANEL_SC8850) {
        add(400, 20, 17, 17, FACE_8850_LED, 0, 0, "EDIT (E)");
        add(439, 20, 17, 17, FACE_8850, 1, -1, "PART left (Left)");
        add(478, 20, 17, 17, FACE_8850, 2, -1, "PART right (Right)");
        add(400, 46, 17, 17, FACE_8850_LED, 3, 1, "DRUM (D)");
        add(439, 46, 17, 17, FACE_8850, 4, -1, "VARIATION (V)");
        add(478, 46, 17, 17, FACE_8850, 5, -1, "INSTRUMENT (N)");
        add(400, 72, 17, 17, FACE_8850_LED, 6, 2, "EFFECTS (X)");
        add(439, 72, 17, 17, FACE_8850, 7, -1, "EXIT (Backspace)");
        add(478, 72, 17, 17, FACE_8850, 8, -1, "ENTER (Return)");
        add(400, 102, 17, 17, FACE_8850_LED, 9, 3, "SHIFT (Shift)");
        add(439, 102, 17, 17, FACE_8850_LED, 10, 4, "SOLO (S)");
        add(478, 102, 17, 17, FACE_8850_LED, 11, 5, "MUTE (M)");
        add(517, 102, 17, 17, FACE_8850, 12, -1, "DEC (-)");
        add(556, 102, 17, 17, FACE_8850, 13, -1, "INC (=)");
        add(178, 149, 17, 17, FACE_8850, 14, -1, "F1");
        add(222, 149, 17, 17, FACE_8850, 15, -1, "F2");
        add(266, 149, 17, 17, FACE_8850, 16, -1, "F3");
        add(310, 149, 17, 17, FACE_8850, 17, -1, "F4");
        add(354, 149, 17, 17, FACE_8850, 18, -1, "INST MAP (I)");
        add(114, 76, 24, 25, FACE_8850_PREVIEW, 20, -1, "PREVIEW (P)");
    } else if (panel == EMU88H_PANEL_CM) {
        controls.push_back({ QRectF(461, 116, 21, 18), LAMP_CM_MIDI, -1, 0, QString() });
    }
}

QRectF
SoundCanvasPanel::toWidget(const QRectF &dp) const
{
    const double s  = std::min(width() / PANEL_W, height() / PANEL_H);
    const double ox = (width() - PANEL_W * s) / 2.0, oy = (height() - PANEL_H * s) / 2.0;
    return QRectF(ox + dp.x() * s, oy + dp.y() * s, dp.width() * s, dp.height() * s);
}

static const QRectF volume_knob(112, 24, 28, 28);
static const QRectF value_knob(517, 17, 76, 76);

const SoundCanvasPanel::Control *
SoundCanvasPanel::controlAt(const QPointF &pos) const
{
    for (const auto &c : controls)
        if (c.bit >= 0 && toWidget(c.rect).adjusted(-1, -1, 1, 1).contains(pos))
            return &c;
    return nullptr;
}

void
SoundCanvasPanel::paintEvent(QPaintEvent *)
{
    const Sprites &sp = sprites();
    QPainter       p(this);
    p.fillRect(rect(), Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.drawPixmap(toWidget(QRectF(0, 0, PANEL_W, PANEL_H)), artwork, artwork.rect());

    /* The displays. */
    const bool on = state == EMU88H_STATE_ON;
    for (unsigned s = 0; s < 2; s++) {
        if (lcd[s].isNull())
            continue;
        const QRectF r = toWidget(lcd_rect(model, s));
        if (state == EMU88H_STATE_OFF || state == EMU88H_STATE_FAILED) {
            p.fillRect(r, QColor(0x27, 0x2c, 0x29));
            if (s == 0) {
                QFont f = font();
                f.setPixelSize(std::max(8, qRound(r.height() * (lcd_rect(model, 0).height() > 30 ? 0.16 : 0.5))));
                p.setFont(f);
                p.setPen(QColor(0x81, 0x87, 0x7f));
                p.drawText(r, Qt::AlignCenter, state == EMU88H_STATE_FAILED ? tr("ROM error") : tr("Power Off"));
            }
            continue;
        }
        if (on && !lcdPowered[s]) { /* standby: the glass has no supply */
            p.fillRect(r, QColor(0x27, 0x2c, 0x29));
            continue;
        }
        if (on)
            p.drawImage(r, lcd[s]);
    }

    /* Lamps and switches. */
    const uint32_t held = pointerButtons | keyboardButtons;
    const bool     pro  = model == EMU88H_SC88PRO || model == EMU88H_VEGSPRO;
    for (const auto &c : controls) {
        const QRectF r   = toWidget(c.rect);
        const bool   lit = c.led >= 0 && (leds & (1u << c.led));
        const bool   dn  = c.bit >= 0 && (held & (1u << c.bit));
        auto         dp  = [this](double x, double y, double w, double h) { return toWidget(QRectF(x, y, w, h)); };
        switch (c.face) {
            case FACE_ALL:
            case FACE_ROUND:
                blit(p, dp(c.rect.x() - 0.5, c.rect.y() - 0.5, 16, 16), lit ? sp.ledOn : (c.face == FACE_ALL ? sp.all : sp.round));
                break;
            case FACE_PREVIEW:
                blit(p, r, sp.preview);
                break;
            case FACE_PLAIN:
                blit(p, r, sp.plain);
                break;
            case FACE_LEFT:
                blit(p, r, sp.left);
                break;
            case FACE_RIGHT:
                blit(p, r, sp.right);
                break;
            case FACE_PART_LEFT:
                blit(p, r, sp.partLeft);
                break;
            case FACE_PART_RIGHT:
                blit(p, r, sp.partRight);
                break;
            case FACE_8850:
                blit(p, dp(c.rect.x() + 2, c.rect.y() + 0.6, 16.667, 17), sp.b8850);
                break;
            case FACE_8850_LED:
                if (lit)
                    blit(p, dp(c.rect.x() - 2, c.rect.y() - 3.133, 21, 21.333), sp.b8850LedOn);
                else
                    blit(p, dp(c.rect.x() + 1.667, c.rect.y() + 1.2, 17, 16.333), sp.b8850LedOff);
                break;
            case FACE_8850_PREVIEW:
                blit(p, dp(c.rect.x() + 0.5, c.rect.y() - 0.3, 26, 26.333), sp.b8850Preview);
                break;
            case LAMP_SELECT:
                blit(p, r, lit ? sp.selOn : sp.selOff);
                break;
            case LAMP_EFX:
                if (pro) {
                    const bool g = leds & 0x80, red = leds & 0x100;
                    blit(p, r, (g && red) ? sp.efxAmber : g ? sp.efxGreen : red ? sp.efxOn : sp.efxOff);
                } else
                    blit(p, r, lit ? sp.efxOn : sp.efxOff);
                break;
            case LAMP_CM_MIDI:
                if (lit)
                    blit(p, r, sp.cmLed);
                break;
            default:
                break;
        }
        if (dn) {
            QPainterPath path;
            const double rad = std::min(r.width(), r.height()) / 2.0;
            path.addRoundedRect(r, rad, rad);
            p.fillPath(path, QColor(0, 0, 0, 90));
        }
    }

    /* The VOLUME knob, and the SC-8850's endless VALUE encoder. */
    const int frame = std::clamp(qRound(gain / 200.0 * 30.0), 0, 30);
    p.drawPixmap(toWidget(volume_knob), sp.knob, QRectF(0, frame * 64, 64, 64));
    if (panel == EMU88H_PANEL_SC8850)
        p.drawPixmap(toWidget(value_knob), sp.valueKnob, QRectF(0, ((valueDetent % 4) + 4) % 4 * 128, 128, 128));
}

void
SoundCanvasPanel::poll()
{
    bool dirty = false;

    const int s = emu88h_state(board_);
    if (s != state) {
        state = s;
        dirty = true;
    }
    const uint32_t l = emu88h_leds(board_);
    if (l != leds) {
        leds  = l;
        dirty = true;
    }
    for (unsigned i = 0; i < 2; i++)
        if (!lcd[i].isNull() && emu88h_lcd_render(board_, i, reinterpret_cast<uint32_t *>(lcd[i].bits()), &lcdRevision[i], &lcdPowered[i]))
            dirty = true;
    if (dirty)
        update();
}

void
SoundCanvasPanel::sendButtons()
{
    const uint32_t b = pointerButtons | keyboardButtons;
    if (b != sentButtons) {
        sentButtons = b;
        emu88h_set_buttons(board_, b);
    }
    update();
}

void
SoundCanvasPanel::togglePower(bool supply)
{
    if (!supply && emu88h_model_power_standby(model) && state == EMU88H_STATE_ON) {
        keyboardButtons |= 1u; /* POWER, a key the firmware reads: held as long as Q is */
        sendButtons();
        return;
    }
    if (state == EMU88H_STATE_OFF || state == EMU88H_STATE_FAILED)
        emu88h_set_power(board_, 1, pointerButtons | keyboardButtons);
    else
        emu88h_set_power(board_, 0, 0);
    poll();
}

void
SoundCanvasPanel::setGain(int g, bool save)
{
    gain = std::clamp(g, 0, 200);
    emu88h_set_gain(board_, gain);
    if (save) {
        config_put_int("output_gain", gain);
        config_save();
    }
    update();
}

void
SoundCanvasPanel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    if (toWidget(volume_knob).adjusted(-4, -4, 4, 4).contains(pos))
        dragKnob = 1;
    else if (panel == EMU88H_PANEL_SC8850 && toWidget(value_knob).contains(pos))
        dragKnob = 2;
    if (dragKnob) {
        dragOrigin = pos;
        dragAccum  = 0.0;
        dragMoved  = false;
        return;
    }
    if (const Control *c = controlAt(pos)) {
        pointerButtons |= 1u << c->bit;
        sendButtons();
    }
}

void
SoundCanvasPanel::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragKnob)
        return;
    const double scale = std::min(width() / PANEL_W, height() / PANEL_H);
    const double dy    = (dragOrigin.y() - event->position().y()) + (event->position().x() - dragOrigin.x());
    dragOrigin         = event->position();
    if (std::abs(dy) > 0.0)
        dragMoved = true;
    if (dragKnob == 1) {
        dragAccum += dy / scale * 2.0; /* 2 % per dp */
        const int step = static_cast<int>(dragAccum);
        if (step) {
            dragAccum -= step;
            setGain(gain + step, false);
        }
    } else {
        dragAccum += dy / scale / 7.0; /* 7 dp per detent, the skin's feel */
        const int step = static_cast<int>(dragAccum);
        if (step) {
            dragAccum -= step;
            valueDetent += step;
            emu88h_turn_encoder(board_, step);
            update();
        }
    }
}

void
SoundCanvasPanel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (dragKnob == 1)
        setGain(gain, true);
    else if (dragKnob == 2 && !dragMoved) {
        /* A click without a turn pushes the encoder, for long enough for the panel scan. */
        pointerButtons |= 1u << 19;
        sendButtons();
        QTimer::singleShot(80, this, [this] {
            pointerButtons &= ~(1u << 19);
            sendButtons();
        });
    }
    dragKnob = 0;
    if (pointerButtons & ~(1u << 19)) {
        pointerButtons &= (1u << 19);
        sendButtons();
    }
}

void
SoundCanvasPanel::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (toWidget(volume_knob).adjusted(-4, -4, 4, 4).contains(event->position()))
        setGain(100, true);
    else
        mousePressEvent(event);
}

void
SoundCanvasPanel::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (!steps)
        return;
    if (toWidget(volume_knob).adjusted(-4, -4, 4, 4).contains(event->position()))
        setGain(gain + steps * 5, true);
    else if (panel == EMU88H_PANEL_SC8850 && toWidget(value_knob).contains(event->position())) {
        valueDetent += steps;
        emu88h_turn_encoder(board_, steps);
        update();
    }
}

int
SoundCanvasPanel::keyButton(const QKeyEvent *event) const
{
    const KeyBinding *table;
    size_t            n;
    if (panel == EMU88H_PANEL_SC8850) {
        table = sc8850_keys;
        n     = std::size(sc8850_keys);
    } else if (la_buttons(model)) {
        table = la_keys;
        n     = std::size(la_keys);
    } else {
        table = sc88_keys;
        n     = std::size(sc88_keys);
    }
    for (size_t i = 0; i < n; i++)
        if (table[i].key == event->key())
            return (emu88h_model_button_mask(model) & (1u << table[i].bit)) ? table[i].bit : -1;
    return -1;
}

void
SoundCanvasPanel::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Q && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        if (!event->isAutoRepeat() && !powerKeyDown)
            togglePower(event->modifiers() & Qt::ShiftModifier);
        powerKeyDown = true;
        return;
    }
    if (emu88h_model_has_knob(model) && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        emu88h_turn_encoder(board_, event->key() == Qt::Key_Up ? 1 : -1);
        return;
    }
    if (event->isAutoRepeat())
        return;
    const int bit = keyButton(event);
    if (bit < 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    keyboardButtons |= 1u << bit;
    sendButtons();
}

void
SoundCanvasPanel::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
        return;
    if (event->key() == Qt::Key_Q) {
        powerKeyDown = false;
        if (emu88h_model_power_standby(model)) {
            keyboardButtons &= ~1u;
            sendButtons();
        }
        return;
    }
    const int bit = keyButton(event);
    if (bit >= 0) {
        keyboardButtons &= ~(1u << bit);
        sendButtons();
    }
}

void
SoundCanvasPanel::focusOutEvent(QFocusEvent *event)
{
    /* Keys released elsewhere never come back here. */
    keyboardButtons = 0;
    powerKeyDown    = false;
    sendButtons();
    QWidget::focusOutEvent(event);
}

bool
SoundCanvasPanel::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto    *help = static_cast<QHelpEvent *>(event);
        const QPointF  pos  = help->pos();
        QString        text;
        if (const Control *c = controlAt(pos))
            text = c->label;
        else if (toWidget(volume_knob).adjusted(-4, -4, 4, 4).contains(pos))
            text = tr("VOLUME %1 % (drag or scroll; double-click for 100 %)").arg(gain);
        else if (panel == EMU88H_PANEL_SC8850 && toWidget(value_knob).contains(pos))
            text = tr("VALUE: drag or scroll to turn, click to push (Space)");
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, this);
        return true;
    }
    if (event->type() == QEvent::Move)
        savePlacement();
    return QWidget::event(event);
}

void
SoundCanvasPanel::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    const bool off = state == EMU88H_STATE_OFF || state == EMU88H_STATE_FAILED;
    menu.addAction(off ? tr("Power &on") : tr("Power &off"), this, [this] { togglePower(true); });
    menu.addAction(tr("&Restart"), this, [this] {
        emu88h_set_power(board_, 0, 0);
        emu88h_set_power(board_, 1, 0);
        poll();
    });
    menu.addSeparator();
    menu.addAction(tr("Default &size"), this, [this] { resize(1040, qRound(1040 * PANEL_H / PANEL_W)); });
    QString keys;
    if (panel == EMU88H_PANEL_SC8850)
        keys = tr("Q power; E EDIT, D DRUM, X EFFECTS, Left/Right PART, V VARIATION, N INSTRUMENT, I INST MAP, "
                  "-/= DEC/INC, Space VALUE push, Backspace EXIT, Return ENTER, Shift SHIFT, S SOLO, M MUTE, P PREVIEW, F1-F4.");
    else if (la_buttons(model))
        keys = tr("Q power; 1-5 PART, R RHYTHM, G SOUND GROUP, S SOUND, V VOLUME, M MASTER VOLUME%1.")
                   .arg(emu88h_model_has_knob(model) ? tr(", Up/Down the VOLUME/VALUE knob") : QString());
    else
        keys = tr("Q power%1; W ALL, E MUTE, R/T PART, Y/U INSTRUMENT, P/[ LEVEL, D/F PAN, G/H REVERB, J/K CHORUS, "
                  "I/O KEY SHIFT, A/S MIDI CH, 1/2 SC-55/SC-88 MAP, Tab PREVIEW, 3/4 USER INST/SELECT, Z X C V B N the bottom row.")
                   .arg(emu88h_model_power_standby(model) ? tr(" (standby; Shift+Q the supply)") : QString());
    auto *help = menu.addAction(tr("Keyboard shortcuts..."));
    connect(help, &QAction::triggered, this, [this, keys] { QToolTip::showText(QCursor::pos(), keys, this); });
    menu.exec(event->globalPos());
}

void
SoundCanvasPanel::savePlacement()
{
    if (!isVisible() || isMinimized())
        return;
    config_put_int("panel_x", pos().x());
    config_put_int("panel_y", pos().y());
    config_put_int("panel_width", width());
}

void
SoundCanvasPanel::resizeEvent(QResizeEvent *event)
{
    savePlacement();
    QWidget::resizeEvent(event);
}

void
SoundCanvasPanel::closeEvent(QCloseEvent *event)
{
    savePlacement();
    config_save();
    QWidget::closeEvent(event);
}

/* ---- Following the device ------------------------------------------------- */

SoundCanvasPanelManager::SoundCanvasPanelManager(QWidget *mainWindow)
    : QObject(mainWindow)
    , mainWindow(mainWindow)
{
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &SoundCanvasPanelManager::poll);
    timer->start(250);
}

void
SoundCanvasPanelManager::showPanel()
{
    if (panel) {
        panel->showNormal();
        panel->raise();
        panel->activateWindow();
    } else if (current) {
        emu88h_retain(current);
        panel = new SoundCanvasPanel(current, mainWindow);
        panel->show();
        panel->activateWindow();
    }
}

QString
SoundCanvasPanelManager::toolTip() const
{
    if (!current)
        return tr("Roland Sound Canvas: not running (are its ROMs in roms/soundcanvas?)");
    QString state;
    switch (emu88h_state(current)) {
        case EMU88H_STATE_BOOTING:
            state = tr(", booting");
            break;
        case EMU88H_STATE_OFF:
            state = tr(", powered off");
            break;
        case EMU88H_STATE_FAILED:
            state = tr(", failed to start");
            break;
        default:
            break;
    }
    return tr("Roland Sound Canvas: %1%2\nClick to show its front panel").arg(QString::fromUtf8(emu88h_model_name(emu88h_model(current))), state);
}

void
SoundCanvasPanelManager::poll()
{
    emu88h *board = static_cast<emu88h *>(soundcanvas_get_board());

    if (panel && panel->board() != board)
        panel->close(); /* the device went, or was replaced */
    if (board != current) {
        /* Holding the board we last saw keeps its address from being reused by the next one. */
        if (current)
            emu88h_release(current);
        current = board;
        if (current)
            emu88h_retain(current);
        pendingOpen = current != nullptr;
        lastState   = -2;
    }
    const int state = current ? emu88h_state(current) : -1;
    if (state != lastState) {
        lastState = state;
        emit changed();
    }
    /* A board that just started shows its panel once; closing it is the user's call then. */
    if (pendingOpen && !panel) {
        pendingOpen = false;
        emu88h_retain(current);
        panel = new SoundCanvasPanel(current, mainWindow);
        panel->show();
    }
    if (board)
        emu88h_release(board);
}
