// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Transport.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

inline bool ConfigureDirectLink(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Join Wii U/3DS LAN link (Internet optional)"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(QObject::tr("For home LAN, enter the Cemu PC's LAN IPv4, UDP port and shared passphrase. No router forwarding is needed.\n"
        "Optional Internet play needs the host's reachable public IPv4/UDP port. No VPN, room server, router change or Pretendo change is performed by this dialog.\n"
        "Configure before starting the game. The passphrase is session-only and is not saved."), &dialog);
    help->setWordWrap(true); layout->addWidget(help);
    auto* fields = new QFormLayout();
    auto* address = new QLineEdit(&dialog);
    address->setPlaceholderText(QObject::tr("Host's numeric IPv4 address"));
    auto* port = new QSpinBox(&dialog); port->setRange(1024, 65535); port->setValue(44660);
    auto* phrase = new QLineEdit(&dialog); phrase->setEchoMode(QLineEdit::Password); phrase->setMaxLength(256);
    fields->addRow(QObject::tr("Host IPv4"), address); fields->addRow(QObject::tr("UDP port"), port);
    fields->addRow(QObject::tr("Shared passphrase"), phrase); layout->addLayout(fields);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Join"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons); dialog.resize(560, dialog.sizeHint().height());
    while (dialog.exec() == QDialog::Accepted) {
        if (WiiU3DSLink::GetTransport().StartDirect(WiiU3DSLink::Role::Nintendo3DS,
                address->text().trimmed().toStdString(), static_cast<std::uint16_t>(port->value()),
                phrase->text().toUtf8().toStdString())) {
            phrase->clear(); return true;
        }
        QMessageBox::warning(&dialog, QObject::tr("Direct link"),
            QString::fromStdString(WiiU3DSLink::DescribeStatus(WiiU3DSLink::GetTransport().GetStatus())));
    }
    phrase->clear(); return false;
}
