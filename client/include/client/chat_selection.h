#pragma once

// Design: each plain-text message renders as a <selectable_text> custom element
// that keeps a normal RmlUi text child. The element reads the child's
// already-wrapped lines via
// ElementText::GetLines() and measures character x-positions with
// ElementUtilities::GetStringWidth, so the highlight it paints in OnRender (which
// RmlUi runs BEFORE children — i.e. behind the glyphs) lines up exactly with the
// rendered text. A single global ChatSelection owns the cross-message range,
// keyed by message id (never by element pointer, since data-for reuses elements).

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

    struct Point {
        int64_t msg_id = 0;
        int     offset = 0;
    };

    std::function<std::vector<std::pair<int64_t, std::string>>()> get_messages;

    struct MsgInfo { float content_y = 0; int length = 0; std::string display; };
    void register_msg(int64_t id, const MsgInfo& info);

    void begin(Point p);
    void extend(Point p);
    void end_drag();
    void clear();

    bool dragging() const { return dragging_; }
    bool has_selection() const;

    bool range_for(int64_t msg_id, int& start, int& end) const;

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

    std::vector<std::pair<int64_t, std::string>> snapshot_;
    std::unordered_map<int64_t, int> snap_index_;
};

class SelectableTextElement : public Rml::Element {
public:
    explicit SelectableTextElement(const Rml::String& tag);
    ~SelectableTextElement() override;

    bool HitTest(Rml::Vector2f point, ChatSelection::Point& out);

    int64_t msg_id() const { return msg_id_; }

protected:
    void OnRender() override;
    void OnResize() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;

private:
    struct Fragment {
        Rml::ElementText* run = nullptr;
        std::string  text;
        int          cp_start = 0;
        int          cp_len   = 0;
        float        x_start  = 0;
    };
    struct VisLine {
        float                 y_top = 0;
        float                 height = 0;
        int                   cp_start = 0;
        int                   cp_end = 0;
        std::vector<Fragment> frags;
    };

    void CollectRuns(Rml::Element* el, std::vector<Rml::ElementText*>& out) const;
    void RebuildLines();
    float GlobalCharX(const VisLine& line, int global_cp) const;
    void ReleaseGeom();

    int64_t                     msg_id_ = 0;
    Rml::CompiledGeometryHandle hl_geom_ = 0;
    std::vector<VisLine>        lines_;
    int                         display_len_ = 0;
    std::string                 display_;
    float                       cached_w_ = -1.0f;
    int                         cached_text_gen_ = -1;
};

}
