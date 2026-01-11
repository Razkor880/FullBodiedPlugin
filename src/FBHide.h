#pragma once

#include "RE/Skyrim.h"

namespace FB::Hide
{
	// Hide/unhide every renderable geometry under the actor's 3D root.
	// On first touch per-actor, we cache the baseline hidden flag and restore it when un-hiding.
	void ApplyHide(RE::ActorHandle a_actor, bool a_hide, bool logOps);

	// Best-effort slot-based hide using BSDismember partitions.
	// If no eligible dismember skin instances are found, this is a no-op (and can log once per slot).
	void ApplyHideSlot(RE::ActorHandle a_actor, std::uint16_t a_slotNumber, bool a_hide, bool logOps);

	// Clears cached baseline/touched state for this actor; attempts to restore baseline if 3D is present.
	void ResetActor(RE::ActorHandle a_actor, bool logOps);

#ifndef NDEBUG
	// Debug only: clears all cached state.
	void ResetAll(bool logOps);
#endif
}
