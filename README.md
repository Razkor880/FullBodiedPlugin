# SKSE "Full Bodied Animations"

Plugin to allow advanced functions for Full Bodied Animations

The intent is that a paired interaction will be triggered with an NPC, in this case using the custom animation file paired_huga.hkx.
Various transform functions will occur over the timeline of the animation. These are driven by an .ini file included in the mod. 

This system does not interact with the usual annotations system for modded skyrim animations as I had trouble getting that to work. However, the .ini file is templated to be easily adapted to annotations. 

A couple notes about the annotation system that we are matching but not using: in short, it is manipulating a vanilla animation file (hkx format) by adding text lines. For example, 

0.000000 NPCKillMoveStart
this says at 0 seconds into the animation, start the kill move.

The goal here is to be able to use a text string like this:
3.000000 FBHeadScale(0.5)

To scale the caster's head to 50% size at 3 seconds into the animation, by using the custom event FB_HeadScale as a trigger.

This concept will be replicated to several additional features. 





FULL BODIED PLUGIN
ARCHITECTURE SUMMARY (FOR GPT / AI ASSISTANTS)

This project is an SKSE plugin for Skyrim AE that applies time-based body transformations during paired animations using a data-driven timeline system.

The system is intentionally split into small, responsibility-focused modules.
Understanding and respecting these boundaries is critical when modifying or extending the plugin.

The animation event that triggers everything is always FBEvent.

FBEvent is a custom animation event authored directly in the paired animation clip:

Specifically added to Paired_HugA’s hkbClipGenerator trigger array

The event name is stored in the behavior string data, formatted the same as the standard annotation system for modded animations

No other animation events should be used to start timelines

HIGH-LEVEL FLOW

A paired animation plays (e.g. Paired_HugA)

The animation fires the custom event FBEvent

AnimationEvents receives the event

AnimationEvents looks up a timeline in the INI

The timeline is dispatched to ActorManager

ActorManager schedules commands over time

FBScaler performs node-level scale changes

On PairEnd / NPCPairedStop, everything is cancelled and reset

MODULE RESPONSIBILITIES

FullBodiedPlugin.cpp

• Plugin entry point
• Initializes logging
• Registers animation listeners
• No gameplay logic
• No animation logic
• No scaling logic

This file should remain minimal and stable.

AnimEventListener.cpp / .h

• Low-level animation graph sink registration
• Attaches the animation event sink to actor graphs
• Handles graph availability timing issues

This module does NOT:
• Parse events
• Interpret tags
• Start timelines
• Apply effects

It exists solely to ensure animation events reach AnimationEvents reliably.

AnimationEvents.cpp / .h

This is the orchestration layer.

Responsibilities:
• Receives animation graph events
• Filters for FBEvent
• Handles debounce logic (to ignore irrelevant spam events)
• Determines when a timeline should start
• Resolves caster vs target actors
• Dispatches work to ActorManager
• Listens for PairEnd / NPCPairedStop and triggers reset

Explicitly does NOT:
• Parse INI files
• Store configuration data
• Perform scaling
• Schedule threads
• Track per-actor runtime state

AnimationEvents should remain thin and event-focused.

FBConfig.cpp / .h

This module owns all configuration and INI parsing.

Responsibilities:
• Locate the INI file
• Parse [General], [Debug], and timeline sections
• Parse timeline command tokens
• Validate syntax and data
• Produce immutable configuration data structures
• Map EventToTimeline entries

Important design note:
• FBConfig does NOT know Skyrim node names
• NodeKey resolution is injected via a resolver callback
• This keeps FBConfig engine-agnostic

FBConfig should never:
• Touch actors
• Touch nodes
• Spawn threads
• Perform runtime logic

ActorManager.cpp / .h

This is the runtime execution engine.

Responsibilities:
• Own per-actor timeline execution
• Schedule commands using detached threads
• Track which nodes were modified
• Handle cancellation safely
• Perform reset on animation end
• Ensure no dangling references or race conditions

ActorManager does NOT:
• Parse configuration
• Interpret animation tags
• Resolve node names
• Modify nodes directly

ActorManager only coordinates execution.

FBScaler.cpp / .h

This module performs the actual visual effect.

Responsibilities:
• Resolve NiNodes on an actor
• Apply scale values
• Cache original scale values
• Restore nodes on reset

FBScaler is deliberately dumb:
• No timing
• No animation awareness
• No config knowledge
• No threading

It should only ever do “set scale X on node Y for actor Z”.

IMPORTANT INVARIANTS

• The start event is always FBEvent
• FBEvent is authored in Paired_HugA’s clip trigger array
• Timelines are driven entirely by INI data
• All runtime state lives in ActorManager
• All configuration lives in FBConfig
• AnimationEvents never mutates actors directly
• FBScaler never schedules or decides timing

INTENDED DESIGN GOAL

This architecture is designed so that:
• Animation logic
• Configuration logic
• Runtime scheduling
• Visual application

Any future AI assistant working on this project should preserve these boundaries.

The AI assistant (most likely you, if you are reading this) shall default to providing high level reports to the user before executing new solutions and ideas. You have been given a set of links to the raw data of each script. Let the user know if these are ever difficult to understand or are not accessible. 
