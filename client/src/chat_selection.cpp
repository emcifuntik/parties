#include <client/chat_selection.h>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Vertex.h>

#include <algorithm>
#include <cstdint>

namespace parties::client {

namespace {

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

int cp_count(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if (!is_cont(c)) ++n;
    return n;
}

size_t byte_of_cp(const std::string& s, int cp) {
    if (cp <= 0) return 0;
    int seen = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!is_cont(static_cast<unsigned char>(s[i]))) {
            if (seen == cp) return i;
            ++seen;
        }
    }
    return s.size();
}

std::string substr_cp(const std::string& s, int start_cp, int end_cp) {
    size_t a = byte_of_cp(s, start_cp);
    size_t b = byte_of_cp(s, end_cp);
    if (b < a) b = a;
    return s.substr(a, b - a);
}

Rml::ColourbPremultiplied selection_colour() {
    constexpr uint8_t r = 0x5B, g = 0x8C, b = 0xFF, a = 97;
    return Rml::ColourbPremultiplied(
        static_cast<uint8_t>(r * a / 255),
        static_cast<uint8_t>(g * a / 255),
        static_cast<uint8_t>(b * a / 255), a);
}

}

ChatSelection& ChatSelection::instance() {
    static ChatSelection s;
    return s;
}

void ChatSelection::register_msg(int64_t id, const MsgInfo& info) {
    if (id != 0) msgs_[id] = info;
}

void ChatSelection::begin(Point p) {
    anchor_ = focus_ = p;
    active_ = true;
    dragging_ = true;
    snapshot_ = get_messages ? get_messages() : std::vector<std::pair<int64_t, std::string>>{};
    snap_index_.clear();
    for (int i = 0; i < static_cast<int>(snapshot_.size()); ++i)
        snap_index_[snapshot_[i].first] = i;
}

void ChatSelection::extend(Point p) {
    if (!active_) return;
    focus_ = p;
}

void ChatSelection::end_drag() { dragging_ = false; }

void ChatSelection::clear() {
    active_ = false;
    dragging_ = false;
    anchor_ = focus_ = Point{};
}

bool ChatSelection::has_selection() const {
    return active_ && !(anchor_.msg_id == focus_.msg_id && anchor_.offset == focus_.offset);
}

int ChatSelection::index_of(int64_t id) const {
    auto si = snap_index_.find(id);
    if (si != snap_index_.end()) return si->second;
    auto it = msgs_.find(id);
    if (it == msgs_.end()) return 0;
    int idx = 0;
    for (auto& [oid, info] : msgs_)
        if (info.content_y < it->second.content_y) ++idx;
    return idx;
}

int ChatSelection::length_of(int64_t id) const {
    auto it = msgs_.find(id);
    return it != msgs_.end() ? it->second.length : 0;
}

std::string ChatSelection::text_of(int64_t id) const {
    auto it = msgs_.find(id);
    if (it != msgs_.end()) return it->second.display;
    auto si = snap_index_.find(id);
    if (si != snap_index_.end()) return snapshot_[si->second].second;
    return {};
}

void ChatSelection::ordered(Point& lo, Point& hi) const {
    const int ia = index_of(anchor_.msg_id);
    const int ib = index_of(focus_.msg_id);
    bool anchor_first;
    if (ia != ib) anchor_first = ia < ib;
    else          anchor_first = anchor_.offset <= focus_.offset;
    lo = anchor_first ? anchor_ : focus_;
    hi = anchor_first ? focus_  : anchor_;
}

bool ChatSelection::range_for(int64_t msg_id, int& start, int& end) const {
    if (!has_selection()) return false;
    Point lo, hi;
    ordered(lo, hi);
    const int il = index_of(lo.msg_id);
    const int ih = index_of(hi.msg_id);
    const int im = index_of(msg_id);
    if (im < il || im > ih) return false;
    start = (msg_id == lo.msg_id) ? lo.offset : 0;
    end   = (msg_id == hi.msg_id) ? hi.offset : length_of(msg_id);
    return end > start;
}

std::string ChatSelection::selected_text() const {
    if (!has_selection()) return {};
    Point lo, hi;
    ordered(lo, hi);
    const int il = index_of(lo.msg_id);
    const int ih = index_of(hi.msg_id);

    std::string out;
    auto append = [&](const std::string& piece) {
        if (!out.empty()) out += '\n';
        out += piece;
    };

    if (!snapshot_.empty()) {
        for (auto& [id, raw] : snapshot_) {
            const int idx = index_of(id);
            if (idx < il || idx > ih) continue;
            if (id == lo.msg_id || id == hi.msg_id) {
                const std::string& disp = text_of(id);
                int a = (id == lo.msg_id) ? lo.offset : 0;
                int b = (id == hi.msg_id) ? hi.offset : cp_count(disp);
                append(substr_cp(disp, a, b));
            } else {
                append(raw);
            }
        }
    } else {
        const std::string& dl = text_of(lo.msg_id);
        if (lo.msg_id == hi.msg_id) {
            out = substr_cp(dl, lo.offset, hi.offset);
        } else {
            out = substr_cp(dl, lo.offset, cp_count(dl));
            append(substr_cp(text_of(hi.msg_id), 0, hi.offset));
        }
    }
    return out;
}

SelectableTextElement::SelectableTextElement(const Rml::String& tag) : Rml::Element(tag) {}

SelectableTextElement::~SelectableTextElement() { ReleaseGeom(); }

void SelectableTextElement::ReleaseGeom() {
    if (hl_geom_) {
        if (auto* ri = Rml::GetRenderInterface()) ri->ReleaseGeometry(hl_geom_);
        hl_geom_ = 0;
    }
}

void SelectableTextElement::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    auto it = changed.find("msgid");
    if (it != changed.end()) {
        msg_id_ = static_cast<int64_t>(it->second.Get<double>(0.0));
        cached_w_ = -1.0f;
    }
}

void SelectableTextElement::OnResize() {
    Rml::Element::OnResize();
    cached_w_ = -1.0f;
}

void SelectableTextElement::CollectRuns(Rml::Element* el, std::vector<Rml::ElementText*>& out) const {
    const int n = el->GetNumChildren(true);
    for (int i = 0; i < n; ++i) {
        Rml::Element* c = el->GetChild(i);
        if (!c) continue;
        if (c->GetTagName() == "#text") {
            if (auto* t = rmlui_dynamic_cast<Rml::ElementText*>(c))
                out.push_back(t);
        } else {
            CollectRuns(c, out);
        }
    }
}

void SelectableTextElement::RebuildLines() {
    lines_.clear();
    display_.clear();
    display_len_ = 0;
    cached_w_ = GetBox().GetSize(Rml::BoxArea::Content).x;

    std::vector<Rml::ElementText*> runs;
    CollectRuns(this, runs);
    if (runs.empty()) return;

    const Rml::Vector2f self_origin = GetAbsoluteOffset(Rml::BoxArea::Content);
    const float box_h = GetBox().GetSize(Rml::BoxArea::Content).y;

    struct Frag { Rml::ElementText* run; std::string text; float x; float y; int cp_len; };
    std::vector<Frag> frags;
    for (auto* run : runs) {
        Rml::Element* parent = run->GetParentNode() ? run->GetParentNode() : this;
        const Rml::Vector2f run_local = parent->GetAbsoluteOffset(Rml::BoxArea::Content) - self_origin;
        for (const auto& gl : run->GetLines()) {
            if (gl.text.empty()) continue;
            frags.push_back({ run, gl.text, run_local.x + gl.position.x,
                              run_local.y + gl.position.y, cp_count(gl.text) });
        }
    }
    if (frags.empty()) return;

    std::sort(frags.begin(), frags.end(), [](const Frag& a, const Frag& b) {
        if (std::abs(a.y - b.y) > 1.0f) return a.y < b.y;
        return a.x < b.x;
    });
    constexpr float kYEps = 2.0f;
    int cp = 0;
    for (size_t i = 0; i < frags.size(); ) {
        const float line_y = frags[i].y;
        VisLine line;
        line.y_top = line_y;
        line.cp_start = cp;
        size_t j = i;
        for (; j < frags.size() && std::abs(frags[j].y - line_y) <= kYEps; ++j) {
            Frag& f = frags[j];
            line.frags.push_back({ f.run, f.text, cp, f.cp_len, f.x });
            display_ += f.text;
            cp += f.cp_len;
        }
        line.cp_end = cp;
        lines_.push_back(std::move(line));
        i = j;
        if (i < frags.size()) { display_ += ' '; ++cp; }
    }
    display_len_ = cp;

    float line_h = box_h;
    if (lines_.size() >= 2) line_h = lines_[1].y_top - lines_[0].y_top;
    if (line_h <= 0) line_h = box_h;
    for (auto& l : lines_) l.height = line_h;
}

float SelectableTextElement::GlobalCharX(const VisLine& line, int global_cp) const {
    if (line.frags.empty()) return 0.0f;
    if (global_cp <= line.frags.front().cp_start) return line.frags.front().x_start;
    for (const Fragment& f : line.frags) {
        const int fend = f.cp_start + f.cp_len;
        if (global_cp <= fend) {
            const int in = global_cp - f.cp_start;
            if (in <= 0) return f.x_start;
            size_t bytes = byte_of_cp(f.text, in);
            int w = Rml::ElementUtilities::GetStringWidth(
                f.run, Rml::StringView(f.text.data(), f.text.data() + bytes));
            return f.x_start + static_cast<float>(w);
        }
    }
    const Fragment& f = line.frags.back();
    int w = Rml::ElementUtilities::GetStringWidth(
        f.run, Rml::StringView(f.text.data(), f.text.data() + f.text.size()));
    return f.x_start + static_cast<float>(w);
}

bool SelectableTextElement::HitTest(Rml::Vector2f point, ChatSelection::Point& out) {
    if (cached_w_ < 0.0f || cached_w_ != GetBox().GetSize(Rml::BoxArea::Content).x)
        RebuildLines();
    out.msg_id = msg_id_;
    out.offset = 0;
    if (lines_.empty()) return true;

    const Rml::Vector2f origin = GetAbsoluteOffset(Rml::BoxArea::Content);
    const float localY = point.y - origin.y;
    const float localX = point.x - origin.x;

    size_t li = 0;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (localY >= lines_[i].y_top) li = i;
    }
    const VisLine& line = lines_[li];

    int chosen = line.cp_start;
    float prev = line.frags.empty() ? 0.0f : line.frags.front().x_start;
    for (int cp = line.cp_start + 1; cp <= line.cp_end; ++cp) {
        float x = GlobalCharX(line, cp);
        if (localX < (prev + x) * 0.5f) { chosen = cp - 1; break; }
        prev = x;
        chosen = cp;
    }
    out.offset = chosen;
    return true;
}

void SelectableTextElement::OnRender() {
    if (msg_id_ == 0) return;

    if (cached_w_ < 0.0f || cached_w_ != GetBox().GetSize(Rml::BoxArea::Content).x)
        RebuildLines();
    if (lines_.empty()) return;

    auto& sel = ChatSelection::instance();
    const Rml::Vector2f origin = GetAbsoluteOffset(Rml::BoxArea::Content);
    sel.register_msg(msg_id_, { origin.y, display_len_, display_ });

    int start = 0, end = 0;
    if (!sel.range_for(msg_id_, start, end)) return;

    std::vector<Rml::Vertex> verts;
    std::vector<int> idx;
    const Rml::ColourbPremultiplied col = selection_colour();
    const float content_w = GetBox().GetSize(Rml::BoxArea::Content).x;

    for (const VisLine& line : lines_) {
        if (end <= line.cp_start || start >= line.cp_end) continue;
        const int s = std::max(start, line.cp_start);
        const int e = std::min(end, line.cp_end);
        const bool continues_past = end > line.cp_end;

        float x0 = GlobalCharX(line, s);
        float x1 = continues_past ? content_w : GlobalCharX(line, e);
        if (x1 <= x0 && !continues_past) continue;
        if (x1 < x0) x1 = x0;
        float y0 = line.y_top;
        float y1 = line.y_top + line.height;

        const int base = static_cast<int>(verts.size());
        verts.push_back({{x0, y0}, col, {0, 0}});
        verts.push_back({{x1, y0}, col, {0, 0}});
        verts.push_back({{x1, y1}, col, {0, 0}});
        verts.push_back({{x0, y1}, col, {0, 0}});
        idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    if (verts.empty()) return;

    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;
    ReleaseGeom();
    hl_geom_ = ri->CompileGeometry({verts.data(), verts.size()}, {idx.data(), idx.size()});
    if (hl_geom_)
        ri->RenderGeometry(hl_geom_, origin, 0);
}

}
