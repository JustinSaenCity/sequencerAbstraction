#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "LevelSequence.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "MediaSource.h"
#include "SequencerTypes.h"
#include "ControlRigSequencerEditorLibrary.h"

#include "SequencerAbstractionBPLibrary.generated.h"

class UControlRig;
class UMovieSceneControlRigParameterTrack;
class UMovieSceneScriptingChannel;
class UMovieSceneScriptingFloatChannel;
class AActor;

USTRUCT(BlueprintType)
struct FSectionLabelEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SequenceAbstraction|SecctionAbstraction")
	TObjectPtr<UMovieSceneSection> Section = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SequenceAbstraction|SectionAbstraction")
	FString Label;
};

USTRUCT(BlueprintType)
struct SEQUENCERABSTRACTION_API FSequencerTimeChangeState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "SequencerAbstraction|Sequencer")
    TObjectPtr<ULevelSequence> LastSequence = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "SequencerAbstraction|Sequencer")
    int32 LastFrameNumber = 0;

    UPROPERTY(BlueprintReadOnly, Category = "SequencerAbstraction|Sequencer")
    float LastSubFrame = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "SequencerAbstraction|Sequencer")
    bool bHasLastSequencerTime = false;
};


UCLASS()
class SEQUENCERABSTRACTION_API UBakeAbstraction : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static bool BakeBindingToControlRig(
        const FGuid& BindingGuid,
        TSubclassOf<UControlRig> ControlRigClass,
        bool bReduceKeys,
        float Tolerance,
        bool bResetControls
    );

private:

};


UCLASS()
class SEQUENCERABSTRACTION_API USectionAbstraction : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:

	/* Matches a skeletal animation section to a specific bone in a skeletal mesh component */
	UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|SectionAbstraction")
    static void MatchSectionByBone(
        UMovieSceneSkeletalAnimationSection* CurrentSection,
        USkeletalMeshComponent* SkelMeshComp,
        FFrameTime CurrentFrame,
        FFrameRate FrameRate,
        FName BoneName
    );
    
    /* Pairs sections with corresponding labels into a list of structs */
	UFUNCTION(BlueprintCallable, Category = "SequenceAbstraction|SectionAbstraction")
	static FSectionLabelEntry CreateSectionLabelEntry(
		UMovieSceneSection* Section,
		const FString& Label
	);

    /* Sets the animation asset for a skeletal animation section */
	UFUNCTION(BlueprintCallable, Category = "SequenceAbstraction|SectionAbstraction")
	static void SetAnimationAsset(
		UMovieSceneSkeletalAnimationSection* Section,
		UAnimSequenceBase* Animation
	);
    
    /* Splits an animation section at a specified frame */
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|SectionAbstraction")
    static void SplitAnimationSection(
        UMovieSceneSection* Section,
        int32 SplitFrame,
        FFrameRate FrameRate,
        UMovieSceneSkeletalAnimationSection*& OutRightSection,
        UMovieSceneSkeletalAnimationSection*& OutLeftSection,
        bool bDeleteKeys = false
    );

    /* Functions moved from SequencerAbstractionBPLibrary class */

	static bool RemoveAnimationSection(
		ULevelSequence* Sequence,
		UMovieSceneSkeletalAnimationSection* Section,
		FSequenceOpenResult& Result
	);


private:
 

};

UCLASS()
class SEQUENCERABSTRACTION_API USequencerAbstractionBPLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // Asset lifecycle
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Assets")
    static ULevelSequence* CreateLevelSequenceAsset(const FString& PackagePath, const FString& AssetName, FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Assets")
    static bool SaveAsset(UObject* Asset);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Assets")
    static ULevelSequence* LoadLevelSequenceAsset(const FString& AssetPath);

    UFUNCTION(BlueprintCallable, Category="SequencerAbstraction|Assets")
    static ULevelSequence* duplicateSequencerToFolder(
        const FString& sourceSequencePath,
        const FString& destinationFolder,
        const FString& newSequenceName
    );

    // Sequencer state
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static TArray<FSequenceBindingInfo> GetBindingsInSequence(ULevelSequence* Sequence);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static ULevelSequence* GetCurrentOpenedLevelSequence();

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static FControlRigSequencerBindingProxy GetRigBindingProxyBasedOnClassFromOpenSequence(TSubclassOf<UControlRig> InClass);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static bool GetCurrentFloatValueFromRigBindingProxy(
        FControlRigSequencerBindingProxy RigBinding,
        FName ControlName,
        float& OutValue,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static TArray<FSequenceTrackInfo> GetAllTracksInCurrentSequence();

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static TArray<UAnimSequence*> GetAllAnimSequencesInCurrentSequence();

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Animation")
    static TArray<FActiveSkeletalAnimationInfo> GetActiveSkeletalAnimationsAtCurrentTime(FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Animation")
    static bool SampleSourceAnimationFromActiveInfo(
        const FActiveSkeletalAnimationInfo& ActiveAnimation,
        FSourceAnimationFrameData& OutFrameData,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Animation")
    static bool SampleSourceAnimationAtTime(
        UAnimSequenceBase* Animation,
        float AnimationTimeSeconds,
        FSourceAnimationFrameData& OutFrameData,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static bool OpenLevelSequenceInSequencer(ULevelSequence* Sequence);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static FGuid AddSkeletalMeshToOpenSequenceFromPath(
        UObject* WorldContextObject,
        const FString& SkeletalMeshAssetPath,
        const FName SpawnedActorLabel,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static FGuid FindPossessableBinding(
        ULevelSequence* Sequence,
        AActor* Actor,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static FGuid FindOrCreatePossessableBinding(
        ULevelSequence* Sequence,
        AActor* Actor,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static UMovieSceneTrack* GetTrackFromGuid(
        ULevelSequence* Sequence,
        FGuid BindingGuid,
        TSubclassOf<UMovieSceneTrack> TrackClass,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static UMovieSceneControlRigParameterTrack* GetControlRigTrackFromGuid(
        ULevelSequence* Sequence,
        FGuid BindingGuid,
        TSubclassOf<UControlRig> ControlRigClass,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer")
    static int32 GetCurrentFrame(FString& ErrorMessage);

    UFUNCTION(BlueprintPure, Category = "SequencerAbstraction|Sequencer")
    static bool currentlyScrubbing();

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer", meta = (DisplayName = "sequencerTimeChanged"))
    static bool sequencerTimeChanged();

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sequencer", meta = (DisplayName = "sequencerTimeChanged (State)"))
    static bool sequencerTimeChangedForState(UPARAM(ref) FSequencerTimeChangeState& State);

    UFUNCTION(BlueprintCallable, Category="SequencerAbstraction|Sequencer")
    static bool FocusLevelSequenceEditor(ULevelSequence* Sequence);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Editor")
    static bool FocusLevelViewport();

    // Content loading
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Content")
    static USkeletalMesh* LoadSkeletalMesh(const FString& AssetPath);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Content")
    static UAnimSequence* LoadAnimSequence(const FString& AssetPath);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Content")
    static TSubclassOf<UControlRig> LoadControlRigClass(const FString& AssetPath);

    // Track helpers
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Tracks")
    static bool RemoveTrackInSequenceByTrackPath(
        ULevelSequence* Sequence,
        const FString& TrackPath,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Tracks")
    static int32 RemoveTracksInSequenceByBindingGuid(
        ULevelSequence* Sequence,
        const FGuid& BindingGuid,
        const FString& OptionalTrackClassName,
        FSequenceOpenResult& Result);

    // Sections
    //UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Track")
    //static void SetTrackDisplayName(UMovieSceneSection* section, const FString& newName);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|SectionAbstraction")
    static bool RemoveAnimSection(
        ULevelSequence* Sequence,
        UMovieSceneSkeletalAnimationSection* Section,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sections")
    static int32 RemoveAnimSectionsByAnimSequence(
        ULevelSequence* Sequence,
        UAnimSequenceBase* Animation,
        UPARAM(meta = (ClampMin = "0")) int32 MaxToRemove,
        FSequenceOpenResult& Result,
        const FGuid& BindingGuid);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sections")
    static bool MoveAnimationSectionStartTo(
        ULevelSequence* Sequence,
        UMovieSceneSection* Section,
        int32 NewStartFrame,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sections")
    static bool MoveAnimationSectionEndTo(
        ULevelSequence* Sequence,
        UMovieSceneSection* Section,
        int32 NewEndFrameInclusive,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Sections")
    static bool SnapSectionToSourceTimecode(
        ULevelSequence* Sequence,
        UMovieSceneSection* Section,
        bool bFocusSection,
        FSequenceOpenResult& Result);

    // Timing helpers
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Timing")
    static void moveSequencerPlayheadToFrame(int32 frame);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Timing")
    static bool SetSequenceFrameRateFromAnimation(
        ULevelSequence* Sequence,
        UAnimSequence* Animation,
        bool bAlsoSetPlaybackRangeToAnim,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Timing")
    static bool SetSequencePlaybackRange(
        ULevelSequence* Sequence,
        int32 StartFrame,
        int32 EndFrame);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Timing")
    static bool GetSequencePlaybackRange(
        ULevelSequence* Sequence,
        int32& OutStartFrame,
        int32& OutEndFrame);

    // Rename the older duplicates so UHT doesn't conflict
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Timing", meta = (DisplayName = "Move Animation Section Start To (Legacy)"))
    static bool MoveAnimationSectionStartTo_Legacy(
        UMovieSceneSkeletalAnimationSection* Section,
        int32 NewStartFrame);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Timing", meta = (DisplayName = "Move Animation Section End To (Legacy)"))
    static bool MoveAnimationSectionEndTo_Legacy(
        UMovieSceneSkeletalAnimationSection* Section,
        int32 NewEndFrame);

    // Binding / adding animation
    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Animation",
        meta = (CPP_Default_RowIndex = "-1", CPP_Default_bAllowOverlapSameRow = "false"))
    static UMovieSceneSkeletalAnimationSection* AddAnimSectionToBinding(
        ULevelSequence* Sequence,
        const FGuid& BindingGuid,
        UAnimSequence* Animation,
        int32 StartFrame,
        int32 RowIndex,
        bool bAllowOverlapSameRow,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Media")
    static int32 RemoveMediaTracksFromBinding(
        ULevelSequence* Sequence,
        const FGuid& BindingGuid,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Media")
    static UMovieSceneSection* AddMediaSourceProxySectionToBinding(
        ULevelSequence* Sequence,
        const FGuid& BindingGuid,
        UMediaSource* MediaSource,
        int32 StartFrame,
        int32 MediaSourceProxyIndex,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static bool AddRigToBinding(
        ULevelSequence* Sequence,
        UObject* WorldContextObject,
        const FGuid& BindingGuid,
        TSubclassOf<UControlRig> ControlRigClass,
        bool bLayered,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static TArray<UMovieSceneScriptingChannel*> GetAllChannelsFromRigBindingSequenceTrack(UMovieSceneTrack* Track);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Channels")
    static bool EvaluateFloatChannelAtTime(
        UMovieSceneScriptingFloatChannel* Channel,
        FFrameTime Time,
        float& OutValue);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Get Keys In Selection Range For Names", CPP_Default_bMatchContains = "true"))
    static TArray<FSequencerKeyInSelectionRangeInfo> GetKeysInSelectionRangeForNames(
        UMovieSceneTrack* Track,
        const TArray<FName>& Names,
        bool bMatchContains,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Select Keys In Selection Range For Names", CPP_Default_bMatchContains = "true", CPP_Default_bClearExistingSelection = "true", CPP_Default_bThrobSelection = "true"))
    static int32 SelectKeysInSelectionRangeForNames(
        UMovieSceneTrack* Track,
        const TArray<FName>& Names,
        bool bMatchContains,
        bool bClearExistingSelection,
        bool bThrobSelection,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Select Keys At Frame For Names", CPP_Default_bMatchContains = "true", CPP_Default_bClearExistingSelection = "true", CPP_Default_bThrobSelection = "true"))
    static int32 SelectKeysAtFrameForNames(
        UMovieSceneTrack* Track,
        int32 DisplayFrame,
        const TArray<FName>& Names,
        bool bMatchContains,
        bool bClearExistingSelection,
        bool bThrobSelection,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Select Bone Keys In Selection Range", CPP_Default_bClearExistingSelection = "true", CPP_Default_bThrobSelection = "true"))
    static int32 SelectBoneKeysInSelectionRange(
        UMovieSceneTrack* Track,
        const TArray<FName>& BoneNames,
        bool bClearExistingSelection,
        bool bThrobSelection,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Select Bone Keys At Frame", CPP_Default_bClearExistingSelection = "true", CPP_Default_bThrobSelection = "true"))
    static int32 SelectBoneKeysAtFrame(
        UMovieSceneTrack* Track,
        int32 DisplayFrame,
        const TArray<FName>& BoneNames,
        bool bClearExistingSelection,
        bool bThrobSelection,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Remove Keys From Selection In Selection Range For Names", CPP_Default_bMatchContains = "true"))
    static int32 RemoveKeysFromSelectionInSelectionRangeForNames(
        UMovieSceneTrack* Track,
        const TArray<FName>& Names,
        bool bMatchContains,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Get Selected Names In Track", CPP_Default_bMatchContains = "true"))
    static TArray<FName> GetSelectedNamesInTrack(
        UMovieSceneTrack* Track,
        const TArray<FName>& Names,
        bool bMatchContains,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Focus Sequencer Rows For Names", CPP_Default_bMatchContains = "true", CPP_Default_bSelectParentInstead = "true", CPP_Default_bClearExistingRowSelection = "true"))
    static int32 FocusSequencerRowsForNames(
        UMovieSceneTrack* Track,
        const TArray<FName>& Names,
        bool bMatchContains,
        bool bSelectParentInstead,
        bool bClearExistingRowSelection,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig",
        meta = (DisplayName = "Focus Sequencer Sections For Names", CPP_Default_bMatchContains = "true", CPP_Default_bClearExistingSelection = "true", CPP_Default_bThrobSelection = "true", CPP_Default_bFrameMatchedSections = "true", CPP_Default_ViewPaddingSeconds = "0.25"))
    static int32 FocusSequencerSectionsForNames(
        UMovieSceneTrack* Track,
        const TArray<FName>& Names,
        bool bMatchContains,
        bool bClearExistingSelection,
        bool bThrobSelection,
        bool bFrameMatchedSections,
        double ViewPaddingSeconds,
        FString& ErrorMessage);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|Bake")
    static bool BakeBindingToAnimSequence(
        ULevelSequence* Sequence,
        UObject* WorldContextObject,
        const FGuid& BindingGuid,
        const FString& TargetPackagePath,
        const FString& NewAssetName,
        FSequenceOpenResult& Result);

    UFUNCTION(BlueprintCallable, Category = "SequencerAbstraction|ControlRig")
    static bool RemoveAllKeysForControlExceptFrame(
        UMovieSceneControlRigParameterTrack* track,
        FName controlName,
        int32 keepFrame,
        FString& errorMessage);

};
