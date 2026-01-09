README


Full Bodied Animation (FBA)

Full Bodied Animation is a modern Skyrim AE animation-driven systems mod built around runtime animation context, configuration-driven timelines, and an SKSE plugin implemented using CommonLibSSE-NG.

The core goal is to allow animation context (paired or unpaired) to drive precise, reversible runtime effects such as node scaling, visibility control, and morph application — without fragile hacks or monolithic systems.

WHAT THIS MOD IS

A framework for animation-driven visual and structural effects

An SKSE plugin that reacts to animation context and events

A system that currently uses INI-driven timelines

Designed to support annotation-driven input long-term using the same internal pipeline

Suitable for paired animations, but not limited to them

WHAT THIS MOD IS NOT

Not a Papyrus-first system

Not a behavior injector or replacer

Not a hard-coded animation solution

Not a monolithic script bundle

HIGH-LEVEL PIPELINE

Animation context becomes visible to the game (event, paired state, actor roles)

The SKSE plugin receives or resolves the event

Configuration data (INI today, annotations later) is queried

Effects are scheduled and applied to the correct actor and nodes

Effects are reverted or reset when the animation context ends

REPOSITORY STRUCTURE (KEY FILES)

C++ (SKSE / CommonLibSSE-NG)

src/FullBodiedPlugin.cpp

src/AnimEventListener.h / .cpp

src/AnimationEvents.h / .cpp

src/ActorManager.h / .cpp

src/FBScaler.h / .cpp

src/FBMorph.h / .cpp

src/FBConfig.h / .cpp

Papyrus

FBMorphBridge.psc

Configuration

FullBodiedIni.ini

Documentation

docs/architecture

docs/design_notes

docs/YourAssignment

HOW TO USE THIS REPOSITORY (HUMANS AND GPTs)

Humans:

Start with this README

Then read docs/architecture

GPTs or automated assistants:

Treat docs/architecture as canonical once systems are marked stable

Treat docs/design_notes as provisional and exploratory

Treat docs/YourAssignment as explicit, temporary instructions that override defaults

Do not invent new systems without aligning to existing module responsibilities

DESIGN PHILOSOPHY

Prefer correct structure over quick wins

Prefer SKSE and behavior correctness over script-heavy solutions

Prefer explicit contracts between systems

Systems are provisional until they behave consistently, then become canon

PROJECT STATUS

This project is actively evolving. Refactors are expected. Experiments are encouraged. Stability is earned, not assumed.
