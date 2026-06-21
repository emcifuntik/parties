#pragma once

// Browser-style text selection + copy for the chat message list.
//
// Design: each plain-text message renders as a <selectable_text> custom element
// that keeps a normal RmlUi text child (so the Slug font engine still draws the
// glyphs). The element reads the child's already-wrapped lines via
// ElementText::GetLines() and measures character x-positions with
// ElementUtilities::GetStringWidth, so the highlight it paints in OnRender (which
// RmlUi runs BEFORE children — i.e. behind the glyphs) lines up exactly with the
// rendered text. A single global ChatSelection owns the cross-message range,
// keyed by message id (never by element pointer, since data-for reuses elements).
//
// Threading: all access happens while the UI mutex is held — input handlers run
// on the message thread under that lock, the render thread holds the same lock
// across Context::Update/Render — so ChatSelection needs no internal locking.

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml { class ElementText; }

namespace parties::client {

class SelectableTextElement;

class ChatSelection {
public:
    static ChatSelection& instance();

    // A caret position: which message, and a codepoint offset into that message's
    // (whitespace-collapsed) display text.
    struct Point {
        int64_t msg_id = 0;
        int     offset = 0;
    };

    // Wired by the platform: returns all chat messages as (id, copy-text) in
    // display order. Snapshotted at drag start so ordering/copy can reach
    // messages that have since scrolled out of the live DOM.
    std::function<std::vector<std::pair<int64_t, std::string>>()> get_messages;

    // Per-message layout, refreshed every frame by the visible elements.
    struct MsgInfo { float content_y = 0; int length = 0; std::string display; };
    void register_msg(int64_t id, const MsgInfo& info);

    // Drag lifecycle (called from input, UI mutex held).
    void begin(Point p);
    void extend(Point p);
    void end_drag();
    void clear();

    bool dragging() const { return dragging_; }
    bool has_selection() const;

    // Selected [start,end) codepoint range within msg_id, or false if this
    // message is entirely outside the selection.
    bool range_for(int64_t msg_id, int& start, int& end) const;

    // The selected text, messages joined with newlines (display order).
    std::string selected_text() const;

private:
    void ordered(Point& lo, Point& hi) const;
    int  index_of(int64_t id) const;
    int  length_of(int64_t id) const;
    std::string text_of(int64_t id) const;

    bool  active_ = false;
    bool  dragging_ = false;
    Point anchor_, focus_;
    std::unordered_map<int64_t, MsgInfo> msgs_;

    // Snapshot of (id -> display index, id -> raw text) taken at begin().
    std::vector<std::pair<int64_t, std::string>> snapshot_;
    std::unordered_map<int64_t, int> snap_index_;
};

// Custom element (tag "selectable_text") used in place of the plain msg-text div.
class SelectableTextElement : public Rml::Element {
public:
    explicit SelectableTextElement(const Rml::String& tag);
    ~SelectableTextElement() override;

    // Map an absolute point to a caret position within this message. Always
    // succeeds for a live element (clamps to the nearest line/char).
    bool HitTest(Rml::Vector2f point, ChatSelection::Point& out);

    int64_t msg_id() const { return msg_id_; }

protected:
    void OnRender() override;
    void OnResize() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;

private:
    // A contiguous run of text on one visual line, coming from one text node
    // (a plain message has one run per line; a URL message's line may be made
    // of several runs — the segment spans — laid out left-to-right).
    struct Fragment {
        Rml::ElementText* run = nullptr;  // for GetStringWidth measuring
        std::string  text;                // display text of this fragment
        int          cp_start = 0;        // global codepoint offset of fragment start
        int          cp_len   = 0;
        float        x_start  = 0;        // local x of fragment start
    };
    // One wrapped visual line: its fragments left-to-right + global cp range.
    struct VisLine {
        float                 y_top = 0;
        float                 height = 0;
        int                   cp_start = 0;   // global cp of first char on the line
        int                   cp_end = 0;     // global cp one past the last char
        std::vector<Fragment> frags;
    };

    void CollectRuns(Rml::Element* el, std::vector<Rml::ElementText*>& out) const;
    void RebuildLines();              // read GetLines() of every run into lines_
    float GlobalCharX(const VisLine& line, int global_cp) const;  // x of a cp edge
    void ReleaseGeom();

    int64_t                     msg_id_ = 0;
    Rml::CompiledGeometryHandle hl_geom_ = 0;
    std::vector<VisLine>        lines_;
    int                         display_len_ = 0;   // total codepoints in display text
    std::string                 display_;           // concatenated display text (lines joined by ' ')
    float                       cached_w_ = -1.0f;
    int                         cached_text_gen_ = -1;
};

} // namespace parties::client
