# SKSE "Full Bodied Animations"

Plugin to allow advanced functions for Full Bodied Animations

The intent is that a paired interaction will be triggered with an NPC, in this case using the custom animation file paired_huga.hkx.
Various transform functions will occur over the timeline of the animation. These are driven by an .ini file included in the mod. 

This system does not interact with the usual annotations system for modded skyrim animations as I had trouble getting that to work. However, the .ini file is templated to be easily adapted to annotations. 

A couple notes about the annotation system that we are avoiding: in short, it is manipulating a vanilla animation file (hkx format) by adding text lines. For example, 

0.000000 NPCKillMoveStart
this says at 0 seconds into the animation, start the kill move.

The goal here is to be able to use a text string like this:
3.000000 FB_HeadSizeS050

To shrink the caster's head to 50% size at 3 seconds into the animation, by using the custom event FB_HeadSize as a trigger.

The trigger is included in 0_master.hkx, in the mod. This is honestly pretty likely to break your game

The caster will use functions like FB_HeadSize, the target will use functions prefixed with "2_" for example "2_FB_HeadSize"

Once established, this pipeline will allow us to begin working on all of the various mesh/expression morphs and transforms that we would like to bake into our animation files. 

FB_HeadShrink is from our new plugin, FB is short for Full Bodied and it is just for testing purposes. You will see all about it in AnimationEvents.cpp.

This is also my first time using GitHub! Message me here or on Discord if anything isn't working or if you have any feedback. I am planning on eventually opening this to public. 
