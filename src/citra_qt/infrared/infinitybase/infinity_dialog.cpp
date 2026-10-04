// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "infinity_dialog.h"

#include "common/file_util.h"
#include "core/hle/service/ir/ir_infinity_base.h"
#include "core/hle/service/ir/infinity_crypto.h"

#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

InfinityBaseWindow* InfinityBaseWindow::inst = nullptr;
std::optional<std::tuple<u8, u32>> InfinityBaseWindow::figure_slots[UI_INFINITY_PADS];
QString last_infinity_path;

// ============================================================================
// Disney Infinity 1.0 Figure Lists (Correct IDs from Dolphin)
// ============================================================================

// Characters (go on Round pad)
const std::map<const u32, const std::string> list_infinity_characters = {
    // The Incredibles
    {0x0F4241, "Mr. Incredible"},
    {0x0F424F, "Syndrome"},
    {0x0F4253, "Violet"},
    {0x0F4252, "Dash"},
    {0x0F424B, "Mrs. Incredible"},

    // Monsters University
    {0x0F4242, "Sulley"},
    {0x0F424A, "Mike Wazowski"},
    {0x0F424E, "Randall"},

    // Pirates of the Caribbean
    {0x0F4243, "Captain Jack Sparrow"},
    {0x0F424D, "Davy Jones"},
    {0x0F424C, "Captain Barbossa"},

    // Cars
    {0x0F4246, "Lightning McQueen"},
    {0x0F4247, "Holley Shiftwell"},
    {0x0F4251, "Mater"},
    {0x0F4254, "Francesco Bernoulli"},

    // The Lone Ranger
    {0x0F4244, "Lone Ranger"},
    {0x0F4245, "Tonto"},

    // Toy Story
    {0x0F4248, "Buzz Lightyear"},
    {0x0F4250, "Woody"},
    {0x0F4249, "Jessie"},

    // Wreck-It Ralph
    {0x0F425C, "Wreck-It Ralph"},
    {0x0F425D, "Vanellope"},

    // Tangled
    {0x0F4257, "Rapunzel"},

    // Frozen
    {0x0F4258, "Anna"},
    {0x0F4259, "Elsa"},

    // Nightmare Before Christmas
    {0x0F4256, "Jack Skellington"},

    // Phineas and Ferb
    {0x0F425A, "Phineas"},
    {0x0F425B, "Agent P"},

    // Sorcerer's Apprentice
    {0x0F4255, "Sorcerer's Apprentice Mickey"},
};

// Play Sets (go on Hexagon pad)
const std::map<const u32, const std::string> list_infinity_playsets = {
    {0x1E8481, "Incredibles/Pirates/Monsters Play Set"},
    {0x1E8482, "The Lone Ranger Play Set"},
    {0x1E8483, "Cars Play Set"},
    {0x1E8484, "Toy Story in Space Play Set"},
};

// Power Discs (go on Hexagon pad)
const std::map<const u32, const std::string> list_infinity_power_discs = {
    // Abilities
    {0x2DC6C3, "Bolt's Super Strength"},
    {0x2DC6C4, "Ralph's Power of Destruction"},
    {0x2DC6C5, "Chernabog's Power"},
    {0x2DC6C6, "C.H.R.O.M.E. Damage Increaser"},
    {0x2DC6C7, "Dr. Doofenshmirtz's Damage-Inator"},
    {0x2DC6C8, "Electro-Charge"},
    {0x2DC6C9, "Fix-It Felix's Repair Power"},
    {0x2DC6CA, "Rapunzel's Healing"},
    {0x2DC6CB, "C.H.R.O.M.E. Armor Shield"},
    {0x2DC6CC, "Star Command Shield"},
    {0x2DC6CD, "Violet's Force Field"},
    {0x2DC6CE, "Pieces of Eight"},
    {0x2DC6CF, "Scrooge McDuck's Lucky Dime"},
    {0x2DC6D1, "Mickey's Sorcerer Hat"},

    // Vehicles
    {0x3D0912, "Mickey's Car"},
    {0x3D0913, "Cinderella's Coach"},
    {0x3D0914, "Electric Mayhem Bus"},
    {0x3D0915, "Cruella De Vil's Car"},
    {0x3D0916, "Pizza Planet Delivery Truck"},
    {0x3D0917, "Mike's New Car"},

    // Aircraft
    {0x3D091A, "Jolly Roger"},
    {0x3D091B, "Dumbo"},
    {0x3D091C, "Calico Helicopter"},

    // Mounts
    {0x3D091D, "Maximus"},
    {0x3D091E, "Angus"},
    {0x3D091F, "Abu the Elephant"},
    {0x3D0920, "Headless Horseman's Horse"},
    {0x3D0921, "Phillipe"},
    {0x3D0922, "Khan"},
    {0x3D0923, "Tantor"},

    // Weapons/Tools
    {0x3D0924, "Dragon Firework Cannon"},
    {0x3D0925, "Stitch's Blaster"},
    {0x3D0926, "Toy Story Mania Blaster"},
    {0x3D0927, "Flamingo Croquet Mallet"},
    {0x3D0928, "Carl Fredricksen's Cane"},
    {0x3D0929, "Hangin' Ten Stitch with Surfboard"},

    // Terrain/Skydomes
    {0x3D0933, "Frozen Flourish"},
    {0x3D0934, "Rapunzel's Kingdom"},
    {0x3D0937, "Sugar Rush Sky"},
    {0x3D093C, "Chill in the Air"},
    {0x3D093D, "Tangled's Floating Lanterns"},
    {0x3D0942, "Nemo's Seascape"},
    {0x3D0943, "Alice's Wonderland"},
    {0x3D0944, "Tulgey Wood"},
};

// Combined list for Hexagon pad (Play Sets + Power Discs)
const std::map<const u32, const std::string>& get_hexagon_figures() {
    static std::map<const u32, const std::string> combined;
    if (combined.empty()) {
        combined.insert(list_infinity_playsets.begin(), list_infinity_playsets.end());
        combined.insert(list_infinity_power_discs.begin(), list_infinity_power_discs.end());
    }
    return combined;
}

// ============================================================================
// Helper Functions
// ============================================================================

// Get the appropriate figure list based on pad type
const std::map<const u32, const std::string>& get_figure_list_for_pad(u8 pad_type) {
    if (pad_type == INFINITY_PAD_ROUND) {
        return list_infinity_characters;
    } else {
        return get_hexagon_figures();
    }
}

// ============================================================================
// CreateInfinityFigureDialog Implementation
// ============================================================================

CreateInfinityFigureDialog::CreateInfinityFigureDialog(QWidget* parent, u8 pad_type)
    : QDialog(parent) {
    
    const bool is_character = (pad_type == INFINITY_PAD_ROUND);
    setWindowTitle(is_character ? tr("Create Disney Infinity Character") 
                                : tr("Create Disney Infinity Play Set / Power Disc"));
    setMinimumSize(QSize(500, 150));

    QVBoxLayout* vbox = new QVBoxLayout();

    // Get the appropriate list based on pad type
    const auto& figure_list = get_figure_list_for_pad(pad_type);

    QComboBox* combo_figlist = new QComboBox();
    QStringList figure_names;
    for (const auto& [id, name] : figure_list) {
        figure_names.append(QString::fromStdString(name));
    }
    combo_figlist->addItems(figure_names);
    combo_figlist->setEditable(true);
    combo_figlist->setInsertPolicy(QComboBox::NoInsert);

    QCompleter* co_compl = new QCompleter(figure_names, this);
    co_compl->setCaseSensitivity(Qt::CaseSensitivity::CaseInsensitive);
    co_compl->setCompletionMode(QCompleter::PopupCompletion);
    co_compl->setFilterMode(Qt::MatchContains);
    combo_figlist->setCompleter(co_compl);

    vbox->addWidget(combo_figlist);

    QHBoxLayout* hbox_buttons = new QHBoxLayout();
    QPushButton* btn_create = new QPushButton(tr("Create"));
    QPushButton* btn_cancel = new QPushButton(tr("Cancel"));
    hbox_buttons->addWidget(btn_create);
    hbox_buttons->addWidget(btn_cancel);
    vbox->addLayout(hbox_buttons);

    setLayout(vbox);

    connect(btn_create, &QAbstractButton::clicked, this, [=, this]() {
        const QString selected_name = combo_figlist->currentText();
        u32 fig_id = 0;

        // Search in the appropriate list
        for (const auto& [id, name] : figure_list) {
            if (QString::fromStdString(name) == selected_name) {
                fig_id = id;
                break;
            }
        }

        if (fig_id == 0) {
            QMessageBox::warning(this, tr("Unknown Figure"),
                                 tr("Please select a valid Disney Infinity figure."),
                                 QMessageBox::Ok);
            return;
        }

        QString sanitized_name = selected_name;
        sanitized_name.replace(QStringLiteral(" "), QStringLiteral("_"));
        QString predef_name = last_infinity_path + sanitized_name + QStringLiteral(".bin");

        file_path = QFileDialog::getSaveFileName(
            this, tr("Create Disney Infinity Figure File"), predef_name,
            tr("Disney Infinity Figure (*.bin);;All Files (*)"));
            
        if (file_path.isEmpty()) {
            return;
        }

        // Create figure with proper encryption
        if (!InfinityCrypto::CreateFigure(file_path.toStdString(), fig_id)) {
            QMessageBox::warning(this, tr("Failed to create figure file!"),
                                 tr("Failed to create figure file:\n%1").arg(file_path),
                                 QMessageBox::Ok);
            return;
        }

        last_infinity_path = QFileInfo(file_path).absolutePath() + QStringLiteral("/");
        accept();
    });

    connect(btn_cancel, &QAbstractButton::clicked, this, &QDialog::reject);

    connect(co_compl, QOverload<const QString&>::of(&QCompleter::activated),
            [=](const QString& text) {
                combo_figlist->setCurrentText(text);
                combo_figlist->setCurrentIndex(combo_figlist->findText(text));
            });
}

QString CreateInfinityFigureDialog::get_file_path() const {
    return file_path;
}

// ============================================================================
// InfinityBaseWindow Implementation
// ============================================================================

InfinityBaseWindow::InfinityBaseWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Disney Infinity Base Manager"));
    setObjectName("infinity_manager");
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(QSize(500, 200));

    QVBoxLayout* vbox_panel = new QVBoxLayout();

    // Helper lambda to add separator line
    auto add_line = [](QVBoxLayout* vbox) {
        QFrame* line = new QFrame();
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        vbox->addWidget(line);
    };

    // Array of pad names for UI
    const char* pad_names[UI_INFINITY_PADS] = {
        "Hexagon Pad (Play Sets / Power Discs)",
        "Round Pad (Characters)"
    };

    // Create UI for each pad
    for (u8 i = 0; i < UI_INFINITY_PADS; i++) {
        QGroupBox* group_box = new QGroupBox(tr(pad_names[i]));
        QVBoxLayout* group_layout = new QVBoxLayout();

        // Figure display field (read-only)
        edit_figures[i] = new QLineEdit();
        edit_figures[i]->setReadOnly(true);
        edit_figures[i]->setText(tr("None"));
        group_layout->addWidget(edit_figures[i]);

        // Buttons
        QHBoxLayout* hbox_buttons = new QHBoxLayout();

        QPushButton* btn_clear = new QPushButton(tr("Clear"));
        QPushButton* btn_create = new QPushButton(tr("Create"));
        QPushButton* btn_load = new QPushButton(tr("Load"));

        hbox_buttons->addWidget(btn_clear);
        hbox_buttons->addWidget(btn_create);
        hbox_buttons->addWidget(btn_load);
        group_layout->addLayout(hbox_buttons);

        group_box->setLayout(group_layout);
        vbox_panel->addWidget(group_box);

        // Connect buttons
        connect(btn_clear, &QAbstractButton::clicked, this, [this, i]() { clear_figure(i); });
        connect(btn_create, &QAbstractButton::clicked, this, [this, i]() { create_figure(i); });
        connect(btn_load, &QAbstractButton::clicked, this, [this, i]() { load_figure(i); });

        if (i < UI_INFINITY_PADS - 1) {
            add_line(vbox_panel);
        }
    }

    setLayout(vbox_panel);

    // Initialize display
    update_edits();
}

InfinityBaseWindow::~InfinityBaseWindow() {
    inst = nullptr;
}

InfinityBaseWindow* InfinityBaseWindow::get_dlg(QWidget* parent) {
    if (inst == nullptr) {
        inst = new InfinityBaseWindow(parent);
    }
    return inst;
}

void InfinityBaseWindow::clear_figure(u8 pad) {
    if (pad >= UI_INFINITY_PADS) {
        return;
    }

    // Convert pad index to pad value (0 = Hexagon = 0x01, 1 = Round = 0x02)
    u8 pad_value = (pad == INFINITY_PAD_HEXAGON) ? 0x01 : 0x02;

    // Remove figure from the base emulator
    Service::IR::g_infinity_base.RemoveFigure(pad_value);
    figure_slots[pad] = std::nullopt;

    update_edits();
}

void InfinityBaseWindow::create_figure(u8 pad) {
    if (pad >= UI_INFINITY_PADS) {
        return;
    }

    CreateInfinityFigureDialog create_dlg(this, pad);
    if (create_dlg.exec() == QDialog::Accepted) {
        load_figure_path(pad, create_dlg.get_file_path());
    }
}

void InfinityBaseWindow::load_figure(u8 pad) {
    if (pad >= UI_INFINITY_PADS) {
        return;
    }

    const QString pad_name = (pad == INFINITY_PAD_HEXAGON) 
        ? tr("Play Set / Power Disc") 
        : tr("Character");

    const QString file_path = QFileDialog::getOpenFileName(
        this, tr("Select Disney Infinity %1 File").arg(pad_name), last_infinity_path,
        tr("Disney Infinity Figure (*.bin);;All Files (*)"));

    if (file_path.isEmpty()) {
        return;
    }

    last_infinity_path = QFileInfo(file_path).absolutePath() + QStringLiteral("/");
    load_figure_path(pad, file_path);
}

void InfinityBaseWindow::load_figure_path(u8 pad, const QString& path) {
    if (pad >= UI_INFINITY_PADS) {
        return;
    }

    FileUtil::IOFile fig_file(path.toStdString(), "rb+");
    if (!fig_file) {
        QMessageBox::warning(
            this, tr("Failed to open figure file!"),
            tr("Failed to open the figure file(%1)!\nFile may not exist or is not accessible.")
                .arg(path),
            QMessageBox::Ok);
        return;
    }

    // Disney Infinity figures are 320 bytes (NTAG213)
    std::array<u8, 320> data{};
    if (fig_file.ReadBytes(data.data(), data.size()) != data.size()) {
        QMessageBox::warning(
            this, tr("Failed to read figure file!"),
            tr("Failed to read the figure file(%1)!\nFile was too small (need at least 320 bytes).")
                .arg(path),
            QMessageBox::Ok);
        return;
    }

    clear_figure(pad);

    // Decrypt figure ID from file data
    u32 fig_id = InfinityCrypto::DecryptFigureId(data);

    // Convert pad index to pad value (0 = Hexagon = 0x01, 1 = Round = 0x02)
    u8 pad_value = (pad == INFINITY_PAD_HEXAGON) ? 0x01 : 0x02;

    // Load figure into the base emulator
    Service::IR::g_infinity_base.SetFigure(pad_value, data, path.toStdString());
    figure_slots[pad] = std::tuple(pad, fig_id);

    update_edits();
}

void InfinityBaseWindow::update_edits() {
    for (u8 i = 0; i < UI_INFINITY_PADS; i++) {
        QString display_string;
        if (auto slot = figure_slots[i]) {
            auto [pad, fig_id] = slot.value();
            
            // Search in appropriate list based on pad type
            const auto& fig_list = get_figure_list_for_pad(i);
            auto found_fig = fig_list.find(fig_id);
            
            if (found_fig != fig_list.end()) {
                display_string = QString::fromStdString(found_fig->second);
            } else {
                display_string = QString(tr("Unknown (ID: 0x%1)")).arg(fig_id, 6, 16, QLatin1Char('0'));
            }
        } else {
            display_string = tr("None");
        }

        edit_figures[i]->setText(display_string);
    }
}
