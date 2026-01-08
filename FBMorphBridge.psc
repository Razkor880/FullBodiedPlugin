Scriptname FBMorphBridge Hidden

; Bridge between your SKSE plugin and NiOverride.
; Uses the same key your plugin uses: "FullBodiedPlugin".

Function FBSetMorph(Actor akActor, String morphName, float value) Global
    if akActor == None
        return
    endif

    NiOverride.SetBodyMorph(akActor, morphName, "FullBodiedPlugin", value)
    NiOverride.UpdateModelWeight(akActor)
EndFunction

Function FBClearMorphs(Actor akActor) Global
    if akActor == None
        return
    endif

    NiOverride.ClearBodyMorphKeys(akActor, "FullBodiedPlugin")
    NiOverride.UpdateModelWeight(akActor)
EndFunction
