// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "timegridview.h"
#include "calendarwindow.h"
#include "dates.h"
#include "eventviews.h"
#include "localedata.h"
#include "style.h"

#include <base/i18n.h>
#include <ui/label.h>
#include <ui/painter.h>
#include <ui/scroll_view.h>

#include <algorithm>
#include <cmath>

using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;

static constexpr double GridTop = 6;

static std::string minuteText(int minute)
{
    return LocaleData::timeText(Time(minute / 60 % 24, minute % 60));
}

// A day's name and number over its column; today in the accent colour.
class TimeGridView::Header : public View
{
public:
    Header(CalendarWindow* window, bool longNames)
        : m_window(window)
        , m_longNames(longNames)
        , m_name(addChild<Label>())
        , m_number(addChild<Label>())
    {
        m_name->setFontSize(Style::points(9));
        m_name->setElide(Label::Elide::None);
        m_number->setFontSize(Style::points(11));
        m_number->setElide(Label::Elide::None);
    }

    void setDate(Date date)
    {
        if (date == m_date)
            return;
        m_date = date;
        m_name->setText(LocaleData::dayName(Dates::weekday(date), m_longNames ? LocaleData::Width::Long
                                                                               : LocaleData::Width::Short));
        m_number->setText(std::to_string(date.day()));
        requestLayout();
    }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const Theme& theme = Theme::current();
        const bool today = m_date == m_window->model()->today();
        m_name->setColor(today ? theme.highlight : theme.disabledText);
        m_number->setWeight(today ? 600 : 400);
        m_number->setColor(today ? theme.highlightedText : theme.text);

        // A Row of the name and the plate, centred by anchors, each centred across it.
        const SizeF name = m_name->sizeHint();
        const SizeF number = m_number->sizeHint();
        const double plate = plateWidth();
        const double rowWidth = name.width + 6 + plate;
        const double rowHeight = std::max(name.height, 24.0);
        const double x = Style::centred(geometry().width, rowWidth);
        const double y = Style::centred(geometry().height, rowHeight);
        m_name->setGeometry({x, y + Style::centred(rowHeight, name.height), name.width, name.height});
        m_plate = {x + name.width + 6, y + Style::centred(rowHeight, 24), plate, 24};
        m_number->setGeometry({m_plate.x + Style::centred(plate, number.width), m_plate.y + Style::centred(24, number.height),
                               number.width, number.height});
    }

    void paint(Painter& painter) override
    {
        if (m_date != m_window->model()->today())
            return;
        painter.fillRoundedRect(m_plate, 12, Theme::current().highlight);
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button != MouseButton::Left)
            return;
        m_double = event.clickCount == 2;
        if (m_double)
            m_window->showDay(m_date);
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && !m_double && localRect().contains(event.position))
            m_window->setCurrentDate(m_date);
    }

private:
    double plateWidth() const { return std::max(24.0, m_number->sizeHint().width + 10); }

    CalendarWindow* m_window;
    bool m_longNames;
    Label* m_name;
    Label* m_number;
    RectF m_plate;
    Date m_date;
    bool m_double = false;
};

// The all-day strip: its hairlines, and a double click for a new all-day event.
class TimeGridView::Strip : public View
{
public:
    explicit Strip(TimeGridView* grid)
        : m_grid(grid)
    {
    }

    bool acceptsPointer() const override { return true; }

protected:
    void paint(Painter& painter) override
    {
        const Color color = Style::lineColor();
        painter.drawHorizontalLine(0, 0, geometry().width, color);
        for (int i = 0; i < m_grid->m_days; ++i)
            painter.drawVerticalLine(Gutter + i * m_grid->columnWidth(), 0, geometry().height, color);
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button != MouseButton::Left || event.position.x < Gutter)
            return;
        m_grid->m_window->clearSelection();
        if (event.clickCount == 2) {
            const Date date = m_grid->rangeStart().addDays(m_grid->columnAt(event.position.x));
            m_grid->m_window->newEvent(DateTime(date, Time(0, 0)), DateTime(date, Time(0, 0)), true);
        }
    }

private:
    TimeGridView* m_grid;
};

namespace {

// A red dot and line at the current time on today's column.
class NowLine : public View
{
protected:
    void paint(Painter& painter) override
    {
        const Color red = Style::red();
        painter.fillEllipse({0, Style::centred(geometry().height, 8), 8, 8}, red);
        const double thickness = 1.5;
        painter.fillRect({4, Style::centred(geometry().height, thickness), geometry().width - 4, thickness}, red);
    }
};

}

// The hours: their lines and labels, new events marked out by dragging,
// and the event blocks.
class TimeGridView::Canvas : public View
{
public:
    explicit Canvas(TimeGridView* grid)
        : m_grid(grid)
    {
        for (int hour = 1; hour < 24; ++hour) {
            Label* label = addChild<Label>(LocaleData::hourText(hour));
            label->setFontSize(Style::points(8));
            label->setTextAlignment(Label::TextAlignment::Right);
            label->setElide(Label::Elide::None);
            m_hours.push_back(label);
        }
        m_preview = addChild<View>();
        m_preview->setName("newEventPreview");
        m_preview->setVisible(false);
        m_previewLabel = m_preview->addChild<Label>();
        m_previewLabel->setFontSize(Style::points(8));
        m_previewLabel->setElide(Label::Elide::None);
        m_now = addChild<NowLine>();
    }

    View* preview() const { return m_preview; }
    View* now() const { return m_now; }
    bool isCreating() const { return m_creating; }
    int fromMinute() const { return std::min(m_anchorMinute, m_currentMinute); }
    int toMinute() const { return std::max(m_anchorMinute, m_currentMinute); }
    int column() const { return m_column; }

    void setNowMinute(int minute)
    {
        m_nowMinute = minute;
        requestLayout();
    }

    double heightForWidth(double) const override { return 24 * HourHeight + 12; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        if (m_grid->m_pendingScroll) {
            m_grid->m_scroll->setScrollPosition({0, *m_grid->m_pendingScroll});
            m_grid->m_pendingScroll.reset();
        }
        const Theme& theme = Theme::current();
        const double columnWidth = m_grid->columnWidth();
        for (int i = 0; i < int(m_hours.size()); ++i) {
            Label* label = m_hours[i];
            label->setColor(theme.disabledText);
            const double height = label->sizeHint().height;
            label->setGeometry({0, GridTop + (i + 1) * HourHeight - height / 2, Gutter - 10, height});
        }

        m_preview->setVisible(m_creating);
        if (m_creating) {
            m_preview->setGeometry({Gutter + m_column * columnWidth + 2, GridTop + fromMinute() / 60.0 * HourHeight,
                                    columnWidth - 4, (toMinute() - fromMinute()) / 60.0 * HourHeight});
            // TRANSLATORS: a time range, such as "9:00 – 10:00".
            m_previewLabel->setText(format(tr("{start} – {end}"),
                                           {{"start", minuteText(fromMinute())}, {"end", minuteText(toMinute())}}));
            m_previewLabel->setColor(theme.text);
            const SizeF size = m_previewLabel->sizeHint();
            m_previewLabel->setGeometry({6, 3, size.width, size.height});
        }

        for (EventBlock* block : m_grid->m_blocks) {
            const EventItem& item = block->item();
            const bool moving = m_grid->m_dragKey == item.key && m_grid->m_dragRecurrenceKey == item.recurrenceKey;
            const double slotWidth = (columnWidth - 4) / item.slots;
            const int shownStart = item.startMinute + (moving ? m_grid->m_dragMinutes : 0);
            const int shownEnd = item.endMinute + (moving ? m_grid->m_dragMinutes + m_grid->m_resizeMinutes : 0);
            block->setGeometry({Gutter + (item.column + (moving ? m_grid->m_dragDays : 0)) * columnWidth + 2
                                    + item.slot * slotWidth,
                                GridTop + shownStart / 60.0 * HourHeight + 1, item.span * slotWidth - 2,
                                std::max(LineHeight, (shownEnd - shownStart) / 60.0 * HourHeight - 2)});
            block->setOpacity(moving ? 0.85 : 1);
            if (moving)
                raiseChild(block);
        }

        const int64_t column = m_grid->rangeStart().daysTo(m_grid->m_window->model()->today());
        m_now->setVisible(column >= 0 && column < m_grid->m_days);
        m_now->setGeometry({Gutter + column * columnWidth - 4, GridTop + m_nowMinute / 60.0 * HourHeight - 4,
                            columnWidth + 4, 8});
        raiseChild(m_now);
    }

    void paint(Painter& painter) override
    {
        const Color color = Style::lineColor();
        // The first hour line would double the strip's border.
        for (int hour = 1; hour < 24; ++hour)
            painter.drawHorizontalLine(Gutter - 6, GridTop + hour * HourHeight, geometry().width - (Gutter - 6), color);
        for (int i = 0; i < m_grid->m_days; ++i)
            painter.drawVerticalLine(Gutter + i * m_grid->columnWidth(), 0, geometry().height, color);

        if (m_creating) {
            const Color highlight = Theme::current().highlight;
            const RectF preview = m_preview->geometry();
            painter.fillRoundedRect(preview, 4, Style::withAlpha(highlight, 0.25));
            painter.strokeRoundedRect(preview, 4, highlight, painter.borderWidth(1));
        }
    }

    // Pressing on empty time and dragging marks the new event's time.
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button != MouseButton::Left || !inArea(event.position))
            return;
        m_pressed = true;
        m_grid->m_window->clearSelection();
        m_grid->m_window->focusView();
        m_column = m_grid->columnAt(event.position.x);
        m_anchorMinute = int(std::floor(minuteAt(event.position.y) / Step)) * Step;
        m_currentMinute = m_anchorMinute;
        m_creating = false;
        if (event.clickCount == 2) {
            m_pressed = false;
            const Date date = m_grid->rangeStart().addDays(m_grid->columnAt(event.position.x));
            const int start = std::min(23 * 60, int(std::floor(minuteAt(event.position.y) / 30)) * 30);
            m_grid->m_window->newEvent(Dates::atMinute(date, start), Dates::atMinute(date, start + 60), false);
        }
        requestLayout();
        requestPaint();
    }

    void pointerMove(const PointerEvent& event) override
    {
        if (!m_pressed)
            return;
        const int minute = m_grid->snap(minuteAt(event.position.y));
        if (!m_creating && std::abs(minute - m_anchorMinute) >= Step)
            m_creating = true;
        if (m_creating)
            m_currentMinute = minute;
        requestLayout();
        requestPaint();
    }

    void pointerRelease(const PointerEvent&) override
    {
        const bool creating = m_pressed && m_creating;
        m_pressed = false;
        if (!creating)
            return;
        m_creating = false;
        requestLayout();
        requestPaint();
        const Date date = m_grid->rangeStart().addDays(m_column);
        if (toMinute() > fromMinute())
            m_grid->m_window->newEvent(Dates::atMinute(date, fromMinute()), Dates::atMinute(date, toMinute()), false);
    }

private:
    bool inArea(PointF point) const
    {
        return point.x >= Gutter && point.y >= GridTop && point.y < GridTop + 24 * HourHeight;
    }

    double minuteAt(double y) const
    {
        return std::clamp((y - GridTop) / HourHeight * 60, 0.0, 24.0 * 60);
    }

    TimeGridView* m_grid;
    std::vector<Label*> m_hours;
    View* m_preview;
    Label* m_previewLabel;
    View* m_now;
    bool m_pressed = false;
    bool m_creating = false;
    int m_column = 0;
    int m_anchorMinute = 0;
    int m_currentMinute = 0;
    int m_nowMinute = 0;
};

TimeGridView::TimeGridView(CalendarWindow* window, int days)
    : m_window(window)
    , m_days(days)
{
    setName(days == 1 ? "dayView" : "weekView");
    if (days > 1) {
        for (int i = 0; i < days; ++i)
            m_headers.push_back(addChild<Header>(window, false));
    }
    // TRANSLATORS: the label of the all-day row in the day and week views, in a narrow column beside it.
    m_allDayLabel = addChild<Label>(tr("all-day"));
    m_allDayLabel->setFontSize(Style::points(8));
    m_allDayLabel->setTextAlignment(Label::TextAlignment::Right);
    m_allDayLabel->setVerticallyCentered(true);
    m_allDayLabel->setElide(Label::Elide::None);
    m_strip = addChild<Strip>(this);
    m_scroll = addChild<ScrollView>();
    m_scroll->setName("hourGrid");
    m_scroll->setBottomCornerRadius(Theme::current().windowRadius);
    m_canvas = m_scroll->setContent<Canvas>(this);

    m_nowTimer.setInterval(std::chrono::seconds(30));
    m_nowTimer.onTimeout = [this] { m_canvas->setNowMinute(Dates::minuteOf(DateTime::currentDateTime())); };
    m_canvas->setNowMinute(Dates::minuteOf(DateTime::currentDateTime()));

    CalendarModel* model = window->model();
    m_subscriptions.push_back(model->revisionChanged.connect([this] { refresh(); }));
    m_subscriptions.push_back(model->todayChanged.connect([this] {
        // The headers mark today and the canvas draws the now line as they lay out.
        for (Header* header : m_headers)
            header->requestLayout();
        m_canvas->requestLayout();
        requestLayout();
        requestPaint();
    }));
    m_subscriptions.push_back(model->weekStartChanged.connect([this] {
        refresh();
        scrollToWorkingHours();
    }));
    m_subscriptions.push_back(window->currentDateChanged.connect([this] {
        const std::string key = m_laidOutKey;
        refresh();
        if (key != m_laidOutKey)
            scrollToWorkingHours();
    }));
    m_subscriptions.push_back(window->selectionChanged.connect([this] {
        for (EventLine* bar : m_bars) {
            bar->requestLayout();
            bar->requestPaint();
        }
        for (EventBlock* block : m_blocks) {
            block->requestLayout();
            block->requestPaint();
        }
    }));

    // Laid out once while preloaded, so the first switch to it is quick too.
    refresh(true);
    scrollToWorkingHours();
}

TimeGridView::~TimeGridView() = default;

Date TimeGridView::rangeStart() const
{
    const Date current = m_window->currentDate();
    return m_days == 7 ? Dates::weekStart(current, m_window->model()->firstDayOfWeek()) : current;
}

double TimeGridView::columnWidth() const
{
    return (geometry().width - Gutter) / m_days;
}

int TimeGridView::columnAt(double x) const
{
    return std::clamp(int(std::floor((x - Gutter) / columnWidth())), 0, m_days - 1);
}

int TimeGridView::snap(double minutes) const
{
    return int(std::round(minutes / Step)) * Step;
}

int TimeGridView::stripLanes() const
{
    return std::max(1, std::min(m_layout.lanes, MaximumLanes));
}

double TimeGridView::stripHeight() const
{
    return stripLanes() * LineHeight + 8;
}

void TimeGridView::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;
    if (live)
        m_nowTimer.start();
    else
        m_nowTimer.stop();
    refresh();
    if (live) {
        m_canvas->setNowMinute(Dates::minuteOf(DateTime::currentDateTime()));
        scrollToWorkingHours();
    }
}

void TimeGridView::refresh(bool force)
{
    const Date start = rangeStart();
    const std::string key = std::to_string(m_window->model()->revision()) + ',' + start.toCompactString() + ','
            + std::to_string(m_days);
    if ((!m_live && !force) || key == m_laidOutKey)
        return;
    m_laidOutKey = key;
    m_layout = m_window->model()->dayLayout(start, m_days, MaximumLanes);
    rebuild();
}

void TimeGridView::rebuild()
{
    for (EventLine* bar : m_bars)
        m_strip->takeChild(bar);
    for (Label* more : m_more)
        m_strip->takeChild(more);
    for (EventBlock* block : m_blocks)
        m_canvas->takeChild(block);
    m_bars.clear();
    m_more.clear();
    m_blocks.clear();

    for (const EventItem& item : m_layout.bars) {
        EventLine* bar = m_strip->addChild<EventLine>(m_window, item);
        bar->onDragFinished = [this, bar](PointF start, PointF point) {
            const int days = columnAt(mapFromWindow(bar->mapToWindow(point)).x)
                    - columnAt(mapFromWindow(bar->mapToWindow(start)).x);
            if (days != 0)
                m_window->moveEvent(bar->item(), days, 0, days, 0);
        };
        m_bars.push_back(bar);
    }
    for (const MoreItem& more : m_layout.more) {
        Label* label = m_strip->addChild<MoreLabel>(m_window, more);
        m_more.push_back(label);
    }
    for (const EventItem& item : m_layout.blocks)
        m_blocks.push_back(m_canvas->addChild<EventBlock>(m_window, this, item));
    m_canvas->raiseChild(m_canvas->now());
    requestLayout();
    requestPaint();
}

void TimeGridView::scrollToWorkingHours()
{
    const Date start = rangeStart();
    const Date today = m_window->model()->today();
    const bool showsToday = start <= today && today < start.addDays(m_days);
    const int hour = showsToday ? std::max(0, DateTime::currentDateTime().time().hour() - 1) : 8;
    // A little above the hour, so its label is not cut by the strip.
    // Applied once the scroll view knows the hours' height.
    m_pendingScroll = std::max(0.0, hour * HourHeight - 6);
    m_canvas->requestLayout();
}

double TimeGridView::scrollTop() const
{
    return m_pendingScroll.value_or(m_scroll->scrollPosition().y);
}

void TimeGridView::setScrollTop(double top)
{
    m_pendingScroll = top;
    m_canvas->requestLayout();
}

void TimeGridView::layout()
{
    const Theme& theme = Theme::current();
    const double width = geometry().width;
    const double columnWidth = this->columnWidth();
    const Date start = rangeStart();
    for (int i = 0; i < int(m_headers.size()); ++i) {
        m_headers[i]->setDate(start.addDays(i));
        m_headers[i]->setGeometry({Gutter + i * columnWidth, 0, columnWidth, headerHeight()});
    }

    m_allDayLabel->setColor(theme.disabledText);
    m_allDayLabel->setGeometry({0, headerHeight(), Gutter - 8, LineHeight + 8});
    m_strip->setGeometry({0, headerHeight(), width, stripHeight()});
    for (EventLine* bar : m_bars) {
        const EventItem& item = bar->item();
        bar->setVisible(item.lane < stripLanes());
        bar->setGeometry({Gutter + item.column * columnWidth + (item.continuesBefore ? 0 : 3),
                          4 + item.lane * LineHeight,
                          item.span * columnWidth - (item.continuesBefore ? 0 : 3) - (item.continuesAfter ? 0 : 3),
                          LineHeight - 2});
    }
    for (Label* more : m_more) {
        const MoreItem& item = static_cast<MoreLabel*>(more)->item();
        more->setGeometry({Gutter + item.column * columnWidth + 8, 4 + (stripLanes() - 1) * LineHeight,
                           columnWidth - 16, LineHeight - 2});
    }

    const double gridTop = headerHeight() + stripHeight();
    m_scroll->setGeometry({0, gridTop, width, std::max(0.0, geometry().height - gridTop)});
}

void TimeGridView::paint(Painter& painter)
{
    painter.drawHorizontalLine(0, headerHeight() + stripHeight(), geometry().width, Style::lineColor());
}

void TimeGridView::dragMove(const EventItem& item, int days, int minutes)
{
    m_dragKey = item.key;
    m_dragRecurrenceKey = item.recurrenceKey;
    m_dragDays = days;
    m_dragMinutes = minutes;
    m_canvas->requestLayout();
}

void TimeGridView::dragResize(const EventItem& item, int minutes)
{
    m_dragKey = item.key;
    m_dragRecurrenceKey = item.recurrenceKey;
    m_resizeMinutes = minutes;
    m_canvas->requestLayout();
}

void TimeGridView::finishDrag(const EventItem& item)
{
    const int days = m_dragDays;
    const int minutes = m_dragMinutes;
    const int resize = m_resizeMinutes;
    m_dragKey.clear();
    m_dragRecurrenceKey.clear();
    m_dragDays = 0;
    m_dragMinutes = 0;
    m_resizeMinutes = 0;
    m_canvas->requestLayout();
    if (days != 0 || minutes != 0 || resize != 0)
        m_window->moveEvent(item, days, minutes, days, minutes + resize);
}
