#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// parties::rml — a thin, modern-C++ wrapper over RmlUi data-model bindings.
//
// RmlUi's DataModelConstructor API is powerful but verbose, and its dirty
// tracking is stringly-typed: every change is `member = value; dirty("member")`
// with the field name duplicated as an unchecked literal. This wrapper removes
// that ceremony:
//
//   * Property<T> (alias Prop<T>) — an observable member. Assignment auto-dirties
//     the bound variable (with a change-guard so identical writes are no-ops).
//     The registered name is supplied once, in build(); call sites never repeat
//     it. For containers, mutate in place via .silent()/.mutate()/.notify().
//
//   * Model — a base class that owns CreateDataModel / GetModelHandle / the
//     handle lifetime, exposes dirty()/dirty_all() (the latter backed by RmlUi's
//     built-in DirtyAllVariables, replacing hand-written name lists), and is
//     safe to mutate before init() (writes are no-ops until the handle exists).
//
//   * Builder — a fluent facade over DataModelConstructor: register_struct /
//     register_array / bind / bind_raw, plus terse event helpers (on / on_args /
//     on_event) that hide the (DataModelHandle, Event&, VariantList&) signature
//     and extract typed arguments.
//
// The wrapper is a pure ergonomic facade: it changes only the C++ side. The RML
// contract is untouched — variables and event handlers are registered under the
// exact same names, with the same argument shapes, as before.
//
// Designed for RmlUi 6.3.x (Rml::String = std::string, Rml::Vector = std::vector,
// Rml::Function = std::function) and C++23.
// ─────────────────────────────────────────────────────────────────────────────

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#include <cassert>
#include <type_traits>
#include <utility>

#ifndef PARTIES_RML_CHECK
// Surface the bool returned by Bind/RegisterArray/RegisterMember (the existing
// code ignores them). A failed bind means a silently-dead RML binding; catch it
// in debug. In release the expression is still evaluated (the bind still runs).
#define PARTIES_RML_CHECK(expr)                                                   \
    do {                                                                          \
        const bool parties_rml_ok_ = (expr);                                      \
        assert(parties_rml_ok_ && "RmlUi data binding failed");                   \
        (void)parties_rml_ok_;                                                    \
    } while (0)
#endif

namespace parties::rml {

class Model;
class Builder;

namespace detail {

// Types for which assignment skips a redundant dirty when the value is unchanged.
// Restricted to cheap, always-comparable scalars/strings on purpose: a container
// like Rml::Vector<Struct> may hold a non-comparable element type (no operator==),
// and the MSVC STL's vector::operator== is unconstrained — so a generic "has !="
// probe would report true yet hard-error when the comparison is instantiated.
// Containers are therefore never guarded: assigning a whole container always
// dirties (which is what callers want when replacing it wholesale).
template <typename T>
inline constexpr bool is_change_guarded_v =
    std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, Rml::String>;

// Invoke a callable with arguments extracted positionally from a VariantList,
// converting each to the declared parameter type (default-constructed if absent).
template <typename... Args, typename Fn, std::size_t... I>
void invoke_args_impl(Fn& fn, const Rml::VariantList& v, std::index_sequence<I...>) {
    fn((I < v.size() ? v[I].template Get<Args>() : Args{})...);
}
template <typename... Args, typename Fn>
void invoke_args(Fn& fn, const Rml::VariantList& v) {
    invoke_args_impl<Args...>(fn, v, std::index_sequence_for<Args...>{});
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Property<T> — observable, auto-dirtying bound member.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class Property {
public:
    using value_type = T;

    Property() = default;
    Property(T initial) : value_(std::move(initial)) {}

    // Bound by address into RmlUi and tied to a single Model — never copied or
    // moved (which would invalidate the pointer RmlUi holds). This also makes
    // the enclosing Model non-copyable/movable, which is correct.
    Property(const Property&) = delete;
    Property(Property&&) = delete;

    // value assignment — auto-dirties (with change-guard).
    Property& operator=(const T& v) { return assign(v); }
    Property& operator=(T&& v) { return assign(std::move(v)); }
    // copying from another Property copies only the VALUE (and dirties via this
    // property's own binding), e.g. chat_model_.flag = lobby_model_.flag;
    Property& operator=(const Property& other) { return assign(other.value_); }

    // read access
    const T& get() const { return value_; }
    operator const T&() const { return value_; }

    // container / in-place mutation (no implicit dirty on element edits)
    T& silent() { return value_; }                       // edit without dirtying
    void notify() { mark_dirty(); }                      // dirty explicitly after silent edits
    template <typename Fn>
    void mutate(Fn&& fn) { std::forward<Fn>(fn)(value_); mark_dirty(); }

    // --- internal: used by Builder during registration ---
    T* address() { return &value_; }
    void bind_to(Model* owner, Rml::String name) {
        owner_ = owner;
        name_ = std::move(name);
    }

private:
    template <typename V>
    Property& assign(V&& v) {
        if constexpr (detail::is_change_guarded_v<T>) {
            if (value_ == v) return *this;               // unchanged → skip dirty
        }
        value_ = std::forward<V>(v);
        mark_dirty();
        return *this;
    }
    void mark_dirty();                                   // defined after Model

    T value_{};
    Model* owner_ = nullptr;
    Rml::String name_;
};

template <typename T>
using Prop = Property<T>;

// ─────────────────────────────────────────────────────────────────────────────
// StructHandle wrapper — fluent member registration.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class StructBuilder {
public:
    explicit StructBuilder(Rml::StructHandle<T> handle) : handle_(handle) {}

    template <typename M>
    StructBuilder& member(const Rml::String& name, M T::* member_ptr) {
        PARTIES_RML_CHECK(handle_.RegisterMember(name, member_ptr));
        return *this;
    }

    explicit operator bool() const { return bool(handle_); }

private:
    Rml::StructHandle<T> handle_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Builder — fluent facade over Rml::DataModelConstructor.
// ─────────────────────────────────────────────────────────────────────────────
class Builder {
public:
    Builder(Rml::DataModelConstructor ctor, Model* owner) : ctor_(ctor), owner_(owner) {}

    Rml::DataModelConstructor& ctor() { return ctor_; }

    // Register a struct type. `configure` receives a StructBuilder<T>&.
    template <typename T, typename Fn>
    Builder& register_struct(Fn&& configure) {
        StructBuilder<T> sb(ctor_.RegisterStruct<T>());
        assert(bool(sb) && "RmlUi struct already registered");
        configure(sb);
        return *this;
    }

    // Register an array container type (must come after its element struct).
    template <typename Container>
    Builder& register_array() {
        PARTIES_RML_CHECK(ctor_.RegisterArray<Container>());
        return *this;
    }

    // Bind an observable Property<T> by name (scalar or container).
    template <typename T>
    Builder& bind(const Rml::String& name, Property<T>& prop) {
        prop.bind_to(owner_, name);
        PARTIES_RML_CHECK(ctor_.Bind(name, prop.address()));
        return *this;
    }

    // Bind a raw pointer by name (for runtime / type-erased models, e.g. the
    // designer's dynamic preview models, where Property<T> does not apply).
    template <typename T>
    Builder& bind_raw(const Rml::String& name, T* ptr) {
        PARTIES_RML_CHECK(ctor_.Bind(name, ptr));
        return *this;
    }

    // Event with no arguments.
    template <typename Fn>
    Builder& on(const Rml::String& name, Fn&& fn) {
        ctor_.BindEventCallback(
            name, [fn = std::forward<Fn>(fn)](Rml::DataModelHandle, Rml::Event&,
                                              const Rml::VariantList&) mutable { fn(); });
        return *this;
    }

    // Event with typed positional arguments extracted from the VariantList,
    // e.g. b.on_args<int>("join_channel", [this](int id){ ... });
    template <typename... Args, typename Fn>
    Builder& on_args(const Rml::String& name, Fn&& fn) {
        ctor_.BindEventCallback(
            name, [fn = std::forward<Fn>(fn)](Rml::DataModelHandle, Rml::Event&,
                                              const Rml::VariantList& args) mutable {
                detail::invoke_args<Args...>(fn, args);
            });
        return *this;
    }

    // Event needing the raw Event& (and args) — e.g. mouse button / key checks.
    template <typename Fn>
    Builder& on_event(const Rml::String& name, Fn&& fn) {
        ctor_.BindEventCallback(
            name, [fn = std::forward<Fn>(fn)](Rml::DataModelHandle, Rml::Event& ev,
                                              const Rml::VariantList& args) mutable {
                fn(ev, args);
            });
        return *this;
    }

private:
    Rml::DataModelConstructor ctor_;
    Model* owner_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Model — base class encapsulating the data-model lifecycle.
// ─────────────────────────────────────────────────────────────────────────────
class Model {
public:
    Model() = default;
    virtual ~Model() = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // Create the data model and register everything via build().
    bool init(Rml::Context* context) {
        if (!context) return false;
        Rml::DataModelConstructor ctor = context->CreateDataModel(model_name());
        if (!ctor) return false;
        Builder builder(ctor, this);
        build(builder);
        handle_ = ctor.GetModelHandle();
        ready_ = true;
        return true;
    }

    // Mark a single bound variable dirty (no-op before init).
    void dirty(const Rml::String& variable) {
        if (ready_) handle_.DirtyVariable(variable);
    }
    // Mark every bound variable dirty (RmlUi built-in; no hand-maintained list).
    void dirty_all() {
        if (ready_) handle_.DirtyAllVariables();
    }

    bool ready() const { return ready_; }

protected:
    // The data-model name passed to Context::CreateDataModel (e.g. "serverlist").
    virtual const char* model_name() const = 0;
    // Register structs, arrays, variables and event callbacks on `b`.
    virtual void build(Builder& b) = 0;

private:
    Rml::DataModelHandle handle_;
    bool ready_ = false;
};

// out-of-line: Property needs the complete Model type to call dirty().
template <typename T>
inline void Property<T>::mark_dirty() {
    if (owner_) owner_->dirty(name_);
}

} // namespace parties::rml
