# SKSE "Full Bodied Animations"

Plugin to allow advanced functions for Full Bodied Animations


Repository so far contains an animation event listener and an event interpreter. For testing, we are trying to get the player character's head to shrink when called for by the animation.

Skyrim animations are weird. In short, we are manipulating a vanilla animation file (hkx format) by adding text lines. For example, 

0.000000 NPCKillMoveStart
this says at 0 seconds into the animation, start the kill move.

The goal here is to be able to use an annotation like this:
3.000000 FB_HeadShrink

To shrink the character's head at 3 seconds into the animation, by using the custom event FB_HeadShrink as a trigger.

Once established, this pipeline will allow us to begin working on all of the various mesh/expression morphs and transforms that we would like to bake into our animation files. 

FB_HeadShrink is from our new plugin, FB is short for Full Bodied and it is just for testing purposes. You will see all about it in AnimationEvents.cpp.

This is also my first time using GitHub! Message me here or on Discord if anything isn't working or if you have any feedback. I am planning on eventually opening this to public. 
