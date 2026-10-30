#pragma once

#include "EngineHost.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace stemcel::native
{

// The native UI's connection to the engine: the in-process counterpart of the Electron
// renderer's WebSocket bridge client.
//
// Commands are the same (type, payload) envelopes the renderer sends, dispatched straight
// into EngineHost on the message thread, so every command keeps the undo, transition and
// crash-firewall handling the WebSocket path has. Engine broadcasts arrive as juce::var
// through the bridge's local sink and are delivered to subscribers on the message thread.
class EngineClient
{
  public:
    using Handler = std::function<void(const juce::var& payload)>;
    using AnyHandler = std::function<void(const juce::String& type, const juce::var& payload)>;

    explicit EngineClient(EngineHost::Options options);
    ~EngineClient();

    EngineClient(const EngineClient&) = delete;
    EngineClient& operator=(const EngineClient&) = delete;
    EngineClient(EngineClient&&) = delete;
    EngineClient& operator=(EngineClient&&) = delete;

    // Opens the audio device and starts the playhead/meter emitter (EngineHost phase 2), then
    // delivers the initial state (PROJECT_STATE, EDIT_UNDO_STATE, ENGINE_AUDIO_STATUS).
    void start();

    // Message thread only.
    void send(const juce::String& type, const juce::var& payload = {});

    // Groups the commands sent inside `fn` into one undo step, as the renderer's
    // runInUndoGroup does with EDIT_GROUP_BEGIN/END.
    void inUndoGroup(const juce::String& label, const std::function<void()>& fn);

    // Subscriptions live until the returned token is passed to unsubscribe().
    int subscribe(const juce::String& type, Handler handler);
    int subscribeAll(AnyHandler handler);
    void unsubscribe(int token);

    // The most recent payload broadcast for `type` (e.g. PROJECT_STATE), or void.
    juce::var latest(const juce::String& type) const;

    EngineHost& host() { return *engineHost; }

    // A fresh id for a track, clip or library item: the UI owns ids, as the renderer does.
    static juce::String newId(const juce::String& prefix);

  private:
    void deliver(const juce::String& type, const juce::var& payload);

    std::unique_ptr<EngineHost> engineHost;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    int nextToken = 1;
    std::map<int, std::pair<juce::String, Handler>> handlers;
    std::map<int, AnyHandler> anyHandlers;
    std::map<juce::String, juce::var> lastPayloads;
    // Commands sent while a command is being dispatched (from a broadcast handler) wait
    // their turn, as they would over the WebSocket, instead of re-entering the engine
    // mid-command.
    bool dispatching = false;
    std::deque<std::pair<juce::String, juce::var>> deferred;
};

// Subscribes for the lifetime of the owning object.
class ScopedSubscription
{
  public:
    ScopedSubscription() = default;
    ScopedSubscription(EngineClient& client, int token) : owner(&client), id(token) {}
    ~ScopedSubscription() { reset(); }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;
    ScopedSubscription(ScopedSubscription&& other) noexcept : owner(other.owner), id(other.id)
    {
        other.owner = nullptr;
    }
    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            owner = other.owner;
            id = other.id;
            other.owner = nullptr;
        }
        return *this;
    }

    void reset()
    {
        if (owner != nullptr) owner->unsubscribe(id);
        owner = nullptr;
    }

  private:
    EngineClient* owner = nullptr;
    int id = 0;
};

// Collects subscriptions; destroying it unsubscribes them all.
class Subscriptions
{
  public:
    explicit Subscriptions(EngineClient& c) : client(c) {}

    void on(const juce::String& type, EngineClient::Handler handler)
    {
        list.emplace_back(client, client.subscribe(type, std::move(handler)));
    }

  private:
    EngineClient& client;
    std::vector<ScopedSubscription> list;
};

} // namespace stemcel::native
