// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>

#include <QDialog>
#include <QLineEdit>

#include "common/common_types.h"

// Disney Infinity has 2 pads: Hexagon (Play Sets/Power Discs) and Round (Characters)
constexpr auto UI_INFINITY_PADS = 2;

// Pad indices for UI
constexpr u8 INFINITY_PAD_HEXAGON = 0;  // Play Sets, Power Discs
constexpr u8 INFINITY_PAD_ROUND = 1;    // Characters

class CreateInfinityFigureDialog : public QDialog {
    Q_OBJECT

public:
    explicit CreateInfinityFigureDialog(QWidget* parent, u8 pad_type);
    QString get_file_path() const;

protected:
    QString file_path;
};

class InfinityBaseWindow : public QDialog {
    Q_OBJECT

public:
    explicit InfinityBaseWindow(QWidget* parent);
    ~InfinityBaseWindow() override;
    static InfinityBaseWindow* get_dlg(QWidget* parent);

    InfinityBaseWindow(InfinityBaseWindow const&) = delete;
    void operator=(InfinityBaseWindow const&) = delete;

protected:
    void clear_figure(u8 pad);
    void create_figure(u8 pad);
    void load_figure(u8 pad);
    void load_figure_path(u8 pad, const QString& path);

    void update_edits();

protected:
    QLineEdit* edit_figures[UI_INFINITY_PADS]{};
    static std::optional<std::tuple<u8, u32>> figure_slots[UI_INFINITY_PADS];  // pad, figure_id

private:
    static InfinityBaseWindow* inst;
};
