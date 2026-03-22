// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once
#include <algorithm>
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    using EventArgs = std::vector<std::any>;
    using HandleEventCallback = std::function<void(const EventArgs&)>;

    class EventHandler;

    class EventHandle
    {
    public:
        EventHandle(EventHandler* handler, const std::string& name, HandleEventCallback callback, void* scope = nullptr,
            bool once = false);

        void callback(const EventArgs& args) const;

        // Remove this event from its handler.
        void off();

        // Mark if the event has been removed
        void setRemoved(bool value);

        [[nodiscard]] bool removed() const { return _removed; }

    private:
        friend class EventHandler;

        EventHandler* _handler;

        std::string _name;

        HandleEventCallback _callback;

        void* _scope;

        bool _once;

        bool _removed;
    };

    /**
     * Abstract base class that implements functionality for event handling
     */
    class EventHandler
    {
    public:
        virtual ~EventHandler() = default;

        EventHandle* on(const std::string& name, HandleEventCallback callback, void* scope = nullptr);
        EventHandle* once(const std::string& name, HandleEventCallback callback, void* scope = nullptr);

        template<typename Callback>
        EventHandle* on(const std::string& name, Callback&& callback, void* scope = nullptr)
            requires(!std::is_same_v<std::decay_t<Callback>, HandleEventCallback>)
        {
            return on(name, adaptCallback(std::forward<Callback>(callback)), scope);
        }

        template<typename Callback>
        EventHandle* once(const std::string& name, Callback&& callback, void* scope = nullptr)
            requires(!std::is_same_v<std::decay_t<Callback>, HandleEventCallback>)
        {
            return once(name, adaptCallback(std::forward<Callback>(callback)), scope);
        }

        // Removes callbacks. If name is empty, all events are removed.
        EventHandler* off(const std::string& name = "", const HandleEventCallback& callback = HandleEventCallback(),
                          void* scope = nullptr);
        EventHandler* offByHandle(EventHandle* handle);

        template<typename Callback>
        EventHandler* off(const std::string& name, Callback&& callback, void* scope = nullptr)
            requires(!std::is_same_v<std::decay_t<Callback>, HandleEventCallback>)
        {
            return off(name, adaptCallback(std::forward<Callback>(callback)), scope);
        }

        void initEventHandler();

        template<typename... Args>
        EventHandler* fire(const std::string& name, Args&&... args);

        [[nodiscard]] bool hasEvent(const std::string& name) const;

    protected:
        EventHandle* addCallback(const std::string& name, HandleEventCallback callback, void* scope = nullptr, bool once = false);

    private:
        void compactRemovedHandles();

        template<typename T>
        struct function_traits;

        template<typename R, typename... FnArgs>
        struct function_traits<R(*)(FnArgs...)>
        {
            using args_tuple = std::tuple<FnArgs...>;
            static constexpr size_t arity = sizeof...(FnArgs);
        };

        template<typename R, typename... FnArgs>
        struct function_traits<std::function<R(FnArgs...)>>
        {
            using args_tuple = std::tuple<FnArgs...>;
            static constexpr size_t arity = sizeof...(FnArgs);
        };

        template<typename C, typename R, typename... FnArgs>
        struct function_traits<R(C::*)(FnArgs...) const>
        {
            using args_tuple = std::tuple<FnArgs...>;
            static constexpr size_t arity = sizeof...(FnArgs);
        };

        template<typename C, typename R, typename... FnArgs>
        struct function_traits<R(C::*)(FnArgs...)>
        {
            using args_tuple = std::tuple<FnArgs...>;
            static constexpr size_t arity = sizeof...(FnArgs);
        };

        template<typename F>
        struct function_traits : function_traits<decltype(&F::operator())> {};

        template<typename Arg>
        static bool canReadArg(const std::any& value)
        {
            using Value = std::remove_cvref_t<Arg>;
            return std::any_cast<Value>(&value) != nullptr;
        }

        template<typename Arg>
        static decltype(auto) readArg(const std::any& value)
        {
            using Value = std::remove_cvref_t<Arg>;
            static_assert(!std::is_lvalue_reference_v<Arg> || std::is_const_v<std::remove_reference_t<Arg>>,
                "Event callback args cannot be non-const lvalue references.");

            const auto* ptr = std::any_cast<Value>(&value);
            if constexpr (std::is_reference_v<Arg>) {
                return static_cast<const Value&>(*ptr);
            } else {
                return static_cast<Value>(*ptr);
            }
        }

        template<typename Tuple, typename Callback, size_t... I>
        static bool invokeTuple(Callback& callback, const EventArgs& args, std::index_sequence<I...>)
        {
            if (args.size() < sizeof...(I)) {
                return false;
            }

            if (!(canReadArg<std::tuple_element_t<I, Tuple>>(args[I]) && ...)) {
                return false;
            }

            callback(readArg<std::tuple_element_t<I, Tuple>>(args[I])...);
            return true;
        }

        template<typename Callback>
        static HandleEventCallback adaptCallback(Callback&& callback)
        {
            using Fn = std::decay_t<Callback>;
            static_assert(std::is_invocable_v<Fn, const EventArgs&> || std::is_invocable_v<Fn> ||
                          (function_traits<Fn>::arity > 0),
                          "Event callback must be invocable as fn(), fn(const EventArgs&) or fn(arg0, ...).");

            if constexpr (std::is_invocable_v<Callback, const EventArgs&>) {
                return [fn = std::forward<Callback>(callback)](const EventArgs& args) mutable {
                    fn(args);
                };
            } else if constexpr (std::is_invocable_v<Callback>) {
                return [fn = std::forward<Callback>(callback)](const EventArgs&) mutable {
                    fn();
                };
            } else {
                using Traits = function_traits<Fn>;
                using Tuple = typename Traits::args_tuple;
                return [fn = std::forward<Callback>(callback)](const EventArgs& args) mutable {
                    (void)invokeTuple<Tuple>(fn, args, std::make_index_sequence<Traits::arity>{});
                };
            }
        }

        // Map of event names to their callback lists
        std::unordered_map<std::string, std::vector<EventHandle*>> _callbacks;

        // Map of currently active (executing) callback lists
        std::unordered_map<std::string, std::vector<EventHandle*>> _callbackActive;

        // Owner storage to avoid leaks while preserving stable handles.
        std::vector<std::unique_ptr<EventHandle>> _eventHandles;
    };

    template<typename... Args>
    EventHandler* EventHandler::fire(const std::string& name, Args&&... args)
    {
        if (name.empty()) {
            return this;
        }

        auto callbacksIt = _callbacks.find(name);
        if (callbacksIt == _callbacks.end()) {
            return this;
        }

        std::vector<EventHandle*>* callbacks = nullptr;
        std::vector<EventHandle*>& callbacksInitial = callbacksIt->second;

        auto activeIt = _callbackActive.find(name);
        if (activeIt == _callbackActive.end()) {
            // when starting callback execution ensure we store a list of initial callbacks
            _callbackActive[name] = callbacksInitial;
        } else if (activeIt->second != callbacksInitial) {
            // if we are trying to execute a callback while there is an active execution right now
            // and the active list has been already modified,
            // then we go to an unoptimized path and clone the callbacks list to ensure execution consistency
            static thread_local std::vector<EventHandle*> tempCallbacks;
            tempCallbacks = callbacksInitial;
            callbacks = &tempCallbacks;
        }

        std::vector<EventHandle*>& activeCallbacks = callbacks ? *callbacks : _callbackActive[name];
        EventArgs eventArgs;
        eventArgs.reserve(sizeof...(Args));
        (eventArgs.emplace_back(std::forward<Args>(args)), ...);

        for (size_t i = 0; i < activeCallbacks.size(); i++) {
            EventHandle* evt = activeCallbacks[i];
            if (!evt || !evt->_callback || evt->_removed)
            {
                continue;
            }

            evt->callback(eventArgs);

            if (evt->_once) {
                // check that callback still exists because user may have unsubscribed in the event handler
                auto existingCallbackIt = _callbacks.find(name);
                if (existingCallbackIt == _callbacks.end())
                {
                    continue;
                }

                std::vector<EventHandle*>& existingCallback = existingCallbackIt->second;
                auto it = std::find(existingCallback.begin(), existingCallback.end(), evt);
                int ind = (it != existingCallback.end()) ? std::distance(existingCallback.begin(), it) : -1;

                if (ind != -1) {
                    if (_callbackActive[name] == existingCallback) {
                        _callbackActive[name] = std::vector<EventHandle*>(existingCallback);
                    }

                    auto callbacksIt2 = _callbacks.find(name);
                    if (callbacksIt2 == _callbacks.end()) continue;

                    std::vector<EventHandle*>& callbacks2 = callbacksIt2->second;
                    callbacks2[ind]->setRemoved(true);
                    callbacks2[ind]->_handler = nullptr;
                    callbacks2[ind]->_callback = HandleEventCallback();
                    callbacks2.erase(callbacks2.begin() + ind);

                    if (callbacks2.empty()) {
                        _callbacks.erase(name);
                    }
                }
            }
        }

        if (!callbacks) {
            _callbackActive.erase(name);
            compactRemovedHandles();
        }

        return this;
    }
}
