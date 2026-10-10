/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The Roland Sound Canvas MIDI device's windows: its
 *             configuration (which of 88emu's boards, and whether its ROMs
 *             are in roms/soundcanvas), and the board's front panel, shown
 *             while the machine runs.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef QT_SOUNDCANVAS_HPP
#define QT_SOUNDCANVAS_HPP

#include <QDialog>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QWidget>

#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QTimer;
class QTreeWidget;

struct emu88h;

class SoundCanvasConfigDialog : public QDialog {
    Q_OBJECT

public:
    /* Settings > Sound > MIDI Out > Configure. True when something changed. */
    static bool configure(QWidget *parent);

private:
    explicit SoundCanvasConfigDialog(QWidget *parent);

    void fillModels(int select);
    void showModel(int model);
    void rescan();

    QListWidget *models;
    QTreeWidget *roms;
    QLabel      *status;
    QLabel      *folder;
    QCheckBox   *factoryReset;
    QCheckBox   *fastBoot;
    QComboBox   *panelScale;
    QPushButton *okButton;
};

class SoundCanvasPanel : public QWidget {
    Q_OBJECT

public:
    explicit SoundCanvasPanel(emu88h *board, QWidget *parent);
    ~SoundCanvasPanel() override;

    emu88h *board() const { return board_; }

    /* The size the panel opens at: the chosen scale of the skin, or the last size. */
    void resizeToDefault();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Control;

    void     buildControls();
    void     poll();
    void     sendButtons();
    void     togglePower(bool supply);
    void     setGain(int gain, bool save);
    void     savePlacement();
    QRectF   toWidget(const QRectF &dp) const;
    const Control *controlAt(const QPointF &pos) const;
    int      keyButton(const QKeyEvent *event) const;

    emu88h *board_;
    int     model;
    int     panel;
    QPixmap artwork;
    QTimer *timer;

    std::vector<Control> controls;
    uint32_t             pointerButtons  = 0;
    uint32_t             keyboardButtons = 0;
    uint32_t             sentButtons     = ~0u;
    uint32_t             leds            = 0;
    int                  state           = -1;
    bool                 powerKeyDown    = false;

    QImage   lcd[2];
    uint64_t lcdRevision[2] = { ~0ull, ~0ull };
    int      lcdPowered[2]  = { 1, 1 };

    int     gain;
    int     valueDetent = 0;    /* the SC-8850's VALUE encoder, for its knurl */
    int     dragKnob    = 0;    /* 1 volume, 2 VALUE */
    QPointF dragOrigin;
    double  dragAccum   = 0.0;
    bool    dragMoved   = false;
};

/* Opens the panel when a Sound Canvas board starts, closes it when it goes; the status bar's
   piano icon shows it again. */
class SoundCanvasPanelManager : public QObject {
    Q_OBJECT

public:
    explicit SoundCanvasPanelManager(QWidget *mainWindow);

    bool    present() const { return current != nullptr; }
    QString toolTip() const;
    void    showPanel();
    QMenu  *menu(); /* the piano icon's: the panel, its start-up option, Configure */

signals:
    void changed(); /* a board started or went, or its power state changed */

private:
    void poll();

    void configure();

    QWidget                   *mainWindow;
    QMenu                     *menu_ = nullptr;
    QPointer<SoundCanvasPanel> panel;
    emu88h                    *current     = nullptr; /* retained */
    bool                       pendingOpen = false;
    int                        lastState   = -2;
};

#endif
