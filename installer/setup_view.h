#pragma once
#include "ui.h"
#include <array>
#include <commctrl.h>

namespace piu {
enum class SetupAppearance { System, Light, Dark, Contrast };

// Presentation only: the standard Windows controls retain their input and accessibility.
class SetupView {
    struct Theme {
        COLORREF background, panel, text, muted, border, accent, accent_text, selected, hover, danger;
        bool dark = false, contrast = false;
    } theme_{};
    struct Interaction { int id; bool hot = false; };
    HWND window_ = nullptr;
    SetupAppearance appearance_ = SetupAppearance::System;
    UINT dpi_ = 96;
    HFONT body_ = nullptr, heading_ = nullptr, small_ = nullptr, title_ = nullptr;
    HBRUSH background_ = nullptr, panel_ = nullptr;
    RECT input_{}, progress_{};
    int content_height_ = 0, viewport_height_ = 0, scroll_ = 0, progress_value_ = 0;
    bool working_ = false, completed_ = false, failed_ = false, removing_ = false;
    std::array<Interaction, 7> interactions_{{{IDC_SYNC}, {IDC_OVERLAY}, {IDC_LAUNCH}, {IDC_BROWSE}, {IDC_APPLY}, {IDCANCEL}, {IDC_ROOT}}};

    int scale(int value) const;
    void theme();
    void fonts();
    void layout(const RECT* suggested = nullptr);
    void scroll_to(int position);
    void reveal(HWND control);
    void paint(HDC dc);
    void paint_control(HWND control, HDC dc);
    static LRESULT CALLBACK control_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
public:
    SetupView() = default;
    ~SetupView();
    SetupView(const SetupView&) = delete;
    SetupView& operator=(const SetupView&) = delete;
    void initialize(HWND window, SetupAppearance appearance = SetupAppearance::System, UINT dpi = 0);
    std::optional<INT_PTR> message(UINT message, WPARAM wparam, LPARAM lparam);
    void refresh(bool installed, bool discards_pending = false);
    void working(bool removing, bool installed);
    void progress(int percent, std::wstring_view status);
    void finish(bool keep);
    void failure();
};
}
