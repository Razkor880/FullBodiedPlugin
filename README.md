# Full Bodied Animation (FBA)

Full Bodied Animation is a modern Skyrim AE animation-driven systems mod built around **annotations, behavior graphs, and an SKSE plugin (CommonLibSSE-NG)**. The project’s goal is to allow HKX animation data to drive precise, runtime visual and structural effects in a clean, future-proof way.

This project explicitly prioritizes:

* Minimal engine and data edits
* Clear separation of responsibilities
* Long-term maintainability
* Architecture-first design

Devourment and similar legacy mods are used **only as reference material**. Their approaches are not replicated.

---

## What This Mod Is

* A framework for **animation-driven effects** (scaling, visibility, morphs, state changes)
* A pipeline from **HKX annotations → behavior graph events → SKSE plugin logic**
* A paired-animation–capable system, but not limited to paired use cases

## What This Mod Is Not

* Not a recreation or continuation of Devourment
* Not a Papyrus-first system
* Not a behavior injector replacement
* Not a monolithic or hard-coded animation solution

---

## High-Level Pipeline

1. **HKX Annotations**

   * Animators annotate clips with semantic, numeric data
2. **Behavior Graph Routing**

   * Annotations are surfaced as real animation events
3. **SKSE Plugin (CommonLibSSE-NG)**

   * Events are received and interpreted
4. **Runtime Effects**

   * NiNode scaling, visibility control, morph application, etc.

---

## Repository Structure (Relevant)

* `src/` – C++ plugin source
* `docs/architecture.md` – Canonical system design
* `docs/design_notes.md` – Experiments, rejected ideas, future planning
* `FBMorphBridge.psc` – Minimal Papyrus bridge (intentionally thin)
* `FullBodiedIni.ini` – User/config-facing tuning only

---

## How to Use This Repository (Humans & GPTs)

If you are a human:

* Start with this README
* Then read `docs/architecture.md`

If you are a GPT or automated assistant:

* Treat `docs/architecture.md` as **canon once marked stable**
* Treat `docs/design_notes.md` as provisional and exploratory
* Do not invent new systems without aligning to existing module contracts

---

## Design Philosophy

* Prefer **correct structure** over quick wins
* Prefer **SKSE + behavior correctness** over script hacks
* Prefer **explicit contracts** between systems
* Systems are **provisional until they work consistently**, then become canon

---

## Status

This project is actively evolving. Expect refactors. Expect ideas to be tested and discarded. This is intentional.
