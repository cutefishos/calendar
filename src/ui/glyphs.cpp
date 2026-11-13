// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "glyphs.h"

#include <ui/icon.h>

static const char* const AddGlyph =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" fill="none">)"
    R"(<path d="M7.5 2.5v10M2.5 7.5h10" stroke="currentColor" stroke-linecap="round"/></svg>)";

void registerCalendarGlyphs()
{
    cutefish::ui::Icon::registerGlyph("calendar-add", AddGlyph);
}
