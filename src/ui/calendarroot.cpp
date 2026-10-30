// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "calendarroot.h"
#include "calendarwindow.h"
#include "headerbar.h"
#include "monthview.h"
#include "searchresults.h"
#include "sidebar.h"
#include "style.h"
#include "timegridview.h"
#include "yearview.h"

#include <base/event_loop.h>
#include <base/url.h>
#include <ui/application.h>
#include <ui/drag.h>
#include <ui/keys.h>
#include <ui/painter.h>
#include <ui/window_header.h>

#include <algorithm>
#include <cmath>

using namespace cutefish::ui;

CalendarContent::CalendarContent(CalendarWindow* window)
    : m_window(window)
    , m_navigation(addChild<NavigationRow>(window))
    , m_viewArea(addChild<View>())
    , m_search(addChild<SearchResults>(window))
{
    setName("keyTarget");
    m_viewArea->setName("viewArea");
    m_viewArea->setClipsChildren(true);
    m_search->setVisible(false);

    m_preload.setSingleShot(true);
    m_preload.onTimeout = [this] { ensureViews(); };
    m_preload.start(std::chrono::seconds(1));
}

CalendarContent::~CalendarContent() = default;

void CalendarContent::ensureViews()
{
    if (!m_day)
        m_day = m_viewArea->addChild<TimeGridView>(m_window, 1);
    if (!m_week)
        m_week = m_viewArea->addChild<TimeGridView>(m_window, 7);
    if (!m_month)
        m_month = m_viewArea->addChild<MonthView>(m_window);
    if (!m_year)
        m_year = m_viewArea->addChild<YearView>(m_window);
    showView(m_current);
}

View* CalendarContent::currentView() const
{
    if (m_current == "day")
        return m_day;
    if (m_current == "week")
        return m_week;
    if (m_current == "year")
        return m_year;
    return m_month;
}

// Views stay built once shown, and the others are built while idle, so
// switching never waits for a view to be created.
void CalendarContent::showView(const std::string& name)
{
    m_current = name;
    if (name == "day" && !m_day)
        m_day = m_viewArea->addChild<TimeGridView>(m_window, 1);
    else if (name == "week" && !m_week)
        m_week = m_viewArea->addChild<TimeGridView>(m_window, 7);
    else if (name == "year" && !m_year)
        m_year = m_viewArea->addChild<YearView>(m_window);
    else if (name == "month" && !m_month)
        m_month = m_viewArea->addChild<MonthView>(m_window);

    if (m_day) {
        m_day->setVisible(name == "day");
        m_day->setLive(name == "day");
    }
    if (m_week) {
        m_week->setVisible(name == "week");
        m_week->setLive(name == "week");
    }
    if (m_month) {
        m_month->setVisible(name == "month");
        m_month->setLive(name == "month");
    }
    if (m_year) {
        m_year->setVisible(name == "year");
        m_year->setLive(name == "year");
    }
    requestLayout();
}

void CalendarContent::setSearchText(const std::string& text)
{
    m_searching = text.find_first_not_of(" \t\n") != std::string::npos;
    m_search->setVisible(m_searching);
    m_search->setText(text);
    requestLayout();
}

void CalendarContent::layout()
{
    const double width = geometry().width;
    const double navigation = m_navigation->rowHeight();
    m_navigation->setGeometry({0, 4, width, navigation});
    const double top = 4 + navigation + 14;
    const double height = std::max(0.0, geometry().height - top);
    const double searchWidth = m_searching ? 300 : 0;
    m_viewArea->setGeometry({0, top, std::max(0.0, width - searchWidth), height});
    m_search->setGeometry({width - searchWidth, top, searchWidth, height});
    for (const auto& view : m_viewArea->children())
        view->setGeometry(m_viewArea->localRect());
}

bool CalendarContent::keyPress(const KeyEvent& event)
{
    const std::string view = m_window->view();
    const int across = 1;
    const int down = view == "day" ? 1 : 7;
    int move = 0;
    if (event.key == key::Left)
        move = -across;
    else if (event.key == key::Right)
        move = across;
    else if (event.key == key::Up)
        move = -down;
    else if (event.key == key::Down)
        move = down;
    if (move != 0 && event.modifiers == NoModifier) {
        m_window->setCurrentDate(m_window->currentDate().addDays(move));
        m_window->clearSelection();
        return true;
    }
    if ((event.key == key::Return || event.key == key::KeypadEnter) && m_window->hasSelection()) {
        m_window->editSelected();
        return true;
    }
    if ((event.key == key::Delete || event.key == key::BackSpace) && m_window->hasSelection()) {
        m_window->deleteSelected();
        return true;
    }
    if (event.key == key::Escape && m_window->hasSelection()) {
        m_window->clearSelection();
        return true;
    }
    return false;
}

// Over the calendar the sidebar needs its own background and edge, title bar included.
class CalendarRoot::OverlayBackground : public View
{
protected:
    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        painter.fillRect(localRect(), theme.background);
        painter.drawVerticalLine(geometry().width - 1 / painter.scale(), 0, geometry().height,
                                 Style::withAlpha(theme.text, 0.1));
    }
};

CalendarRoot::CalendarRoot(CalendarWindow* window)
    : m_window(window)
    , m_content(addChild<CalendarContent>(window))
    , m_overlay(addChild<OverlayBackground>())
    , m_sidebar(addChild<Sidebar>(window))
    , m_header(addChild<WindowHeader>(CalendarWindow::HeaderHeight))
{
    m_headerBar = m_header->setContent<HeaderBar>(window);
    m_header->setContentUnderButtons(true);
}

CalendarRoot::~CalendarRoot() = default;

void CalendarRoot::layout()
{
    m_window->sizeChanged();
    const double width = geometry().width;
    const double height = geometry().height;
    // Set only when it changes: the header lays itself out again for it.
    const double margin = m_headerBar->buttonsTopMargin();
    if (margin != m_buttonMargin) {
        m_buttonMargin = margin;
        m_header->setButtonMargin(margin);
    }
    m_header->setGeometry({0, 0, width, CalendarWindow::HeaderHeight});

    const double sidebarX = -CalendarWindow::SidebarWidth * (1 - m_window->sidebarReveal());
    m_sidebar->setVisible(m_window->sidebarReveal() > 0);
    m_sidebar->setGeometry({sidebarX, CalendarWindow::HeaderHeight, CalendarWindow::SidebarWidth,
                            std::max(0.0, height - CalendarWindow::HeaderHeight)});
    m_overlay->setVisible(m_sidebar->isVisible() && !m_window->sidebarDocked());
    m_overlay->setGeometry({sidebarX, 0, CalendarWindow::SidebarWidth, height});

    const double contentX = CalendarWindow::SidebarWidth * m_window->dockReveal();
    m_content->setGeometry({contentX, CalendarWindow::HeaderHeight, std::max(0.0, width - contentX),
                            std::max(0.0, height - CalendarWindow::HeaderHeight)});
}

// The calendar's side of the window, title bar included; the sidebar keeps the window colour.
void CalendarRoot::paint(Painter& painter)
{
    const double x = CalendarWindow::SidebarWidth * m_window->dockReveal();
    painter.fillRect({x, 0, geometry().width - x, geometry().height}, Theme::current().secondBackground);
}

bool CalendarRoot::canDrop(const DropOffer& offer) const
{
    return offer.hasMimeType("text/uri-list");
}

// The URLs of a text/uri-list that name calendar files.
static std::vector<std::string> calendarFiles(const std::string& list)
{
    std::vector<std::string> files;
    for (const std::string& url : cutefish::base::url::parseUriList(list)) {
        std::string suffix = url.size() >= 4 ? url.substr(url.size() - 4) : std::string();
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
        if (suffix == ".ics")
            files.push_back(url);
    }
    return files;
}

void CalendarRoot::dragEnter(const DragEvent& event)
{
    m_dropFiles.clear();
    m_dragAlive = std::make_shared<bool>(true);
    std::weak_ptr<bool> alive = m_dragAlive;
    event.offer->read("text/uri-list", [this, alive](std::string data) {
        if (!alive.expired())
            m_dropFiles = calendarFiles(data);
    });
}

uint32_t CalendarRoot::dragUpdate(const DragEvent&)
{
    return m_dropFiles.empty() ? NoAction : CopyAction;
}

void CalendarRoot::dragLeave()
{
    m_dragAlive.reset();
    m_dropFiles.clear();
}

uint32_t CalendarRoot::drop(const DragEvent&)
{
    m_dragAlive.reset();
    if (m_dropFiles.empty())
        return NoAction;
    // After the drop is answered: the import dialog is a window of its own.
    Application::instance()->loop().post([window = m_window, files = std::move(m_dropFiles)] {
        window->importFiles(files);
    });
    m_dropFiles.clear();
    return CopyAction;
}
