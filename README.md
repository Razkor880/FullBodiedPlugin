# Full Bodied Animation (FBA)

Full Bodied Animation is a modern Skyrim AE animation-driven systems mod built around **runtime animation context**, **configuration-driven timelines**, and an **SKSE plugin implemented using CommonLibSSE-NG**.

The core goal is to allow animation context (paired or unpaired) to drive precise, reversible runtime effects such as node scaling, visibility control, and morph application — without fragile hacks or monolithic systems.

---

## What This Mod Is

- A framework for animation-driven visual and structural effects
- An SKSE plugin that reacts to animation context and events
- A system that **currently uses INI-driven timelines**
- Designed to support **annotation-driven input long-term** using the same internal pipeline
- Suitable for paired animations, but not limited to them

---

## What This Mod Is Not

- Not a Papyrus-first system
- Not a behavior injector or replacer
- Not a hard-coded animation solution
- Not a monolithic script bundle

---

## High-Level Pipeline

1. Animation context becomes visible to the game (event, paired state, actor roles)
2. The SKSE plugin receives or resolves the event
3. Configuration data (INI today, annotations later) is queried
4. Effects are scheduled and applied to the correct actor and nodes
5. Effects are reverted or reset when the animation context ends

---

## Repository Structure (Key Files)

### C++ (SKSE / CommonLibSSE-NG)
- `src/FullBodiedPlugin.cpp`
- `src/AnimationEvents.h / .cpp`
- `src/ActorManager.h / .cpp`
- `src/FBScaler.h / .cpp`
- `src/FBMorph.h / .cpp`
- `src/FBHide.h / .cpp`
- `src/FBConfig.h / .cpp`
- `src/FBUpdatePump.h / .cpp`

### Papyrus
- `FBMorphBridge.psc`

### Configuration
- `FullBodiedIni.ini`

### Documentation
- `docs/architecture`
- `docs/design_notes`
- `docs/YourAssignment`

---

## How to Use This Repository (Humans & GPTs)

### Humans
- Start with this README
- Then read `docs/architecture`

### GPTs / Automated Assistants
- Treat `docs/architecture` as **canonical once systems are marked stable**
- Treat `docs/design_notes` as **provisional and exploratory**
- Treat `docs/YourAssignment` as **explicit, temporary instructions that override defaults**
- Do not invent new systems without aligning to existing module responsibilities

---

## Design Philosophy

- Prefer correct structure over quick wins
- Prefer SKSE and behavior correctness over script-heavy solutions
- Prefer explicit contracts between systems
- Systems are **provisional until they behave consistently**, then become canon

---

## Project Status

This project is actively evolving.  
Refactors are expected. Experiments are encouraged.  
Stability is earned, not assumed.
