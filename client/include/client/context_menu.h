#pragma once

#include <string>
#include <vector>

typedef struct HWND__* HWND;

namespace parties::client {

class ContextMenu {
public:
    struct Item {
        std::wstring label;
        int id = 0;
        bool danger = false;    // red text (e.g. "Kick", "Delete")
        bool separator = false; // horizontal divider (label/id ignored)
    };

    // Show popup at cursor position. Blocks until selection or dismiss.
    // Returns selected item ID, or 0 if dismissed.
    static int show(HWND parent, const std::vector<Item>& items);
};

} // namespace parties::client
