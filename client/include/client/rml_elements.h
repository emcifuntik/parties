#pragma once

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
    void OnDetach(Rml::Element*) override { delete this; }

private:
    std::function<void(Rml::Event&)> callback_;
};

}
