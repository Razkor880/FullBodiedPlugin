#pragma once

//#include "RE/Skyrim.h"

namespace FB::Hide
{
	// Broad hide/unhide for all render geometry on an actor.
	// - hide=true: capture baseline on first touch, then hide all shapes
	// - hide=false: best-effort restore baseline (does NOT clear state)
	void ApplyHide(RE::ActorHandle actor, bool hide, bool logOps);

	// Best-effort restore baseline (if 3D present), then clear all stored state for this actor.
	// If 3D missing, safely clears state and optionally logs that restore was skipped.
	void ResetActor(RE::ActorHandle actor, bool logOps);

#ifndef NDEBUG
	// Debug-only escape hatch: clears all stored state (does not attempt restore).
	void ResetAll(bool logOps);
#endif
}
