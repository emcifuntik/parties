#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// parties::rml element helpers — companion to rml_binding.h for the *custom
// element* binding surface (distinct from data-model bindings).
//
//   * ElementRegistry — registers custom RmlUi elements via RmlUi's own
//     ElementInstancerGeneric<T> (new T(tag) / delete), replacing the
//     hand-written pass-through ElementInstancer classes, and owns the
//     instancer lifetimes in one place.
//
//   * OwnedListener — a self-deleting Rml::EventListener. RmlUi never deletes
//     listeners attached via Element::AddEventListener, so this frees itself on
//     OnDetach, fixing the manual-wiring leak.
// ─────────────────────────────────────────────────────────────────────────────

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <memory>
#include <vector>

namespace parties::rml {

// Registers custom RmlUi elements and owns their instancers. Keep one registry
// alive for the process lifetime (RmlUi's Factory stores raw pointers to the
// instancers), e.g. as a member of the application object.
class ElementRegistry {
public:
    // Register element type ElementT under the given RML tag. ElementT must have
    // a constructor taking (const Rml::String& tag).
    template <typename ElementT>
    void add(const Rml::String& tag) {
        auto instancer = std::make_unique<Rml::ElementInstancerGeneric<ElementT>>();
        Rml::Factory::RegisterElementInstancer(tag, instancer.get());
        instancers_.push_back(std::move(instancer));
    }

private:
    std::vector<std::unique_ptr<Rml::ElementInstancer>> instancers_;
};

// Self-owning event listener: deletes itself once RmlUi detaches it.
class OwnedListener final : public Rml::EventListener {
public:
    explicit OwnedListener(std::function<void(Rml::Event&)> callback)
        : callback_(std::move(callback)) {}

    void ProcessEvent(Rml::Event& event) override {
        if (callback_) callback_(event);
    }
    void OnDetach(Rml::Element* /*element*/) override { delete this; }

private:
    std::function<void(Rml::Event&)> callback_;
};

} // namespace parties::rml
