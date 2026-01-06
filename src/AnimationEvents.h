#pragma once
#include "RE/Skyrim.h"

// Register the animation graph event sink to an actor.
void RegisterAnimationEventSink(RE::Actor* actor);

// Loads/refreshes FullBodiedIni.ini (preferred) / fallback.
void LoadFBConfig();

// Backward-compatible wrapper (older code calls this).
void LoadHeadScaleConfig();

// Applies head scale immediately (used by other features / testing).
void HeadScale(RE::Actor* actor, float scale);
