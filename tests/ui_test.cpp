// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "calendarmodel.h"
#include "calendarstore.h"
#include "localedata.h"
#include "ui/calendarroot.h"
#include "ui/calendarwindow.h"
#include "ui/eventdialog.h"
#include "ui/glyphs.h"
#include "ui/dialogs.h"
#include "ui/searchresults.h"

#include <base/event_loop.h>
#include <ui/application.h>
#include <ui/combo_box.h>
#include <ui/dialog_window.h>
#include <ui/drag.h>
#include <ui/label.h>
#include <ui/keys.h>
#include <ui/menu.h>
#include <ui/scroll_view.h>
#include <ui/test_input.h>
#include <ui/text_field.h>
#include <ui/theme.h>
#include <ui/view.h>
#include <ui/window.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>

namespace fs = std::filesystem;
using namespace cutefish::ui;

static uint64_t g_frameTime = 1000000000;

// Frames at 60 Hz, as many as the time asks for, so animations run.
static void advance(Window& window, int milliseconds = 0)
{
    const uint64_t end = g_frameTime + uint64_t(milliseconds) * 1000000;
    do {
        g_frameTime += 16666667;
        TestInput::presentFrame(window, g_frameTime);
    } while (g_frameTime < end);
}

// Runs what the event loop has queued, such as moves done after a drag.
static void runPosted()
{
    cutefish::base::EventLoop& loop = Application::instance()->loop();
    loop.post([&loop] { loop.quit(); });
    loop.run();
}

static void collect(View* view, const std::string& name, std::vector<View*>* out)
{
    if (!view->isVisible())
        return;
    if (view->name() == name)
        out->push_back(view);
    for (const auto& child : view->children())
        collect(child.get(), name, out);
}

static std::vector<View*> find(Window* window, const std::string& name)
{
    std::vector<View*> views;
    collect(window->contentView(), name, &views);
    return views;
}

// Scrolls the hour grid so the hour is at its top.
static void showHour(Window* window, int hour)
{
    for (View* grid : find(window, "hourGrid"))
        static_cast<ScrollView*>(grid)->setScrollPosition({0, hour * 48.0});
    advance(*window);
}

static PointF centreOf(const View* view)
{
    return view->mapToWindow({view->geometry().width / 2, view->geometry().height / 2});
}

namespace {

class UiTest : public testing::Test
{
protected:
    void SetUp() override
    {
        std::string pattern = (fs::temp_directory_path() / "calendar-ui-XXXXXX").string();
        m_dir = mkdtemp(pattern.data());
        m_store = std::make_unique<CalendarStore>(m_dir + "/calendars");
        m_model = std::make_unique<CalendarModel>(m_store.get(), m_dir + "/calendar.conf");
        m_store->load();
    }

    void TearDown() override
    {
        m_window.reset();
        m_model.reset();
        m_store.reset();
        Theme::setCurrent(Theme::light());
        std::error_code error;
        fs::remove_all(m_dir, error);
    }

    CalendarWindow& open()
    {
        m_window = std::make_unique<CalendarWindow>(m_model.get(), false);
        m_window->show();
        advance(*m_window->window());
        return *m_window;
    }

    std::string addEvent(const std::string& title, Date day, int startMinute, int minutes, bool allDay = false)
    {
        const DateTime start(day, Time(startMinute / 60, startMinute % 60));
        EventForm form = m_model->newEventData(start, allDay ? DateTime(day, Time(0, 0)) : start.addSecs(minutes * 60),
                                              allDay);
        form.title = title;
        EXPECT_TRUE(m_model->saveEvent(form, EventFile::AllEvents));
        return title;
    }

    std::string m_dir;
    std::unique_ptr<CalendarStore> m_store;
    std::unique_ptr<CalendarModel> m_model;
    std::unique_ptr<CalendarWindow> m_window;
};

}

TEST_F(UiTest, EveryViewStepsAndComesBack)
{
    for (const bool dark : {false, true}) {
        Theme::setCurrent(dark ? Theme::dark() : Theme::light());
        for (const auto& [view, name] : {std::pair{"day", "dayView"}, std::pair{"week", "weekView"},
                                         std::pair{"month", "monthView"}, std::pair{"year", "yearView"}}) {
            m_model->setView(view);
            CalendarWindow& window = open();
            EXPECT_EQ(find(window.window(), name).size(), 1u) << view;
            EXPECT_EQ(find(window.window(), "periodTitle").size(), 1u);

            // Every view steps forward and back to where it was.
            const Date range = window.rangeStart();
            window.step(1);
            EXPECT_GT(window.rangeStart(), range) << view;
            window.step(-1);
            EXPECT_EQ(window.rangeStart(), range) << view;
            advance(*window.window());
        }
    }
}

TEST_F(UiTest, EventsShowInEveryView)
{
    const Date today = Date::currentDate();
    addEvent("Planning", today, 10 * 60, 90);
    addEvent("Trip", today, 0, 0, true);

    m_model->setView("month");
    CalendarWindow& window = open();
    EXPECT_EQ(find(window.window(), "eventLine").size(), 2u);

    window.setView("week");
    advance(*window.window());
    EXPECT_EQ(find(window.window(), "eventBlock").size(), 1u);
    EXPECT_EQ(find(window.window(), "eventLine").size(), 1u);

    window.setView("day");
    advance(*window.window());
    EXPECT_EQ(find(window.window(), "eventBlock").size(), 1u);
}

TEST_F(UiTest, EditorCreatesEvent)
{
    CalendarWindow& window = open();
    const DateTime start(Date::currentDate(), Time(14, 0));
    window.newEvent(start, start.addSecs(3600), false);
    EventDialog* editor = window.editor();
    ASSERT_TRUE(editor);
    advance(*editor->window());
    const std::vector<View*> titles = find(editor->window(), "titleField");
    ASSERT_EQ(titles.size(), 1u);
    static_cast<TextField*>(titles.front())->setText("Dentist");
    editor->save();
    runPosted();
    EXPECT_FALSE(window.editor());

    const std::vector<Occurrence> today = m_store->occurrences(Date::currentDate(), Date::currentDate().addDays(1));
    ASSERT_EQ(today.size(), 1u);
    EXPECT_EQ(today.front().summary, "Dentist");
    EXPECT_EQ(today.front().start, start);
}

TEST_F(UiTest, SearchFindsEvent)
{
    addEvent("Piano lesson", Date::currentDate().addDays(3), 9 * 60, 60);
    CalendarWindow& window = open();

    const std::vector<View*> fields = find(window.window(), "searchField");
    ASSERT_EQ(fields.size(), 1u);
    TestInput::click(*window.window(), centreOf(fields.front()));
    TestInput::typeText(*window.window(), "piano");
    advance(*window.window());
    const std::vector<View*> panels = find(window.window(), "searchResults");
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_EQ(static_cast<SearchResults*>(panels.front())->results().size(), 1u);

    // Escape clears the search and returns to the view.
    TestInput::typeKey(*window.window(), key::Escape);
    advance(*window.window());
    EXPECT_TRUE(find(window.window(), "searchResults").empty());
}

TEST_F(UiTest, SidebarFollowsWidth)
{
    CalendarWindow& window = open();
    Window* frame = window.window();
    EXPECT_EQ(find(frame, "sidebar").size(), 1u);

    window.toggleSidebar();
    advance(*frame, 300);
    EXPECT_TRUE(find(frame, "sidebar").empty());
    EXPECT_FALSE(m_model->sidebarVisible());
    window.toggleSidebar();
    advance(*frame, 300);
    EXPECT_EQ(find(frame, "sidebar").size(), 1u);

    // A narrow window hides it, shows it over the calendar on demand, and
    // gives it back when wide again.
    frame->resize({780, 640});
    advance(*frame, 300);
    EXPECT_TRUE(find(frame, "sidebar").empty());
    EXPECT_TRUE(m_model->sidebarVisible());
    window.toggleSidebar();
    advance(*frame, 300);
    EXPECT_EQ(find(frame, "sidebar").size(), 1u);
    EXPECT_TRUE(window.sidebarOverlay());
    // A press on the calendar puts it away.
    TestInput::click(*frame, {600, 400});
    advance(*frame, 300);
    EXPECT_FALSE(window.sidebarOverlay());
    EXPECT_TRUE(find(frame, "sidebar").empty());
    window.toggleSidebar();
    advance(*frame, 300);
    frame->resize({1100, 740});
    advance(*frame, 300);
    EXPECT_EQ(window.dockReveal(), 1.0);
    EXPECT_FALSE(window.sidebarOverlay());
}

TEST_F(UiTest, KeysMoveTheDayAndShortcutsSwitchViews)
{
    m_model->setView("month");
    CalendarWindow& window = open();
    Window* frame = window.window();
    const Date start = window.currentDate();
    TestInput::typeKey(*frame, key::Right);
    EXPECT_EQ(window.currentDate(), start.addDays(1));
    TestInput::typeKey(*frame, key::Down);
    EXPECT_EQ(window.currentDate(), start.addDays(8));
    TestInput::typeKey(*frame, key::Home);
    EXPECT_EQ(window.currentDate(), m_model->today());

    TestInput::typeKey(*frame, '2', ControlModifier);
    EXPECT_EQ(window.view(), "week");
    TestInput::typeKey(*frame, key::PageDown);
    EXPECT_EQ(window.currentDate(), m_model->today().addDays(7));
    TestInput::typeKey(*frame, key::Left, AltModifier);
    EXPECT_EQ(window.currentDate(), m_model->today());
    TestInput::typeKey(*frame, '4', ControlModifier);
    EXPECT_EQ(window.view(), "year");
    TestInput::typeKey(*frame, '3', ControlModifier);
    EXPECT_EQ(window.view(), "month");
}

TEST_F(UiTest, SelectedEventIsEditedAndDeletedFromTheKeyboard)
{
    addEvent("Review", Date::currentDate(), 15 * 60, 60);
    m_model->setView("day");
    CalendarWindow& window = open();
    Window* frame = window.window();
    showHour(frame, 14);
    const std::vector<View*> blocks = find(frame, "eventBlock");
    ASSERT_EQ(blocks.size(), 1u);

    TestInput::click(*frame, centreOf(blocks.front()));
    EXPECT_TRUE(window.hasSelection());
    TestInput::typeKey(*frame, key::Return);
    ASSERT_TRUE(window.editor());
    window.editor()->close();
    runPosted();

    TestInput::typeKey(*frame, key::Delete);
    runPosted();
    EXPECT_TRUE(m_store->occurrences(Date::currentDate(), Date::currentDate().addDays(1)).empty());
    EXPECT_TRUE(m_model->canUndo());
    m_model->undo();
    EXPECT_EQ(m_store->occurrences(Date::currentDate(), Date::currentDate().addDays(1)).size(), 1u);

    advance(*frame);
    TestInput::click(*frame, centreOf(find(frame, "eventBlock").front()));
    TestInput::typeKey(*frame, key::Escape);
    EXPECT_FALSE(window.hasSelection());
}

TEST_F(UiTest, DraggingOnTheHoursMarksANewEvent)
{
    m_model->setView("day");
    CalendarWindow& window = open();
    Window* frame = window.window();
    const std::vector<View*> grids = find(frame, "hourGrid");
    ASSERT_EQ(grids.size(), 1u);
    auto* scroll = static_cast<ScrollView*>(grids.front());
    scroll->setScrollPosition({0, 9 * 48});
    advance(*frame);
    View* canvas = scroll->contentView();

    // From 10:00 to 11:30 on the hour grid, 48 px an hour below a 6 px margin.
    const PointF from = canvas->mapToWindow({300, 6 + 10 * 48 + 2});
    const PointF to = canvas->mapToWindow({300, 6 + 11.5 * 48});
    TestInput::press(*frame, from);
    TestInput::movePointer(*frame, {from.x, (from.y + to.y) / 2});
    TestInput::movePointer(*frame, to);
    advance(*frame);
    EXPECT_EQ(find(frame, "newEventPreview").size(), 1u);
    TestInput::release(*frame, to);
    ASSERT_TRUE(window.editor());
    EXPECT_EQ(window.editor()->event().start, DateTime(window.currentDate(), Time(10, 0)));
    EXPECT_EQ(window.editor()->event().end, DateTime(window.currentDate(), Time(11, 30)));
}

TEST_F(UiTest, ANewEventMarkedToTheLastHourEndsAtMidnight)
{
    m_model->setView("day");
    CalendarWindow& window = open();
    Window* frame = window.window();
    showHour(frame, 20);
    View* canvas = static_cast<ScrollView*>(find(frame, "hourGrid").front())->contentView();
    const Date next = window.currentDate().addDays(1);

    const PointF from = canvas->mapToWindow({300, 6 + 23 * 48 + 2});
    const PointF to = canvas->mapToWindow({300, 6 + 24 * 48 + 4});
    TestInput::press(*frame, from);
    TestInput::movePointer(*frame, {from.x, from.y + 30});
    TestInput::movePointer(*frame, to);
    TestInput::release(*frame, to);
    ASSERT_TRUE(window.editor());
    EXPECT_EQ(window.editor()->event().start, DateTime(window.currentDate(), Time(23, 0)));
    EXPECT_EQ(window.editor()->event().end, DateTime(next, Time(0, 0)));
    window.editor()->close();
    runPosted();

    // A double click in the last hour gives it an hour too.
    TestInput::click(*frame, canvas->mapToWindow({300, 6 + 23.5 * 48 + 10}));
    TestInput::click(*frame, canvas->mapToWindow({300, 6 + 23.5 * 48 + 10}));
    ASSERT_TRUE(window.editor());
    EXPECT_EQ(window.editor()->event().start, DateTime(window.currentDate(), Time(23, 0)));
    EXPECT_EQ(window.editor()->event().end, DateTime(next, Time(0, 0)));
}

TEST_F(UiTest, DraggingABlockMovesTheEvent)
{
    addEvent("Review", Date::currentDate(), 15 * 60, 60);
    m_model->setView("day");
    CalendarWindow& window = open();
    Window* frame = window.window();
    showHour(frame, 14);
    View* block = find(frame, "eventBlock").front();
    const PointF start = centreOf(block);
    TestInput::press(*frame, start);
    TestInput::movePointer(*frame, {start.x, start.y + 10});
    TestInput::movePointer(*frame, {start.x, start.y + 48});
    TestInput::release(*frame, {start.x, start.y + 48});
    runPosted();
    const std::vector<Occurrence> today = m_store->occurrences(Date::currentDate(), Date::currentDate().addDays(1));
    ASSERT_EQ(today.size(), 1u);
    EXPECT_EQ(today.front().start, DateTime(Date::currentDate(), Time(16, 0)));
}

TEST_F(UiTest, CalendarRowsShowAndHideCalendars)
{
    CalendarWindow& window = open();
    const std::vector<View*> lists = find(window.window(), "calendarList");
    ASSERT_EQ(lists.size(), 1u);
    View* row = lists.front()->children().front()->children().front().get();
    TestInput::click(*window.window(), centreOf(row));
    EXPECT_FALSE(m_model->calendars().front().visible);
    runPosted();
    advance(*window.window());
    row = lists.front()->children().front()->children().front().get();
    TestInput::click(*window.window(), centreOf(row));
    EXPECT_TRUE(m_model->calendars().front().visible);
}

TEST_F(UiTest, RepeatingEventsAskWhichOccurrences)
{
    const Date today = Date::currentDate();
    const DateTime start(today, Time(9, 0));
    EventForm form = m_model->newEventData(start, start.addSecs(1800), false);
    form.title = "Standup";
    form.repeat = Repeat::Daily;
    ASSERT_TRUE(m_model->saveEvent(form, EventFile::AllEvents));
    CalendarWindow& window = open();

    const std::vector<Occurrence> week = m_store->occurrences(today, today.addDays(3));
    ASSERT_EQ(week.size(), 3u);
    const Occurrence& second = week[0].start < week[1].start ? week[1] : week[0];
    window.deleteEvent(second.key, second.recurrenceKey);
    ScopeDialog* dialog = window.scopeDialog();
    ASSERT_TRUE(dialog);
    advance(*dialog->window());
    const std::vector<View*> only = find(dialog->window(), "thisEventButton");
    ASSERT_EQ(only.size(), 1u);
    TestInput::click(*dialog->window(), centreOf(only.front()));
    runPosted();
    EXPECT_EQ(m_store->occurrences(today, today.addDays(3)).size(), 2u);
    EXPECT_EQ(m_model->undoText(), "Undo Delete");
}

TEST_F(UiTest, WeekNumbersFollowTheSetting)
{
    m_model->setView("month");
    CalendarWindow& window = open();
    const auto numbers = [&] {
        size_t count = 0;
        for (View* month : find(window.window(), "monthView")) {
            for (const auto& child : month->children()) {
                auto* label = dynamic_cast<Label*>(child.get());
                if (label && label->isVisible() && label->geometry().x == 0 && label->geometry().width == 28)
                    ++count;
            }
        }
        return count;
    };
    EXPECT_EQ(numbers(), 0u);
    m_model->setShowWeekNumbers(true);
    advance(*window.window());
    EXPECT_EQ(numbers(), 6u);
}

TEST_F(UiTest, MiniMonthAndYearViewMoveTheDay)
{
    m_model->setView("year");
    CalendarWindow& window = open();
    Window* frame = window.window();
    // A double click on a day of the year opens the day.
    const std::vector<View*> years = find(frame, "yearView");
    ASSERT_EQ(years.size(), 1u);
    window.setCurrentDate(Date(window.currentDate().year(), 3, 1));
    window.showMonth(Date(window.currentDate().year(), 6, 15));
    EXPECT_EQ(window.view(), "month");
    EXPECT_EQ(window.currentDate(), Date(window.currentDate().year(), 6, 15));

    // The sidebar's month follows, and a click there selects the day.
    const std::vector<View*> cards = find(frame, "miniMonth");
    ASSERT_EQ(cards.size(), 1u);
    advance(*frame);
    window.setCurrentDate(m_model->today());
    advance(*frame);
    View* days = cards.front()->children().front()->children().back().get();
    // The last row's middle cell lies in the next month or late in this one.
    TestInput::click(*frame, days->mapToWindow({days->geometry().width / 2, days->geometry().height - 5}));
    EXPECT_NE(window.currentDate(), m_model->today());
}

TEST_F(UiTest, NewCalendarIsCreatedFromTheSidebar)
{
    CalendarWindow& window = open();
    const std::vector<View*> buttons = find(window.window(), "addCalendar");
    ASSERT_EQ(buttons.size(), 1u);
    TestInput::click(*window.window(), centreOf(buttons.front()));
    CalendarDialog* dialog = window.calendarDialog();
    ASSERT_TRUE(dialog);
    advance(*dialog->window());
    TestInput::typeText(*dialog->window(), "Work");
    const std::vector<View*> saves = find(dialog->window(), "calendarSave");
    ASSERT_EQ(saves.size(), 1u);
    TestInput::click(*dialog->window(), centreOf(saves.front()));
    runPosted();
    ASSERT_EQ(m_model->calendars().size(), 2u);
    EXPECT_EQ(m_model->calendars().back().name, "Work");
    EXPECT_FALSE(window.calendarDialog());
}

namespace {

// A drag of files from another application.
class FileOffer : public DropOffer
{
public:
    explicit FileOffer(std::string uriList)
        : m_uriList(std::move(uriList))
    {
    }

    const std::vector<std::string>& mimeTypes() const override { return m_types; }
    uint32_t sourceActions() const override { return CopyAction; }
    void read(const std::string&, std::function<void(std::string data)> done) override { done(m_uriList); }

private:
    std::vector<std::string> m_types{"text/uri-list"};
    std::string m_uriList;
};

}

TEST_F(UiTest, DroppedCalendarFilesAreImported)
{
    const std::string file = m_dir + "/trip.ics";
    FILE* out = fopen(file.c_str(), "w");
    ASSERT_TRUE(out);
    fputs("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:trip-1\r\nDTSTAMP:20261001T000000Z\r\n"
          "SUMMARY:Trip\r\nDTSTART;VALUE=DATE:20261020\r\nDTEND;VALUE=DATE:20261021\r\nEND:VEVENT\r\n"
          "END:VCALENDAR\r\n",
          out);
    fclose(out);

    CalendarWindow& window = open();
    Window* frame = window.window();
    // Only calendar files are taken.
    FileOffer other("file:///tmp/notes.txt\r\n");
    EXPECT_EQ(TestInput::dragEnter(*frame, {600, 400}, &other), uint32_t(NoAction));
    EXPECT_EQ(TestInput::drop(*frame), uint32_t(NoAction));
    runPosted();
    EXPECT_FALSE(window.importDialog());

    FileOffer files("# dropped\r\nfile://" + file + "\r\n");
    EXPECT_EQ(TestInput::dragEnter(*frame, {600, 400}, &files), uint32_t(CopyAction));
    EXPECT_EQ(TestInput::drop(*frame), uint32_t(CopyAction));
    runPosted();
    ImportDialog* dialog = window.importDialog();
    ASSERT_TRUE(dialog);
    advance(*dialog->window());
    const std::vector<View*> imports = find(dialog->window(), "importButton");
    ASSERT_EQ(imports.size(), 1u);
    TestInput::click(*dialog->window(), centreOf(imports.front()));
    runPosted();
    const Date day(2026, 10, 20);
    const std::vector<Occurrence> found = m_store->occurrences(day, day.addDays(1));
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found.front().summary, "Trip");
}

TEST(EventDialogTest, ReadsTypedTimes)
{
    EXPECT_EQ(EventDialog::parseTime("9", -1), 9 * 60);
    EXPECT_EQ(EventDialog::parseTime("930", -1), 9 * 60 + 30);
    EXPECT_EQ(EventDialog::parseTime("9:30", -1), 9 * 60 + 30);
    EXPECT_EQ(EventDialog::parseTime(" 21:30 ", -1), 21 * 60 + 30);
    EXPECT_EQ(EventDialog::parseTime("9:30 PM", -1), 21 * 60 + 30);
    EXPECT_EQ(EventDialog::parseTime("9pm", -1), 21 * 60);
    EXPECT_EQ(EventDialog::parseTime("12am", -1), 0);
    EXPECT_EQ(EventDialog::parseTime("12 pm", -1), 12 * 60);
    EXPECT_EQ(EventDialog::parseTime("\u4e0b\u53483:15", -1), -1);
    EXPECT_EQ(EventDialog::parseTime("3:15\u4e0b\u5348", -1), 15 * 60 + 15);
    EXPECT_EQ(EventDialog::parseTime("24:00", 42), 42);
    EXPECT_EQ(EventDialog::parseTime("noon", 42), 42);
}

TEST_F(UiTest, TypedTimesMoveTheEvent)
{
    CalendarWindow& window = open();
    const DateTime start(Date::currentDate(), Time(14, 0));
    window.newEvent(start, start.addSecs(3600), false);
    EventDialog* editor = window.editor();
    ASSERT_TRUE(editor);
    Window* dialog = editor->window();
    advance(*dialog);
    const std::vector<View*> boxes = find(dialog, "startTime");
    ASSERT_EQ(boxes.size(), 1u);
    auto* box = static_cast<ComboBox*>(boxes.front());
    ASSERT_TRUE(box->isEditable());
    box->field()->setFocus();
    box->field()->selectAll();
    TestInput::typeText(*dialog, "9:40pm");
    TestInput::typeKey(*dialog, key::Return);
    // The editor keeps the hour's length and shows the time it read.
    EXPECT_EQ(box->editText(), LocaleData::timeText(Time(21, 40)));
    EXPECT_EQ(box->currentIndex(), -1);
    EXPECT_EQ(static_cast<ComboBox*>(find(dialog, "endTime").front())->editText(), LocaleData::timeText(Time(22, 40)));
}

TEST_F(UiTest, EscapeClosesTheDatePickerBeforeTheEditor)
{
    CalendarWindow& window = open();
    const DateTime start(Date::currentDate(), Time(14, 0));
    window.newEvent(start, start.addSecs(3600), false);
    EventDialog* editor = window.editor();
    ASSERT_TRUE(editor);
    Window* dialog = editor->window();
    advance(*dialog);
    const std::vector<View*> dates = find(dialog, "startDate");
    ASSERT_EQ(dates.size(), 1u);
    auto* date = static_cast<DateButton*>(dates.front());
    TestInput::click(*dialog, centreOf(date));
    EXPECT_TRUE(date->isPopupOpen());
    TestInput::typeKey(*dialog, key::Escape);
    runPosted();
    EXPECT_FALSE(date->isPopupOpen());
    ASSERT_TRUE(window.editor());
    TestInput::typeKey(*dialog, key::Escape);
    runPosted();
    EXPECT_FALSE(window.editor());
}

TEST_F(UiTest, WindowShowsItsMenusInTheStatusBar)
{
    CalendarWindow& window = open();
    Menu* menu = window.window()->mainMenu();
    ASSERT_TRUE(menu);
    const std::vector<MenuRole> roles{MenuRole::Application, MenuRole::None, MenuRole::None, MenuRole::None,
                                      MenuRole::Windows};
    for (size_t i = 0; i < roles.size(); ++i) {
        ASSERT_TRUE(menu->submenu(i)) << i;
        EXPECT_EQ(menu->submenu(i)->role(), roles[i]) << i;
    }
    EXPECT_FALSE(menu->submenu(roles.size()));

    Application::instance()->languageChanged.emit();
    advance(*window.window(), 300);
    ASSERT_TRUE(window.window()->mainMenu());
    EXPECT_EQ(window.window()->mainMenu()->submenu(0)->role(), MenuRole::Application);
}

TEST_F(UiTest, LanguageChangeKeepsWhereTheUserWas)
{
    const Date day = Date::currentDate().addDays(3);
    addEvent("Piano lesson", day, 9 * 60, 60);
    m_model->setView("week");
    CalendarWindow& window = open();
    Window* frame = window.window();
    const std::vector<EventItem> found = m_model->search("Piano");
    ASSERT_EQ(found.size(), 1u);
    window.revealEvent(found.front());
    window.toggleSidebar();
    auto* search = static_cast<TextField*>(find(frame, "searchField").front());
    search->setText("piano");
    search->onTextChanged();
    const DateTime start(day, Time(14, 0));
    window.newEvent(start, start.addSecs(3600), false);
    EventDialog* editor = window.editor();
    ASSERT_TRUE(editor);
    advance(*editor->window());
    const View* titleBefore = find(editor->window(), "titleField").front();
    static_cast<TextField*>(find(editor->window(), "titleField").front())->setText("Dentist");
    advance(*frame, 300);
    const View* searchBefore = find(frame, "searchField").front();

    Application::instance()->languageChanged.emit();
    advance(*frame, 300);
    ASSERT_EQ(find(frame, "searchField").size(), 1u);
    EXPECT_NE(find(frame, "searchField").front(), searchBefore);
    EXPECT_EQ(window.view(), "week");
    EXPECT_EQ(window.currentDate(), day);
    EXPECT_TRUE(window.isSelected(found.front()));
    EXPECT_FALSE(m_model->sidebarVisible());
    EXPECT_TRUE(find(frame, "sidebar").empty());
    EXPECT_EQ(static_cast<TextField*>(find(frame, "searchField").front())->text(), "piano");
    const std::vector<View*> panels = find(frame, "searchResults");
    ASSERT_EQ(panels.size(), 1u);
    EXPECT_EQ(static_cast<SearchResults*>(panels.front())->results().size(), 1u);

    // The editor stays open with what was typed, in views made again.
    ASSERT_EQ(window.editor(), editor);
    advance(*editor->window());
    const std::vector<View*> titles = find(editor->window(), "titleField");
    ASSERT_EQ(titles.size(), 1u);
    EXPECT_NE(titles.front(), titleBefore);
    EXPECT_EQ(static_cast<TextField*>(titles.front())->text(), "Dentist");
    editor->save();
    runPosted();
    EXPECT_FALSE(window.editor());
    const std::vector<Occurrence> saved = m_store->occurrences(day, day.addDays(1));
    ASSERT_EQ(saved.size(), 2u);
    EXPECT_TRUE(saved[0].summary == "Dentist" || saved[1].summary == "Dentist");
}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    Application application(argc, argv, Application::Platform::Headless);
    registerCalendarGlyphs();
    return RUN_ALL_TESTS();
}
