#pragma once

#include "RE/Skyrim.h"

void RegisterAnimationEventSink(RE::Actor* actor);  //  Required declaration
void RegisterPlayerAnimationEventSink();            // Optional wrapper for player
void ShrinkHead(RE::Actor* actor);                  // Already exists
