// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "calendarmodel.h"

#include <base/signal.h>
#include <base/timer.h>
#include <ui/animation.h>
#include <ui/geometry.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::ui {
class AboutDialog;
class ConfirmDialog;
class FileChooser;
class Menu;
class Toast;
class View;
class Window;
}

class CalendarDialog;
class CalendarRoot;
class EventDialog;
class ImportDialog;
class ScopeDialog;
class SettingsDialog;
class SubscriptionDialog;

// The calendar's main window: the selected day and view, the selected event,
// and every action the views, menus and shortcuts start.
class CalendarWindow
{
public:
    // Tests open it at its default size, whatever the user's last size was.
    explicit CalendarWindow(CalendarModel* model, bool restoresSize = true);
    ~CalendarWindow();

    CalendarWindow(const CalendarWindow&) = delete;
    CalendarWindow& operator=(const CalendarWindow&) = delete;

    void show();
    void activate();
    cutefish::ui::Window* window() const { return m_window.get(); }
    CalendarModel* model() const { return m_model; }
    CalendarRoot* root() const { return m_root; }
    // The event editor while it is open.
    EventDialog* editor() const { return m_editor.get(); }
    ScopeDialog* scopeDialog() const { return m_scopeDialog.get(); }
    CalendarDialog* calendarDialog() const { return m_calendarDialog.get(); }
    ImportDialog* importDialog() const { return m_importDialog.get(); }
    SubscriptionDialog* subscriptionDialog() const { return m_subscriptionDialog.get(); }
    cutefish::ui::ConfirmDialog* confirmDialog() const { return m_confirmDialog.get(); }
    cutefish::ui::Toast* toast() const { return m_toast.get(); }

    static constexpr double SidebarWidth = 240;
    static constexpr double HeaderHeight = 48;
    // The title and window buttons end on this line; content spacing is measured from it.
    static constexpr double HeaderBaseline = 28;

    // The selected day; the views show the period around it.
    Date currentDate() const { return m_currentDate; }
    void setCurrentDate(Date date);
    // "day", "week", "month" or "year".
    std::string view() const;
    Date rangeStart() const;
    // The month or day in bold, the year in regular weight.
    std::vector<std::pair<std::string, bool>> periodTitle() const;

    bool narrow() const;
    bool sidebarDocked() const;
    bool sidebarOverlay() const { return m_sidebarOverlay; }
    void setSidebarOverlay(bool overlay);
    // 0 to 1 as the sidebar slides in, and as it docks.
    double sidebarReveal() const { return m_sidebarReveal; }
    double dockReveal() const { return m_dockReveal; }

    bool isSelected(const EventItem& item) const;
    void selectEvent(const EventItem* item);
    void clearSelection() { selectEvent(nullptr); }
    bool hasSelection() const { return !m_selectedKey.empty(); }

    void setView(const std::string& name);
    void step(int direction);
    void goToday();
    void showDay(Date date);
    void showMonth(Date date);
    void toggleSidebar();
    void focusView();
    void focusSearch();
    void revealEvent(const EventItem& item);

    void newEvent(const DateTime& start, const DateTime& end, bool allDay);
    // A timed hour on the day: the next whole hour today, nine o'clock otherwise.
    void newEventOn(Date date);
    void editEvent(const std::string& key, const std::string& recurrenceKey);
    void editSelected();
    void deleteEvent(const std::string& key, const std::string& recurrenceKey);
    void deleteSelected();
    // Dragging moves the start and the end by whole days and minutes.
    void moveEvent(const EventItem& item, int startDays, int startMinutes, int endDays, int endMinutes);

    // Positions are in the main window's coordinates.
    void showEventMenu(const EventItem& item, cutefish::ui::PointF position);
    void showDayMenu(Date date, cutefish::ui::PointF position);
    void showCalendarMenu(const CalendarItem& calendar, cutefish::ui::PointF position);

    void newCalendar();
    void newSubscription();
    void editCalendar(const CalendarItem& calendar);
    void refreshCalendar(const CalendarItem& calendar);
    void importFiles(const std::vector<std::string>& files);
    void openImportChooser();
    void openSettings();
    void openAbout();
    void showPassiveNotification(const std::string& text, int timeout = 0, const std::string& actionText = {},
                                 std::function<void()> action = {});
    // After the window's size changed: a narrow window loses the docked sidebar.
    void sizeChanged();

    cutefish::base::Signal<> currentDateChanged;
    cutefish::base::Signal<> viewChanged;
    cutefish::base::Signal<> selectionChanged;
    cutefish::base::Signal<> sidebarChanged;

private:
    struct ScopeQuestion
    {
        std::string title;
        std::string text;
        std::string thisText;
        std::string laterText;
        bool seriesOnly = false;
        bool destructive = false;
    };

    void openEditor(const EventForm& form);
    void saveFromEditor(const EventForm& original, const EventForm& form);
    void removeWithUndo(const std::string& key, const std::string& recurrenceKey, const std::string& title, int scope);
    void askScope(const ScopeQuestion& question, std::function<void(int scope)> chosen);
    void confirmDeleteCalendar(const CalendarItem& calendar);
    void exportCalendar(const CalendarItem& calendar);
    void installShortcuts();
    // The menus the status bar shows while the window is active.
    void buildMainMenu();
    void updateSidebarState(bool animate);
    // The views, which show text.
    void buildContent();
    void retranslate();

    CalendarModel* m_model;
    // The window shows it until the window goes, so it outlives the window.
    std::unique_ptr<cutefish::ui::Menu> m_mainMenu;
    std::unique_ptr<cutefish::ui::Window> m_window;
    CalendarRoot* m_root = nullptr;

    Date m_currentDate;
    std::string m_selectedKey;
    std::string m_selectedRecurrenceKey;
    bool m_sidebarOverlay = false;
    bool m_wasNarrow = false;
    double m_sidebarReveal = 0;
    double m_dockReveal = 0;
    double m_revealFrom = 0;
    double m_revealTo = 0;
    double m_dockFrom = 0;
    double m_dockTo = 0;
    std::unique_ptr<cutefish::ui::Animation> m_revealAnimation;
    std::unique_ptr<cutefish::ui::Animation> m_dockAnimation;

    std::unique_ptr<cutefish::ui::Menu> m_contextMenu;
    std::unique_ptr<EventDialog> m_editor;
    std::unique_ptr<ScopeDialog> m_scopeDialog;
    std::unique_ptr<CalendarDialog> m_calendarDialog;
    std::unique_ptr<ImportDialog> m_importDialog;
    std::unique_ptr<SubscriptionDialog> m_subscriptionDialog;
    std::unique_ptr<SettingsDialog> m_settingsDialog;
    std::unique_ptr<cutefish::ui::ConfirmDialog> m_confirmDialog;
    std::unique_ptr<cutefish::ui::FileChooser> m_fileChooser;
    std::unique_ptr<cutefish::ui::AboutDialog> m_aboutDialog;
    std::unique_ptr<cutefish::ui::Toast> m_toast;

    std::vector<cutefish::base::Subscription> m_subscriptions;
    cutefish::base::Subscription m_languageChanged;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
