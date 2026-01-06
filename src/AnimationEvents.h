#pragma once
#include "RE/Skyrim.h"

// Register the animation graph event sink to an actor.
void RegisterAnimationEventSink(RE::Actor* actor);

// Loads/refreshes FullBodiedIni.ini (preferred) / fallback.
void LoadFBConfig();

// Backward-compatible wrapper (older call sites may still call this).
void LoadHeadScaleConfig();

// Immediate scale helper (debug/manual use)
void HeadScale(RE::Actor* actor, float scale);
