// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "headerbar.h"
#include "calendarwindow.h"
#include "style.h"

#include <base/i18n.h>
#include <ui/header_button.h>
#include <ui/icon.h>
#include <ui/label.h>
#include <ui/painter.h>
#include <ui/search_field.h>
#include <ui/segmented_control.h>
#include <ui/window.h>
#include <ui/window_header.h>

#include <algorithm>
#include <cmath>

using namespace cutefish::ui;
using cutefish::i18n::tr;

static constexpr double ButtonSize = 32;
static const std::vector<std::string> Views{"day", "week", "month", "year"};

NavigationButton::NavigationButton(std::string icon, std::string text)
{
    if (!icon.empty()) {
        m_icon = addChild<Icon>(std::move(icon), 16);
    } else {
        m_label = addChild<Label>(std::move(text));
        m_label->setFontSize(Style::points(9));
        m_label->setWeight(500);
        m_label->setElide(Label::Elide::None);
    }
}

SizeF NavigationButton::sizeHint() const
{
    return {m_icon ? 30 : m_label->sizeHint().width + 28, 30};
}

void NavigationButton::layout()
{
    const SizeF size = geometry().size();
    if (m_icon)
        m_icon->setGeometry({Style::centred(size.width, 16), Style::centred(size.height, 16), 16, 16});
    if (m_label) {
        const SizeF text = m_label->sizeHint();
        m_label->setGeometry({Style::centred(size.width, text.width), Style::centred(size.height, text.height),
                              text.width, text.height});
    }
}

void NavigationButton::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    const double alpha = m_pressed ? 0.12 : m_hovered ? 0.08 : 0.05;
    painter.fillRoundedRect(localRect(), geometry().height / 2, Style::withAlpha(theme.text, alpha));
}

void NavigationButton::pointerEnter()
{
    m_hovered = true;
    requestPaint();
}

void NavigationButton::pointerLeave()
{
    m_hovered = false;
    requestPaint();
}

void NavigationButton::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    m_pressed = true;
    requestPaint();
}

void NavigationButton::pointerRelease(const PointerEvent& event)
{
    if (!m_pressed)
        return;
    m_pressed = false;
    requestPaint();
    if (localRect().contains(event.position) && onClicked)
        onClicked();
}

HeaderBar::HeaderBar(CalendarWindow* window)
    : m_window(window)
    , m_title(addChild<Label>())
    , m_sidebarButton(addChild<HeaderButton>(std::string(), ButtonSize))
    , m_viewSwitch(addChild<SegmentedControl>(std::vector<std::string>{tr("Day"), tr("Week"), tr("Month"), tr("Year")}))
    , m_search(addChild<SearchField>(tr("Search events")))
    , m_menuButton(addChild<HeaderButton>("menu", ButtonSize))
{
    // Never shown: the window buttons centre on its capitals.
    m_title->setName("buttonsMetrics");
    m_title->setFontSize(Style::points(11));
    m_title->setBaseline(CalendarWindow::HeaderBaseline);
    m_title->setVisible(false);
    m_sidebarButton->setName("sidebarButton");
    m_sidebarButton->setGlyph("calendar-sidebar", 24);
    m_sidebarButton->onClicked = [window] { window->toggleSidebar(); };
    m_viewSwitch->setName("viewSwitch");
    m_viewSwitch->setFontSize(Style::points(9));
    m_viewSwitch->onCurrentChanged = [window](int index) {
        if (index >= 0 && index < int(Views.size()) && Views[size_t(index)] != window->view())
            window->setView(Views[size_t(index)]);
    };
    m_search->setName("searchField");
    m_search->setFontSize(Style::points(9));
    m_search->onTextChanged = [this] {
        if (onSearchChanged)
            onSearchChanged(m_search->text());
    };
    m_menuButton->setName("menuButton");
    m_menuButton->onClicked = [this, window] { window->showMainMenu(m_menuButton); };

    m_subscriptions.push_back(window->viewChanged.connect([this] { requestLayout(); }));
    m_subscriptions.push_back(window->sidebarChanged.connect([this] { requestLayout(); }));
}

HeaderBar::~HeaderBar() = default;

double HeaderBar::buttonsTopMargin() const
{
    return m_title->capCenter() - ButtonSize / 2;
}

void HeaderBar::layout()
{
    const Theme& theme = Theme::current();
    Window* window = this->window();
    const double windowWidth = window ? window->size().width : geometry().width;
    const double top = buttonsTopMargin();
    const bool docked = m_window->sidebarDocked();
    const bool overlay = m_window->sidebarOverlay();

    // The toggle ends at the sidebar's edge, or follows the window buttons.
    const WindowHeader* header = dynamic_cast<const WindowHeader*>(parent());
    const double inset = header ? header->leadingInset() : 0;
    double x = docked ? CalendarWindow::SidebarWidth - 12 - ButtonSize : inset + 11;
    m_sidebarButton->setChecked(overlay);
    m_sidebarButton->setGeometry({x, top, ButtonSize, ButtonSize});
    const double startEnd = x + ButtonSize;

    double end = geometry().width;
    end -= ButtonSize;
    m_menuButton->setGeometry({end, top, ButtonSize, ButtonSize});
    end -= 12;
    const double searchWidth = windowWidth >= 1000 ? 220 : 160;
    end -= searchWidth;
    m_search->setGeometry({end, top + (ButtonSize - 30) / 2, searchWidth, 30});
    m_search->setFill(Style::controlTint());
    m_search->setGlyphColor(theme.disabledText);
    m_search->setOpacity(overlay ? 0 : 1);
    m_search->setEnabled(!overlay);

    // Centred on the window, moved aside only as far as the buttons beside it need.
    const double start = startEnd + 12;
    const double stop = end - 12;
    const double width = std::max(240.0, std::min(300.0, stop - start));
    m_viewSwitch->setTrackColor(Style::controlTint());
    m_viewSwitch->setVisible(!overlay);
    m_viewSwitch->setCurrentIndex(int(std::find(Views.begin(), Views.end(), m_window->view()) - Views.begin()));
    m_viewSwitch->setGeometry({std::max(start, std::min(stop - width, (windowWidth - width) / 2)),
                               top + (ButtonSize - 36) / 2, width, 36});
}

NavigationRow::NavigationRow(CalendarWindow* window)
    : m_window(window)
    , m_title(addChild<Label>())
    , m_previous(addChild<NavigationButton>("chevron-left"))
    , m_today(addChild<NavigationButton>(std::string(), tr("Today")))
    , m_next(addChild<NavigationButton>("chevron-right"))
{
    m_title->setName("periodTitle");
    m_title->setFontSize(Style::points(18));
    m_previous->setName("previousButton");
    m_previous->onClicked = [window] { window->step(-1); };
    m_today->setName("todayButton");
    m_today->onClicked = [window] { window->goToday(); };
    m_next->setName("nextButton");
    m_next->onClicked = [window] { window->step(1); };

    const auto update = [this] {
        std::vector<Label::Span> spans;
        for (const auto& [text, bold] : m_window->periodTitle())
            spans.push_back({text, bold ? 700 : 0, {}});
        m_title->setSpans(std::move(spans));
        requestLayout();
    };
    m_subscriptions.push_back(window->currentDateChanged.connect(update));
    m_subscriptions.push_back(window->viewChanged.connect(update));
    m_subscriptions.push_back(window->model()->weekStartChanged.connect(update));
    update();
}

NavigationRow::~NavigationRow() = default;

// Capitals centred on the buttons beside it.
double NavigationRow::titleTop() const
{
    const double scale = window() ? window()->scale() : 1;
    return Style::snap(15 + m_title->capHeight() / 2 - m_title->baselineOffset(), scale);
}

// The row takes whole pixels, its height rounded up.
double NavigationRow::rowHeight() const
{
    return std::ceil(std::max(30.0, titleTop() + m_title->sizeHint().height));
}

void NavigationRow::layout()
{
    const double width = geometry().width;
    double x = width - 12;
    for (NavigationButton* button : {m_next, m_today, m_previous}) {
        // A layout takes whole pixels of the implicit width.
        const double buttonWidth = std::ceil(button->sizeHint().width);
        x -= buttonWidth;
        button->setGeometry({x, 0, buttonWidth, 30});
        x -= 8;
    }
    m_title->setGeometry({16, titleTop(), std::max(0.0, x - 16), m_title->sizeHint().height});
}
