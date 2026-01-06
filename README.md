# SKSE "Full Bodied Animations"

Plugin to allow advanced functions for Full Bodied Animations

The intent is that a paired interaction will be triggered with an NPC, in this case using the custom animation file paired_huga.hkx.
Various transform functions will occur over the timeline of the animation. These are driven by an .ini file included in the mod. 

This system does not interact with the usual annotations system for modded skyrim animations as I had trouble getting that to work. However, the .ini file is templated to be easily adapted to annotations. 

A couple notes about the annotation system that we are avoiding: in short, it is manipulating a vanilla animation file (hkx format) by adding text lines. For example, 

0.000000 NPCKillMoveStart
this says at 0 seconds into the animation, start the kill move.

The goal here is to be able to use a text string like this:
3.000000 FBHeadScale(0.5)

To scale the caster's head to 50% size at 3 seconds into the animation, by using the custom event FB_HeadScale as a trigger.

The trigger is included in 0_master.hkx, in the mod. This is honestly pretty likely to break your game

The caster will use functions like FB_HeadSize, the target will use functions prefixed with "2_" for example "2_FBHeadScale(0.25)"

This pipeline will allow us to begin working on all of the various mesh/expression morphs and transforms that we would like to associate with our animation files. 

There will be a new .cpp for each function. Currently there is one started for body scaling, which so far only includes heads, but will eventually include all body nodes. 

There will be another for bodyslide manipulations, one for sfx, etc. 
