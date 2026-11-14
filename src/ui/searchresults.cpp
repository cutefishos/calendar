// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "searchresults.h"
#include "calendarwindow.h"
#include "colors.h"
#include "localedata.h"
#include "style.h"

#include <base/i18n.h>
#include <ui/label.h>
#include <ui/layout.h>
#include <ui/painter.h>
#include <ui/scroll_view.h>

#include <cmath>

using namespace cutefish::ui;
using cutefish::i18n::tr;

namespace {

// A day's name over its events, at the bottom of its line.
class ResultHeading : public View
{
public:
    explicit ResultHeading(std::string text)
        : m_label(addChild<Label>(std::move(text)))
    {
        m_label->setFontSize(Style::points(8));
        m_label->setWeight(500);
    }

    SizeF sizeHint() const override { return {0, 30}; }
    double heightForWidth(double) const override { return 30; }

protected:
    void layout() override
    {
        m_label->setColor(Theme::current().disabledText);
        const double height = m_label->sizeHint().height;
        m_label->setGeometry({16, 30 - 4 - height, std::max(0.0, geometry().width - 32), height});
    }

private:
    Label* m_label;
};

// One event: its colour, title, time and location.
class ResultRow : public View
{
public:
    ResultRow(CalendarWindow* window, EventItem event)
        : m_window(window)
        , m_event(std::move(event))
        , m_title(addChild<Label>(m_event.title.empty() ? std::string(tr("New Event")) : m_event.title))
        , m_time(addChild<Label>(m_event.allDay ? std::string(tr("All day")) : m_event.startText))
        , m_location(addChild<Label>(m_event.location))
    {
        m_time->setFontSize(Style::points(8));
        m_time->setElide(Label::Elide::None);
        m_location->setFontSize(Style::points(8));
        m_location->setVisible(!m_event.location.empty());
    }

    double height() const { return m_event.location.empty() ? 36 : 50; }
    SizeF sizeHint() const override { return {0, height()}; }
    double heightForWidth(double) const override { return height(); }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const Theme& theme = Theme::current();
        m_time->setColor(theme.disabledText);
        m_location->setColor(theme.disabledText);

        const double left = 16 + 8 + 10;
        const double right = geometry().width - 16;
        const SizeF title = m_title->sizeHint();
        const SizeF time = {std::ceil(m_time->sizeHint().width), m_time->sizeHint().height};
        const double line = std::max(title.height, time.height);
        const double location = m_location->isVisible() ? m_location->sizeHint().height : 0;
        const double block = line + (m_location->isVisible() ? 1 + location : 0);
        // As RowLayout and ColumnLayout place them: whole pixels.
        const double top = std::round((geometry().height - block) / 2);
        m_time->setGeometry({std::round(right - time.width), top + std::round((line - time.height) / 2), time.width,
                             time.height});
        m_title->setGeometry({left, top + std::round((line - title.height) / 2),
                              std::max(0.0, right - time.width - 8 - left), title.height});
        if (m_location->isVisible())
            m_location->setGeometry({left, top + line + 1, std::max(0.0, right - left), location});
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        const RectF background{6, 0, geometry().width - 12, geometry().height};
        if (m_window->isSelected(m_event))
            painter.fillRoundedRect(background, theme.smallRadius,
                                    color(CalendarColor::SearchResultSelected));
        else if (m_hovered)
            painter.fillRoundedRect(background, theme.smallRadius, Style::withAlpha(theme.text, 0.04));
        painter.fillEllipse({16, 14, 8, 8}, Style::toColor(m_event.color));
    }

    void pointerEnter() override
    {
        m_hovered = true;
        requestPaint();
    }

    void pointerLeave() override
    {
        m_hovered = false;
        requestPaint();
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && event.clickCount == 2) {
            m_double = true;
            m_window->editEvent(m_event.key, m_event.recurrenceKey);
            return;
        }
        m_double = false;
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && !m_double && localRect().contains(event.position))
            m_window->revealEvent(m_event);
    }

private:
    CalendarWindow* m_window;
    EventItem m_event;
    Label* m_title;
    Label* m_time;
    Label* m_location;
    bool m_hovered = false;
    bool m_double = false;
};

}

static std::string headingText(Date date, Date today)
{
    if (date == today)
        return tr("Today");
    if (date == today.addDays(1))
        return tr("Tomorrow");
    return LocaleData::longDate(date);
}

SearchResults::SearchResults(CalendarWindow* window)
    : m_window(window)
    , m_scroll(addChild<ScrollView>())
    , m_empty(addChild<Label>(tr("No Events Found")))
{
    setName("searchResults");
    m_scroll->setBottomCornerRadius(Theme::current().windowRadius);
    m_list = m_scroll->setContent<Column>();
    m_list->setMargins({0, 6, 0, 6});

    CalendarModel* model = window->model();
    m_subscriptions.push_back(model->revisionChanged.connect([this] { update(); }));
    m_subscriptions.push_back(window->selectionChanged.connect([this] {
        for (const auto& row : m_list->children())
            row->requestPaint();
    }));
}

SearchResults::~SearchResults() = default;

void SearchResults::setText(const std::string& text)
{
    if (text == m_text)
        return;
    m_text = text;
    update();
}

void SearchResults::update()
{
    m_results = m_window->model()->search(m_text);
    while (!m_list->children().empty())
        m_list->takeChild(m_list->children().back().get());

    const Date today = m_window->model()->today();
    Date last;
    for (const EventItem& item : m_results) {
        const Date day = item.allDay ? item.firstDay : item.start.date();
        if (!last.isValid() || day != last) {
            m_list->addChild<ResultHeading>(headingText(day, today))->setLayoutHints({.fillWidth = true});
            last = day;
        }
        m_list->addChild<ResultRow>(m_window, item)->setLayoutHints({.fillWidth = true});
    }
    m_empty->setVisible(m_results.empty());
    requestLayout();
}

void SearchResults::layout()
{
    const Theme& theme = Theme::current();
    m_scroll->setGeometry(localRect());
    m_empty->setColor(theme.disabledText);
    const SizeF empty = m_empty->sizeHint();
    m_empty->setGeometry({Style::centred(geometry().width, empty.width), Style::centred(geometry().height, empty.height),
                          empty.width, empty.height});
}

void SearchResults::paint(Painter& painter)
{
    painter.drawVerticalLine(0, 0, geometry().height, Style::lineColor());
}
