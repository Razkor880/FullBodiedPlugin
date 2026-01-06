#pragma once

#include "RE/Skyrim.h"

void RegisterAnimationEventSink(RE::Actor* actor);  //  Required declaration
void ShrinkHead(RE::Actor* actor, float scale);
void LoadHeadShrinkConfig();
