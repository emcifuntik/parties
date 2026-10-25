#pragma once

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#include <cassert>
#include <type_traits>
#include <utility>

#ifndef PARTIES_RML_CHECK
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

template <typename... Args, typename Fn, std::size_t... I>
void invoke_args_impl(Fn& fn, const Rml::VariantList& v, std::index_sequence<I...>) {
    fn((I < v.size() ? v[I].template Get<Args>() : Args{})...);
}
template <typename... Args, typename Fn>
void invoke_args(Fn& fn, const Rml::VariantList& v) {
    invoke_args_impl<Args...>(fn, v, std::index_sequence_for<Args...>{});
}

}

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

    Property& operator=(const T& v) { return assign(v); }
    Property& operator=(T&& v) { return assign(std::move(v)); }
    Property& operator=(const Property& other) { return assign(other.value_); }

    const T& get() const { return value_; }
    operator const T&() const { return value_; }

    T& silent() { return value_; }
    void notify() { mark_dirty(); }
    template <typename Fn>
    void mutate(Fn&& fn) { std::forward<Fn>(fn)(value_); mark_dirty(); }

    T* address() { return &value_; }
    void bind_to(Model* owner, Rml::String name) {
        owner_ = owner;
        name_ = std::move(name);
    }

private:
    template <typename V>
    Property& assign(V&& v) {
        if constexpr (detail::is_change_guarded_v<T>) {
            if (value_ == v) return *this;
        }
        value_ = std::forward<V>(v);
        mark_dirty();
        return *this;
    }
    void mark_dirty();

    T value_{};
    Model* owner_ = nullptr;
    Rml::String name_;
};

template <typename T>
using Prop = Property<T>;

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

class Builder {
public:
    Builder(Rml::DataModelConstructor ctor, Model* owner) : ctor_(ctor), owner_(owner) {}

    Rml::DataModelConstructor& ctor() { return ctor_; }

    template <typename T, typename Fn>
    Builder& register_struct(Fn&& configure) {
        StructBuilder<T> sb(ctor_.RegisterStruct<T>());
        assert(bool(sb) && "RmlUi struct already registered");
        configure(sb);
        return *this;
    }

    template <typename Container>
    Builder& register_array() {
        PARTIES_RML_CHECK(ctor_.RegisterArray<Container>());
        return *this;
    }

    template <typename T>
    Builder& bind(const Rml::String& name, Property<T>& prop) {
        prop.bind_to(owner_, name);
        PARTIES_RML_CHECK(ctor_.Bind(name, prop.address()));
        return *this;
    }

    template <typename T>
    Builder& bind_raw(const Rml::String& name, T* ptr) {
        PARTIES_RML_CHECK(ctor_.Bind(name, ptr));
        return *this;
    }

    template <typename Fn>
    Builder& on(const Rml::String& name, Fn&& fn) {
        ctor_.BindEventCallback(
            name, [fn = std::forward<Fn>(fn)](Rml::DataModelHandle, Rml::Event&,
                                              const Rml::VariantList&) mutable { fn(); });
        return *this;
    }

    template <typename... Args, typename Fn>
    Builder& on_args(const Rml::String& name, Fn&& fn) {
        ctor_.BindEventCallback(
            name, [fn = std::forward<Fn>(fn)](Rml::DataModelHandle, Rml::Event&,
                                              const Rml::VariantList& args) mutable {
                detail::invoke_args<Args...>(fn, args);
            });
        return *this;
    }

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

class Model {
public:
    Model() = default;
    virtual ~Model() = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

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

    void dirty(const Rml::String& variable) {
        if (ready_) handle_.DirtyVariable(variable);
    }
    void dirty_all() {
        if (ready_) handle_.DirtyAllVariables();
    }

    bool ready() const { return ready_; }

protected:
    virtual const char* model_name() const = 0;
    virtual void build(Builder& b) = 0;

private:
    Rml::DataModelHandle handle_;
    bool ready_ = false;
};

template <typename T>
inline void Property<T>::mark_dirty() {
    if (owner_) owner_->dirty(name_);
}

}
