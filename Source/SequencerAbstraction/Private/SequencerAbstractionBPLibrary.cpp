#include "SequencerAbstractionBPLibrary.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "ISequencer.h"
#include "ILevelSequenceEditorToolkit.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MVVM/SectionModelStorageExtension.h"
#include "MVVM/Selection/Selection.h"
#include "MVVM/ViewModels/ChannelModel.h"
#include "MVVM/ViewModels/SectionModel.h"
#include "MVVM/ViewModels/SequencerEditorViewModel.h"
#include "LevelSequence.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Logging/LogMacros.h"

#include "MovieScene.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneTimeUnit.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "MovieSceneBindingProxy.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "MovieSceneObjectBindingID.h" // UE::MovieScene::FRelativeObjectBindingID, FMovieSceneObjectBindingID
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "LevelSequenceEditorSubsystem.h"
#include "ExtensionLibraries/MovieSceneSectionExtensions.h"
#include "MovieSceneScriptingChannel.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "MediaSource.h"
#include "MovieSceneMediaSection.h"
#include "MovieSceneMediaTrack.h"

#include "LevelEditor.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"
#include "Modules/ModuleManager.h"
#include "IAssetViewport.h"

#include "Exporters/AnimSeqExportOption.h"
#include "Animation/AnimationSettings.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimCurveTypes.h"
#include "Factories/AnimSequenceFactory.h"

#include "Animation/SkeletalMeshActor.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "SequencerTools.h"
#include "Units/Execution/RigUnit_InverseExecution.h"
#include "Modules/ModuleManager.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/UnrealType.h"

static FString MakeMasterTrackKey(UMovieSceneTrack* Track, int32 Index)
{
    return FString::Printf(TEXT("MASTER::%s::%d"), *Track->GetClass()->GetName(), Index);
}

static FString MakeBindingTrackKey(const FGuid& Guid, UMovieSceneTrack* Track, int32 Index)
{
    return FString::Printf(TEXT("BIND::%s::%s::%d"), *Guid.ToString(), *Track->GetClass()->GetName(), Index);
}

namespace
{
    TWeakObjectPtr<ULevelSequence> LastSequencerTimeSequence;
    FFrameTime LastSequencerTime;
    bool bHasLastSequencerTime = false;

    static bool isSequencerTimeDifferent(
        ULevelSequence* LastSequence,
        const bool bHasLastTime,
        const int32 LastFrameNumber,
        const float LastSubFrame,
        ULevelSequence* CurrentSequence,
        const FFrameTime& CurrentTime)
    {
        return
            LastSequence != CurrentSequence ||
            !bHasLastTime ||
            LastFrameNumber != CurrentTime.FrameNumber.Value ||
            !FMath::IsNearlyEqual(LastSubFrame, CurrentTime.GetSubFrame());
    }
}

ULevelSequence* USequencerAbstractionBPLibrary::CreateLevelSequenceAsset(
    const FString& PackagePath,
    const FString& AssetName,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        Result.Error = TEXT("PackagePath or AssetName is empty.");
        return nullptr;
    }

    if (!PackagePath.StartsWith(TEXT("/")))
    {
        Result.Error = TEXT("PackagePath must start with '/Game/...'");
        return nullptr;
    }

    if (AssetName.Contains(TEXT("/")) || AssetName.Contains(TEXT(".")))
    {
        Result.Error = TEXT("AssetName must be a simple name (no '/' or extension).");
        return nullptr;
    }

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UObject* NewAsset = AssetTools.CreateAsset(
        AssetName,
        PackagePath,
        ULevelSequence::StaticClass(),
        nullptr
    );

    ULevelSequence* Seq = Cast<ULevelSequence>(NewAsset);
    if (!Seq)
    {
        Result.Error = TEXT("Failed to create LevelSequence asset.");
        return nullptr;
    }

    if (!Seq->GetMovieScene())
    {
        Seq->MovieScene = NewObject<UMovieScene>(Seq, NAME_None, RF_Transactional);
        Seq->MovieScene->SetFlags(RF_Transactional);
        Seq->MarkPackageDirty();
    }

    Result.bSuccess = true;
    return Seq;
}

bool USequencerAbstractionBPLibrary::SaveAsset(UObject* Asset)
{
    if (!Asset) return false;
    return UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
}

ULevelSequence* USequencerAbstractionBPLibrary::LoadLevelSequenceAsset(const FString& AssetPath)
{
    return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(AssetPath));
}

ULevelSequence* USequencerAbstractionBPLibrary::GetCurrentOpenedLevelSequence()
{
    return ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
}

FControlRigSequencerBindingProxy USequencerAbstractionBPLibrary::GetRigBindingProxyBasedOnClassFromOpenSequence(TSubclassOf<UControlRig> InClass)
{
#if !WITH_EDITOR
    return FControlRigSequencerBindingProxy();
#else
    UClass* TargetClass = InClass.Get();
    if (!TargetClass)
    {
        return FControlRigSequencerBindingProxy();
    }

    ULevelSequence* Sequence = GetCurrentOpenedLevelSequence();
    if (!Sequence)
    {
        return FControlRigSequencerBindingProxy();
    }

    const TArray<FControlRigSequencerBindingProxy> ControlRigs =
        UControlRigSequencerEditorLibrary::GetControlRigs(Sequence);

    for (const FControlRigSequencerBindingProxy& ControlRigBinding : ControlRigs)
    {
        UControlRig* ControlRig = ControlRigBinding.ControlRig;
        if (ControlRig && ControlRig->GetClass() == TargetClass)
        {
            return ControlRigBinding;
        }
    }

    return FControlRigSequencerBindingProxy();
#endif
}

bool USequencerAbstractionBPLibrary::GetCurrentFloatValueFromRigBindingProxy(
    FControlRigSequencerBindingProxy RigBinding,
    FName ControlName,
    float& OutValue,
    FString& ErrorMessage)
{
    OutValue = 0.0f;

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return false;
#else
    ErrorMessage.Empty();

    ULevelSequence* Sequence = GetCurrentOpenedLevelSequence();
    if (!Sequence)
    {
        ErrorMessage = TEXT("No Level Sequence is currently opened.");
        return false;
    }

    UControlRig* ControlRig = RigBinding.ControlRig;
    if (!ControlRig)
    {
        ErrorMessage = TEXT("Rig binding has no Control Rig.");
        return false;
    }

    if (ControlName.IsNone())
    {
        ErrorMessage = TEXT("ControlName is None.");
        return false;
    }

    if (!RigBinding.Proxy.BindingID.IsValid())
    {
        ErrorMessage = TEXT("Rig binding proxy has an invalid BindingID.");
        return false;
    }

    if (RigBinding.Proxy.Sequence != Sequence)
    {
        ErrorMessage = TEXT("Rig binding proxy does not belong to the currently opened Level Sequence.");
        return false;
    }

    UMovieSceneControlRigParameterTrack* Track = RigBinding.Track;
    if (!Track)
    {
        ErrorMessage = TEXT("Rig binding proxy has no Control Rig parameter track.");
        return false;
    }

    if (Track->GetControlRig() != ControlRig)
    {
        ErrorMessage = TEXT("Rig binding proxy track does not reference the same Control Rig instance.");
        return false;
    }

    FRigControlElement* ControlElement = ControlRig->FindControl(ControlName);
    if (!ControlElement)
    {
        ErrorMessage = FString::Printf(
            TEXT("Control Rig does not contain control '%s'."),
            *ControlName.ToString());
        return false;
    }

    const ERigControlType ControlType = ControlElement->Settings.ControlType;
    if (ControlType != ERigControlType::Float && ControlType != ERigControlType::ScaleFloat)
    {
        ErrorMessage = FString::Printf(
            TEXT("Control '%s' is not a float control."),
            *ControlName.ToString());
        return false;
    }

    const FMovieSceneSequencePlaybackParams CurrentTickPosition =
        ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition(EMovieSceneTimeUnit::TickResolution);
    const FFrameTime CurrentTickTime = CurrentTickPosition.Frame;

    UMovieSceneControlRigParameterSection* FirstScalarSection = nullptr;
    for (UMovieSceneSection* RawSection : Track->GetAllSections())
    {
        UMovieSceneControlRigParameterSection* Section = Cast<UMovieSceneControlRigParameterSection>(RawSection);
        if (!Section || !Section->HasScalarParameter(ControlName))
        {
            continue;
        }

        if (!FirstScalarSection)
        {
            FirstScalarSection = Section;
        }

        if (Section->GetRange().Contains(CurrentTickTime.FrameNumber))
        {
            const TOptional<float> EvaluatedValue = Section->EvaluateScalarParameter(CurrentTickTime, ControlName);
            if (EvaluatedValue.IsSet())
            {
                OutValue = EvaluatedValue.GetValue();
                return true;
            }
        }
    }

    if (FirstScalarSection)
    {
        const TOptional<float> EvaluatedValue = FirstScalarSection->EvaluateScalarParameter(CurrentTickTime, ControlName);
        if (EvaluatedValue.IsSet())
        {
            OutValue = EvaluatedValue.GetValue();
            return true;
        }
    }

    ErrorMessage = FString::Printf(
        TEXT("No scalar channel found for control '%s' on the proxy's Control Rig track."),
        *ControlName.ToString());
    return false;

#endif
}

bool USequencerAbstractionBPLibrary::OpenLevelSequenceInSequencer(ULevelSequence* Sequence)
{
    if (!Sequence)
    {
        return false;
    }

    if (!Sequence->GetMovieScene())
    {
        UE_LOG(LogTemp, Warning, TEXT("OpenLevelSequenceInSequencer: Sequence has no MovieScene. Not opening."));
        return false;
    }

    return ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence);
}

ULevelSequence* USequencerAbstractionBPLibrary::duplicateSequencerToFolder(
    const FString& sourceSequencePath,
    const FString& destinationFolder,
    const FString& newSequenceName
)
{
#if !WITH_EDITOR
    return nullptr;
#else

    if (!UEditorAssetLibrary::DoesAssetExist(sourceSequencePath))
    {
        return nullptr;
    }

    if (!UEditorAssetLibrary::DoesDirectoryExist(destinationFolder))
    {
        UEditorAssetLibrary::MakeDirectory(destinationFolder);
    }

    FString newAssetPath = destinationFolder + TEXT("/") + newSequenceName;

    UObject* duplicated = UEditorAssetLibrary::DuplicateAsset(sourceSequencePath, newAssetPath);

    if (!duplicated)
    {
        return nullptr;
    }

    return Cast<ULevelSequence>(duplicated);

#endif
}

static void FillSections(UMovieSceneTrack* Track, FSequenceTrackInfo& Info)
{
    Info.Sections.Reset();

    if (!Track) return;

    for (UMovieSceneSection* S : Track->GetAllSections())
    {
        if (!S) continue;

        FSequenceSectionInfo SI;
        SI.Section = S;
        SI.RowIndex = S->GetRowIndex();

        const TRange<FFrameNumber> R = S->GetRange();
        if (R.HasLowerBound()) SI.StartTick = R.GetLowerBoundValue().Value;
        if (R.HasUpperBound()) SI.EndTickExclusive = R.GetUpperBoundValue().Value;

        if (const UMovieSceneSkeletalAnimationSection* AnimSec = Cast<UMovieSceneSkeletalAnimationSection>(S))
        {
            SI.AnimSequence = AnimSec->Params.Animation;
        }

        Info.Sections.Add(SI);
    }
}

TArray<FSequenceTrackInfo> USequencerAbstractionBPLibrary::GetAllTracksInCurrentSequence()
{
    TArray<FSequenceTrackInfo> Out;

    ULevelSequence* Seq = GetCurrentOpenedLevelSequence();
    if (!Seq)
    {
        return Out;
    }

    const UMovieScene* MovieScene = Seq->GetMovieScene();
    if (!MovieScene)
    {
        return Out;
    }

    // Master / Root Tracks
    const TArray<UMovieSceneTrack*>& MasterTracks = MovieScene->GetTracks();

    for (int32 i = 0; i < MasterTracks.Num(); ++i)
    {
        UMovieSceneTrack* Track = MasterTracks[i];
        if (!Track)
        {
            continue;
        }

        FSequenceTrackInfo Info;
        Info.bIsMasterTrack = true;
        Info.DisplayName = Track->GetDisplayName().ToString();
        Info.TrackType = Track->GetClass()->GetName();
        Info.BindingGuid = FGuid();
        Info.ObjectBindingPath = TEXT("");
        Info.TrackPath = FString::Printf(
            TEXT("MASTER::%s::%d"),
            *Info.TrackType,
            i
        );
        FillSections(Track, Info);

        Out.Add(Info);
    }

    // Object Bindings + Tracks
    const TArray<FMovieSceneBinding>& Bindings =
        static_cast<const UMovieScene*>(MovieScene)->GetBindings();

    for (const FMovieSceneBinding& Binding : Bindings)
    {
        const FGuid Guid = Binding.GetObjectGuid();
        const TArray<UMovieSceneTrack*>& Tracks = Binding.GetTracks();

        for (int32 i = 0; i < Tracks.Num(); ++i)
        {
            UMovieSceneTrack* Track = Tracks[i];
            if (!Track)
            {
                continue;
            }

            FSequenceTrackInfo Info;
            Info.bIsMasterTrack = false;
            Info.DisplayName = Track->GetDisplayName().ToString();
            Info.TrackType = Track->GetClass()->GetName();
            Info.BindingGuid = Guid;
            Info.ObjectBindingPath = TEXT(""); // Resolve later if needed
            Info.TrackPath = FString::Printf(
                TEXT("BIND::%s::%s::%d"),
                *Guid.ToString(),
                *Info.TrackType,
                i
            );
            FillSections(Track, Info);

            Out.Add(Info);
        }
    }

    return Out;
}

TArray<UAnimSequence*> USequencerAbstractionBPLibrary::GetAllAnimSequencesInCurrentSequence()
{
    TArray<UAnimSequence*> Out;

    TArray<FSequenceTrackInfo> Tracks = USequencerAbstractionBPLibrary::GetAllTracksInCurrentSequence();
    int32 NumTracks = Tracks.Num();

    for (int32 i=0; i < NumTracks; i++)
    {
        TArray<FSequenceSectionInfo> Sections = Tracks[i].Sections;
        int32 NumSections = Sections.Num();

        for (int32 j=0; j < NumSections; j++)
        {
            UMovieSceneSection* Section = Sections[j].Section;
            UMovieSceneSkeletalAnimationSection* AnimSection = Cast<UMovieSceneSkeletalAnimationSection>(Section);
            if (!AnimSection) continue;

            UAnimSequence* AnimSeq = Cast<UAnimSequence>(AnimSection->Params.Animation);

            Out.Add(AnimSeq);
        }
    }

    return Out;
}

static FString GetBindingDisplayNameFromGuid(const UMovieScene* MovieScene, const FGuid& BindingGuid)
{
    if (!MovieScene || !BindingGuid.IsValid())
    {
        return FString();
    }

    UMovieScene* MovieSceneMutable = const_cast<UMovieScene*>(MovieScene);
    if (FMovieSceneSpawnable* Spawnable = MovieSceneMutable->FindSpawnable(BindingGuid))
    {
        return Spawnable->GetName();
    }

    if (FMovieScenePossessable* Possessable = MovieSceneMutable->FindPossessable(BindingGuid))
    {
        return Possessable->GetName();
    }

    return BindingGuid.ToString();
}

TArray<FActiveSkeletalAnimationInfo> USequencerAbstractionBPLibrary::GetActiveSkeletalAnimationsAtCurrentTime(FString& ErrorMessage)
{
    TArray<FActiveSkeletalAnimationInfo> Out;

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return Out;
#else
    ErrorMessage.Empty();

    ULevelSequence* Sequence = GetCurrentOpenedLevelSequence();
    if (!Sequence)
    {
        ErrorMessage = TEXT("No Level Sequence is currently opened.");
        return Out;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        ErrorMessage = TEXT("Sequence has no MovieScene.");
        return Out;
    }

    const FMovieSceneSequencePlaybackParams CurrentDisplayPosition =
        ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition(EMovieSceneTimeUnit::DisplayRate);
    const FMovieSceneSequencePlaybackParams CurrentTickPosition =
        ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition(EMovieSceneTimeUnit::TickResolution);

    const FFrameTime CurrentDisplayTime = CurrentDisplayPosition.Frame;
    const FFrameTime CurrentTickTime = CurrentTickPosition.Frame;
    const FFrameNumber CurrentTickFrame = CurrentTickTime.FrameNumber;
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    const UMovieScene* ConstMovieScene = MovieScene;
    for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
    {
        const FGuid BindingGuid = Binding.GetObjectGuid();
        const FString BindingName = GetBindingDisplayNameFromGuid(MovieScene, BindingGuid);

        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            UMovieSceneSkeletalAnimationTrack* AnimTrack = Cast<UMovieSceneSkeletalAnimationTrack>(Track);
            if (!AnimTrack)
            {
                continue;
            }

            for (UMovieSceneSection* Section : AnimTrack->GetAllSections())
            {
                UMovieSceneSkeletalAnimationSection* AnimSection = Cast<UMovieSceneSkeletalAnimationSection>(Section);
                if (!AnimSection || !AnimSection->IsActive())
                {
                    continue;
                }

                const TRange<FFrameNumber> SectionRange = AnimSection->GetRange();
                if (!SectionRange.Contains(CurrentTickFrame))
                {
                    continue;
                }

                UAnimSequenceBase* Animation = AnimSection->GetAnimation();
                if (!Animation)
                {
                    continue;
                }

                FActiveSkeletalAnimationInfo Info;
                Info.Animation = Animation;
                Info.Section = AnimSection;
                Info.BindingGuid = BindingGuid;
                Info.BindingName = BindingName;
                Info.SequencerDisplayFrame = CurrentDisplayTime.FrameNumber.Value;
                Info.SequencerTickFrame = CurrentTickFrame.Value;
                Info.AnimationTimeSeconds = static_cast<float>(AnimSection->MapTimeToAnimation(CurrentTickTime, TickResolution));
                Info.RowIndex = AnimSection->GetRowIndex();
                Out.Add(Info);
            }
        }
    }

    return Out;
#endif
}

bool USequencerAbstractionBPLibrary::SampleSourceAnimationFromActiveInfo(
    const FActiveSkeletalAnimationInfo& ActiveAnimation,
    FSourceAnimationFrameData& OutFrameData,
    FString& ErrorMessage)
{
    return SampleSourceAnimationAtTime(
        ActiveAnimation.Animation.Get(),
        ActiveAnimation.AnimationTimeSeconds,
        OutFrameData,
        ErrorMessage);
}

bool USequencerAbstractionBPLibrary::SampleSourceAnimationAtTime(
    UAnimSequenceBase* Animation,
    float AnimationTimeSeconds,
    FSourceAnimationFrameData& OutFrameData,
    FString& ErrorMessage)
{
    OutFrameData = {};

    if (!Animation)
    {
        ErrorMessage = TEXT("Animation is null.");
        return false;
    }

    const IAnimationDataModel* DataModel = Animation->GetDataModel();
    if (!DataModel)
    {
        ErrorMessage = TEXT("Animation has no source data model.");
        return false;
    }

    const double PlayLength = DataModel->GetPlayLength();
    const double SampleTime = FMath::Clamp(
        static_cast<double>(AnimationTimeSeconds),
        0.0,
        FMath::Max(0.0, PlayLength));

    const FFrameRate SourceFrameRate = DataModel->GetFrameRate();
    const FFrameTime SourceFrameTime = SourceFrameRate.AsFrameTime(SampleTime);

    EAnimInterpolationType Interpolation = EAnimInterpolationType::Linear;
    if (const UAnimSequence* AnimSequence = Cast<UAnimSequence>(Animation))
    {
        Interpolation = AnimSequence->Interpolation;
    }

    OutFrameData.Animation = Animation;
    OutFrameData.AnimationTimeSeconds = static_cast<float>(SampleTime);
    OutFrameData.AnimationFrame = SourceFrameTime.GetFrame().Value;

    TArray<FName> BoneTrackNames;
    DataModel->GetBoneTrackNames(BoneTrackNames);
    OutFrameData.Bones.Reserve(BoneTrackNames.Num());

    for (const FName& BoneName : BoneTrackNames)
    {
        FSourceAnimationBoneFrameData BoneData;
        BoneData.BoneName = BoneName;
        BoneData.LocalTransform = DataModel->EvaluateBoneTrackTransform(BoneName, SourceFrameTime, Interpolation);
        OutFrameData.Bones.Add(BoneData);
    }

    const TArray<FFloatCurve>& FloatCurves = DataModel->GetFloatCurves();
    OutFrameData.Curves.Reserve(FloatCurves.Num());

    for (const FFloatCurve& FloatCurve : FloatCurves)
    {
        FSourceAnimationCurveFrameData CurveData;
        CurveData.CurveName = FloatCurve.GetName();
        CurveData.Value = FloatCurve.Evaluate(static_cast<float>(SampleTime));
        OutFrameData.Curves.Add(CurveData);
    }

    ErrorMessage.Empty();
    return true;
}

TArray<FSequenceBindingInfo> USequencerAbstractionBPLibrary::GetBindingsInSequence(ULevelSequence* Sequence)
{
    TArray<FSequenceBindingInfo> Out;

    if (!Sequence)
    {
        return Out;
    }

    const UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        return Out;
    }

    const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
    Out.Reserve(Bindings.Num());
    UMovieScene* MovieSceneMutable = const_cast<UMovieScene*>(MovieScene);

    for (const FMovieSceneBinding& Binding : Bindings)
    {
        const FGuid Guid = Binding.GetObjectGuid();

        FSequenceBindingInfo Info;
        Info.BindingGuid = Guid;
        Info.ObjectBindingId = FMovieSceneObjectBindingID(Guid);
        Info.TrackCount = Binding.GetTracks().Num();

        if (FMovieSceneSpawnable* Spawnable = MovieSceneMutable->FindSpawnable(Guid))
        {
            Info.bIsSpawnable = true;
            Info.DisplayName = Spawnable->GetName();

            UObject* TemplateObj = Spawnable->GetObjectTemplate();
            if (TemplateObj)
            {
                Info.BoundObjectClass = TemplateObj->GetClass()->GetName();
            }
        }
        else if (FMovieScenePossessable* Possessable = MovieSceneMutable->FindPossessable(Guid))
        {
            Info.bIsPossessable = true;
            Info.DisplayName = Possessable->GetName();

            const UClass* ObjClass = Possessable->GetPossessedObjectClass();
            if (ObjClass)
            {
                Info.BoundObjectClass = ObjClass->GetName();
            }
        }
        else
        {
            // Fallback: if neither spawnable nor possessable resolves, keep something stable
            Info.DisplayName = Guid.ToString();
        }

        Out.Add(Info);
    }

    return Out;
}

USkeletalMesh* USequencerAbstractionBPLibrary::LoadSkeletalMesh(const FString& AssetPath)
{
    return Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
}

UAnimSequence* USequencerAbstractionBPLibrary::LoadAnimSequence(const FString& AssetPath)
{
    return Cast<UAnimSequence>(UEditorAssetLibrary::LoadAsset(AssetPath));
}

TSubclassOf<UControlRig> USequencerAbstractionBPLibrary::LoadControlRigClass(const FString& AssetPath)
{
    if (AssetPath.IsEmpty())
    {
        return nullptr;
    }

    // For Control Rig assets, you must load the GENERATED class
    // Path format example:
    // "/Game/Rigs/MyRig.MyRig_C"

    UClass* LoadedClass = LoadObject<UClass>(nullptr, *AssetPath);
    if (!LoadedClass)
    {
        return nullptr;
    }

    if (!LoadedClass->IsChildOf(UControlRig::StaticClass()))
    {
        return nullptr;
    }

    return LoadedClass;
}

static USkeletalMeshComponent* ResolveSkelCompFromBinding(const TArray<UObject*>& BoundObjects)
{
    for (UObject* Obj : BoundObjects)
    {
        if (!Obj) continue;

        // 1) Binding directly to a component
        if (USkeletalMeshComponent* Comp = Cast<USkeletalMeshComponent>(Obj))
        {
            return Comp;
        }

        // 2) Binding to an actor
        if (AActor* Actor = Cast<AActor>(Obj))
        {
            // SkeletalMeshActor
            if (ASkeletalMeshActor* SkelActor = Cast<ASkeletalMeshActor>(Actor))
            {
                if (USkeletalMeshComponent* Comp = SkelActor->GetSkeletalMeshComponent())
                {
                    return Comp;
                }
            }

            // Any actor that has a skeletal mesh component
            if (USkeletalMeshComponent* Comp = Actor->FindComponentByClass<USkeletalMeshComponent>())
            {
                return Comp;
            }
        }
    }

    return nullptr;
}

static UWorld* ResolveWorldForEditorSequencerAction(UObject* WorldContextObject)
{
    if (WorldContextObject)
    {
        if (UWorld* World = WorldContextObject->GetWorld())
        {
            return World;
        }
    }

    if (!GEditor)
    {
        return nullptr;
    }

    if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
    {
        return EditorWorld;
    }

    for (const FWorldContext& WorldContext : GEditor->GetWorldContexts())
    {
        if (WorldContext.WorldType == EWorldType::Editor || WorldContext.WorldType == EWorldType::PIE)
        {
            if (UWorld* World = WorldContext.World())
            {
                return World;
            }
        }
    }

    return nullptr;
}

static bool ParseTrackPath(const FString& TrackPath, bool& bOutIsMaster, FGuid& OutGuid, FString& OutTypeName, int32& OutIndex)
{
    bOutIsMaster = false;
    OutGuid.Invalidate();
    OutTypeName.Reset();
    OutIndex = INDEX_NONE;

    TArray<FString> Parts;
    TrackPath.ParseIntoArray(Parts, TEXT("::"), true);

    if (Parts.Num() < 3)
    {
        return false;
    }

    const FString& Kind = Parts[0];
    if (Kind.Equals(TEXT("MASTER"), ESearchCase::IgnoreCase))
    {
        // MASTER::Type::Index
        if (Parts.Num() != 3) return false;
        bOutIsMaster = true;
        OutTypeName = Parts[1];
        OutIndex = FCString::Atoi(*Parts[2]);
        return OutIndex >= 0;
    }

    if (Kind.Equals(TEXT("BIND"), ESearchCase::IgnoreCase))
    {
        // BIND::Guid::Type::Index
        if (Parts.Num() != 4) return false;

        bOutIsMaster = false;
        if (!FGuid::Parse(Parts[1], OutGuid)) return false;

        OutTypeName = Parts[2];
        OutIndex = FCString::Atoi(*Parts[3]);
        return OutIndex >= 0;
    }

    return false;
}

bool USequencerAbstractionBPLibrary::RemoveTrackInSequenceByTrackPath(
    ULevelSequence* Sequence,
    const FString& TrackPath,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence)
    {
        Result.Error = TEXT("Sequence is null.");
        return false;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return false;
    }

    bool bIsMaster = false;
    FGuid BindingGuid;
    FString TypeName;
    int32 Index = INDEX_NONE;

    if (!ParseTrackPath(TrackPath, bIsMaster, BindingGuid, TypeName, Index))
    {
        Result.Error = TEXT("Invalid TrackPath format.");
        return false;
    }

    MovieScene->Modify();

    if (bIsMaster)
    {
        const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetTracks();
        if (!Tracks.IsValidIndex(Index) || !Tracks[Index])
        {
            Result.Error = TEXT("Master track index invalid.");
            return false;
        }

        UMovieSceneTrack* Track = Tracks[Index];
        const bool bRemoved = MovieScene->RemoveTrack(*Track); // UE 5.7 :contentReference[oaicite:3]{index=3}
        if (!bRemoved)
        {
            Result.Error = TEXT("Failed to remove master track.");
            return false;
        }
    }
    else
    {
        FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid); // :contentReference[oaicite:4]{index=4}
        if (!Binding)
        {
            Result.Error = TEXT("BindingGuid not found in MovieScene.");
            return false;
        }

        const TArray<UMovieSceneTrack*>& Tracks = Binding->GetTracks();
        if (!Tracks.IsValidIndex(Index) || !Tracks[Index])
        {
            Result.Error = TEXT("Binding track index invalid.");
            return false;
        }

        UMovieSceneTrack* Track = Tracks[Index];
        const bool bRemoved = Binding->RemoveTrack(*Track, MovieScene); // UE 5.7 :contentReference[oaicite:5]{index=5}
        if (!bRemoved)
        {
            Result.Error = TEXT("Failed to remove binding track.");
            return false;
        }
    }

    Sequence->MarkPackageDirty();
    Result.bSuccess = true;
    return true;
}

int32 USequencerAbstractionBPLibrary::RemoveTracksInSequenceByBindingGuid(
    ULevelSequence* Sequence,
    const FGuid& BindingGuid,
    const FString& OptionalTrackClassName,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence)
    {
        Result.Error = TEXT("Sequence is null.");
        return 0;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return 0;
    }

    if (!BindingGuid.IsValid())
    {
        Result.Error = TEXT("BindingGuid is invalid.");
        return 0;
    }

    FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid); // :contentReference[oaicite:6]{index=6}
    if (!Binding)
    {
        Result.Error = TEXT("BindingGuid not found in MovieScene.");
        return 0;
    }

    MovieScene->Modify();

    // Copy pointers first (we�re going to remove while iterating)
    TArray<UMovieSceneTrack*> TracksToRemove;
    {
        const TArray<UMovieSceneTrack*>& Tracks = Binding->GetTracks();
        for (UMovieSceneTrack* T : Tracks)
        {
            if (!T) continue;

            if (!OptionalTrackClassName.IsEmpty())
            {
                if (!T->GetClass()->GetName().Equals(OptionalTrackClassName, ESearchCase::IgnoreCase))
                {
                    continue;
                }
            }

            TracksToRemove.Add(T);
        }
    }

    int32 RemovedCount = 0;
    for (UMovieSceneTrack* Track : TracksToRemove)
    {
        if (!Track) continue;
        if (Binding->RemoveTrack(*Track, MovieScene)) // :contentReference[oaicite:7]{index=7}
        {
            ++RemovedCount;
        }
    }

    if (RemovedCount > 0)
    {
        Sequence->MarkPackageDirty();
    }

    Result.bSuccess = true;
    return RemovedCount;
}

bool USequencerAbstractionBPLibrary::SetSequenceFrameRateFromAnimation(
    ULevelSequence* Sequence,
    UAnimSequence* Animation,
    bool bAlsoSetPlaybackRangeToAnim,
    FSequenceOpenResult& Result)
{
    Result = {};
    if (!Sequence || !Animation)
    {
        Result.Error = TEXT("Sequence or Animation is null.");
        return false;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return false;
    }

    const FFrameRate AnimRate = Animation->GetSamplingFrameRate();
    if (AnimRate.Numerator <= 0 || AnimRate.Denominator <= 0)
    {
        Result.Error = TEXT("Animation sampling frame rate is invalid.");
        return false;
    }

    MovieScene->Modify();
    MovieScene->SetPlaybackRangeLocked(false);

    MovieScene->SetDisplayRate(AnimRate);

    // Choose ONE policy and stick to it everywhere.
    // If you want subframe precision:
    const FFrameRate TickRes(AnimRate.Numerator * 1000, AnimRate.Denominator);
    MovieScene->SetTickResolutionDirectly(TickRes);

    if (bAlsoSetPlaybackRangeToAnim)
    {
        const double Seconds = Animation->GetPlayLength();

        // Compute end EXCLUSIVE in ticks
        const FFrameTime EndDisplayExclusive = (Seconds * AnimRate); // already time, conceptually exclusive boundary
        const FFrameTime EndTickExclusive = FFrameRate::TransformTime(EndDisplayExclusive, AnimRate, TickRes);

        const int32 DurationTicks = FMath::Max(1, EndTickExclusive.CeilToFrame().Value);
        MovieScene->SetPlaybackRange(FFrameNumber(0), DurationTicks, true);

        MovieScene->SetWorkingRange(0.0, Seconds);
        MovieScene->SetViewRange(0.0, Seconds);
    }

    Sequence->MarkPackageDirty();
    Result.bSuccess = true;
    return true;
}

static AActor* FindActorByLabelOrName(UWorld* World, const FName LabelOrName)
{
    if (!World) return nullptr;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A) continue;

#if WITH_EDITOR
        if (A->GetActorLabel() == LabelOrName.ToString())
            return A;
#endif
        if (A->GetFName() == LabelOrName)
            return A;
    }
    return nullptr;
}

void USequencerAbstractionBPLibrary::moveSequencerPlayheadToFrame(int32 frame)
{
    FMovieSceneSequencePlaybackParams playbackParams;
    playbackParams.Frame = FFrameTime(frame);

    ULevelSequenceEditorBlueprintLibrary::SetLocalPosition(
        playbackParams,
        EMovieSceneTimeUnit::DisplayRate
    );

    ULevelSequenceEditorBlueprintLibrary::ForceUpdate();
}

bool USequencerAbstractionBPLibrary::SetSequencePlaybackRange(
    ULevelSequence* Sequence,
    int32 StartFrame,
    int32 EndFrame)
{
    if (!Sequence) return false;

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene) return false;

    if (EndFrame < StartFrame) return false;

    MovieScene->Modify();
    MovieScene->SetPlaybackRangeLocked(false);

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickRes = MovieScene->GetTickResolution();

    // Treat inputs as DISPLAY frames, inclusive end.
    const FFrameTime StartDisplay(StartFrame);

    // Make end EXCLUSIVE by adding +1 display frame before converting.
    const FFrameTime EndDisplayExclusive(EndFrame);

    const FFrameTime StartTicks = FFrameRate::TransformTime(StartDisplay, DisplayRate, TickRes);
    const FFrameTime EndTicksEx = FFrameRate::TransformTime(EndDisplayExclusive, DisplayRate, TickRes);

    const FFrameNumber StartTickFrame = StartTicks.FloorToFrame();
    const FFrameNumber EndTickFrameEx = EndTicksEx.CeilToFrame();

    int32 DurationTicks = (EndTickFrameEx - StartTickFrame).Value;
    DurationTicks = FMath::Max(1, DurationTicks);

    MovieScene->SetPlaybackRange(StartTickFrame, DurationTicks, /*bAlwaysMarkDirty*/ true);

    // UI �outer� range is seconds and is typically inclusive-looking; use exact frame boundaries.
    const double StartSeconds = DisplayRate.AsSeconds(FFrameNumber(StartFrame));
    const double EndSeconds = DisplayRate.AsSeconds(FFrameNumber(EndFrame)); // exclusive in seconds
    MovieScene->SetWorkingRange(StartSeconds, EndSeconds);
    MovieScene->SetViewRange(StartSeconds, EndSeconds);

    Sequence->MarkPackageDirty();
    return true;
}

bool USequencerAbstractionBPLibrary::GetSequencePlaybackRange(
    ULevelSequence* Sequence,
    int32& OutStartFrame,
    int32& OutEndFrame)
{
    OutStartFrame = 0;
    OutEndFrame = 0;

    if (!Sequence) return false;

    const UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene) return false;

    // PlaybackRange is in TICK resolution, and its upper bound is EXCLUSIVE.
    const TRange<FFrameNumber> TickRange = MovieScene->GetPlaybackRange();

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickRes = MovieScene->GetTickResolution();

    const FFrameTime StartTicks(TickRange.GetLowerBoundValue());
    const FFrameTime EndTicksEx(TickRange.GetUpperBoundValue()); // exclusive

    const FFrameTime StartDisplay = FFrameRate::TransformTime(StartTicks, TickRes, DisplayRate);
    const FFrameTime EndDisplayEx = FFrameRate::TransformTime(EndTicksEx, TickRes, DisplayRate);

    const int32 StartDispFrame = StartDisplay.FloorToFrame().Value;

    // Convert exclusive end back to inclusive end by subtracting 1 display frame.
    const int32 EndDispFrameInclusive = EndDisplayEx.CeilToFrame().Value - 1;

    OutStartFrame = StartDispFrame;
    OutEndFrame = FMath::Max(OutStartFrame, EndDispFrameInclusive);
    return true;
}

bool USequencerAbstractionBPLibrary::MoveAnimationSectionStartTo_Legacy(
    UMovieSceneSkeletalAnimationSection* Section,
    int32 NewStartFrame)
{
    if (!Section) return false;

    TRange<FFrameNumber> Range = Section->GetRange();
    int32 Length = Range.Size<FFrameNumber>().Value;

    Section->SetRange(
        TRange<FFrameNumber>(
            FFrameNumber(NewStartFrame),
            FFrameNumber(NewStartFrame + Length)
        )
    );

    return true;
}

bool USequencerAbstractionBPLibrary::MoveAnimationSectionEndTo_Legacy(
    UMovieSceneSkeletalAnimationSection* Section,
    int32 NewEndFrame)
{
    if (!Section) return false;

    TRange<FFrameNumber> Range = Section->GetRange();
    int32 Length = Range.Size<FFrameNumber>().Value;

    int32 NewStart = NewEndFrame - Length;

    Section->SetRange(
        TRange<FFrameNumber>(
            FFrameNumber(NewStart),
            FFrameNumber(NewEndFrame)
        )
    );

    return true;
}

FGuid USequencerAbstractionBPLibrary::AddSkeletalMeshToOpenSequenceFromPath(
    UObject* WorldContextObject,
    const FString& SkeletalMeshAssetPath,
    const FName SpawnedActorLabel,
    FSequenceOpenResult& Result)
{
    Result = {};
    if (!WorldContextObject)
    {
        Result.Error = TEXT("WorldContextObject is null.");
        return FGuid();
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        Result.Error = TEXT("Invalid world.");
        return FGuid();
    }

    ULevelSequence* Sequence = GetCurrentOpenedLevelSequence();
    if (!Sequence)
    {
        Result.Error = TEXT("No LevelSequence is currently open in Sequencer.");
        return FGuid();
    }

    USkeletalMesh* Mesh = Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(SkeletalMeshAssetPath));
    if (!Mesh)
    {
        Result.Error = TEXT("Failed to load SkeletalMesh from path.");
        return FGuid();
    }

    ASkeletalMeshActor* SkelActor = World->SpawnActor<ASkeletalMeshActor>();
    if (!SkelActor || !SkelActor->GetSkeletalMeshComponent())
    {
        Result.Error = TEXT("Failed to spawn ASkeletalMeshActor.");
        return FGuid();
    }

#if WITH_EDITOR
    if (!SpawnedActorLabel.IsNone())
    {
        SkelActor->SetActorLabel(SpawnedActorLabel.ToString());
    }
#endif

    SkelActor->GetSkeletalMeshComponent()->SetSkeletalMesh(Mesh);

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return FGuid();
    }

    const FString PossessableName =
#if WITH_EDITOR
        SkelActor->GetActorLabel();
#else
        SkelActor->GetName();
#endif

    const FGuid BindingGuid = MovieScene->AddPossessable(PossessableName, SkelActor->GetClass());
    Sequence->BindPossessableObject(BindingGuid, *SkelActor, World);

    // Add and remove track to initialize
    MovieScene->Modify();
    Sequence->Modify();
    UMovieSceneSkeletalAnimationTrack* Track = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
    if (!Track)
    {
        Track = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
    }
    MovieScene->RemoveTrack(*Track);

    Result.bSuccess = true;
    return BindingGuid;
}

static void NotifySequencerMovieSceneChanged(
    ULevelSequence* Sequence,
    EMovieSceneDataChangeType ChangeType = EMovieSceneDataChangeType::MovieSceneStructureItemAdded)
{
#if WITH_EDITOR
    if (!Sequence || !GEditor) return;

    if (UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
    {
        if (IAssetEditorInstance* Inst = EditorSubsystem->FindEditorForAsset(Sequence, /*bFocusIfOpen*/ false))
        {
            if (ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(Inst))
            {
                if (TSharedPtr<ISequencer> Seq = Toolkit->GetSequencer())
                {
                    Seq->NotifyMovieSceneDataChanged(ChangeType);
                    Seq->ForceEvaluate();
                }
            }
        }
    }
#endif
}

static void FocusMovieSceneOnSection(UMovieScene* MovieScene, UMovieSceneSection* Section, float PaddingSeconds = 0.4f)
{
    if (!MovieScene || !Section || !Section->HasStartFrame() || !Section->HasEndFrame())
    {
        return;
    }

    MovieScene->SetPlaybackRangeLocked(false);

    const FFrameNumber StartTick = Section->GetInclusiveStartFrame();
    const FFrameNumber EndTick = FMath::Max(StartTick + 1, Section->GetExclusiveEndFrame() - 1);
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>::Inclusive(StartTick, EndTick));

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const double StartSeconds = TickResolution.AsSeconds(FFrameTime(StartTick));
    const double EndSeconds = TickResolution.AsSeconds(FFrameTime(EndTick));
    MovieScene->SetWorkingRange(StartSeconds - PaddingSeconds, EndSeconds + PaddingSeconds);
    MovieScene->SetViewRange(StartSeconds - PaddingSeconds, EndSeconds + PaddingSeconds);
}

FGuid USequencerAbstractionBPLibrary::FindOrCreatePossessableBinding(
    ULevelSequence* Sequence,
    AActor* Actor,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !Actor)
    {
        Result.Error = TEXT("Sequence or Actor is null.");
        return FGuid();
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return FGuid();
    }

    const FString ActorLabel = Actor->GetActorLabel();
    const UClass* ActorClass = Actor->GetClass();

    for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
    {
        FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid());
        if (Possessable &&
            Possessable->GetName() == ActorLabel &&
            Possessable->GetPossessedObjectClass() == ActorClass)
        {
            if (UWorld* World = Actor->GetWorld())
            {
                Sequence->Modify();
                Sequence->UnbindPossessableObjects(Binding.GetObjectGuid());
                Sequence->BindPossessableObject(Binding.GetObjectGuid(), *Actor, World);
                Sequence->MarkPackageDirty();
            }

            Result.bSuccess = true;
            return Binding.GetObjectGuid();
        }
    }

    UWorld* World = Actor->GetWorld();
    if (!World)
    {
        Result.Error = TEXT("Actor has no valid World.");
        return FGuid();
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(ActorLabel, Actor->GetClass());
    Sequence->BindPossessableObject(BindingGuid, *Actor, World);
    MovieScene->Modify();
    Sequence->MarkPackageDirty();
    NotifySequencerMovieSceneChanged(Sequence);

    Result.bSuccess = BindingGuid.IsValid();
    if (!Result.bSuccess)
    {
        Result.Error = TEXT("Failed to create possessable binding.");
    }
    return BindingGuid;
}

FGuid USequencerAbstractionBPLibrary::FindPossessableBinding(
    ULevelSequence* Sequence,
    AActor* Actor,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !Actor)
    {
        Result.Error = TEXT("Sequence or Actor is null.");
        return FGuid();
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return FGuid();
    }

    const FString ActorLabel = Actor->GetActorLabel();
    const UClass* ActorClass = Actor->GetClass();

    for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
    {
        FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid());
        if (Possessable &&
            Possessable->GetName() == ActorLabel &&
            Possessable->GetPossessedObjectClass() == ActorClass)
        {
            if (UWorld* World = Actor->GetWorld())
            {
                Sequence->Modify();
                Sequence->UnbindPossessableObjects(Binding.GetObjectGuid());
                Sequence->BindPossessableObject(Binding.GetObjectGuid(), *Actor, World);
                Sequence->MarkPackageDirty();
            }

            Result.bSuccess = true;
            return Binding.GetObjectGuid();
        }
    }

    Result.Error = FString::Printf(
        TEXT("No possessable binding found for actor '%s'."),
        *ActorLabel);
    return FGuid();
}

bool USequencerAbstractionBPLibrary::SnapSectionToSourceTimecode(
    ULevelSequence* Sequence,
    UMovieSceneSection* Section,
    bool bFocusSection,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !Section)
    {
        Result.Error = TEXT("Sequence or Section is null.");
        return false;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return false;
    }

    if (!GEditor)
    {
        Result.Error = TEXT("GEditor is not available.");
        return false;
    }

    ULevelSequenceEditorSubsystem* LevelSequenceEditorSubsystem =
        GEditor->GetEditorSubsystem<ULevelSequenceEditorSubsystem>();
    if (!LevelSequenceEditorSubsystem)
    {
        Result.Error = TEXT("LevelSequenceEditorSubsystem is not available.");
        return false;
    }

    LevelSequenceEditorSubsystem->SnapSectionsToTimelineUsingSourceTimecode({ Section });
    if (bFocusSection)
    {
        FocusMovieSceneOnSection(MovieScene, Section);
    }
    NotifySequencerMovieSceneChanged(Sequence, EMovieSceneDataChangeType::TrackValueChanged);

    Result.bSuccess = true;
    return true;
}
static int32 PickRowIndexForRange(
    const UMovieSceneSkeletalAnimationTrack* Track,
    const TRange<FFrameNumber>& NewRange,
    bool bAllowOverlapSameRow)
{
    if (!Track) return 0;
    if (bAllowOverlapSameRow) return 0;

    // Find the lowest row index that doesn't overlap with existing sections on that row.
    // (Simple O(n^2) scan; fine for typical section counts.)
    int32 Row = 0;
    for (;;)
    {
        bool bOverlapsThisRow = false;
        for (UMovieSceneSection* S : Track->GetAllSections())
        {
            if (!S) continue;
            if (S->GetRowIndex() != Row) continue;
            if (S->GetRange().Overlaps(NewRange))
            {
                bOverlapsThisRow = true;
                break;
            }
        }

        if (!bOverlapsThisRow)
        {
            return Row;
        }
        ++Row;
    }
}

UMovieSceneSkeletalAnimationSection* USequencerAbstractionBPLibrary::AddAnimSectionToBinding(
    ULevelSequence* Sequence,
    const FGuid& BindingGuid,
    UAnimSequence* Animation,
    int32 StartFrame,
    int32 RowIndex,
    bool bAllowOverlapSameRow,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !Animation || !BindingGuid.IsValid())
    {
        Result.Error = TEXT("Invalid inputs (Sequence / BindingGuid / Animation).");
        return nullptr;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return nullptr;
    }

    if (!MovieScene->FindBinding(BindingGuid))
    {
        Result.Error = TEXT("BindingGuid not found in MovieScene.");
        return nullptr;
    }

    MovieScene->Modify();
    Sequence->Modify();

    // One track per binding; multiple sections/rows inside it.
    UMovieSceneSkeletalAnimationTrack* Track =
        MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);

    if (!Track)
    {
        Track = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
    }

    if (!Track)
    {
        Result.Error = TEXT("Failed to create/get skeletal animation track.");
        return nullptr;
    }

    Track->Modify();

    UMovieSceneSkeletalAnimationSection* Section =
        Cast<UMovieSceneSkeletalAnimationSection>(Track->CreateNewSection());

    if (!Section)
    {
        Result.Error = TEXT("Failed to create skeletal animation section.");
        return nullptr;
    }

    Section->Modify();
    Section->Params.Animation = Animation;

    // Convert StartFrame (display frames) + length (seconds) => tick range
    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickRes = MovieScene->GetTickResolution();

    if (DisplayRate.Numerator <= 0 || DisplayRate.Denominator <= 0 ||
        TickRes.Numerator <= 0 || TickRes.Denominator <= 0)
    {
        Result.Error = TEXT("Invalid DisplayRate or TickResolution.");
        return nullptr;
    }

    const double Seconds = Animation->GetPlayLength();

    const FFrameTime StartDisplayTime(StartFrame);
    const FFrameTime EndDisplayTimeExclusive = StartDisplayTime + (Seconds * DisplayRate);

    const FFrameTime StartTickTime = FFrameRate::TransformTime(StartDisplayTime, DisplayRate, TickRes);
    const FFrameTime EndTickTimeEx = FFrameRate::TransformTime(EndDisplayTimeExclusive, DisplayRate, TickRes);

    const FFrameNumber StartTick = StartTickTime.FloorToFrame();
    FFrameNumber EndTickEx = EndTickTimeEx.CeilToFrame();

    if (EndTickEx <= StartTick)
    {
        EndTickEx = StartTick + 1;
    }

    const TRange<FFrameNumber> NewRange(StartTick, EndTickEx);

    // Row selection
    int32 FinalRow = RowIndex;
    if (FinalRow < 0)
    {
        FinalRow = PickRowIndexForRange(Track, NewRange, bAllowOverlapSameRow);
    }
    else if (!bAllowOverlapSameRow)
    {
        // If caller specified a row, but overlap isn't allowed, bump if needed.
        for (UMovieSceneSection* S : Track->GetAllSections())
        {
            if (!S) continue;
            if (S->GetRowIndex() != FinalRow) continue;
            if (S->GetRange().Overlaps(NewRange))
            {
                FinalRow = PickRowIndexForRange(Track, NewRange, /*bAllowOverlapSameRow*/ false);
                break;
            }
        }
    }

    Section->SetRowIndex(FinalRow);
    Section->SetRange(NewRange);

    Track->AddSection(*Section);
    Track->MarkAsChanged();

    Sequence->MarkPackageDirty();
    Result.bSuccess = true;
    return Section;
}

UMovieSceneSection* USequencerAbstractionBPLibrary::AddMediaSourceProxySectionToBinding(
    ULevelSequence* Sequence,
    const FGuid& BindingGuid,
    UMediaSource* MediaSource,
    int32 StartFrame,
    int32 MediaSourceProxyIndex,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !BindingGuid.IsValid() || !MediaSource)
    {
        Result.Error = TEXT("Invalid inputs (Sequence / BindingGuid / MediaSource).");
        return nullptr;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return nullptr;
    }

    if (!MovieScene->FindBinding(BindingGuid))
    {
        Result.Error = TEXT("BindingGuid not found in MovieScene.");
        return nullptr;
    }

    MovieScene->Modify();
    Sequence->Modify();

    UMovieSceneMediaTrack* MediaTrack = MovieScene->FindTrack<UMovieSceneMediaTrack>(BindingGuid);
    if (!MediaTrack)
    {
        MediaTrack = MovieScene->AddTrack<UMovieSceneMediaTrack>(BindingGuid);
        if (MediaTrack)
        {
            MediaTrack->SetDisplayName(FText::FromString(TEXT("Media")));
        }
    }

    if (!MediaTrack)
    {
        Result.Error = TEXT("Failed to create/get media track.");
        return nullptr;
    }

    MediaTrack->Modify();

    const FMovieSceneObjectBindingID ObjectBindingID{
        UE::MovieScene::FRelativeObjectBindingID(BindingGuid)
    };
    UMovieSceneSection* Section = MediaTrack->AddNewMediaSourceProxy(
        MediaSource,
        ObjectBindingID,
        MediaSourceProxyIndex,
        FFrameNumber(StartFrame));

    if (!Section)
    {
        Result.Error = TEXT("Failed to create media section.");
        return nullptr;
    }

    if (UMovieSceneMediaSection* MediaSection = Cast<UMovieSceneMediaSection>(Section))
    {
        MediaSection->bHasMediaPlayerProxy = true;
    }

    MediaTrack->MarkAsChanged();
    Sequence->MarkPackageDirty();
    NotifySequencerMovieSceneChanged(Sequence);

    Result.bSuccess = true;
    return Section;
}

int32 USequencerAbstractionBPLibrary::RemoveMediaTracksFromBinding(
    ULevelSequence* Sequence,
    const FGuid& BindingGuid,
    FSequenceOpenResult& Result)
{
    return RemoveTracksInSequenceByBindingGuid(
        Sequence,
        BindingGuid,
        UMovieSceneMediaTrack::StaticClass()->GetName(),
        Result);
}

bool USequencerAbstractionBPLibrary::AddRigToBinding(
    ULevelSequence* Sequence,
    UObject* WorldContextObject,
    const FGuid& BindingGuid,
    TSubclassOf<UControlRig> ControlRigClass,
    bool bLayered,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !WorldContextObject || !BindingGuid.IsValid() || !*ControlRigClass)
    {
        Result.Error = TEXT("Invalid inputs.");
        return false;
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        Result.Error = TEXT("Invalid World.");
        return false;
    }

    const FMovieSceneBindingProxy BindingProxy(BindingGuid, Sequence);

    UMovieSceneTrack* Track = UControlRigSequencerEditorLibrary::FindOrCreateControlRigTrack(
        World,
        Sequence,
        ControlRigClass.Get(),
        BindingProxy,
        bLayered
    );

    if (!Track)
    {
        Result.Error = TEXT("Failed to create/find Control Rig track.");
        return false;
    }

    Result.bSuccess = true;
    return true;
}

bool USequencerAbstractionBPLibrary::BakeBindingToAnimSequence(
    ULevelSequence* Sequence,
    UObject* WorldContextObject,
    const FGuid& BindingGuid,
    const FString& TargetPackagePath,
    const FString& NewAssetName,
    FSequenceOpenResult& Result)
{
#if !WITH_EDITOR
    Result = {};
    Result.Error = TEXT("BakeBindingToAnimSequence is editor-only.");
    return false;
#else
    Result = {};

    if (!Sequence || !WorldContextObject || !BindingGuid.IsValid())
    {
        Result.Error = TEXT("Invalid inputs (Sequence / WorldContextObject / BindingGuid).");
        return false;
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        Result.Error = TEXT("Invalid World.");
        return false;
    }

    if (TargetPackagePath.IsEmpty() || NewAssetName.IsEmpty())
    {
        Result.Error = TEXT("TargetPackagePath or NewAssetName is empty.");
        return false;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return false;
    }

    // ---- Resolve bound SkeletalMeshComponent ----
    USkeletalMeshComponent* SkelComp = nullptr;
    {
        UE::MovieScene::FRelativeObjectBindingID RelID(BindingGuid);
        FMovieSceneObjectBindingID BindingID(RelID);

        const TArray<UObject*> BoundObjects = ULevelSequenceEditorBlueprintLibrary::GetBoundObjects(BindingID);
        SkelComp = ResolveSkelCompFromBinding(BoundObjects);
    }

    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || !SkelComp->GetSkeletalMeshAsset()->GetSkeleton())
    {
        Result.Error = TEXT("Could not resolve a valid USkeletalMeshComponent/Skeleton from the binding.");
        return false;
    }

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    if (DisplayRate.Numerator <= 0 || DisplayRate.Denominator <= 0)
    {
        Result.Error = TEXT("Invalid MovieScene DisplayRate.");
        return false;
    }

    // ---- Ensure we have an editor ISequencer player (critical for ControlRig / eval correctness) ----
    TSharedPtr<ISequencer> EditorSequencer;
    if (UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
    {
        // Ensure editor is open so we can get the Sequencer player.
        if (!EditorSubsystem->FindEditorForAsset(Sequence, /*bFocusIfOpen*/ false))
        {
            EditorSubsystem->OpenEditorForAsset(Sequence);
        }

        if (IAssetEditorInstance* Inst = EditorSubsystem->FindEditorForAsset(Sequence, /*bFocusIfOpen*/ false))
        {
            if (ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(Inst))
            {
                EditorSequencer = Toolkit->GetSequencer();
            }
        }
    }

    if (!EditorSequencer.IsValid())
    {
        Result.Error = TEXT("Failed to get active ISequencer for this LevelSequence (editor not open?).");
        return false;
    }

    // ---- Create AnimSequence asset via factory (avoids half-initialized data model issues) ----
    const FString CleanPath = TargetPackagePath.EndsWith(TEXT("/"))
        ? TargetPackagePath.LeftChop(1)
        : TargetPackagePath;

    FString PackageName = CleanPath + TEXT("/") + NewAssetName;
    PackageName = PackageName.Replace(TEXT("//"), TEXT("/"));

    UAnimationSettings* AnimSettings = UAnimationSettings::Get();
    const FFrameRate PrevDefaultRate = AnimSettings ? AnimSettings->DefaultFrameRate : FFrameRate(30, 1);

    // Temporarily align DefaultFrameRate so the new AnimSequence data model is initialized compatibly
    if (AnimSettings)
    {
        AnimSettings->Modify();
        AnimSettings->DefaultFrameRate = DisplayRate;
        AnimSettings->SaveConfig();
    }

    struct FRestoreAnimSettingsRate
    {
        UAnimationSettings* Settings = nullptr;
        FFrameRate PrevRate;
        ~FRestoreAnimSettingsRate()
        {
            if (Settings)
            {
                Settings->Modify();
                Settings->DefaultFrameRate = PrevRate;
                Settings->SaveConfig();
            }
        }
    } RestoreRate{ AnimSettings, PrevDefaultRate };

    // Create via AssetTools + UAnimSequenceFactory (preferred in-editor path)
    UAnimSequence* NewAnim = nullptr;
    {
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        IAssetTools& AssetTools = AssetToolsModule.Get();

        UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
        Factory->TargetSkeleton = SkelComp->GetSkeletalMeshAsset()->GetSkeleton();

        // Folder must be /Game/... style. Assume caller passes that.
        NewAnim = Cast<UAnimSequence>(
            AssetTools.CreateAsset(*NewAssetName, *CleanPath, UAnimSequence::StaticClass(), Factory)
        );
    }

    if (!NewAnim)
    {
        Result.Error = TEXT("Failed to create AnimSequence asset.");
        return false;
    }

    // ---- Export options ----
    UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage());
    if (!ExportOptions)
    {
        Result.Error = TEXT("Failed to create UAnimSeqExportOption.");
        return false;
    }

    ExportOptions->bExportTransforms = true;
    ExportOptions->bExportMorphTargets = true;
    ExportOptions->bExportAttributeCurves = true;

    ExportOptions->bBakeTimecode = false;
    ExportOptions->bTimecodeRateOverride = false;

    // Force sampling rate to match sequence display rate (your documented fix intent)
    ExportOptions->bUseCustomFrameRate = true;
    ExportOptions->CustomFrameRate = DisplayRate;

    // ---- Params: force use of MovieScene playback range (UE 5.7+) ----
    FAnimExportSequenceParameters Params;
    Params.MovieSceneSequence = Sequence;
    Params.RootMovieSceneSequence = Sequence;

    Params.Player = EditorSequencer.Get(); // IMovieScenePlayer

#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
    Params.bForceUseOfMovieScenePlaybackRange = true;
#endif

    // Anchor evaluation at playback start like your old code (helps ensure first sample is correct)
    const FFrameNumber StartTick = MovieScene->GetPlaybackRange().GetLowerBoundValue();
    EditorSequencer->SetGlobalTime(StartTick);
    EditorSequencer->ForceEvaluate();

    SkelComp->TickComponent(0.f, LEVELTICK_All, nullptr);
    SkelComp->RefreshBoneTransforms();
    SkelComp->FinalizeBoneTransform();

    const bool bOk = MovieSceneToolHelpers::ExportToAnimSequence(NewAnim, ExportOptions, Params, SkelComp);
    if (!bOk)
    {
        Result.Error = TEXT("MovieSceneToolHelpers::ExportToAnimSequence failed.");
        return false;
    }

    NewAnim->MarkPackageDirty();
    Result.bSuccess = true;
    return true;
#endif // WITH_EDITOR
}

// Function body moved to USectionAbstraction
bool USequencerAbstractionBPLibrary::RemoveAnimSection(
    ULevelSequence* Sequence,
    UMovieSceneSkeletalAnimationSection* Section,
    FSequenceOpenResult& Result)
{
    return USectionAbstraction::RemoveAnimationSection(Sequence, Section, Result);
}

int32 USequencerAbstractionBPLibrary::RemoveAnimSectionsByAnimSequence(
    ULevelSequence* Sequence,
    UAnimSequenceBase* Animation,
    int32 MaxToRemove,
    FSequenceOpenResult& Result,
    const FGuid& BindingGuid)
{
    Result = {};
    if (!Sequence || !Animation)
    {
        Result.Error = TEXT("Sequence or Animation is null.");
        return 0;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return 0;
    }

    if (MaxToRemove <= 0)
    {
        MaxToRemove = TNumericLimits<int32>::Max();
    }

    int32 RemovedCount = 0;
    MovieScene->Modify();

    // Iterate bindings (or just the requested one)
    auto ProcessBinding = [&](const FGuid& BindingGuid)
        {
            UMovieSceneSkeletalAnimationTrack* Track =
                MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
            if (!Track) return;

            Track->Modify();

            TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
            for (UMovieSceneSection* S : Sections)
            {
                if (RemovedCount >= MaxToRemove) break;

                UMovieSceneSkeletalAnimationSection* AnimSection = Cast<UMovieSceneSkeletalAnimationSection>(S);
                if (!AnimSection) continue;

                if (AnimSection->Params.Animation == Animation)
                {
                    AnimSection->Modify();
                    const int32 Before = Track->GetAllSections().Num();
                    Track->RemoveSection(*AnimSection);
                    const int32 After = Track->GetAllSections().Num();
                    if (After < Before)
                    {
                        ++RemovedCount;
                    }
                }
            }

            if (RemovedCount > 0)
            {
                Track->MarkAsChanged();
            }
        };

    if (BindingGuid.IsValid())
    {
        ProcessBinding(BindingGuid);
    }
    else
    {
        for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            ProcessBinding(Binding.GetObjectGuid());
            if (RemovedCount >= MaxToRemove) break;
        }
    }

    if (RemovedCount == 0)
    {
        Result.Error = TEXT("No matching animation sections found.");
        return 0;
    }

    Sequence->MarkPackageDirty();
    Result.bSuccess = true;
    return RemovedCount;
}

static FFrameNumber DisplayFrameToTickFrame(const UMovieScene* MovieScene, int32 DisplayFrame, bool bRoundUp)
{
    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickRes = MovieScene->GetTickResolution();

    const FFrameTime DisplayTime(DisplayFrame);
    const FFrameTime TickTime = FFrameRate::TransformTime(DisplayTime, DisplayRate, TickRes);

    return bRoundUp ? TickTime.CeilToFrame() : TickTime.FloorToFrame();
}


bool USequencerAbstractionBPLibrary::MoveAnimationSectionStartTo(
    ULevelSequence* Sequence,
    UMovieSceneSection* Section,
    int32 NewStartFrame,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !Section)
    {
        Result.Error = TEXT("Sequence or Section is null.");
        return false;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return false;
    }

    TRange<FFrameNumber> Range = Section->GetRange();
    if (!Range.HasLowerBound() || !Range.HasUpperBound())
    {
        Result.Error = TEXT("Section range is not bounded.");
        return false;
    }

    const FFrameNumber OldStart = Range.GetLowerBoundValue();
    const FFrameNumber OldEndEx = Range.GetUpperBoundValue();
    const int32 DurationTicks = (OldEndEx - OldStart).Value;
    if (DurationTicks <= 0)
    {
        Result.Error = TEXT("Section has invalid duration.");
        return false;
    }

    const FFrameNumber NewStartTick = DisplayFrameToTickFrame(MovieScene, NewStartFrame, /*bRoundUp*/ false);
    const FFrameNumber NewEndTickEx = NewStartTick + DurationTicks;

    MovieScene->Modify();
    Section->Modify();
    
    // Notify parent track to recalculate overlapping sections and easing
    UMovieSceneTrack* ParentTrack = Section->GetTypedOuter<UMovieSceneTrack>();

    if (ParentTrack)
    {
        ParentTrack->Modify();
		ParentTrack->MarkAsChanged();
		ParentTrack->UpdateEasing();
    }

    Section->SetRange(TRange<FFrameNumber>(NewStartTick, NewEndTickEx));

    Sequence->MarkPackageDirty();
    Result.bSuccess = true;
    return true;
}

bool USequencerAbstractionBPLibrary::MoveAnimationSectionEndTo(
    ULevelSequence* Sequence,
    UMovieSceneSection* Section,
    int32 NewEndFrameInclusive,
    FSequenceOpenResult& Result)
{
    Result = {};

    if (!Sequence || !Section)
    {
        Result.Error = TEXT("Sequence or Section is null.");
        return false;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Result.Error = TEXT("Sequence has no MovieScene.");
        return false;
    }

    TRange<FFrameNumber> Range = Section->GetRange();
    if (!Range.HasLowerBound() || !Range.HasUpperBound())
    {
        Result.Error = TEXT("Section range is not bounded.");
        return false;
    }

    const FFrameNumber OldStart = Range.GetLowerBoundValue();
    const FFrameNumber OldEndEx = Range.GetUpperBoundValue();
    const int32 DurationTicks = (OldEndEx - OldStart).Value;

    if (DurationTicks <= 0)
    {
        Result.Error = TEXT("Section has invalid duration.");
        return false;
    }

    // Convert inclusive display end -> exclusive tick end
    const int32 NewEndDisplayEx = NewEndFrameInclusive + 1;
    FFrameNumber NewEndTickEx = DisplayFrameToTickFrame(MovieScene, NewEndDisplayEx, /*bRoundUp*/ true);

    // Preserve duration: NewStart = NewEndEx - Duration
    FFrameNumber NewStartTick = NewEndTickEx - DurationTicks;

    // Clamp to keep at least 1 tick long if caller gives too small end
    if (NewStartTick >= NewEndTickEx)
    {
        NewStartTick = NewEndTickEx - 1;
    }

    MovieScene->Modify();
    Section->Modify();

	// Notify parent track to recalculate overlapping sections and easing
    UMovieSceneTrack* ParentTrack = Section->GetTypedOuter<UMovieSceneTrack>();

    if (ParentTrack)
    {
        ParentTrack->Modify();
        ParentTrack->MarkAsChanged();
        ParentTrack->UpdateEasing();
    }

    Section->SetRange(TRange<FFrameNumber>(NewStartTick, NewEndTickEx));

    Sequence->MarkPackageDirty();
    Result.bSuccess = true;
    return true;

}

bool USequencerAbstractionBPLibrary::RemoveAllKeysForControlExceptFrame(
    UMovieSceneControlRigParameterTrack* Track,
    FName ControlName,
    int32 KeepFrame,
    FString& ErrorMessage)
{
#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only function.");
    return false;
#else
    ErrorMessage.Empty();

    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return false;
    }

    UMovieSceneSection* RawSection = Track->GetSectionToKey(ControlName);
    if (!RawSection)
    {
        ErrorMessage = FString::Printf(
            TEXT("Could not find section for control '%s'."),
            *ControlName.ToString());
        return false;
    }

    UMovieSceneControlRigParameterSection* Section =
        Cast<UMovieSceneControlRigParameterSection>(RawSection);
    if (!Section)
    {
        ErrorMessage = TEXT("Section is not a UMovieSceneControlRigParameterSection.");
        return false;
    }

    Section->Modify();
    Track->Modify();

    TArray<FBoolParameterNameAndCurve>& BoolParams = Section->GetBoolParameterNamesAndCurves();

    FBoolParameterNameAndCurve* MatchingParam = nullptr;
    for (FBoolParameterNameAndCurve& Param : BoolParams)
    {
        if (Param.ParameterName == ControlName)
        {
            MatchingParam = &Param;
            break;
        }
    }

    if (!MatchingParam)
    {
        ErrorMessage = FString::Printf(
            TEXT("No bool parameter found for control '%s'."),
            *ControlName.ToString());
        return false;
    }

    FMovieSceneBoolChannel& Channel = MatchingParam->ParameterCurve;

    TArray<FFrameNumber> KeyTimes;
    TArray<FKeyHandle> KeyHandles;
    Channel.GetKeys(TRange<FFrameNumber>::All(), &KeyTimes, &KeyHandles);

    const FFrameNumber KeepFrameNumber(KeepFrame);

    TArray<FKeyHandle> HandlesToDelete;
    HandlesToDelete.Reserve(KeyHandles.Num());

    for (int32 KeyIndex = 0; KeyIndex < KeyHandles.Num(); ++KeyIndex)
    {
        if (KeyTimes.IsValidIndex(KeyIndex) && KeyTimes[KeyIndex] != KeepFrameNumber)
        {
            HandlesToDelete.Add(KeyHandles[KeyIndex]);
        }
    }

    if (HandlesToDelete.Num() > 0)
    {
        Channel.DeleteKeys(HandlesToDelete);
    }

    // Helps editor state/undo update cleanly
    Section->MarkAsChanged();
    if (UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>())
    {
        MovieScene->Modify();
    }

    ErrorMessage = FString::Printf(
        TEXT("Deleted %d keys for '%s'."),
        HandlesToDelete.Num(),
        *ControlName.ToString());

    return true;
#endif
}

UMovieSceneTrack* USequencerAbstractionBPLibrary::GetTrackFromGuid(
    ULevelSequence* Sequence,
    FGuid BindingGuid,
    TSubclassOf<UMovieSceneTrack> TrackClass,
    FString& ErrorMessage)
{
#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return nullptr;
#else

    ErrorMessage.Empty();

    if (!Sequence)
    {
        ErrorMessage = TEXT("Sequence is null.");
        return nullptr;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();

    if (!MovieScene)
    {
        ErrorMessage = TEXT("MovieScene is null.");
        return nullptr;
    }

    const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);

    if (!Binding)
    {
        ErrorMessage = TEXT("Binding GUID not found.");
        return nullptr;
    }

    const TArray<UMovieSceneTrack*>& Tracks = Binding->GetTracks();

    for (UMovieSceneTrack* Track : Tracks)
    {
        if (!Track)
        {
            continue;
        }

        if (!TrackClass || Track->IsA(TrackClass))
        {
            return Track;
        }
    }

    ErrorMessage = TEXT("No matching track found.");
    return nullptr;

#endif
}

UMovieSceneControlRigParameterTrack* USequencerAbstractionBPLibrary::GetControlRigTrackFromGuid(
    ULevelSequence* Sequence,
    FGuid BindingGuid,
    TSubclassOf<UControlRig> ControlRigClass,
    FString& ErrorMessage)
{
#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return nullptr;
#else

    ErrorMessage.Empty();

    if (!Sequence)
    {
        ErrorMessage = TEXT("Sequence is null.");
        return nullptr;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        ErrorMessage = TEXT("MovieScene is null.");
        return nullptr;
    }

    const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
    if (!Binding)
    {
        ErrorMessage = TEXT("Binding GUID not found.");
        return nullptr;
    }

    UClass* TargetControlRigClass = ControlRigClass.Get();
    for (UMovieSceneTrack* Track : Binding->GetTracks())
    {
        UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track);
        if (!ControlRigTrack)
        {
            continue;
        }

        UControlRig* ControlRig = ControlRigTrack->GetControlRig();
        if (!TargetControlRigClass || (ControlRig && ControlRig->GetClass()->IsChildOf(TargetControlRigClass)))
        {
            return ControlRigTrack;
        }
    }

    if (TargetControlRigClass)
    {
        ErrorMessage = FString::Printf(
            TEXT("No Control Rig track found for class '%s'."),
            *TargetControlRigClass->GetName());
    }
    else
    {
        ErrorMessage = TEXT("No Control Rig track found.");
    }

    return nullptr;

#endif
}

int32 USequencerAbstractionBPLibrary::GetCurrentFrame(FString& ErrorMessage)
{
#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else

    ErrorMessage.Empty();

    ULevelSequence* Sequence = USequencerAbstractionBPLibrary::GetCurrentOpenedLevelSequence();

    if (!Sequence)
    {
        ErrorMessage = TEXT("No Level Sequence is currently opened.");
        return 0;
    }

    FFrameTime FrameTime = ULevelSequenceEditorBlueprintLibrary::GetCurrentTime();

    return FrameTime.FrameNumber.Value;

#endif
}

bool USequencerAbstractionBPLibrary::currentlyScrubbing()
{
#if !WITH_EDITOR
    return false;
#else
    if (ULevelSequenceEditorBlueprintLibrary::IsPlaying())
    {
        return true;
    }

    ULevelSequence* Sequence = USequencerAbstractionBPLibrary::GetCurrentOpenedLevelSequence();
    if (!Sequence || !GEditor)
    {
        return false;
    }

    UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!EditorSubsystem)
    {
        return false;
    }

    IAssetEditorInstance* EditorInstance = EditorSubsystem->FindEditorForAsset(Sequence, /*bFocusIfOpen*/ false);
    if (!EditorInstance)
    {
        return false;
    }

    ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(EditorInstance);
    if (!Toolkit)
    {
        return false;
    }

    TSharedPtr<ISequencer> Sequencer = Toolkit->GetSequencer();
    return Sequencer.IsValid() && Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Scrubbing;
#endif
}

bool USequencerAbstractionBPLibrary::sequencerTimeChanged()
{
#if !WITH_EDITOR
    return false;
#else
    // This function uses one shared cache for the whole Blueprint library.
    // With multiple independent callers, the first caller after a Sequencer time
    // change updates the cache and returns true; later callers see the new cached
    // time and return false. Use sequencerTimeChangedForState when each process,
    // widget, or timer needs to detect the same time change independently.
    ULevelSequence* Sequence = USequencerAbstractionBPLibrary::GetCurrentOpenedLevelSequence();
    if (!Sequence)
    {
        LastSequencerTimeSequence = nullptr;
        LastSequencerTime = FFrameTime();
        bHasLastSequencerTime = false;
        return false;
    }

    const FFrameTime CurrentTime = ULevelSequenceEditorBlueprintLibrary::GetCurrentTime();
    if (isSequencerTimeDifferent(
        LastSequencerTimeSequence.Get(),
        bHasLastSequencerTime,
        LastSequencerTime.FrameNumber.Value,
        LastSequencerTime.GetSubFrame(),
        Sequence,
        CurrentTime))
    {
        LastSequencerTimeSequence = Sequence;
        LastSequencerTime = CurrentTime;
        bHasLastSequencerTime = true;
        return true;
    }

    return false;
#endif
}

bool USequencerAbstractionBPLibrary::sequencerTimeChangedForState(FSequencerTimeChangeState& State)
{
#if !WITH_EDITOR
    return false;
#else
    ULevelSequence* Sequence = USequencerAbstractionBPLibrary::GetCurrentOpenedLevelSequence();
    if (!Sequence)
    {
        State.LastSequence = nullptr;
        State.LastFrameNumber = 0;
        State.LastSubFrame = 0.0f;
        State.bHasLastSequencerTime = false;
        return false;
    }

    const FFrameTime CurrentTime = ULevelSequenceEditorBlueprintLibrary::GetCurrentTime();
    if (isSequencerTimeDifferent(
        State.LastSequence.Get(),
        State.bHasLastSequencerTime,
        State.LastFrameNumber,
        State.LastSubFrame,
        Sequence,
        CurrentTime))
    {
        State.LastSequence = Sequence;
        State.LastFrameNumber = CurrentTime.FrameNumber.Value;
        State.LastSubFrame = CurrentTime.GetSubFrame();
        State.bHasLastSequencerTime = true;
        return true;
    }

    return false;
#endif
}

bool USequencerAbstractionBPLibrary::FocusLevelSequenceEditor(ULevelSequence* Sequence)
{
#if !WITH_EDITOR
    return false;
#else
    if (!Sequence || !GEditor)
    {
        return false;
    }

    UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!EditorSubsystem)
    {
        return false;
    }

    IAssetEditorInstance* EditorInstance = EditorSubsystem->FindEditorForAsset(Sequence, false);
    if (!EditorInstance)
    {
        EditorSubsystem->OpenEditorForAsset(Sequence);
        EditorInstance = EditorSubsystem->FindEditorForAsset(Sequence, false);
    }

    if (!EditorInstance)
    {
        return false;
    }

    EditorInstance->FocusWindow(Sequence);
    return true;
#endif
}

bool USequencerAbstractionBPLibrary::FocusLevelViewport()
{
#if !WITH_EDITOR
    return false;
#else
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    FLevelEditorModule& levelEditorModule =
        FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");

    TSharedPtr<IAssetViewport> activeViewport = levelEditorModule.GetFirstActiveViewport();
    if (!activeViewport.IsValid())
    {
        return false;
    }

    TSharedRef<SWidget> viewportWidget = activeViewport->AsWidget();

    return FSlateApplication::Get().SetKeyboardFocus(viewportWidget, EFocusCause::SetDirectly);
#endif
}

