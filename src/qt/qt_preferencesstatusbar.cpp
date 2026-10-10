/*
 * 86Box-Next: Preferences > Status bar -- the order of the icon groups at the bottom left of the
 * main window, saved as status_icon_order (see MachineStatus::iconOrder()).
 */
#include "qt_preferencesstatusbar.hpp"
#include "qt_machinestatus.hpp"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstdio>

extern "C" {
#include <86box/86box.h>
}

PreferencesStatusBar::PreferencesStatusBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *icons       = new QGroupBox(tr("Icon order"));
    auto *iconsLayout = new QHBoxLayout(icons);
    iconOrder         = new QListWidget;
    iconOrder->setToolTip(tr("The order of the icons at the bottom left of the window, top to bottom here "
                             "being left to right there. Drag a group or use the buttons to move it."));
    iconOrder->setDragDropMode(QAbstractItemView::InternalMove);
    iconOrder->setDefaultDropAction(Qt::MoveAction);
    iconsLayout->addWidget(iconOrder, 1);

    auto *buttons = new QVBoxLayout;
    auto *up      = new QPushButton(tr("Move &up"));
    auto *down    = new QPushButton(tr("Move &down"));
    auto *reset   = new QPushButton(tr("De&fault order"));
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addStretch(1);
    buttons->addWidget(reset);
    iconsLayout->addLayout(buttons);
    layout->addWidget(icons, 1);

    const auto move = [this](int delta) {
        const int row = iconOrder->currentRow();
        if ((row < 0) || (row + delta < 0) || (row + delta >= iconOrder->count()))
            return;
        iconOrder->insertItem(row + delta, iconOrder->takeItem(row));
        iconOrder->setCurrentRow(row + delta);
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(reset, &QPushButton::clicked, this, [this] { fillIconOrder(MachineStatus::iconGroups()); });
    fillIconOrder(MachineStatus::iconOrder());
}

void
PreferencesStatusBar::fillIconOrder(const QStringList &order)
{
    iconOrder->clear();
    for (const QString &key : order) {
        auto *item = new QListWidgetItem(MachineStatus::iconGroupName(key));
        item->setData(Qt::UserRole, key);
        iconOrder->addItem(item);
    }
    iconOrder->setCurrentRow(0);
}

void
PreferencesStatusBar::save()
{
    /* Kept only when it differs from the default order. */
    QStringList order;
    for (int i = 0; i < iconOrder->count(); i++)
        order.append(iconOrder->item(i)->data(Qt::UserRole).toString());
    const QByteArray text = (order == MachineStatus::iconGroups()) ? QByteArray() : order.join(',').toUtf8();
    snprintf(status_icon_order, sizeof(status_icon_order), "%s", text.constData());
}
