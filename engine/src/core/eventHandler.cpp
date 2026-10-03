// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025
//
#include "eventHandler.h"

#include <cassert>

namespace visutwin::canvas
{
    EventCallbackIdentity EventHandler::identityOf(const HandleEventCallback& callback)
    {
        if (const auto* fn = callback.target<void (*)(const EventArgs&)>()) {
            return reinterpret_cast<EventCallbackIdentity>(*fn);
        }
        return nullptr;
    }

    void EventHandle::setRemoved(const bool value)
    {
        if (value) {
            _removed = true;
        }
    }

    void EventHandle::callback(const EventArgs& args) const
    {
        if (_callback) {
            _callback(args);
        }
    }

    void EventHandle::off()
    {
        if (_handler && !_removed) {
            _handler->offByHandle(this);
        }
    }

    EventHandle::EventHandle(EventHandler* handler, const std::string& name, HandleEventCallback callback, void* scope,
            bool once, EventCallbackIdentity identity): _handler(handler), _name(name), _callback(std::move(callback)),
            _scope(scope), _once(once), _removed(false), _identity(identity) {}

    EventHandler::~EventHandler()
    {
        for (auto& handle : _eventHandles) {
            if (handle) {
                handle->setRemoved(true);
                handle->_handler = nullptr;
                handle->_callback = HandleEventCallback();
            }
        }
    }

    EventHandlePtr EventHandler::on(const std::string& name, HandleEventCallback callback, void* scope)
    {
        const EventCallbackIdentity identity = identityOf(callback);
        return addCallback(name, std::move(callback), scope, false, identity);
    }

    EventHandlePtr EventHandler::once(const std::string& name, HandleEventCallback callback, void* scope)
    {
        const EventCallbackIdentity identity = identityOf(callback);
        return addCallback(name, std::move(callback), scope, true, identity);
    }

    EventHandlePtr EventHandler::addCallback(const std::string& name, HandleEventCallback callback, void* scope, bool once,
        EventCallbackIdentity identity)
    {
        if (_callbacks.find(name) == _callbacks.end()) {
            _callbacks[name] = std::vector<EventHandle*>();
        }

        // if we are adding a callback to the list that is executing right now,
        // ensure we preserve an initial list before modifications
        auto activeIt = _callbackActive.find(name);
        if (activeIt != _callbackActive.end()) {
            auto callbackIt = _callbacks.find(name);
            if (callbackIt != _callbacks.end() && activeIt->second == callbackIt->second) {
                _callbackActive[name] = std::vector<EventHandle*>(activeIt->second);
            }
        }

        auto evt = std::make_shared<EventHandle>(this, name, std::move(callback), scope, once, identity);
        _eventHandles.push_back(evt);

        _callbacks[name].push_back(evt.get());
        return evt;
    }

    EventHandler* EventHandler::off(const std::string& name, const HandleEventCallback& callback, void* scope)
    {
        const EventCallbackIdentity identity = identityOf(callback);
        // A non-empty std::function that is not a plain handler pointer names no function,
        // so it can find nothing; that is a caller's mistake, not a request to remove all.
        assert((!callback || identity) && "off(name, callback): this std::function cannot be compared; "
            "keep the EventHandlePtr that on() returned, or remove by scope");
        return removeCallbacks(name, identity, !callback, scope);
    }

    EventHandler* EventHandler::removeCallbacks(const std::string& name, const EventCallbackIdentity identity,
        const bool matchAll, void* scope)
    {
        auto preserveActiveList = [this](const std::string& eventName) {
            auto activeIt = _callbackActive.find(eventName);
            if (activeIt == _callbackActive.end()) {
                return;
            }

            auto callbackIt = _callbacks.find(eventName);
            if (callbackIt != _callbacks.end() && activeIt->second == callbackIt->second) {
                _callbackActive[eventName] = std::vector<EventHandle*>(activeIt->second);
            }
        };

        auto removePredicate = [&](EventHandle* evt) {
            if (!evt) {
                return false;
            }

            if (scope && evt->_scope != scope) {
                return false;
            }

            if (matchAll) {
                return true;
            }

            return identity && evt->_identity == identity;
        };

        if (name.empty()) {
            for (const auto& [eventName, callbacks] : _callbacks) {
                (void)callbacks;
                preserveActiveList(eventName);
            }

            for (auto& [_, callbacks] : _callbacks) {
                callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                    [&](EventHandle* evt) {
                        if (removePredicate(evt)) {
                            evt->setRemoved(true);
                            evt->_handler = nullptr;
                            evt->_callback = HandleEventCallback();
                            return true;
                        }
                        return false;
                    }), callbacks.end());
            }

            for (auto it = _callbacks.begin(); it != _callbacks.end();) {
                if (it->second.empty()) {
                    it = _callbacks.erase(it);
                } else {
                    ++it;
                }
            }

            compactRemovedHandles();

            return this;
        }

        preserveActiveList(name);
        auto callbackIt = _callbacks.find(name);
        if (callbackIt == _callbacks.end()) {
            return this;
        }

        std::vector<EventHandle*>& callbacks = callbackIt->second;
        callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
            [&](EventHandle* evt) {
                if (removePredicate(evt)) {
                    evt->setRemoved(true);
                    evt->_handler = nullptr;
                    evt->_callback = HandleEventCallback();
                    return true;
                }
                return false;
            }), callbacks.end());

        if (callbacks.empty()) {
            _callbacks.erase(name);
        }

        compactRemovedHandles();

        return this;
    }

    EventHandler* EventHandler::offByHandle(EventHandle* handle)
    {
        if (!handle || handle->_removed) {
            return this;
        }

        const std::string& name = handle->_name;
        handle->setRemoved(true);
        handle->_handler = nullptr;
        handle->_callback = HandleEventCallback();

        auto activeIt = _callbackActive.find(name);
        if (activeIt != _callbackActive.end()) {
            auto callbackIt = _callbacks.find(name);
            if (callbackIt != _callbacks.end() && activeIt->second == callbackIt->second) {
                _callbackActive[name] = std::vector<EventHandle*>(activeIt->second);
            }
        }

        auto callbacksIt = _callbacks.find(name);
        if (callbacksIt == _callbacks.end()) {
            return this;
        }

        std::vector<EventHandle*>& callbacks = callbacksIt->second;
        callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), handle), callbacks.end());

        if (callbacks.empty()) {
            _callbacks.erase(name);
        }

        compactRemovedHandles();

        return this;
    }

    bool EventHandler::hasEvent(const std::string& name) const
    {
        auto it = _callbacks.find(name);
        return it != _callbacks.end() && !it->second.empty();
    }

    void EventHandler::initEventHandler()
    {
        for (auto& [_, handles] : _callbacks) {
            for (auto* handle : handles) {
                if (handle) {
                    handle->setRemoved(true);
                    handle->_handler = nullptr;
                    handle->_callback = HandleEventCallback();
                }
            }
        }
        _callbacks.clear();
        _callbackActive.clear();
        compactRemovedHandles();
    }

    void EventHandler::compactRemovedHandles()
    {
        // Callback lists (including clone-path snapshots that never appear in
        // _callbackActive) can hold removed handles while any fire() is on the
        // stack — releasing them now would dangle the pointers those loops read.
        if (_dispatchDepth > 0 || !_callbackActive.empty()) {
            return;
        }

        _eventHandles.erase(std::remove_if(_eventHandles.begin(), _eventHandles.end(),
            [](const std::shared_ptr<EventHandle>& handle) {
                return !handle || handle->removed();
            }), _eventHandles.end());
    }
}
