// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <base/signal.h>
#include <ui/view.h>

#include <functional>
#include <string>
#include <vector>

namespace cutefish::ui {
class HeaderButton;
class Icon;
class Label;
class SearchField;
class SegmentedControl;
}

class CalendarWindow;

// A pill on the calendar's surface: a round one for an icon, a wider one for text.
class NavigationButton : public cutefish::ui::View
{
public:
    // An icon name, or the text with an empty one.
    NavigationButton(std::string icon, std::string text = {});

    cutefish::ui::SizeF sizeHint() const override;
    bool acceptsPointer() const override { return true; }

    std::function<void()> onClicked;

protected:
    void layout() override;
    void paint(cutefish::ui::Painter& painter) override;
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const cutefish::ui::PointerEvent& event) override;
    void pointerRelease(const cutefish::ui::PointerEvent& event) override;

private:
    cutefish::ui::Icon* m_icon = nullptr;
    cutefish::ui::Label* m_label = nullptr;
    bool m_hovered = false;
    bool m_pressed = false;
};

// The title bar's tools: the sidebar button at the start, the view switch in
// the middle, search at the end.
class HeaderBar : public cutefish::ui::View
{
public:
    explicit HeaderBar(CalendarWindow* window);
    ~HeaderBar() override;

    // Where the window buttons go from the top: on the title's capitals.
    double buttonsTopMargin() const;
    cutefish::ui::SearchField* searchField() const { return m_search; }
    cutefish::ui::HeaderButton* sidebarButton() const { return m_sidebarButton; }

    std::function<void(const std::string& text)> onSearchChanged;

protected:
    void layout() override;

private:
    CalendarWindow* m_window;
    cutefish::ui::Label* m_title;
    cutefish::ui::HeaderButton* m_sidebarButton;
    cutefish::ui::SegmentedControl* m_viewSwitch;
    cutefish::ui::SearchField* m_search;
    std::vector<cutefish::base::Subscription> m_subscriptions;
};

// The period's title with the previous, today and next buttons.
class NavigationRow : public cutefish::ui::View
{
public:
    explicit NavigationRow(CalendarWindow* window);
    ~NavigationRow() override;

    // The title can reach below the buttons.
    double rowHeight() const;

protected:
    void layout() override;

private:
    double titleTop() const;

    CalendarWindow* m_window;
    cutefish::ui::Label* m_title;
    NavigationButton* m_previous;
    NavigationButton* m_today;
    NavigationButton* m_next;
    std::vector<cutefish::base::Subscription> m_subscriptions;
};
