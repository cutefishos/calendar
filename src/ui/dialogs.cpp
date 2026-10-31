// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs.h"
#include "dates.h"
#include "localedata.h"
#include "minimonth.h"
#include "style.h"
#include "text.h"

#include <base/event_loop.h>
#include <base/i18n.h>
#include <ui/application.h>
#include <ui/cell_layout.h>
#include <ui/color_provider.h>
#include <ui/combo_box.h>
#include <ui/dialog_window.h>
#include <ui/drop_down_popup.h>
#include <ui/header_button.h>
#include <ui/icon.h>
#include <ui/keys.h>
#include <ui/label.h>
#include <ui/layout.h>
#include <ui/painter.h>
#include <ui/round_image_button.h>
#include <ui/scroll_view.h>
#include <ui/switch.h>
#include <ui/text_field.h>
#include <ui/window.h>

#include <algorithm>
#include <cmath>

using namespace cutefish::ui;
using cutefish::i18n::tr;
using cutefish::i18n::trn;

static constexpr double TitleSize = 14;

// Destroys the window from the event loop, after the event being handled.
static void release(std::unique_ptr<Window>& window)
{
    if (!window)
        return;
    std::shared_ptr<Window> doomed(std::move(window));
    Application::instance()->loop().post([doomed] {});
}

// The close control, centred in the 32-pixel corner slot.
static constexpr double kCloseControl = 26;

// The dialog's view: its column, and the close button in the corner over it.
class SmallDialog::Root : public View
{
public:
    explicit Root(SmallDialog* dialog)
        : m_dialog(dialog)
        , m_column(addChild<ColumnLayout>())
        , m_close(addChild<HeaderButton>("close", kCloseControl))
    {
        m_column->setPadding({16, 16, 16, 12});
        m_column->setSpacing(16);
        m_close->setWindowControl(true);
        m_close->onClicked = [this] { m_dialog->close(); };
    }

    ColumnLayout* column() const { return m_column; }

    double heightForWidth(double width) const override { return m_column->heightForWidth(width); }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        m_column->setGeometry({0, 0, geometry().width, m_column->heightForWidth(geometry().width)});
        const double inset = 8 + (32 - kCloseControl) / 2;
        m_close->setGeometry({inset, inset, kCloseControl, kCloseControl});
    }

    // The empty parts of the window move it.
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && window())
            window()->startMove();
    }

private:
    SmallDialog* m_dialog;
    ColumnLayout* m_column;
    HeaderButton* m_close;
};

SmallDialog::SmallDialog(Window* parent, std::string title, double width)
    : m_parent(parent)
    , m_window(std::make_unique<Window>())
    , m_width(width)
{
    auto root = std::make_unique<Root>(this);
    m_column = root->column();
    m_window->setContentView(std::move(root));
    m_window->setBackgroundColor(ColorId::SecondBackground);
    m_window->setTitle(title);
    m_window->setTransientParent(parent, true);
    m_window->onCloseRequested = [this] { close(); };
    m_window->addShortcut(NoModifier, key::Escape, [this] { close(); });
}

SmallDialog::~SmallDialog() = default;

Label* SmallDialog::addTitle(const std::string& text)
{
    if (!m_texts) {
        m_texts = m_column->addChild<Column>();
        m_texts->setSpacing(4);
        // Clear of the close corner of the window.
        m_texts->setLayoutHints({.fillWidth = true, .margins = {32, 0, 0, 0}});
    }
    Label* label = m_texts->addChild<Label>(text);
    label->setFontSize(TitleSize);
    label->setWeight(600);
    label->setWrap(true);
    label->setLayoutHints({.fillWidth = true});
    return label;
}

Label* SmallDialog::addText(const std::string& text)
{
    Label* label = m_texts->addChild<Label>(text);
    label->setFontSize(TitleSize);
    label->setColor(ColorId::DisabledText);
    label->setWrap(true);
    label->setFixedLineHeight(20);
    label->setLayoutHints({.fillWidth = true});
    return label;
}

void SmallDialog::setWidth(double width)
{
    m_width = width;
}

void SmallDialog::show()
{
    if (!m_window)
        return;
    m_window->setMinimumSize({m_width, 0});
    m_window->setMaximumSize({m_width, 100000});
    m_window->resize({m_width, 0});
    m_window->setFitsContentHeight(true);
    m_window->show();
}

void SmallDialog::close()
{
    release(m_window);
    if (onClosed)
        onClosed();
}

static RowLayout* addActions(ColumnLayout* column)
{
    RowLayout* actions = column->addChild<RowLayout>();
    actions->setSpacing(8);
    actions->setLayoutHints({.horizontalAlignment = Alignment::End});
    return actions;
}

ScopeDialog::ScopeDialog(Window* parent, const Question& question)
    : SmallDialog(parent, question.title, 360)
{
    addTitle(question.title);
    if (!question.text.empty())
        addText(question.text);

    RowLayout* actions = addActions(column());
    DialogButton* cancel = actions->addChild<DialogButton>(tr("Cancel"));
    cancel->onClicked = [this] { close(); };
    if (!question.seriesOnly) {
        DialogButton* only = actions->addChild<DialogButton>(question.thisText);
        only->setName("thisEventButton");
        only->onClicked = [this] {
            close();
            if (onChosen)
                onChosen(0);
        };
    }
    DialogButton* later = actions->addChild<DialogButton>(question.laterText, true);
    later->setName("laterEventsButton");
    later->setDestructive(question.destructive);
    later->onClicked = [this] {
        close();
        if (onChosen)
            onChosen(1);
    };
    later->setFocus();
    setWidth(std::max(360.0, actions->sizeHint().width + 32));
}

namespace {

// A colour to pick: a ring around it when chosen.
class Swatch : public View
{
public:
    Swatch(Rgb color, std::function<void()> clicked)
        : m_color(color)
        , m_clicked(std::move(clicked))
    {
        setName("swatch");
    }

    void setChosen(bool chosen)
    {
        m_chosen = chosen;
        requestPaint();
    }

    SizeF sizeHint() const override { return {28, 28}; }
    bool acceptsPointer() const override { return true; }

protected:
    void paint(Painter& painter) override
    {
        const Color color = Style::toColor(m_color);
        if (m_chosen)
            painter.drawRoundedRect(localRect(), {localRect().width / 2}, Color{0, 0, 0, 0}, 2, color);
        const double margin = m_chosen ? 4 : 2;
        const RectF dot = localRect().adjusted(margin, margin, -margin, -margin);
        painter.fillRoundedRect(dot, dot.width / 2, color);
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && localRect().contains(event.position) && m_clicked)
            m_clicked();
    }

private:
    Rgb m_color;
    std::function<void()> m_clicked;
    bool m_chosen = false;
};

}

static Rgb rgbOf(Color color)
{
    return Rgb{color.r, color.g, color.b};
}

static const std::vector<Rgb>& calendarColors()
{
    static const std::vector<Rgb> colors{rgbOf(accent::blue), rgbOf(accent::purple), rgbOf(accent::pink),
                                         rgbOf(accent::red), rgbOf(accent::orange), rgbOf(accent::green),
                                         rgbOf(accent::grey)};
    return colors;
}

CalendarDialog::CalendarDialog(Window* parent, CalendarModel* model, std::optional<CalendarItem> calendar)
    : SmallDialog(parent, calendar ? tr("Edit Calendar") : tr("New Calendar"), 360)
    , m_model(model)
    , m_id(calendar ? calendar->id : std::string())
    , m_color(calendar ? calendar->color : rgbOf(accent::blue))
{
    Label* title = column()->addChild<Label>(calendar ? tr("Edit Calendar") : tr("New Calendar"));
    title->setFontSize(TitleSize);
    title->setWeight(600);
    title->setLayoutHints({.fillWidth = true, .margins = {32, 0, 0, 0}});
    title->setName("dialogTitle");

    m_name = column()->addChild<TextField>(calendar ? calendar->name : std::string());
    m_name->setName("calendarName");
    m_name->setPlaceholder(tr("Name"));
    m_name->setLayoutHints({.fillWidth = true});
    m_name->onAccepted = [this] { accept(); };

    Row* swatches = column()->addChild<Row>();
    swatches->setSpacing(10);
    swatches->setLayoutHints({.horizontalAlignment = Alignment::Center});
    for (const Rgb& color : calendarColors()) {
        m_swatches.push_back(swatches->addChild<Swatch>(color, [this, color] {
            m_color = color;
            updateSwatches();
        }));
    }
    updateSwatches();

    RowLayout* actions = addActions(column());
    actions->addChild<DialogButton>(tr("Cancel"))->onClicked = [this] { close(); };
    m_save = actions->addChild<DialogButton>(calendar ? tr("Save") : tr("Create"), true);
    m_save->setName("calendarSave");
    m_save->onClicked = [this] { accept(); };
    const auto updateSave = [this] { m_save->setEnabled(!trimmed(m_name->text()).empty()); };
    m_name->onTextChanged = updateSave;
    updateSave();

    m_name->setFocus();
    m_name->selectAll();
}

void CalendarDialog::updateSwatches()
{
    for (size_t i = 0; i < m_swatches.size(); ++i)
        static_cast<Swatch*>(m_swatches[i])->setChosen(calendarColors()[i] == m_color);
}

void CalendarDialog::accept()
{
    const std::string name = trimmed(m_name->text());
    if (name.empty())
        return;
    if (!m_id.empty())
        m_model->updateCalendar(m_id, name, m_color);
    else
        m_model->createCalendar(name, m_color);
    close();
}

// The first colour no calendar has yet.
static Rgb unusedColor(const CalendarModel* model)
{
    const std::vector<CalendarItem> calendars = model->calendars();
    for (const Rgb& color : calendarColors()) {
        if (std::none_of(calendars.begin(), calendars.end(), [&color](const CalendarItem& calendar) {
                return calendar.color == color;
            }))
            return color;
    }
    return calendarColors().front();
}

// Shows or hides the holiday calendars: their heading and a chevron.
class SubscriptionDialog::HolidayToggle : public View
{
public:
    explicit HolidayToggle(std::function<void()> toggled)
        : m_toggled(std::move(toggled))
        , m_label(addChild<Label>(tr("Public Holidays")))
        , m_chevron(addChild<Icon>("chevron-right", 16))
    {
        setName("holidaysToggle");
        m_label->setVerticallyCentered(true);
    }

    void setOpen(bool open)
    {
        m_chevron->setIconName(open ? "chevron-down" : "chevron-right");
    }

    SizeF sizeHint() const override { return {0, 32}; }
    double heightForWidth(double) const override { return 32; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        const double height = geometry().height;
        m_label->setColor(ColorId::Text);
        m_label->setGeometry({0, 0, std::max(0.0, width - 16 - 8), height});
        m_chevron->setColor(Theme::current().disabledText);
        m_chevron->setGeometry({width - 16, Style::centred(height, 16), 16, 16});
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && localRect().contains(event.position) && m_toggled)
            m_toggled();
    }

private:
    std::function<void()> m_toggled;
    Label* m_label;
    Icon* m_chevron;
};

// A holiday calendar to pick; a check once subscribed to.
class SubscriptionDialog::HolidayRow : public View
{
public:
    HolidayRow(std::string name, std::function<void()> clicked)
        : m_clicked(std::move(clicked))
        , m_name(addChild<Label>(std::move(name)))
        , m_check(addChild<Icon>("check", 16))
    {
        setName("holidayRow");
    }

    bool isSubscribed() const { return m_subscribed; }
    bool isChosen() const { return m_chosen; }

    void setState(bool subscribed, bool chosen)
    {
        if (subscribed == m_subscribed && chosen == m_chosen)
            return;
        m_subscribed = subscribed;
        m_chosen = chosen;
        requestLayout();
        requestPaint();
    }

    SizeF sizeHint() const override { return {0, 34}; }
    double heightForWidth(double) const override { return 34; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const Theme& theme = Theme::current();
        const double width = geometry().width;
        const double height = geometry().height;
        m_check->setVisible(m_subscribed);
        m_check->setColor(m_chosen ? theme.highlightedText : theme.highlight);
        m_check->setGeometry({width - 10 - 16, Style::centred(height, 16), 16, 16});
        m_name->setColor(m_chosen ? theme.highlightedText : m_subscribed ? theme.disabledText : theme.text);
        // As the delegate's RowLayout places it: whole pixels, centred.
        const double nameHeight = m_name->sizeHint().height;
        m_name->setGeometry({10, std::round((height - nameHeight) / 2), std::max(0.0, width - 20 - 16 - 8),
                             nameHeight});
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        // Inset 6 into the radius 10 card.
        const double radius = theme.mediumRadius - 6;
        if (m_chosen)
            painter.fillRoundedRect(localRect(), radius, theme.highlight);
        else if (m_subscribed)
            return;
        else if (m_pressed)
            painter.fillRoundedRect(localRect(), radius, Style::withAlpha(theme.text, 0.08));
        else if (m_hovered)
            painter.fillRoundedRect(localRect(), radius, Style::withAlpha(theme.text, 0.04));
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
        if (event.button == MouseButton::Left && !m_subscribed) {
            m_pressed = true;
            requestPaint();
        }
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (!m_pressed)
            return;
        m_pressed = false;
        requestPaint();
        if (localRect().contains(event.position) && m_clicked)
            m_clicked();
    }

private:
    std::function<void()> m_clicked;
    Label* m_name;
    Icon* m_check;
    bool m_subscribed = false;
    bool m_chosen = false;
    bool m_hovered = false;
    bool m_pressed = false;
};

namespace {

// A card of rows that scroll under a fixed height.
class HolidayList : public View
{
public:
    static constexpr double Inset = 6;
    static constexpr double VisibleRows = 6;

    HolidayList()
        : m_scroll(addChild<ScrollView>())
    {
        setName("holidayList");
        m_scroll->setCornerRadius(Theme::current().mediumRadius);
        m_column = m_scroll->setContent<Column>();
        m_column->setMargins(Margins::all(Inset));
        m_column->setSpacing(0);
    }

    Column* column() const { return m_column; }
    ScrollView* scrollView() const { return m_scroll; }

    double heightForWidth(double) const override { return VisibleRows * 34 + 2 * Inset; }
    SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override
    {
        m_scroll->setGeometry(localRect());
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        painter.fillRoundedRect(localRect(), theme.mediumRadius, Style::withAlpha(theme.text, 0.04));
    }

private:
    ScrollView* m_scroll;
    Column* m_column = nullptr;
};

}

SubscriptionDialog::SubscriptionDialog(Window* parent, CalendarModel* model)
    : SmallDialog(parent, tr("New Subscription"), 380)
    , m_model(model)
    , m_color(unusedColor(model))
    , m_feeds(HolidayFeeds::list())
{
    addTitle(tr("New Subscription"))->setName("dialogTitle");
    addText(tr("A calendar published on the web, such as public holidays. It is kept up to date and cannot be "
               "edited."));

    m_url = column()->addChild<TextField>();
    m_url->setName("subscriptionUrl");
    m_url->setPlaceholder(tr("webcal:// or https:// address"));
    m_url->setLayoutHints({.fillWidth = true});
    m_url->onAccepted = [this] { accept(); };
    m_url->onTextChanged = [this] {
        m_error->setVisible(false);
        update();
    };

    // Up against the field above.
    m_error = column()->addChild<Label>();
    m_error->setName("subscriptionError");
    m_error->setFontSize(Style::points(8));
    m_error->setWrap(true);
    m_error->setVisible(false);
    m_error->setLayoutHints({.fillWidth = true, .margins = {4, -10, 0, 0}});

    m_holidayToggle = column()->addChild<HolidayToggle>([this] { setHolidaysShown(!holidaysShown()); });
    m_holidayToggle->setLayoutHints({.fillWidth = true, .margins = {0, -8, 0, -8}});
    auto* holidays = column()->addChild<HolidayList>();
    holidays->setLayoutHints({.fillWidth = true});
    holidays->setVisible(false);
    m_holidayList = holidays;
    for (const HolidayFeed& feed : m_feeds) {
        const std::string url = feed.url;
        HolidayRow* row = holidays->column()->addChild<HolidayRow>(feed.name, [this, url] {
            m_url->setText(url);
            update();
        });
        row->setLayoutHints({.fillWidth = true});
        m_holidayRows.push_back(row);
    }

    Row* swatches = column()->addChild<Row>();
    swatches->setSpacing(10);
    swatches->setLayoutHints({.horizontalAlignment = Alignment::Center});
    for (const Rgb& color : calendarColors()) {
        m_swatches.push_back(swatches->addChild<Swatch>(color, [this, color] {
            m_color = color;
            updateSwatches();
        }));
    }
    updateSwatches();

    RowLayout* actions = addActions(column());
    actions->addChild<DialogButton>(tr("Cancel"))->onClicked = [this] { close(); };
    m_subscribe = actions->addChild<DialogButton>(tr("Subscribe"), true);
    m_subscribe->setName("subscribeButton");
    m_subscribe->onClicked = [this] { accept(); };
    update();

    m_url->setFocus();
}

SubscriptionDialog::~SubscriptionDialog()
{
    if (isBusy())
        m_model->subscriptions()->cancelSubscribe(m_pending);
}

void SubscriptionDialog::updateSwatches()
{
    for (size_t i = 0; i < m_swatches.size(); ++i)
        static_cast<Swatch*>(m_swatches[i])->setChosen(calendarColors()[i] == m_color);
}

bool SubscriptionDialog::holidaysShown() const
{
    return m_holidayList->isVisible();
}

void SubscriptionDialog::setHolidaysShown(bool shown)
{
    m_holidayList->setVisible(shown);
    m_holidayToggle->setOpen(shown);
    column()->requestLayout();
}

void SubscriptionDialog::update()
{
    const std::string url = Subscriptions::normalizedUrl(m_url->text());
    std::vector<std::string> subscribed;
    for (const CalendarInfo& calendar : m_model->store()->calendars()) {
        if (calendar.isReadOnly())
            subscribed.push_back(calendar.url);
    }
    for (size_t i = 0; i < m_holidayRows.size(); ++i) {
        const std::string& feed = m_feeds[i].url;
        const bool isSubscribed = std::find(subscribed.begin(), subscribed.end(), feed) != subscribed.end();
        m_holidayRows[i]->setState(isSubscribed, !isSubscribed && feed == url);
        m_holidayRows[i]->setEnabled(!isBusy());
    }
    m_url->setEnabled(!isBusy());
    m_subscribe->setText(isBusy() ? tr("Subscribing…") : tr("Subscribe"));
    m_subscribe->setEnabled(!isBusy() && !trimmed(m_url->text()).empty());
}

void SubscriptionDialog::accept()
{
    if (isBusy() || trimmed(m_url->text()).empty())
        return;
    const auto showError = [this](const std::string& error) {
        m_error->setText(error);
        m_error->setColor(Style::red());
        m_error->setVisible(true);
        update();
        m_url->setFocus();
        m_url->selectAll();
    };
    // Checked here, as subscribe() would answer before returning.
    if (Subscriptions::normalizedUrl(m_url->text()).empty()) {
        showError(tr("This is not a web address."));
        return;
    }
    m_error->setVisible(false);
    m_pending = m_model->subscriptions()->subscribe(m_url->text(), m_color, [this, showError](const std::string& id,
                                                                                             const std::string& error) {
        m_pending = -1;
        if (id.empty()) {
            showError(error);
            return;
        }
        auto subscribed = onSubscribed;
        close();
        if (subscribed)
            subscribed(id);
    });
    update();
}

static std::string fileNameOf(const std::string& file)
{
    std::string name = file.substr(file.find_last_of('/') + 1);
    std::string decoded;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size()) {
            decoded.push_back(char(std::stoi(name.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(name[i]);
        }
    }
    return decoded;
}

ImportDialog::ImportDialog(Window* parent, CalendarModel* model, std::vector<std::string> files)
    : SmallDialog(parent, tr("Import Events"), 380)
    , m_model(model)
    , m_files(std::move(files))
{
    addTitle(tr("Import Events"));
    Label* text = addText(m_files.size() == 1 ? fileNameOf(m_files.front())
                                              : trn("{n} file", "{n} files", m_files.size()));
    text->setWrap(false);
    text->setFixedLineHeight(0);
    text->setElide(Label::Elide::Middle);

    RowLayout* target = column()->addChild<RowLayout>();
    target->setSpacing(12);
    target->setLayoutHints({.fillWidth = true});
    target->addChild<Label>(tr("Add to"));
    std::vector<std::string> names;
    int current = 0;
    const std::vector<CalendarItem> calendars = model->writableCalendars();
    for (size_t i = 0; i < calendars.size(); ++i) {
        names.push_back(calendars[i].name);
        if (calendars[i].id == model->defaultCalendar())
            current = int(i);
    }
    m_calendar = target->addChild<ComboBox>(names);
    m_calendar->setName("importCalendarBox");
    m_calendar->setCurrentIndex(current);
    m_calendar->setLayoutHints({.fillWidth = true});

    RowLayout* actions = addActions(column());
    actions->addChild<DialogButton>(tr("Cancel"))->onClicked = [this] { close(); };
    DialogButton* import = actions->addChild<DialogButton>(tr("Import"), true);
    import->setName("importButton");
    import->onClicked = [this] { accept(); };
    window()->addShortcut(NoModifier, key::Return, [this] { accept(); });
    window()->addShortcut(NoModifier, key::KeypadEnter, [this] { accept(); });
}

int ImportDialog::calendarIndex() const
{
    return m_calendar->currentIndex();
}

void ImportDialog::setCalendarIndex(int index)
{
    m_calendar->setCurrentIndex(index);
}

void ImportDialog::accept()
{
    const std::vector<CalendarItem> calendars = m_model->writableCalendars();
    const int index = m_calendar->currentIndex();
    const std::vector<std::string> files = m_files;
    const std::string calendarId = index >= 0 && size_t(index) < calendars.size() ? calendars[size_t(index)].id
                                                                                  : m_model->defaultCalendar();
    auto import = onImport;
    close();
    if (import)
        import(files, calendarId);
}

View* FormCard::addRow(const std::string& text, double height)
{
    View* row = addChild<View>();
    row->setName("formRow");
    row->setLayoutHints({.preferredHeight = height});
    Label* label = row->addChild<Label>(text);
    label->setVerticallyCentered(true);
    Row* controls = row->addChild<Row>();
    controls->setSpacing(8);
    return controls;
}

void FormCard::addWide(std::unique_ptr<View> view, double height)
{
    view->setLayoutHints({.preferredHeight = height});
    addChild(std::move(view));
}

void FormCard::setRowVisible(View* row, bool visible)
{
    while (row && row->parent() != this)
        row = row->parent();
    if (row)
        row->setVisible(visible);
}

std::vector<View*> FormCard::visibleRows() const
{
    std::vector<View*> rows;
    for (const auto& child : children()) {
        if (child->isVisible())
            rows.push_back(child.get());
    }
    return rows;
}

// A separator between fields takes one device pixel.
static double rowHeight(const View* row)
{
    if (row->name() == "separator")
        return 1 / (row->window() ? row->window()->scale() : 1.0);
    return row->layoutHints().preferredHeight;
}

double FormCard::heightForWidth(double) const
{
    double height = 0;
    for (View* row : visibleRows())
        height += rowHeight(row);
    return height;
}

void FormCard::layout()
{
    const double width = geometry().width;
    double y = 0;
    for (View* row : visibleRows()) {
        const double height = rowHeight(row);
        // The rows' layout puts each on a whole pixel.
        row->setGeometry({row->name() == "separator" ? 14.0 : 0.0, std::round(y),
                          row->name() == "separator" ? width - 28 : width, height});
        y += height;
        if (row->name() != "formRow")
            continue;
        // The label on the left, the controls on the right, both centred.
        auto* label = static_cast<Label*>(row->children()[0].get());
        View* controls = row->children()[1].get();
        // Both centred by anchors.
        const SizeF size = controls->sizeHint();
        controls->setGeometry({width - 14 - size.width, Style::centred(height, size.height), size.width, size.height});
        const double labelHeight = label->sizeHint().height;
        label->setGeometry({14, Style::centred(height, labelHeight), std::max(0.0, width - 28 - size.width - 8),
                            labelHeight});
    }
}

void FormCard::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    painter.fillRoundedRect(localRect(), theme.bigRadius, theme.secondBackground, themeCorners());
    // Separators and hairlines are RoundedRectangles, snapped as shapes.
    const std::vector<View*> rows = visibleRows();
    for (size_t i = 0; i < rows.size(); ++i) {
        const RectF rect = rows[i]->geometry();
        if (rows[i]->name() == "separator")
            painter.fillRoundedRect(rect, 0, Style::separatorColor());
        // A hairline at the bottom of each row but the last, in from the card's sides.
        else if (rows[i]->name() == "formRow" && i + 1 < rows.size())
            painter.fillRoundedRect({14, rect.bottom() - 1 / painter.scale(), geometry().width - 28, 1 / painter.scale()},
                                    0, Style::separatorColor());
    }
}

SettingsDialog::SettingsDialog(Window* parent, CalendarModel* model)
    : m_dialog(std::make_unique<DialogWindow>(parent, tr("Settings"), 420))
{
    auto content = std::make_unique<ColumnLayout>();
    content->setSpacing(Theme::current().largeSpacing);
    FormCard* card = content->addChild<FormCard>();
    card->setLayoutHints({.fillWidth = true});

    // 0 follows the language.
    static const std::vector<int> weekStarts{0, 7, 1, 6};
    std::vector<std::string> names;
    for (int day : weekStarts)
        names.push_back(day == 0 ? std::string(tr("System Default")) : LocaleData::dayName(day % 7));
    ComboBox* weekStart = card->addRow(tr("Week Starts On"))->addChild<ComboBox>(names);
    weekStart->setName("weekStartBox");
    weekStart->setLayoutHints({.preferredWidth = 180});
    weekStart->setCurrentIndex(std::max(0, int(std::find(weekStarts.begin(), weekStarts.end(), model->weekStart())
                                               - weekStarts.begin()) % int(weekStarts.size())));
    weekStart->onActivated = [model](int index) { model->setWeekStart(weekStarts[size_t(index)]); };

    Switch* numbers = card->addRow(tr("Show Week Numbers"))->addChild<Switch>();
    numbers->setName("weekNumbersSwitch");
    numbers->setChecked(model->showWeekNumbers());
    numbers->onToggled = [model](bool checked) { model->setShowWeekNumbers(checked); };

    m_dialog->setContentView(std::move(content));
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_dialog->close(); });
    m_dialog->onClosed = [this] {
        if (onClosed)
            onClosed();
    };
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::show()
{
    m_dialog->show();
}

Window* SettingsDialog::window() const
{
    return m_dialog->window();
}

// The bubble's month: its name, arrows and days.
class DatePopup::Content : public View
{
public:
    Content(DatePopup* popup, CalendarModel* model, Date selected)
        : m_popup(popup)
        , m_month(Dates::monthStart(selected))
        , m_label(addChild<Label>())
        , m_previous(addChild<RoundImageButton>("chevron-left", 24))
        , m_next(addChild<RoundImageButton>("chevron-right", 24))
        , m_days(addChild<MiniMonth>(model))
    {
        m_label->setFontSize(Style::points(9));
        m_label->setWeight(500);
        m_label->setVerticallyCentered(true);
        m_previous->setIconSize(16);
        m_next->setIconSize(16);
        m_previous->onClicked = [this] { setMonth(m_month.addMonths(-1)); };
        m_next->onClicked = [this] { setMonth(m_month.addMonths(1)); };
        m_days->setCellHeight(28);
        m_days->setSelectedDate(selected);
        m_days->onDayClicked = [this](Date date) {
            auto picked = m_popup->onPicked;
            m_popup->close();
            if (picked)
                picked(date);
        };
        setMonth(m_month);
    }

    cutefish::ui::SizeF sizeHint() const override { return {252, heightForWidth(252)}; }
    double heightForWidth(double) const override { return 10 + 24 + 4 + m_days->sizeHint().height + 14; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        // The column is 2 px taller than its rows, as its margins take 22 of
        // the 24 added; the Layout spreads that over both rows, a pixel lower.
        const double width = geometry().width;
        const double top = 10 + 1;
        m_label->setGeometry({12 + 4, top, width - 24 - 4 - 48, 24});
        m_previous->setGeometry({width - 12 - 48, top, 24, 24});
        m_next->setGeometry({width - 12 - 24, top, 24, 24});
        m_days->setGeometry({12, top + 24 + 4, width - 24, m_days->sizeHint().height});
    }

private:
    void setMonth(Date month)
    {
        m_month = month;
        m_label->setText(LocaleData::monthName(month.month()) + " " + std::to_string(month.year()));
        m_days->setMonth(month);
    }

    DatePopup* m_popup;
    Date m_month;
    Label* m_label;
    RoundImageButton* m_previous;
    RoundImageButton* m_next;
    MiniMonth* m_days;
};

DatePopup::DatePopup(CalendarModel* model, View* anchor, Date selected)
    : m_model(model)
    , m_popup(std::make_unique<DropDownPopup>())
{
    m_popup->setContent<Content>(this, model, selected);
    m_popup->onClosed = [this] {
        if (onClosed)
            onClosed();
    };
    m_popup->popupFor(anchor, 3);
}

DatePopup::~DatePopup() = default;

bool DatePopup::isOpen() const
{
    return m_popup->isOpen();
}

void DatePopup::close()
{
    m_popup->close();
}

Window* DatePopup::window() const
{
    return m_popup->window();
}

DateButton::DateButton(CalendarModel* model, Date date)
    : m_model(model)
    , m_date(date)
    , m_label(addChild<Label>(LocaleData::longDate(date)))
{
    m_label->setTextAlignment(Label::TextAlignment::Center);
    m_label->setVerticallyCentered(true);
}

DateButton::~DateButton() = default;

void DateButton::setDate(Date date)
{
    m_date = date;
    m_label->setText(LocaleData::longDate(date));
    requestLayout();
}

bool DateButton::isPopupOpen() const
{
    return m_popup && m_popup->isOpen();
}

Window* DateButton::popupWindow() const
{
    return isPopupOpen() ? m_popup->window() : nullptr;
}

void DateButton::dismiss()
{
    if (m_popup)
        m_popup->close();
}

SizeF DateButton::sizeHint() const
{
    // As tall as the combo boxes beside it.
    return {std::max(150.0, m_label->sizeHint().width + 24), 32 + 3};
}

void DateButton::layout()
{
    m_label->setGeometry(localRect());
}

void DateButton::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    const Color background = color(m_pressed ? ColorId::ComboBoxBackgroundPressed : ColorId::ComboBoxBackground);
    painter.drawRoundedRect(localRect(), {theme.smallRadius, 0, themeCorners()}, background, 1,
                            isPopupOpen() || hasFocus() ? color(ColorId::ComboBoxFocusBorder) : background);
}

void DateButton::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    m_pressed = true;
    requestPaint();
}

void DateButton::pointerRelease(const PointerEvent& event)
{
    if (!m_pressed)
        return;
    m_pressed = false;
    requestPaint();
    if (localRect().contains(event.position))
        open();
}

bool DateButton::keyPress(const KeyEvent& event)
{
    if (event.modifiers == NoModifier && (event.key == key::Space || event.key == key::Return)) {
        open();
        return true;
    }
    return false;
}

void DateButton::open()
{
    m_popup = std::make_unique<DatePopup>(m_model, this, m_date);
    m_popup->onPicked = [this](Date date) {
        if (onPicked)
            onPicked(date);
    };
    m_popup->onClosed = [this] { requestPaint(); };
    requestPaint();
}
