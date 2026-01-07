class AnimEventListener : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
public:
    static AnimEventListener* GetSingleton();

    void RegisterToPlayer();  // <-- must exist exactly like this

    RE::BSEventNotifyControl ProcessEvent(
        const RE::BSAnimationGraphEvent* a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource) override;
};