// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "calendarwindow.h"
#include "calendarroot.h"
#include "dates.h"
#include "dialogs.h"
#include "eventdialog.h"
#include "headerbar.h"
#include "localedata.h"
#include "sidebar.h"
#include "timegridview.h"

#include <base/event_loop.h>
#include <base/i18n.h>
#include <ui/about_dialog.h>
#include <ui/application.h>
#include <ui/dialog_window.h>
#include <ui/file_chooser.h>
#include <ui/keys.h>
#include <ui/menu.h>
#include <ui/search_field.h>
#include <ui/toast.h>
#include <ui/window.h>

#include <algorithm>
#include <cmath>

using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;
using cutefish::i18n::trn;

static const std::vector<std::string> Views{"day", "week", "month", "year"};

// Deletes the object from the event loop, after the event being handled,
// which may still run in it.
template<typename T>
static void deleteLater(std::unique_ptr<T>& object)
{
    if (!object)
        return;
    std::shared_ptr<T> doomed(std::move(object));
    Application::instance()->loop().post([doomed] {});
}

CalendarWindow::CalendarWindow(CalendarModel* model, bool restoresSize)
    : m_model(model)
    , m_window(std::make_unique<Window>())
    , m_currentDate(model->today())
{
    m_window->setMinimumSize({640, 600});
    m_window->resize({1100, 740});
    if (restoresSize)
        m_window->setRestoresSize("Main");
    m_wasNarrow = narrow();
    m_sidebarReveal = sidebarDocked() ? 1 : 0;
    m_dockReveal = sidebarDocked() ? 1 : 0;

    buildContent();
    m_toast = std::make_unique<Toast>(m_window.get());
    m_window->onStatesChanged = [this] { m_root->headerBar()->requestLayout(); };
    m_window->onActiveChanged = [this] { m_root->headerBar()->requestLayout(); };
    // A press on the calendar puts away the sidebar shown over it.
    m_window->onPointerPressed = [this](PointF position, MouseButton) {
        const double sidebarEnd = SidebarWidth * m_sidebarReveal;
        if (m_sidebarOverlay && position.y >= HeaderHeight && position.x >= sidebarEnd)
            setSidebarOverlay(false);
    };

    m_revealAnimation = std::make_unique<Animation>(m_window.get());
    m_revealAnimation->setDuration(std::chrono::milliseconds(200));
    m_revealAnimation->setEasing(Animation::Easing::OutCubic);
    m_revealAnimation->onUpdate = [this](double value) {
        m_sidebarReveal = m_revealFrom + (m_revealTo - m_revealFrom) * value;
        m_root->requestLayout();
        m_root->requestPaint();
    };
    m_dockAnimation = std::make_unique<Animation>(m_window.get());
    m_dockAnimation->setDuration(std::chrono::milliseconds(200));
    m_dockAnimation->setEasing(Animation::Easing::OutCubic);
    m_dockAnimation->onUpdate = [this](double value) {
        m_dockReveal = m_dockFrom + (m_dockTo - m_dockFrom) * value;
        m_root->requestLayout();
        m_root->requestPaint();
    };

    m_subscriptions.push_back(model->viewChanged.connect([this] {
        m_root->content()->showView(view());
        viewChanged.emit();
    }));
    m_subscriptions.push_back(model->sidebarVisibleChanged.connect([this] { updateSidebarState(true); }));
    m_subscriptions.push_back(model->dateRequested.connect([this](Date date) {
        setCurrentDate(date);
        if (view() == "year")
            setView("day");
    }));
    m_subscriptions.push_back(model->importRequested.connect([this](const std::vector<std::string>& files) {
        importFiles(files);
    }));
    m_subscriptions.push_back(model->activationRequested.connect([this] { activate(); }));
    m_subscriptions.push_back(model->weekStartChanged.connect([this] { currentDateChanged.emit(); }));
    m_languageChanged = Application::instance()->languageChanged.connect([this] { retranslate(); });
    installShortcuts();
    buildMainMenu();
}

CalendarWindow::~CalendarWindow() = default;

void CalendarWindow::buildContent()
{
    m_window->setTitle(tr("Calendar"));
    auto root = std::make_unique<CalendarRoot>(this);
    m_root = root.get();
    m_window->setContentView(std::move(root));
    m_root->content()->showView(view());
    m_root->headerBar()->onSearchChanged = [this](const std::string& text) {
        m_root->content()->setSearchText(text);
    };
}

// The views are made again; the day, the view, the selection, the sidebar,
// the search and an open event editor with its input stay. Questions close,
// and the dialogs being typed into stay as they are.
void CalendarWindow::retranslate()
{
    SearchField* search = m_root->headerBar()->searchField();
    const std::string searchText = search->text();
    const bool searchFocused = search->hasFocus();
    const bool viewFocused = m_root->content()->hasFocus();
    const Date shownMonth = m_root->sidebar()->shownMonth();
    std::optional<double> scrollTop;
    if (view() == "day" || view() == "week")
        scrollTop = static_cast<TimeGridView*>(m_root->content()->currentView())->scrollTop();

    m_contextMenu.reset();
    if (m_scopeDialog)
        m_scopeDialog->close();
    m_confirmDialog.reset();

    buildContent();
    buildMainMenu();
    search = m_root->headerBar()->searchField();
    search->setText(searchText);
    m_root->content()->setSearchText(searchText);
    m_root->sidebar()->setShownMonth(shownMonth);
    if (scrollTop)
        static_cast<TimeGridView*>(m_root->content()->currentView())->setScrollTop(*scrollTop);
    if (searchFocused)
        search->setFocus(FocusReason::Keyboard);
    else if (viewFocused)
        focusView();

    if (m_editor)
        m_editor->retranslate();
    if (m_importDialog) {
        const std::vector<std::string> files = m_importDialog->files();
        const int calendar = m_importDialog->calendarIndex();
        m_importDialog.reset();
        importFiles(files);
        m_importDialog->setCalendarIndex(calendar);
    }
    if (m_settingsDialog) {
        m_settingsDialog.reset();
        openSettings();
    }
    if (m_aboutDialog) {
        m_aboutDialog.reset();
        openAbout();
    }
}

void CalendarWindow::show()
{
    m_window->show();
    focusView();
}

void CalendarWindow::activate()
{
    m_window->show();
    m_window->activate();
}

void CalendarWindow::setCurrentDate(Date date)
{
    if (date == m_currentDate || !date.isValid())
        return;
    m_currentDate = date;
    currentDateChanged.emit();
}

std::string CalendarWindow::view() const
{
    const std::string& name = m_model->view();
    return std::find(Views.begin(), Views.end(), name) != Views.end() ? name : std::string("month");
}

Date CalendarWindow::rangeStart() const
{
    const std::string name = view();
    if (name == "week")
        return Dates::weekStart(m_currentDate, m_model->firstDayOfWeek());
    if (name == "day")
        return m_currentDate;
    if (name == "year")
        return Date(m_currentDate.year(), 1, 1);
    return Dates::monthGridStart(m_currentDate, m_model->firstDayOfWeek());
}

// Month and year in the order the language writes them; with `until`, a run of months.
static std::vector<std::pair<std::string, bool>> monthTitle(Date date, std::optional<Date> until = std::nullopt,
                                                            bool abbreviated = false)
{
    const bool brief = until || abbreviated;
    const bool cjk = LocaleData::isCjk();
    const bool korean = LocaleData::language() == "ko";
    const auto name = [&](int month) {
        if (cjk)
            return std::to_string(month) + (korean ? "월" : "月");
        return LocaleData::monthName(month, brief ? LocaleData::Width::Short : LocaleData::Width::Long);
    };
    std::string months = name(date.month());
    if (until)
        months += " – " + name(until->month());
    if (cjk)
        return {{std::to_string(date.year()) + (korean ? "년 " : "年"), false}, {months, true}};
    return {{months, true}, {" " + std::to_string(date.year()), false}};
}

std::vector<std::pair<std::string, bool>> CalendarWindow::periodTitle() const
{
    const std::string name = view();
    if (name == "day") {
        // The language's long date, the weekday included and the year in regular weight.
        if (LocaleData::isCjk()) {
            const bool korean = LocaleData::language() == "ko";
            std::vector<std::pair<std::string, bool>> title = monthTitle(m_currentDate);
            title.back().first += std::to_string(m_currentDate.day()) + (korean ? "일 " : "日 ")
                    + LocaleData::dayName(Dates::weekday(m_currentDate));
            return title;
        }
        return {{LocaleData::longDateWithoutYear(m_currentDate), true},
                {" " + std::to_string(m_currentDate.year()), false}};
    }
    if (name == "year")
        return {{std::to_string(m_currentDate.year()), false}};
    if (name == "week") {
        const Date first = rangeStart();
        const Date last = first.addDays(6);
        if (first.month() == last.month())
            return monthTitle(first);
        if (first.year() == last.year())
            return monthTitle(first, last);
        std::vector<std::pair<std::string, bool>> title = monthTitle(first, std::nullopt, true);
        title.push_back({" – ", false});
        for (const auto& part : monthTitle(last, std::nullopt, true))
            title.push_back(part);
        return title;
    }
    return monthTitle(m_currentDate);
}

bool CalendarWindow::narrow() const
{
    // Below this the sidebar leaves room for the calendar, and comes back above it.
    return m_window->size().width < 900;
}

bool CalendarWindow::sidebarDocked() const
{
    return m_model->sidebarVisible() && !narrow();
}

void CalendarWindow::setSidebarOverlay(bool overlay)
{
    if (overlay == m_sidebarOverlay)
        return;
    m_sidebarOverlay = overlay;
    updateSidebarState(true);
}

void CalendarWindow::sizeChanged()
{
    const bool isNarrow = narrow();
    if (isNarrow == m_wasNarrow)
        return;
    m_wasNarrow = isNarrow;
    m_sidebarOverlay = false;
    // Only a drag resize slides the sidebar; the size at startup or from the compositor just sets it.
    updateSidebarState(m_window->isInteractivelyResized());
}

void CalendarWindow::updateSidebarState(bool animate)
{
    const double reveal = sidebarDocked() || m_sidebarOverlay ? 1 : 0;
    const double dock = sidebarDocked() ? 1 : 0;
    if (!animate) {
        m_revealAnimation->stop();
        m_dockAnimation->stop();
        m_revealTo = reveal;
        m_dockTo = dock;
        m_sidebarReveal = reveal;
        m_dockReveal = dock;
    } else {
        if (reveal != m_revealTo || !m_revealAnimation->isRunning()) {
            m_revealFrom = m_sidebarReveal;
            m_revealTo = reveal;
            if (m_revealFrom != m_revealTo)
                m_revealAnimation->start();
        }
        if (dock != m_dockTo || !m_dockAnimation->isRunning()) {
            m_dockFrom = m_dockReveal;
            m_dockTo = dock;
            if (m_dockFrom != m_dockTo)
                m_dockAnimation->start();
        }
    }
    m_root->requestLayout();
    m_root->requestPaint();
    sidebarChanged.emit();
}

bool CalendarWindow::isSelected(const EventItem& item) const
{
    return !m_selectedKey.empty() && item.key == m_selectedKey && item.recurrenceKey == m_selectedRecurrenceKey;
}

void CalendarWindow::selectEvent(const EventItem* item)
{
    const std::string key = item ? item->key : std::string();
    const std::string recurrenceKey = item ? item->recurrenceKey : std::string();
    if (key == m_selectedKey && recurrenceKey == m_selectedRecurrenceKey)
        return;
    m_selectedKey = key;
    m_selectedRecurrenceKey = recurrenceKey;
    selectionChanged.emit();
}

void CalendarWindow::setView(const std::string& name)
{
    m_model->setView(name);
    focusView();
}

void CalendarWindow::step(int direction)
{
    const std::string name = view();
    if (name == "day")
        setCurrentDate(m_currentDate.addDays(direction));
    else if (name == "week")
        setCurrentDate(m_currentDate.addDays(7 * direction));
    else if (name == "year")
        setCurrentDate(m_currentDate.addMonths(12 * direction));
    else
        setCurrentDate(m_currentDate.addMonths(direction));
}

void CalendarWindow::goToday()
{
    setCurrentDate(m_model->today());
}

void CalendarWindow::showDay(Date date)
{
    setCurrentDate(date);
    setView("day");
}

void CalendarWindow::showMonth(Date date)
{
    setCurrentDate(date);
    setView("month");
}

void CalendarWindow::toggleSidebar()
{
    if (narrow())
        setSidebarOverlay(!m_sidebarOverlay);
    else
        m_model->setSidebarVisible(!m_model->sidebarVisible());
}

void CalendarWindow::focusView()
{
    m_root->content()->setFocus();
}

void CalendarWindow::focusSearch()
{
    m_root->headerBar()->searchField()->setFocus(FocusReason::Keyboard);
}

void CalendarWindow::revealEvent(const EventItem& item)
{
    setCurrentDate(item.allDay ? item.firstDay : item.start.date());
    selectEvent(&item);
}

void CalendarWindow::openEditor(const EventForm& form)
{
    if (m_editor)
        m_editor->close();
    m_editor = std::make_unique<EventDialog>(m_window.get(), m_model, form);
    EventDialog* editor = m_editor.get();
    editor->onSaveRequested = [this, editor](const EventForm& data) { saveFromEditor(editor->event(), data); };
    editor->onDeleteRequested = [this, editor] {
        deleteEvent(editor->event().key, editor->event().recurrenceKey);
    };
    editor->onClosed = [this, editor] {
        if (m_editor.get() == editor)
            deleteLater(m_editor);
    };
    editor->show();
}

void CalendarWindow::newEvent(const DateTime& start, const DateTime& end, bool allDay)
{
    openEditor(m_model->newEventData(start, end, allDay));
}

void CalendarWindow::newEventOn(Date date)
{
    int hour = 9;
    if (date == m_model->today())
        hour = std::min(23, DateTime::currentDateTime().time().hour() + 1);
    const DateTime start = Dates::atMinute(date, hour * 60);
    newEvent(start, start.addSecs(3600), false);
}

void CalendarWindow::editEvent(const std::string& key, const std::string& recurrenceKey)
{
    if (const std::optional<EventForm> form = m_model->eventData(key, recurrenceKey))
        openEditor(*form);
}

void CalendarWindow::editSelected()
{
    editEvent(m_selectedKey, m_selectedRecurrenceKey);
}

void CalendarWindow::saveFromEditor(const EventForm& original, const EventForm& form)
{
    const auto closeEditor = [this] {
        if (m_editor)
            m_editor->close();
    };
    if (original.key.empty() || !original.recurring) {
        if (m_model->saveEvent(form, 2))
            closeEditor();
        return;
    }
    // A new rule or kind of event cannot apply to one occurrence alone.
    const bool seriesOnly = form.repeat != original.repeat || form.allDay != original.allDay
            || form.repeatEnd != original.repeatEnd
            || (form.repeatEnd == 1 && form.repeatUntil != original.repeatUntil)
            || (form.repeatEnd == 2 && form.repeatCount != original.repeatCount);
    askScope({tr("Save changes to this repeating event?"),
              seriesOnly ? tr("The new repeat applies to this and the following events.")
                         : tr("Change only this event, or this and the events after it?"),
              tr("Only This Event"), original.firstOccurrence ? tr("All Events") : tr("All Future Events"), seriesOnly,
              false},
             [this, form, closeEditor](int scope) {
                 if (m_model->saveEvent(form, scope))
                     closeEditor();
             });
}

void CalendarWindow::removeWithUndo(const std::string& key, const std::string& recurrenceKey,
                                    const std::string& title, int scope)
{
    if (!m_model->removeEvent(key, recurrenceKey, scope))
        return;
    if (m_editor)
        m_editor->close();
    clearSelection();
    showPassiveNotification(format(tr("“{title}” deleted"), {{"title", title}}), 4000, tr("Undo"),
                            [this] { m_model->undo(); });
}

void CalendarWindow::deleteEvent(const std::string& key, const std::string& recurrenceKey)
{
    const std::optional<EventForm> form = m_model->eventData(key, recurrenceKey);
    if (key.empty() || !form || form->readOnly)
        return;
    const std::string title = form->title.empty() ? std::string(tr("New Event")) : form->title;
    if (!form->recurring) {
        removeWithUndo(key, recurrenceKey, title, 2);
        return;
    }
    const bool first = m_model->isFirstOccurrence(key, recurrenceKey);
    askScope({format(tr("Delete “{title}”?"), {{"title", title}}), tr("This is a repeating event."),
              tr("Only This Event"), first ? tr("All Events") : tr("All Future Events"), false, true},
             [this, key, recurrenceKey, title](int scope) { removeWithUndo(key, recurrenceKey, title, scope); });
}

void CalendarWindow::deleteSelected()
{
    deleteEvent(m_selectedKey, m_selectedRecurrenceKey);
}

void CalendarWindow::moveEvent(const EventItem& item, int startDays, int startMinutes, int endDays, int endMinutes)
{
    if (item.readOnly)
        return;
    const std::string key = item.key;
    const std::string recurrenceKey = item.recurrenceKey;
    const auto move = [this, key, recurrenceKey, startDays, startMinutes, endDays, endMinutes](int scope) {
        m_model->moveOccurrence(key, recurrenceKey, startDays, startMinutes, endDays, endMinutes, scope);
    };
    if (!item.recurring) {
        // Later: the dragged item is rebuilt by the change.
        Application::instance()->loop().post([move] { move(2); });
        return;
    }
    const bool first = m_model->isFirstOccurrence(key, recurrenceKey);
    askScope({tr("Move this repeating event?"), tr("Move only this event, or this and the events after it?"),
              tr("Only This Event"), first ? tr("All Events") : tr("All Future Events"), false, false},
             move);
}

void CalendarWindow::askScope(const ScopeQuestion& question, std::function<void(int scope)> chosen)
{
    Window* parent = m_editor ? m_editor->window() : m_window.get();
    if (m_scopeDialog)
        m_scopeDialog->close();
    m_scopeDialog = std::make_unique<ScopeDialog>(parent, ScopeDialog::Question{question.title, question.text,
                                                                               question.thisText, question.laterText,
                                                                               question.seriesOnly,
                                                                               question.destructive});
    ScopeDialog* dialog = m_scopeDialog.get();
    dialog->onChosen = std::move(chosen);
    dialog->onClosed = [this, dialog] {
        if (m_scopeDialog.get() == dialog)
            deleteLater(m_scopeDialog);
    };
    dialog->show();
}

void CalendarWindow::showEventMenu(const EventItem& item, PointF position)
{
    const std::string key = item.key;
    const std::string recurrenceKey = item.recurrenceKey;
    m_contextMenu = std::make_unique<Menu>();
    m_contextMenu->addItem(item.readOnly ? tr("Show Details…") : tr("Edit…"),
                           [this, key, recurrenceKey] { editEvent(key, recurrenceKey); });
    m_contextMenu->addSeparator();
    const size_t remove = m_contextMenu->addItem(tr("Delete"), [this, key, recurrenceKey] {
        deleteEvent(key, recurrenceKey);
    });
    m_contextMenu->setItemEnabled(remove, !item.readOnly);
    m_contextMenu->popup(m_window.get(), position);
}

void CalendarWindow::showDayMenu(Date date, PointF position)
{
    m_contextMenu = std::make_unique<Menu>();
    m_contextMenu->addItem(tr("New Event"), [this, date] { newEventOn(date); });
    m_contextMenu->addItem(tr("New All-Day Event"), [this, date] {
        newEvent(DateTime(date, Time(0, 0)), DateTime(date, Time(0, 0)), true);
    });
    m_contextMenu->popup(m_window.get(), position);
}

void CalendarWindow::showCalendarMenu(const CalendarItem& calendar, PointF position)
{
    m_contextMenu = std::make_unique<Menu>();
    m_contextMenu->addItem(tr("Edit…"), [this, calendar] { editCalendar(calendar); });
    if (calendar.readOnly) {
        const size_t refresh = m_contextMenu->addItem(tr("Refresh"), [this, calendar] { refreshCalendar(calendar); });
        m_contextMenu->setItemEnabled(refresh, !m_model->subscriptions()->isRefreshing(calendar.id));
    }
    m_contextMenu->addItem(tr("Export…"), [this, calendar] { exportCalendar(calendar); });
    m_contextMenu->addSeparator();
    const size_t remove = m_contextMenu->addItem(calendar.readOnly ? tr("Unsubscribe…") : tr("Delete…"),
                                                 [this, calendar] { confirmDeleteCalendar(calendar); });
    m_contextMenu->setItemEnabled(remove, m_model->canRemoveCalendar(calendar.id));
    m_contextMenu->popup(m_window.get(), position);
}

void CalendarWindow::newCalendar()
{
    editCalendar({});
}

void CalendarWindow::newSubscription()
{
    m_subscriptionDialog = std::make_unique<SubscriptionDialog>(m_window.get(), m_model);
    SubscriptionDialog* dialog = m_subscriptionDialog.get();
    dialog->onSubscribed = [this](const std::string& id) {
        showPassiveNotification(format(tr("Subscribed to “{name}”"), {{"name", m_model->store()->calendar(id).name}}));
    };
    dialog->onClosed = [this, dialog] {
        if (m_subscriptionDialog.get() == dialog)
            deleteLater(m_subscriptionDialog);
    };
    dialog->show();
}

void CalendarWindow::refreshCalendar(const CalendarItem& calendar)
{
    const std::string name = calendar.name;
    m_model->subscriptions()->refresh(calendar.id, [this, name, alive = std::weak_ptr<bool>(m_alive)](
                                                       const std::string& error) {
        if (!alive.expired() && !error.empty())
            showPassiveNotification(format(tr("“{name}” could not be refreshed. {error}"),
                                           {{"name", name}, {"error", error}}));
    });
}

void CalendarWindow::editCalendar(const CalendarItem& calendar)
{
    m_calendarDialog = std::make_unique<CalendarDialog>(m_window.get(), m_model,
                                                        calendar.id.empty() ? std::nullopt
                                                                            : std::optional<CalendarItem>(calendar));
    CalendarDialog* dialog = m_calendarDialog.get();
    dialog->onClosed = [this, dialog] {
        if (m_calendarDialog.get() == dialog)
            deleteLater(m_calendarDialog);
    };
    dialog->show();
}

void CalendarWindow::confirmDeleteCalendar(const CalendarItem& calendar)
{
    const std::string id = calendar.id;
    if (calendar.readOnly) {
        m_confirmDialog = std::make_unique<ConfirmDialog>(
            m_window.get(), format(tr("Unsubscribe from “{name}”?"), {{"name", calendar.name}}),
            tr("Its events are removed from this computer."));
        m_confirmDialog->setConfirmText(tr("Unsubscribe"));
        m_confirmDialog->setDestructive(true);
        m_confirmDialog->onAccepted = [this, id] { m_model->removeCalendar(id); };
        m_confirmDialog->show();
        return;
    }
    const int count = m_model->eventCount(calendar.id);
    m_confirmDialog = std::make_unique<ConfirmDialog>(
        m_window.get(), format(tr("Delete “{name}”?"), {{"name", calendar.name}}),
        trn("Its event is moved to the trash.", "Its {n} events are moved to the trash.", count));
    m_confirmDialog->setConfirmText(tr("Delete"));
    m_confirmDialog->setDestructive(true);
    m_confirmDialog->onAccepted = [this, id] { m_model->removeCalendar(id); };
    m_confirmDialog->show();
}

void CalendarWindow::exportCalendar(const CalendarItem& calendar)
{
    m_fileChooser = std::make_unique<FileChooser>(m_window.get());
    m_fileChooser->setMode(FileChooser::Mode::SaveFile);
    m_fileChooser->setTitle(tr("Export"));
    // TRANSLATORS: keep the ASCII parentheses and the pattern in them unchanged.
    m_fileChooser->setFilters({FileChooser::Filter::fromNameFilter(tr("Calendar files (*.ics)"))});
    // The chooser opens in the suggested file's folder, not only with its name.
    const std::string suggested = m_model->exportFileName(calendar.id);
    m_fileChooser->setCurrentFolder(suggested.substr(0, suggested.find_last_of('/')));
    m_fileChooser->setCurrentFile(suggested);
    const std::string id = calendar.id;
    m_fileChooser->onAccepted = [this, id](const std::vector<std::string>& paths, const std::string&) {
        if (paths.empty())
            return;
        std::string path = paths.front();
        // A name typed without a suffix gets the calendar one.
        if (path.substr(path.find_last_of('/') + 1).find('.') == std::string::npos)
            path += ".ics";
        if (!m_model->exportCalendar(id, path))
            showPassiveNotification(tr("The calendar could not be exported."));
    };
    m_fileChooser->open();
}

void CalendarWindow::importFiles(const std::vector<std::string>& files)
{
    m_importDialog = std::make_unique<ImportDialog>(m_window.get(), m_model, files);
    ImportDialog* dialog = m_importDialog.get();
    dialog->onImport = [this](const std::vector<std::string>& files, const std::string& calendarId) {
        int total = 0;
        int failed = 0;
        for (const std::string& file : files) {
            const int count = m_model->importFile(file, calendarId);
            if (count < 0)
                ++failed;
            else
                total += count;
        }
        if (failed > 0 && total == 0)
            showPassiveNotification(tr("No events could be read from the file."));
        else
            showPassiveNotification(trn("{n} event imported", "{n} events imported", total));
    };
    dialog->onClosed = [this, dialog] {
        if (m_importDialog.get() == dialog)
            deleteLater(m_importDialog);
    };
    dialog->show();
}

void CalendarWindow::openImportChooser()
{
    m_fileChooser = std::make_unique<FileChooser>(m_window.get());
    m_fileChooser->setMode(FileChooser::Mode::OpenFiles);
    m_fileChooser->setTitle(tr("Import"));
    m_fileChooser->setFilters({FileChooser::Filter::fromNameFilter(tr("Calendar files (*.ics)")),
                               // TRANSLATORS: keep the ASCII parentheses and the pattern in them unchanged.
                               FileChooser::Filter::fromNameFilter(tr("All files (*)"))});
    m_fileChooser->onAccepted = [this](const std::vector<std::string>& paths, const std::string&) {
        if (!paths.empty())
            importFiles(paths);
    };
    m_fileChooser->open();
}

void CalendarWindow::openSettings()
{
    m_settingsDialog = std::make_unique<SettingsDialog>(m_window.get(), m_model);
    SettingsDialog* dialog = m_settingsDialog.get();
    dialog->onClosed = [this, dialog] {
        if (m_settingsDialog.get() == dialog)
            deleteLater(m_settingsDialog);
    };
    dialog->show();
}

void CalendarWindow::openAbout()
{
    if (m_aboutDialog) {
        m_aboutDialog->show();
        return;
    }

    m_aboutDialog = std::make_unique<AboutDialog>(m_window.get());
    m_aboutDialog->setName(tr("Calendar"));
    m_aboutDialog->setDescription(tr("Keeps track of your events."));
    m_aboutDialog->setIconSource("office-calendar");
    m_aboutDialog->setLink("https://github.com/cutefishos/calendar");
    m_aboutDialog->onClosed = [this] { deleteLater(m_aboutDialog); };
    m_aboutDialog->show();
    m_aboutDialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_aboutDialog->close(); });
}

void CalendarWindow::showPassiveNotification(const std::string& text, int timeout, const std::string& actionText,
                                             std::function<void()> action)
{
    m_toast->show(text, timeout > 0 ? std::chrono::milliseconds(timeout) : Toast::Normal, actionText,
                  std::move(action));
}

void CalendarWindow::installShortcuts()
{
    Window* window = m_window.get();
    window->addShortcut(ControlModifier, key::letter('n'), [this] { newEventOn(m_currentDate); });
    window->addShortcut(ControlModifier, key::letter('f'), [this] { focusSearch(); });
    window->addShortcut(ControlModifier, key::letter('t'), [this] { goToday(); });
    window->addShortcut(NoModifier, key::Home, [this] { goToday(); });
    window->addShortcut(NoModifier, key::PageUp, [this] { step(-1); });
    window->addShortcut(AltModifier, key::Left, [this] { step(-1); });
    window->addShortcut(NoModifier, key::PageDown, [this] { step(1); });
    window->addShortcut(AltModifier, key::Right, [this] { step(1); });
    for (int i = 0; i < 4; ++i)
        window->addShortcut(ControlModifier, uint32_t('1' + i), [this, i] { setView(Views[size_t(i)]); });
    window->addShortcut(ControlModifier, key::letter('z'), [this] {
        if (m_model->canUndo())
            m_model->undo();
    });
    window->addShortcut(ControlModifier, uint32_t(','), [this] { openSettings(); });
    window->addShortcut(ControlModifier, key::letter('q'), [] { Application::instance()->quit(); });
    window->addShortcut(NoModifier, key::Escape, [this] {
        TextField* search = m_root->headerBar()->searchField();
        if (search->hasFocus()) {
            search->setText({});
            m_root->content()->setSearchText({});
            focusView();
        } else if (m_sidebarOverlay) {
            setSidebarOverlay(false);
        }
    });
}

void CalendarWindow::buildMainMenu()
{
    m_window->setMainMenu(nullptr);
    m_mainMenu = std::make_unique<Menu>();

    Menu* application = m_mainMenu->addSubmenu(tr("Calendar"));
    application->setRole(MenuRole::Application);
    application->addItem(tr("About Calendar"), [this] { openAbout(); });
    application->addSeparator();
    const size_t settings = application->addItem(tr("Settings…"), [this] { openSettings(); });
    application->setItemKeyEquivalent(settings, ",", ControlModifier);
    application->addSeparator();
    application->addItem(tr("Hide Calendar"), [this] { m_window->hideApplication(); });
    application->addSeparator();
    const size_t quit = application->addItem(tr("Quit Calendar"), [] { Application::instance()->quit(); });
    application->setItemKeyEquivalent(quit, "q", ControlModifier);

    Menu* file = m_mainMenu->addSubmenu(tr("File"));
    const size_t newEvent = file->addItem(tr("New Event"), [this] { newEventOn(m_currentDate); });
    file->setItemKeyEquivalent(newEvent, "n", ControlModifier);
    file->addItem(tr("New Calendar…"), [this] { newCalendar(); });
    file->addItem(tr("New Subscription…"), [this] { newSubscription(); });
    file->addSeparator();
    file->addItem(tr("Import…"), [this] { openImportChooser(); });

    Menu* edit = m_mainMenu->addSubmenu(tr("Edit"));
    const size_t undo = edit->addItem(tr("Undo"), [this] {
        if (m_model->canUndo())
            m_model->undo();
    });
    edit->setItemKeyEquivalent(undo, "z", ControlModifier);
    edit->addSeparator();
    const size_t editEvent = edit->addItem(tr("Edit Event…"), [this] {
        if (hasSelection())
            editSelected();
    });
    edit->setItemKeyEquivalent(editEvent, "Return", NoModifier);
    const size_t remove = edit->addItem(tr("Delete"), [this] {
        if (hasSelection())
            deleteSelected();
    });
    edit->setItemKeyEquivalent(remove, "Delete", NoModifier);
    edit->addSeparator();
    const size_t find = edit->addItem(tr("Find"), [this] { focusSearch(); });
    edit->setItemKeyEquivalent(find, "f", ControlModifier);
    edit->onAboutToShow = [this, edit, undo, editEvent, remove] {
        edit->setItemText(undo, m_model->canUndo() ? m_model->undoText() : std::string(tr("Undo")));
        edit->setItemEnabled(undo, m_model->canUndo());
        const std::optional<EventForm> selected = hasSelection()
                ? m_model->eventData(m_selectedKey, m_selectedRecurrenceKey)
                : std::nullopt;
        edit->setItemText(editEvent, selected && selected->readOnly ? tr("Show Details…") : tr("Edit Event…"));
        edit->setItemEnabled(editEvent, selected.has_value());
        edit->setItemEnabled(remove, selected && !selected->readOnly);
    };

    Menu* viewMenu = m_mainMenu->addSubmenu(tr("View"));
    const std::vector<std::string> viewTitles{tr("Day"), tr("Week"), tr("Month"), tr("Year")};
    for (size_t i = 0; i < Views.size(); ++i) {
        const size_t row = viewMenu->addItem(viewTitles[i], [this, i] { setView(Views[i]); });
        viewMenu->setItemKeyEquivalent(row, std::string(1, char('1' + i)), ControlModifier);
    }
    viewMenu->addSeparator();
    const size_t sidebar = viewMenu->addItem(tr("Hide Sidebar"), [this] { toggleSidebar(); });
    viewMenu->addSeparator();
    const size_t today = viewMenu->addItem(tr("Go to Today"), [this] { goToday(); });
    viewMenu->setItemKeyEquivalent(today, "t", ControlModifier);
    const size_t previous = viewMenu->addItem(tr("Previous"), [this] { step(-1); });
    viewMenu->setItemKeyEquivalent(previous, "Left", AltModifier);
    const size_t next = viewMenu->addItem(tr("Next"), [this] { step(1); });
    viewMenu->setItemKeyEquivalent(next, "Right", AltModifier);
    viewMenu->onAboutToShow = [this, viewMenu, sidebar] {
        for (size_t i = 0; i < Views.size(); ++i)
            viewMenu->setItemChecked(i, Views[i] == view());
        const bool shown = narrow() ? m_sidebarOverlay : m_model->sidebarVisible();
        viewMenu->setItemText(sidebar, shown ? tr("Hide Sidebar") : tr("Show Sidebar"));
    };

    Menu* windowMenu = m_mainMenu->addSubmenu(tr("Window"));
    windowMenu->setRole(MenuRole::Windows);
    windowMenu->addItem(tr("Minimize"), [this] { m_window->minimize(); });
    windowMenu->addItem(tr("Zoom"), [this] { m_window->setMaximized(!m_window->states().maximized); });

    m_window->setMainMenu(m_mainMenu.get());
}
