// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "eventdialog.h"
#include "dates.h"
#include "dialogs.h"
#include "localedata.h"
#include "style.h"
#include "text.h"

#include <base/i18n.h>
#include <ui/cell_layout.h>
#include <ui/combo_box.h>
#include <ui/dialog_window.h>
#include <ui/keys.h>
#include <ui/label.h>
#include <ui/layout.h>
#include <ui/painter.h>
#include <ui/switch.h>
#include <ui/text_area.h>
#include <ui/text_field.h>
#include <ui/window.h>

#include <algorithm>
#include <cctype>
#include <regex>

using namespace cutefish::ui;
using cutefish::i18n::tr;
using cutefish::i18n::trn;

static std::string timeText(int minute)
{
    return LocaleData::timeText(Time(minute / 60, minute % 60));
}

static std::vector<std::string> times()
{
    std::vector<std::string> list;
    for (int minute = 0; minute < 24 * 60; minute += 15)
        list.push_back(timeText(minute));
    return list;
}

int EventDialog::parseTime(const std::string& text, int fallback)
{
    // Typed times: "9", "930", "9:30", "21:30", "9:30 pm", "9pm"; the
    // morning and afternoon words of Chinese too. Seconds, which the C
    // locale's times carry, are dropped.
    static const std::regex pattern(
        "^(\\d{1,2})(?:[:.h]?(\\d{2})(?::\\d{2})?)?\\s*(a|p|am|pm|\u4e0a\u5348|\u4e0b\u5348)?$");
    // Time formats put a narrow or no-break space before am and pm.
    std::string lower = text;
    for (const std::string space : {"\u202f", "\u00a0", "\u2009"}) {
        for (size_t at = lower.find(space); at != std::string::npos; at = lower.find(space, at))
            lower.replace(at, space.size(), " ");
    }
    lower = trimmed(lower);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    std::smatch match;
    if (!std::regex_match(lower, match, pattern))
        return fallback;
    int hour = std::stoi(match[1].str());
    const int minute = match[2].matched ? std::stoi(match[2].str()) : 0;
    const std::string meridiem = match[3].str();
    if (meridiem.starts_with('p') || meridiem == "\u4e0b\u5348") {
        if (hour < 12)
            hour += 12;
    } else if ((meridiem.starts_with('a') || meridiem == "\u4e0a\u5348") && hour == 12) {
        hour = 0;
    }
    if (hour > 23 || minute > 59)
        return fallback;
    return hour * 60 + minute;
}

// A single line of the editor's fields, without a frame, the card being one.
static TextField* plainField(FormCard* card, const std::string& text, const std::string& placeholder)
{
    auto field = std::make_unique<TextField>(text);
    TextField* pointer = field.get();
    field->setPlaceholder(placeholder);
    field->setName("formField");
    field->setFrameVisible(false);
    field->setTextMargins(14, 14);
    card->addWide(std::move(field), 44);
    return pointer;
}

static std::string dialogTitle(const EventForm& event)
{
    return event.key.empty() ? tr("New Event") : event.readOnly ? tr("Event") : tr("Edit Event");
}

EventDialog::EventDialog(Window* parent, CalendarModel* model, EventForm event)
    : m_model(model)
    , m_event(std::move(event))
    , m_dialog(std::make_unique<DialogWindow>(parent, dialogTitle(m_event), 480))
{
    // Cancel or Close closes it.
    m_dialog->setCloseVisible(false);
    m_allDay = m_event.allDay;
    m_startDate = m_event.start.date();
    m_endDate = m_event.end.date();
    m_startMinute = m_event.allDay ? 9 * 60 : Dates::minuteOf(m_event.start);
    m_endMinute = m_event.allDay ? 10 * 60 : Dates::minuteOf(m_event.end);
    m_repeat = m_event.repeat;
    m_repeatEnd = m_event.repeatEnd;
    m_repeatUntil = m_event.repeatUntil;
    m_repeatCount = m_event.repeatCount;
    m_alarmIndex = alarmIndexFor(m_event.alarm, m_event.alarmOffset);
    buildContent();

    // Escape puts away an open date picker first.
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] {
        for (DateButton* button : {m_startDateButton, m_endDateButton, m_untilButton}) {
            if (button->isPopupOpen()) {
                button->dismiss();
                return;
            }
        }
        close();
    });
    m_dialog->window()->addShortcut(ControlModifier, key::Return, [this] { save(); });
    m_dialog->window()->addShortcut(ControlModifier, key::KeypadEnter, [this] { save(); });
    m_dialog->onClosed = [this] {
        if (onClosed)
            onClosed();
    };
}

EventDialog::~EventDialog() = default;

void EventDialog::buildContent()
{
    // Layouts, which round sizes up and positions to whole pixels.
    auto content = std::make_unique<ColumnLayout>();
    content->setName("eventDialog");
    content->setSpacing(12);
    content->setPadding({0, 0, 0, 8});

    FormCard* names = content->addChild<FormCard>();
    names->setLayoutHints({.fillWidth = true});
    m_title = plainField(names, m_event.title, tr("Title"));
    m_title->setName("titleField");
    m_title->setFontSize(Style::points(11));
    m_title->setFontWeight(500);
    auto separator = std::make_unique<View>();
    separator->setName("separator");
    names->addWide(std::move(separator), 0);
    m_location = plainField(names, m_event.location, tr("Location"));
    m_location->setName("locationField");
    m_title->onAccepted = [this] { save(); };
    m_location->onAccepted = [this] { save(); };

    FormCard* timing = content->addChild<FormCard>();
    timing->setLayoutHints({.fillWidth = true});
    m_allDaySwitch = timing->addRow(tr("All Day"))->addChild<Switch>();
    m_allDaySwitch->setName("allDaySwitch");
    m_allDaySwitch->setChecked(m_allDay);
    m_allDaySwitch->onToggled = [this](bool checked) {
        m_allDay = checked;
        m_alarmIndex = m_allDay == m_event.allDay ? alarmIndexFor(m_event.alarm, m_event.alarmOffset) : 0;
        update();
    };

    View* starts = timing->addRow(tr("Starts"));
    m_startDateButton = starts->addChild<DateButton>(m_model, m_startDate);
    m_startDateButton->setName("startDate");
    m_startDateButton->onPicked = [this](Date date) { setStart(date, m_startMinute); };
    m_startTime = starts->addChild<ComboBox>(times());
    m_startTime->setName("startTime");
    setUpTimeBox(m_startTime, [this] { return m_startMinute; }, [this](int minute) { setStart(m_startDate, minute); });

    View* ends = timing->addRow(tr("Ends"));
    m_endDateButton = ends->addChild<DateButton>(m_model, m_endDate);
    m_endDateButton->setName("endDate");
    m_endDateButton->onPicked = [this](Date date) {
        m_endDate = date;
        update();
    };
    m_endTime = ends->addChild<ComboBox>(times());
    m_endTime->setName("endTime");
    setUpTimeBox(m_endTime, [this] { return m_endMinute; }, [this](int minute) {
        m_endMinute = minute;
        update();
    });

    // Up against the card above.
    m_error = content->addChild<Label>(tr("The event ends before it starts."));
    m_error->setFontSize(Style::points(8));
    m_error->setLayoutHints({.fillWidth = true, .margins = {14, -6, 0, 0}});

    m_repeatCard = content->addChild<FormCard>();
    m_repeatCard->setLayoutHints({.fillWidth = true});
    // Custom only for a rule made elsewhere.
    std::vector<std::string> repeats{tr("Never"), tr("Every Day"), tr("Every Weekday"), tr("Every Week"),
                                     tr("Every Month"), tr("Every Year")};
    m_repeatValues = {0, 1, 2, 3, 4, 5};
    if (m_event.repeat == Repeat::Custom) {
        repeats.push_back(tr("Custom"));
        m_repeatValues.push_back(Repeat::Custom);
    }
    m_repeatBox = m_repeatCard->addRow(tr("Repeat"))->addChild<ComboBox>(repeats);
    m_repeatBox->setName("repeatBox");
    m_repeatBox->setLayoutHints({.preferredWidth = 200});
    m_repeatBox->onActivated = [this](int index) {
        m_repeat = m_repeatValues[size_t(index)];
        update();
    };

    m_repeatEndRow = m_repeatCard->addRow(tr("End Repeat"));
    m_repeatEndBox = m_repeatEndRow->addChild<ComboBox>(std::vector<std::string>{tr("Never"), tr("On Date"),
                                                                                 // TRANSLATORS: as in "After 3 times".
                                                                                 tr("After")});
    m_repeatEndBox->setName("repeatEndBox");
    m_repeatEndBox->onActivated = [this](int index) {
        m_repeatEnd = index;
        update();
    };
    m_untilButton = m_repeatEndRow->addChild<DateButton>(m_model, m_repeatUntil);
    m_untilButton->onPicked = [this](Date date) {
        m_repeatUntil = date;
        update();
    };
    m_countField = m_repeatEndRow->addChild<TextField>(std::to_string(m_repeatCount));
    m_countField->setName("repeatCountField");
    m_countField->setLayoutHints({.preferredWidth = 56});
    m_countField->setHorizontallyCentered(true);
    // As an integer validator from 1 to 999.
    m_countField->setValidator([](const std::string& text) {
        return text.size() <= 3 && std::all_of(text.begin(), text.end(), ::isdigit);
    });
    m_countField->onTextEdited = [this] {
        const std::string text = trimmed(m_countField->text());
        if (!text.empty() && text.size() <= 3 && std::all_of(text.begin(), text.end(), ::isdigit) && std::stoi(text) >= 1) {
            m_repeatCount = std::stoi(text);
            update();
        }
    };
    m_timesLabel = m_repeatEndRow->addChild<Label>();

    m_alarmBox = m_repeatCard->addRow(tr("Alert"))->addChild<ComboBox>();
    m_alarmBox->setName("alarmBox");
    m_alarmBox->setLayoutHints({.preferredWidth = 200});
    m_alarmBox->onActivated = [this](int index) {
        m_alarmIndex = index;
        update();
    };

    std::vector<std::string> calendars;
    int calendarIndex = 0;
    const std::vector<CalendarItem> items = m_event.readOnly ? m_model->calendars() : m_model->writableCalendars();
    for (size_t i = 0; i < items.size(); ++i) {
        calendars.push_back(items[i].name);
        if (items[i].id == m_event.calendarId)
            calendarIndex = int(i);
    }
    m_calendarBox = m_repeatCard->addRow(tr("Calendar"))->addChild<ComboBox>(calendars);
    m_calendarBox->setName("calendarBox");
    m_calendarBox->setLayoutHints({.preferredWidth = 200});
    m_calendarBox->setCurrentIndex(calendarIndex);

    FormCard* notes = content->addChild<FormCard>();
    notes->setLayoutHints({.fillWidth = true});
    auto notesArea = std::make_unique<TextArea>(m_event.notes);
    m_notes = notesArea.get();
    m_notes->setName("notesField");
    m_notes->setPlaceholder(tr("Notes"));
    m_notes->setWrapMode(TextArea::WrapMode::Wrap);
    m_notes->setFrameVisible(false);
    m_notes->setPadding({14, 12, 14, 12});
    notes->addWide(std::move(notesArea), 84);

    RowLayout* actions = content->addChild<RowLayout>();
    actions->setSpacing(8);
    actions->setLayoutHints({.fillWidth = true, .margins = {0, 4, 0, 0}});
    if (m_event.readOnly) {
        // A subscription's event: shown as it is, with nothing to save.
        for (TextField* field : {m_title, m_location})
            field->setReadOnly(true);
        m_notes->setReadOnly(true);
        for (View* control : std::initializer_list<View*>{m_allDaySwitch, m_startDateButton, m_startTime,
                                                          m_endDateButton, m_endTime, m_repeatBox, m_repeatEndBox,
                                                          m_untilButton, m_countField, m_alarmBox, m_calendarBox})
            control->setEnabled(false);
        actions->addChild<View>()->setLayoutHints({.fillWidth = true});
        DialogButton* done = actions->addChild<DialogButton>(tr("Close"), true);
        done->setName("closeButton");
        done->onClicked = [this] { close(); };
    } else if (!isNew()) {
        DialogButton* remove = actions->addChild<DialogButton>(tr("Delete Event"));
        remove->setName("deleteButton");
        remove->onClicked = [this] {
            if (onDeleteRequested)
                onDeleteRequested();
        };
    }
    if (!m_event.readOnly) {
        actions->addChild<View>()->setLayoutHints({.fillWidth = true});
        actions->addChild<DialogButton>(tr("Cancel"))->onClicked = [this] { close(); };
        m_saveButton = actions->addChild<DialogButton>(isNew() ? tr("Add") : tr("Save"), true);
        m_saveButton->setName("saveButton");
        m_saveButton->onClicked = [this] { save(); };
    }

    m_dialog->setContentView(std::move(content));
    update();
}

void EventDialog::retranslate()
{
    // Times being typed are read first, as leaving their boxes would.
    for (ComboBox* box : {m_startTime, m_endTime}) {
        if (box->onAccepted)
            box->onAccepted();
    }
    const std::string title = m_title->text();
    const std::string location = m_location->text();
    const std::string notes = m_notes->text();
    const std::string count = m_countField->text();
    const int calendar = m_calendarBox->currentIndex();
    const auto fields = [this] { return std::vector<View*>{m_title, m_location, m_notes, m_countField}; };
    const std::vector<View*> before = fields();
    const size_t focused = size_t(std::find_if(before.begin(), before.end(), [](View* field) {
                                      return field->hasFocus();
                                  }) - before.begin());

    m_dialog->setTitle(dialogTitle(m_event));
    m_shownStartMinute = -1;
    m_shownEndMinute = -1;
    buildContent();
    m_title->setText(title);
    m_location->setText(location);
    m_notes->setText(notes);
    m_countField->setText(count);
    m_calendarBox->setCurrentIndex(calendar);
    if (focused < before.size())
        fields()[focused]->setFocus();
}

// Typing replaces the text; it shows the time again once read.
void EventDialog::setUpTimeBox(ComboBox* box, std::function<int()> minute, std::function<void(int)> changed)
{
    box->setEditable(true);
    box->setLayoutHints({.preferredWidth = 110});
    const auto commit = [box, minute, changed](int value) {
        changed(value);
        showTime(box, minute());
    };
    box->onActivated = [commit](int index) { commit(index * 15); };
    box->onAccepted = [box, minute, commit] { commit(parseTime(box->editText(), minute())); };
    box->onFocusLost = box->onAccepted;
}

void EventDialog::showTime(ComboBox* box, int minute)
{
    box->setCurrentIndex(minute % 15 == 0 ? minute / 15 : -1);
    box->setEditText(timeText(minute));
}

Window* EventDialog::window() const
{
    return m_dialog->window();
}

void EventDialog::show()
{
    m_dialog->show();
    if (!m_event.readOnly)
        m_title->setFocus();
}

void EventDialog::close()
{
    m_dialog->close();
}

DateTime EventDialog::start() const
{
    return m_allDay ? DateTime(m_startDate, Time(0, 0)) : Dates::atMinute(m_startDate, m_startMinute);
}

DateTime EventDialog::end() const
{
    return m_allDay ? DateTime(m_endDate, Time(0, 0)) : Dates::atMinute(m_endDate, m_endMinute);
}

// Alarm choices, in minutes from the start; all-day ones count from midnight.
std::vector<EventDialog::Alarm> EventDialog::alarmChoices() const
{
    std::vector<Alarm> list{{tr("None"), 0, 0}};
    if (m_allDay) {
        list.push_back({tr("On the Day (9:00)"), 1, 9 * 60});
        list.push_back({trn("{n} Day Before (9:00)", "{n} Days Before (9:00)", 1), 1, -15 * 60});
        list.push_back({trn("{n} Day Before (9:00)", "{n} Days Before (9:00)", 2), 1, -(24 + 15) * 60});
        list.push_back({trn("{n} Week Before (9:00)", "{n} Weeks Before (9:00)", 1), 1, -(6 * 24 + 15) * 60});
    } else {
        list.push_back({tr("At Time of Event"), 1, 0});
        list.push_back({trn("{n} Minute Before", "{n} Minutes Before", 5), 1, -5});
        list.push_back({trn("{n} Minute Before", "{n} Minutes Before", 10), 1, -10});
        list.push_back({trn("{n} Minute Before", "{n} Minutes Before", 15), 1, -15});
        list.push_back({trn("{n} Minute Before", "{n} Minutes Before", 30), 1, -30});
        list.push_back({trn("{n} Hour Before", "{n} Hours Before", 1), 1, -60});
        list.push_back({trn("{n} Hour Before", "{n} Hours Before", 2), 1, -120});
        list.push_back({trn("{n} Day Before", "{n} Days Before", 1), 1, -24 * 60});
        list.push_back({trn("{n} Day Before", "{n} Days Before", 2), 1, -48 * 60});
        list.push_back({trn("{n} Week Before", "{n} Weeks Before", 1), 1, -7 * 24 * 60});
    }
    const bool kept = std::any_of(list.begin(), list.end(), [this](const Alarm& choice) {
        return choice.alarm == m_event.alarm && choice.offset == m_event.alarmOffset;
    });
    if (m_event.alarm == 2 || (m_event.alarm == 1 && m_event.allDay == m_allDay && !kept))
        list.push_back({tr("Custom"), m_event.alarm, m_event.alarmOffset});
    return list;
}

int EventDialog::alarmIndexFor(int alarm, int offset) const
{
    const std::vector<Alarm> choices = alarmChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].alarm == alarm && choices[i].offset == offset)
            return int(i);
    }
    return 0;
}

void EventDialog::setStart(Date date, int minute)
{
    const int64_t length = start().secsTo(end());
    m_startDate = date;
    m_startMinute = minute;
    const DateTime moved = start().addSecs(length);
    m_endDate = moved.date();
    m_endMinute = Dates::minuteOf(moved);
    update();
}

void EventDialog::update()
{
    m_startDateButton->setDate(m_startDate);
    m_endDateButton->setDate(m_endDate);
    m_startTime->setVisible(!m_allDay);
    m_endTime->setVisible(!m_allDay);
    // Only when the time changed, so text being typed stays.
    if (m_startMinute != m_shownStartMinute) {
        m_shownStartMinute = m_startMinute;
        showTime(m_startTime, m_startMinute);
    }
    if (m_endMinute != m_shownEndMinute) {
        m_shownEndMinute = m_endMinute;
        showTime(m_endTime, m_endMinute);
    }

    const bool valid = isValid();
    m_error->setVisible(!valid);
    m_error->setColor(Style::red());
    if (m_saveButton)
        m_saveButton->setEnabled(valid);

    m_repeatBox->setCurrentIndex(std::max(0, int(std::find(m_repeatValues.begin(), m_repeatValues.end(), m_repeat)
                                                 - m_repeatValues.begin()) % int(m_repeatValues.size())));
    m_repeatCard->setRowVisible(m_repeatEndBox, m_repeat != Repeat::None && m_repeat != Repeat::Custom);
    m_repeatEndBox->setCurrentIndex(m_repeatEnd);
    m_repeatEndBox->setLayoutHints({.preferredWidth = m_repeatEnd == 0 ? 200.0 : 110.0});
    m_untilButton->setVisible(m_repeatEnd == 1);
    m_untilButton->setDate(m_repeatUntil);
    m_countField->setVisible(m_repeatEnd == 2);
    m_timesLabel->setVisible(m_repeatEnd == 2);
    // TRANSLATORS: after "After" and a number field, as in "After 3 times".
    m_timesLabel->setText(trn("time", "times", m_repeatCount));

    std::vector<std::string> alarms;
    for (const Alarm& choice : alarmChoices())
        alarms.push_back(choice.text);
    if (alarms != m_alarmBox->items())
        m_alarmBox->setItems(alarms);
    m_alarmBox->setCurrentIndex(m_alarmIndex);
}

void EventDialog::save()
{
    if (m_event.readOnly || !isValid())
        return;
    EventForm form = m_event;
    form.title = m_title->text();
    form.location = m_location->text();
    form.notes = m_notes->text();
    form.allDay = m_allDay;
    form.start = start();
    form.end = end();
    form.repeat = m_repeat;
    form.repeatEnd = m_repeatEnd;
    form.repeatUntil = m_repeatUntil;
    form.repeatCount = m_repeatCount;
    const std::vector<Alarm> choices = alarmChoices();
    const Alarm& choice = choices[size_t(std::clamp(m_alarmIndex, 0, int(choices.size()) - 1))];
    form.alarm = choice.alarm;
    form.alarmOffset = choice.offset;
    const std::vector<CalendarItem> calendars = m_model->writableCalendars();
    const int index = m_calendarBox->currentIndex();
    if (index >= 0 && size_t(index) < calendars.size())
        form.calendarId = calendars[size_t(index)].id;
    if (onSaveRequested)
        onSaveRequested(form);
}
