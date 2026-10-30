#include "EngineClient.h"

#include "Log.h"

#include <utility>

namespace stemcel::native
{

EngineClient::EngineClient(EngineHost::Options options)
    : engineHost(std::make_unique<EngineHost>(std::move(options)))
{
    // Broadcasts can come from worker threads (stem progress, peak jobs); hop to the
    // message thread before touching any UI state.
    engineHost->bridge().setLocalSink(
        [this, flag = alive](const juce::String& type, const juce::var& payload)
        {
            if (juce::MessageManager::existsAndIsCurrentThread())
            {
                deliver(type, payload);
                return;
            }
            juce::MessageManager::callAsync(
                [this, flag, type, payload]
                {
                    if (flag->load()) deliver(type, payload);
                });
        });
}

EngineClient::~EngineClient()
{
    alive->store(false);
    engineHost->bridge().setLocalSink({});
}

void EngineClient::start()
{
    engineHost->startEngine();
    engineHost->sendInitialState([this](const juce::String& type, const juce::var& payload) { deliver(type, payload); });
}

void EngineClient::send(const juce::String& type, const juce::var& payload)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (dispatching)
    {
        deferred.emplace_back(type, payload);
        return;
    }
    dispatching = true;
    struct Reset
    {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{dispatching};
    engineHost->dispatch(type, payload);
    while (!deferred.empty())
    {
        auto [nextType, nextPayload] = std::move(deferred.front());
        deferred.pop_front();
        engineHost->dispatch(nextType, nextPayload);
    }
}

void EngineClient::inUndoGroup(const juce::String& label, const std::function<void()>& fn)
{
    auto* begin = new juce::DynamicObject();
    begin->setProperty("label", label);
    send("EDIT_GROUP_BEGIN", juce::var(begin));
    // END is always sent, even if the body throws, so a group can never stay open.
    struct EndGroup
    {
        EngineClient& client;
        ~EndGroup() { client.send("EDIT_GROUP_END"); }
    } endGroup{*this};
    fn();
}

int EngineClient::subscribe(const juce::String& type, Handler handler)
{
    const int token = nextToken++;
    handlers.emplace(token, std::make_pair(type, std::move(handler)));
    return token;
}

int EngineClient::subscribeAll(AnyHandler handler)
{
    const int token = nextToken++;
    anyHandlers.emplace(token, std::move(handler));
    return token;
}

void EngineClient::unsubscribe(int token)
{
    handlers.erase(token);
    anyHandlers.erase(token);
}

juce::var EngineClient::latest(const juce::String& type) const
{
    if (const auto it = lastPayloads.find(type); it != lastPayloads.end()) return it->second;
    return {};
}

juce::String EngineClient::newId(const juce::String& prefix)
{
    return prefix + "-" + juce::Uuid().toDashedString();
}

void EngineClient::deliver(const juce::String& type, const juce::var& payload)
{
    lastPayloads[type] = payload;

    // Copy first: a handler may subscribe or unsubscribe while being called.
    std::vector<Handler> matching;
    for (const auto& [token, entry] : handlers)
        if (entry.first == type) matching.push_back(entry.second);
    std::vector<AnyHandler> all;
    all.reserve(anyHandlers.size());
    for (const auto& [token, handler] : anyHandlers) all.push_back(handler);

    for (const auto& handler : matching) handler(payload);
    for (const auto& handler : all) handler(type, payload);
}

} // namespace stemcel::native
