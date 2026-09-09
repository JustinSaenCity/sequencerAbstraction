using UnrealBuildTool;
using System.IO;

public class SequencerAbstraction : ModuleRules
{
    public SequencerAbstraction(ReadOnlyTargetRules Target) : base(Target)
    {
        // Do NOT set: Type = ModuleType.Editor;  (not valid in UE 5.7 rules)

        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "MediaAssets",
            "ControlRig",
            "ControlRigEditor",
        });

        if (Target.bBuildEditor)
        {
            PrivateIncludePaths.AddRange(new[]
            {
                Path.Combine(EngineDirectory, "Plugins/MovieScene/SequencerScripting/Source/SequencerScripting/Private/KeysAndChannels"),
            });

            PrivateDependencyModuleNames.AddRange(new[]
            {
                "UnrealEd",
                "EditorSubsystem",
                "AssetTools",
                "AssetRegistry",
                "ContentBrowser",

                "LevelSequence",
                "MovieScene",
                "MovieSceneTracks",
                "MovieSceneTools",
                "AnimationCore",
                "MediaCompositing",
                "Sequencer",
                "SequencerCore",
                "EditorScriptingUtilities",
                "LevelSequenceEditor",
                "SequencerScripting",
                "SequencerScriptingEditor",

                "RigVM",

                "LevelEditor",
                "Slate",
                "SlateCore",
            });
        }
    }
}
