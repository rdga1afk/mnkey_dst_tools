#pragma once
// "Kenshi data" tab (#196 F1): read-only view of the install's items (category tree of fcs_layout.def, item list, card of fields from fcs.def). Draws with ImGui;
// the data model is DataBrowser (data_browser.h). Loads the install (KENSHI_DIR) on the first draw.
namespace KenshiDataPanel {
void Draw();
}
