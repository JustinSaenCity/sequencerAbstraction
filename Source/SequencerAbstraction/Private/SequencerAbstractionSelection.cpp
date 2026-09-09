#include "SequencerAbstractionBPLibrary.h"

#include "Editor.h"
#include "ILevelSequenceEditorToolkit.h"
#include "IKeyArea.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTimeUnit.h"
#include "MovieSceneTrack.h"
#include "MovieSceneScriptingFloat.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "ControlRig.h"
#include "ExtensionLibraries/MovieSceneSectionExtensions.h"
#include "MovieSceneScriptingChannel.h"
#include "MVVM/Extensions/ITrackAreaExtension.h"
#include "MVVM/Extensions/IOutlinerExtension.h"
#include "MVVM/Selection/Selection.h"
#include "MVVM/ViewModels/ChannelModel.h"
#include "MVVM/ViewModels/SequencerEditorViewModel.h"
#include "MVVM/ViewModels/ViewModel.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

static ULevelSequence* GetLevelSequenceFromTrack(UMovieSceneTrack* Track);

TArray<UMovieSceneScriptingChannel*> USequencerAbstractionBPLibrary::GetAllChannelsFromRigBindingSequenceTrack(UMovieSceneTrack* Track)
{
    TArray<UMovieSceneScriptingChannel*> AllChannels;
    if (!Track)
    {
        return AllChannels;
    }

    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    for (UMovieSceneSection* Section : Sections)
    {
        if (!Section)
        {
            continue;
        }

        AllChannels.Append(UMovieSceneSectionExtensions::GetAllChannels(Section));
    }

    return AllChannels;
}

bool USequencerAbstractionBPLibrary::EvaluateFloatChannelAtTime(
    UMovieSceneScriptingFloatChannel* Channel,
    FFrameTime Time,
    float& OutValue)
{
    OutValue = 0.0f;

    if (!Channel || !Channel->OwningSection.IsValid())
    {
        return false;
    }

    FMovieSceneChannelProxy& ChannelProxy = Channel->OwningSection->GetChannelProxy();
    TMovieSceneChannelHandle<FMovieSceneFloatChannel> FloatChannelHandle =
        ChannelProxy.GetChannelByName<FMovieSceneFloatChannel>(Channel->ChannelName);

    const FMovieSceneFloatChannel* FloatChannel = FloatChannelHandle.Get();
    return FloatChannel && FloatChannel->Evaluate(Time, OutValue);
}

static FString GetScriptingKeyValueString(UMovieSceneScriptingKey* Key)
{
    if (!Key)
    {
        return FString();
    }

    UFunction* GetValueFunction = Key->FindFunction(TEXT("GetValue"));
    if (!GetValueFunction)
    {
        return FString();
    }

    FProperty* ReturnProperty = nullptr;
    for (TFieldIterator<FProperty> It(GetValueFunction); It; ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            ReturnProperty = *It;
            break;
        }
    }

    if (!ReturnProperty)
    {
        return FString();
    }

    uint8* Params = static_cast<uint8*>(FMemory_Alloca(GetValueFunction->ParmsSize));
    FMemory::Memzero(Params, GetValueFunction->ParmsSize);

    for (TFieldIterator<FProperty> It(GetValueFunction); It; ++It)
    {
        It->InitializeValue_InContainer(Params);
    }

    Key->ProcessEvent(GetValueFunction, Params);

    FString ValueString;
    ReturnProperty->ExportTextItem_Direct(
        ValueString,
        ReturnProperty->ContainerPtrToValuePtr<void>(Params),
        nullptr,
        Key,
        PPF_None);

    for (TFieldIterator<FProperty> It(GetValueFunction); It; ++It)
    {
        It->DestroyValue_InContainer(Params);
    }

    return ValueString;
}

static bool DoesChannelMatchAnyRequestedName(
    const FName ChannelName,
    const TArray<FName>& Names,
    const bool bMatchContains,
    FName& OutRequestedName)
{
    OutRequestedName = NAME_None;

    if (Names.Num() == 0)
    {
        return true;
    }

    const FString ChannelString = ChannelName.ToString();
    for (const FName& Name : Names)
    {
        if (Name.IsNone())
        {
            continue;
        }

        const FString RequestedString = Name.ToString();
        const bool bMatches = bMatchContains
            ? ChannelString.Contains(RequestedString, ESearchCase::IgnoreCase)
            : ChannelString.Equals(RequestedString, ESearchCase::IgnoreCase);

        if (bMatches)
        {
            OutRequestedName = Name;
            return true;
        }
    }

    return false;
}

static TSharedPtr<ISequencer> GetOpenSequencerForSelection(ULevelSequence* Sequence)
{
#if WITH_EDITOR
    if (!Sequence || !GEditor)
    {
        return nullptr;
    }

    UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!EditorSubsystem)
    {
        return nullptr;
    }

    if (IAssetEditorInstance* Inst = EditorSubsystem->FindEditorForAsset(Sequence, false))
    {
        if (ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(Inst))
        {
            return Toolkit->GetSequencer();
        }
    }
#endif

    return nullptr;
}

static TSharedPtr<ISequencer> EnsureOpenSequencerForTrack(UMovieSceneTrack* Track, FString& ErrorMessage)
{
    ULevelSequence* Sequence = GetLevelSequenceFromTrack(Track);
    if (!Sequence)
    {
        ErrorMessage = TEXT("Could not resolve Level Sequence from track.");
        return nullptr;
    }

    if (USequencerAbstractionBPLibrary::GetCurrentOpenedLevelSequence() != Sequence)
    {
        ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence);
    }

    TSharedPtr<ISequencer> Sequencer = GetOpenSequencerForSelection(Sequence);
    if (!Sequencer.IsValid())
    {
        ErrorMessage = TEXT("Could not resolve open Sequencer for this track.");
    }

    return Sequencer;
}

static void GatherSequencerChannels(
    TSharedPtr<UE::Sequencer::FViewModel> DataModel,
    TSet<TSharedPtr<UE::Sequencer::FChannelModel>>& Channels)
{
    using namespace UE::Sequencer;

    if (!DataModel)
    {
        return;
    }

    constexpr bool bIncludeThis = true;
    for (const FViewModelPtr& Child : DataModel->GetDescendants(bIncludeThis))
    {
        if (TSharedPtr<ITrackAreaExtension> TrackArea = Child.ImplicitCast())
        {
            for (const FViewModelPtr& TrackAreaModel : TrackArea->GetTrackAreaModelList())
            {
                if (TSharedPtr<FChannelModel> Channel = TrackAreaModel.ImplicitCast())
                {
                    Channels.Add(Channel);
                }
            }
        }
        else if (TSharedPtr<FChannelModel> Channel = Child.ImplicitCast())
        {
            Channels.Add(Channel);
        }
    }
}

static void GatherChannelsForTrack(
    TSharedPtr<ISequencer> Sequencer,
    UMovieSceneTrack* Track,
    TArray<TSharedPtr<UE::Sequencer::FChannelModel>>& OutChannels)
{
    using namespace UE::Sequencer;

    OutChannels.Reset();
    if (!Sequencer.IsValid() || !Track || !Sequencer->GetViewModel().IsValid())
    {
        return;
    }

    TSet<TSharedPtr<FChannelModel>> UniqueChannels;
    GatherSequencerChannels(Sequencer->GetViewModel()->GetRootModel(), UniqueChannels);

    OutChannels.Reserve(UniqueChannels.Num());
    for (TSharedPtr<FChannelModel> Channel : UniqueChannels)
    {
        if (!Channel)
        {
            continue;
        }

        UMovieSceneSection* Section = Channel->GetSection();
        if (Section && Section->GetTypedOuter<UMovieSceneTrack>() == Track)
        {
            OutChannels.Add(Channel);
        }
    }
}

static TRange<FFrameNumber> MakeSingleDisplayFrameRange(UMovieScene* MovieScene, int32 DisplayFrame)
{
    if (!MovieScene)
    {
        return TRange<FFrameNumber>::Empty();
    }

    const FFrameTime TickTime = FFrameRate::TransformTime(
        FFrameTime(DisplayFrame),
        MovieScene->GetDisplayRate(),
        MovieScene->GetTickResolution());

    const FFrameNumber TickFrame = TickTime.FrameNumber;
    return TRange<FFrameNumber>(TickFrame, TickFrame);
}

static int32 SelectKeyHandlesForChannels(
    TSharedPtr<ISequencer> Sequencer,
    UMovieSceneTrack* Track,
    const TRange<FFrameNumber>& SelectionRange,
    TFunctionRef<bool(const TSharedPtr<UE::Sequencer::FChannelModel>&)> ShouldUseChannel,
    bool bClearExistingSelection,
    bool bThrobSelection)
{
    using namespace UE::Sequencer;

    if (!Sequencer.IsValid() || !Sequencer->GetViewModel().IsValid())
    {
        return 0;
    }

    TSharedPtr<FSequencerSelection> Selection = Sequencer->GetViewModel()->GetSelection();
    if (!Selection)
    {
        return 0;
    }

    FSelectionEventSuppressor EventSuppressor = Selection->SuppressEvents();

    if (bClearExistingSelection)
    {
        TUniqueFragmentSelectionSet<FKeyHandle, FChannelModel>& BaseKeySelection = Selection->KeySelection;
        BaseKeySelection.Empty();
        Selection->TrackArea.Empty();
    }

    TArray<TSharedPtr<FChannelModel>> Channels;
    GatherChannelsForTrack(Sequencer, Track, Channels);

    int32 NumSelected = 0;
    TArray<FKeyHandle> HandlesScratch;
    for (const TSharedPtr<FChannelModel>& Channel : Channels)
    {
        if (!Channel || !ShouldUseChannel(Channel))
        {
            continue;
        }

        TSharedPtr<IKeyArea> KeyArea = Channel->GetKeyArea();
        if (!KeyArea)
        {
            continue;
        }

        HandlesScratch.Reset();
        KeyArea->GetKeyHandles(HandlesScratch, SelectionRange);

        for (const FKeyHandle KeyHandle : HandlesScratch)
        {
            Selection->KeySelection.Select(Channel, KeyHandle);
            ++NumSelected;
        }
    }

    if (NumSelected > 0 && bThrobSelection)
    {
        Sequencer->ThrobKeySelection();
    }

    return NumSelected;
}

static int32 DeselectKeyHandlesForChannels(
    TSharedPtr<ISequencer> Sequencer,
    UMovieSceneTrack* Track,
    const TRange<FFrameNumber>& SelectionRange,
    TFunctionRef<bool(const TSharedPtr<UE::Sequencer::FChannelModel>&)> ShouldUseChannel)
{
    using namespace UE::Sequencer;

    if (!Sequencer.IsValid() || !Sequencer->GetViewModel().IsValid())
    {
        return 0;
    }

    TSharedPtr<FSequencerSelection> Selection = Sequencer->GetViewModel()->GetSelection();
    if (!Selection)
    {
        return 0;
    }

    FSelectionEventSuppressor EventSuppressor = Selection->SuppressEvents();

    TArray<TSharedPtr<FChannelModel>> Channels;
    GatherChannelsForTrack(Sequencer, Track, Channels);

    int32 NumDeselected = 0;
    TArray<FKeyHandle> HandlesScratch;
    for (const TSharedPtr<FChannelModel>& Channel : Channels)
    {
        if (!Channel || !ShouldUseChannel(Channel))
        {
            continue;
        }

        TSharedPtr<IKeyArea> KeyArea = Channel->GetKeyArea();
        if (!KeyArea)
        {
            continue;
        }

        HandlesScratch.Reset();
        KeyArea->GetKeyHandles(HandlesScratch, SelectionRange);

        for (const FKeyHandle KeyHandle : HandlesScratch)
        {
            if (Selection->KeySelection.IsSelected(KeyHandle))
            {
                TUniqueFragmentSelectionSet<FKeyHandle, FChannelModel>& BaseKeySelection = Selection->KeySelection;
                BaseKeySelection.Deselect(KeyHandle);
                ++NumDeselected;
            }
        }
    }

    return NumDeselected;
}

static ULevelSequence* GetLevelSequenceFromTrack(UMovieSceneTrack* Track)
{
    if (!Track)
    {
        return nullptr;
    }

    if (UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>())
    {
        return MovieScene->GetTypedOuter<ULevelSequence>();
    }

    return nullptr;
}

static TSet<FName> BuildRequestedBoneNameSet(const TArray<FName>& BoneNames)
{
    TSet<FName> BoneNameSet;
    BoneNameSet.Reserve(BoneNames.Num());

    for (const FName& BoneName : BoneNames)
    {
        if (!BoneName.IsNone())
        {
            BoneNameSet.Add(BoneName);
        }
    }

    return BoneNameSet;
}

static bool IsAnimatableNonCurveControl(UControlRig* ControlRig, const FRigControlElement* ControlElement)
{
    if (!ControlRig || !ControlElement)
    {
        return false;
    }

    const FRigControlSettings& Settings = ControlElement->Settings;
    if (Settings.bIsCurve || Settings.bIsTransientControl || ControlRig->IsCurveControl(ControlElement))
    {
        return false;
    }

    return Settings.AnimationType == ERigControlAnimationType::AnimationControl;
}

static UControlRig* ResolveControlRigFromSectionOrTrack(
    UMovieSceneControlRigParameterSection* Section,
    UMovieSceneTrack* Track)
{
    if (Section)
    {
        if (UControlRig* ControlRig = Section->GetControlRig())
        {
            return ControlRig;
        }
    }

    if (const UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
    {
        return ControlRigTrack->GetControlRig();
    }

    return nullptr;
}

static TSet<FName> BuildFkBoneControlNameSet(
    UControlRig* ControlRig,
    const TSet<FName>& RequestedBoneNames)
{
    TSet<FName> BoneControlNames;
    if (!ControlRig)
    {
        return BoneControlNames;
    }

    URigHierarchy* Hierarchy = ControlRig->GetHierarchy();
    if (!Hierarchy)
    {
        return BoneControlNames;
    }

    BoneControlNames.Reserve(Hierarchy->GetBonesFast().Num());

    if (UFKControlRig* FKControlRig = Cast<UFKControlRig>(ControlRig))
    {
        for (FRigBoneElement* BoneElement : Hierarchy->GetBones(true))
        {
            if (!BoneElement)
            {
                continue;
            }

            const FName BoneName = BoneElement->GetFName();
            const FName ControlName = UFKControlRig::GetControlName(BoneName, ERigElementType::Bone);
            if (ControlName.IsNone())
            {
                continue;
            }

            if (RequestedBoneNames.Num() > 0 &&
                !RequestedBoneNames.Contains(BoneName) &&
                !RequestedBoneNames.Contains(ControlName))
            {
                continue;
            }

            FRigControlElement* ControlElement = FKControlRig->FindControl(ControlName);
            if (IsAnimatableNonCurveControl(FKControlRig, ControlElement))
            {
                BoneControlNames.Add(ControlName);
            }
        }

        return BoneControlNames;
    }

    TSet<FName> HierarchyBoneNames;
    HierarchyBoneNames.Reserve(Hierarchy->GetBonesFast().Num());
    for (FRigBoneElement* BoneElement : Hierarchy->GetBones(true))
    {
        if (BoneElement)
        {
            HierarchyBoneNames.Add(BoneElement->GetFName());
        }
    }

    for (FRigControlElement* ControlElement : Hierarchy->GetControls(true))
    {
        if (!ControlElement || !IsAnimatableNonCurveControl(ControlRig, ControlElement))
        {
            continue;
        }

        const FName ControlName = ControlElement->GetFName();
        if (!HierarchyBoneNames.Contains(ControlName))
        {
            continue;
        }

        if (RequestedBoneNames.Num() > 0 && !RequestedBoneNames.Contains(ControlName))
        {
            continue;
        }

        BoneControlNames.Add(ControlName);
    }

    return BoneControlNames;
}

static int32 SelectBoneKeysInternal(
    UMovieSceneTrack* Track,
    const TArray<FName>& BoneNames,
    int32 StartDisplayFrame,
    int32 EndDisplayFrame,
    TOptional<int32> ExactDisplayFrame,
    bool bClearExistingSelection,
    bool bThrobSelection,
    FString& ErrorMessage)
{
#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return 0;
    }

    const TSet<FName> RequestedBoneNames = BuildRequestedBoneNameSet(BoneNames);

    UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
    if (!MovieScene)
    {
        ErrorMessage = TEXT("Could not resolve MovieScene from track.");
        return 0;
    }

    TSharedPtr<ISequencer> Sequencer = EnsureOpenSequencerForTrack(Track, ErrorMessage);
    if (!Sequencer.IsValid())
    {
        return 0;
    }

    TMap<UMovieSceneControlRigParameterSection*, TSet<FName>> BoneControlNamesBySection;
    for (UMovieSceneSection* Section : Track->GetAllSections())
    {
        UMovieSceneControlRigParameterSection* ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(Section);
        if (!ControlRigSection)
        {
            continue;
        }

        UControlRig* ControlRig = ResolveControlRigFromSectionOrTrack(ControlRigSection, Track);
        const TSet<FName> BoneControlNames = BuildFkBoneControlNameSet(ControlRig, RequestedBoneNames);
        if (BoneControlNames.Num() > 0)
        {
            BoneControlNamesBySection.Add(ControlRigSection, BoneControlNames);
        }
    }

    if (BoneControlNamesBySection.Num() == 0)
    {
        return 0;
    }

    const TRange<FFrameNumber> KeyRange = ExactDisplayFrame.IsSet()
        ? MakeSingleDisplayFrameRange(MovieScene, ExactDisplayFrame.GetValue())
        : MovieScene->GetSelectionRange();

    return SelectKeyHandlesForChannels(
        Sequencer,
        Track,
        KeyRange,
        [&BoneControlNamesBySection](const TSharedPtr<UE::Sequencer::FChannelModel>& Channel)
        {
            UMovieSceneControlRigParameterSection* ControlRigSection =
                Cast<UMovieSceneControlRigParameterSection>(Channel->GetSection());
            if (!ControlRigSection)
            {
                return false;
            }

            FMovieSceneChannel* MovieSceneChannel = Channel->GetChannel();
            if (!MovieSceneChannel)
            {
                return false;
            }

            const TSet<FName>* BoneControlNames = BoneControlNamesBySection.Find(ControlRigSection);
            if (!BoneControlNames)
            {
                return false;
            }

            const UE::MovieScene::FControlRigChannelMetaData ControlRigMetaData =
                ControlRigSection->GetChannelMetaData(MovieSceneChannel);
            return ControlRigMetaData && BoneControlNames->Contains(ControlRigMetaData.GetControlName());
        },
        bClearExistingSelection,
        bThrobSelection);
#endif
}

TArray<FSequencerKeyInSelectionRangeInfo> USequencerAbstractionBPLibrary::GetKeysInSelectionRangeForNames(
    UMovieSceneTrack* Track,
    const TArray<FName>& Names,
    bool bMatchContains,
    FString& ErrorMessage)
{
    TArray<FSequencerKeyInSelectionRangeInfo> OutKeys;
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return OutKeys;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return OutKeys;
    }

    const int32 RawStartFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeStart();
    const int32 RawEndFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeEnd();
    const int32 StartFrame = FMath::Min(RawStartFrame, RawEndFrame);
    const int32 EndFrame = FMath::Max(RawStartFrame, RawEndFrame);

    const TArray<UMovieSceneScriptingChannel*> Channels = GetAllChannelsFromRigBindingSequenceTrack(Track);
    for (UMovieSceneScriptingChannel* Channel : Channels)
    {
        if (!Channel)
        {
            continue;
        }

        FName RequestedName = NAME_None;
        if (!DoesChannelMatchAnyRequestedName(Channel->ChannelName, Names, bMatchContains, RequestedName))
        {
            continue;
        }

        const TArray<UMovieSceneScriptingKey*> Keys = Channel->GetKeys();
        for (UMovieSceneScriptingKey* Key : Keys)
        {
            if (!Key)
            {
                continue;
            }

            const FFrameTime KeyTime = Key->GetTime(EMovieSceneTimeUnit::DisplayRate);
            const int32 DisplayFrame = KeyTime.FrameNumber.Value;
            if (DisplayFrame < StartFrame || DisplayFrame > EndFrame)
            {
                continue;
            }

            FSequencerKeyInSelectionRangeInfo Info;
            Info.Key = Key;
            Info.RequestedName = RequestedName;
            Info.ChannelName = Channel->ChannelName;
            Info.DisplayFrame = DisplayFrame;
            Info.SubFrame = KeyTime.GetSubFrame();
            Info.ValueString = GetScriptingKeyValueString(Key);
            OutKeys.Add(Info);
        }
    }

    if (OutKeys.Num() == 0)
    {
        ErrorMessage = FString::Printf(
            TEXT("No keys found between selection frames %d and %d for the requested names."),
            StartFrame,
            EndFrame);
    }

    return OutKeys;
#endif
}

int32 USequencerAbstractionBPLibrary::SelectKeysInSelectionRangeForNames(
    UMovieSceneTrack* Track,
    const TArray<FName>& Names,
    bool bMatchContains,
    bool bClearExistingSelection,
    bool bThrobSelection,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return 0;
    }

    UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
    if (!MovieScene)
    {
        ErrorMessage = TEXT("Could not resolve MovieScene from track.");
        return 0;
    }

    TSharedPtr<ISequencer> Sequencer = EnsureOpenSequencerForTrack(Track, ErrorMessage);
    if (!Sequencer.IsValid())
    {
        return 0;
    }

    const TRange<FFrameNumber> SelectionRange = MovieScene->GetSelectionRange();
    const int32 NumSelected = SelectKeyHandlesForChannels(
        Sequencer,
        Track,
        SelectionRange,
        [&Names, bMatchContains](const TSharedPtr<UE::Sequencer::FChannelModel>& Channel)
        {
            if (!Channel)
            {
                return false;
            }

            FName RequestedName = NAME_None;
            return DoesChannelMatchAnyRequestedName(Channel->GetChannelName(), Names, bMatchContains, RequestedName);
        },
        bClearExistingSelection,
        bThrobSelection);

    if (NumSelected == 0)
    {
        const int32 RawStartFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeStart();
        const int32 RawEndFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeEnd();
        const int32 StartFrame = FMath::Min(RawStartFrame, RawEndFrame);
        const int32 EndFrame = FMath::Max(RawStartFrame, RawEndFrame);
        ErrorMessage = FString::Printf(
            TEXT("No keys selected between selection frames %d and %d for the requested names."),
            StartFrame,
            EndFrame);
        return 0;
    }

    return NumSelected;
#endif
}

int32 USequencerAbstractionBPLibrary::SelectKeysAtFrameForNames(
    UMovieSceneTrack* Track,
    int32 DisplayFrame,
    const TArray<FName>& Names,
    bool bMatchContains,
    bool bClearExistingSelection,
    bool bThrobSelection,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return 0;
    }

    UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
    if (!MovieScene)
    {
        ErrorMessage = TEXT("Could not resolve MovieScene from track.");
        return 0;
    }

    TSharedPtr<ISequencer> Sequencer = EnsureOpenSequencerForTrack(Track, ErrorMessage);
    if (!Sequencer.IsValid())
    {
        return 0;
    }

    const TRange<FFrameNumber> KeyRange = MakeSingleDisplayFrameRange(MovieScene, DisplayFrame);
    const int32 NumSelected = SelectKeyHandlesForChannels(
        Sequencer,
        Track,
        KeyRange,
        [&Names, bMatchContains](const TSharedPtr<UE::Sequencer::FChannelModel>& Channel)
        {
            if (!Channel)
            {
                return false;
            }

            FName RequestedName = NAME_None;
            return DoesChannelMatchAnyRequestedName(Channel->GetChannelName(), Names, bMatchContains, RequestedName);
        },
        bClearExistingSelection,
        bThrobSelection);

    if (NumSelected == 0)
    {
        ErrorMessage = FString::Printf(
            TEXT("No keys selected at display frame %d for the requested names."),
            DisplayFrame);
        return 0;
    }

    return NumSelected;
#endif
}

int32 USequencerAbstractionBPLibrary::SelectBoneKeysInSelectionRange(
    UMovieSceneTrack* Track,
    const TArray<FName>& BoneNames,
    bool bClearExistingSelection,
    bool bThrobSelection,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    const int32 RawStartFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeStart();
    const int32 RawEndFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeEnd();
    const int32 StartFrame = FMath::Min(RawStartFrame, RawEndFrame);
    const int32 EndFrame = FMath::Max(RawStartFrame, RawEndFrame);

    const int32 NumSelected = SelectBoneKeysInternal(
        Track,
        BoneNames,
        StartFrame,
        EndFrame,
        TOptional<int32>(),
        bClearExistingSelection,
        bThrobSelection,
        ErrorMessage);

    if (NumSelected == 0 && ErrorMessage.IsEmpty())
    {
        ErrorMessage = FString::Printf(
            TEXT("No bone keys selected between selection frames %d and %d."),
            StartFrame,
            EndFrame);
    }

    return NumSelected;
#endif
}

int32 USequencerAbstractionBPLibrary::SelectBoneKeysAtFrame(
    UMovieSceneTrack* Track,
    int32 DisplayFrame,
    const TArray<FName>& BoneNames,
    bool bClearExistingSelection,
    bool bThrobSelection,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    const int32 NumSelected = SelectBoneKeysInternal(
        Track,
        BoneNames,
        DisplayFrame,
        DisplayFrame,
        DisplayFrame,
        bClearExistingSelection,
        bThrobSelection,
        ErrorMessage);

    if (NumSelected == 0 && ErrorMessage.IsEmpty())
    {
        ErrorMessage = FString::Printf(
            TEXT("No bone keys selected at display frame %d."),
            DisplayFrame);
    }

    return NumSelected;
#endif
}

int32 USequencerAbstractionBPLibrary::RemoveKeysFromSelectionInSelectionRangeForNames(
    UMovieSceneTrack* Track,
    const TArray<FName>& Names,
    bool bMatchContains,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return 0;
    }

    UMovieScene* MovieScene = Track->GetTypedOuter<UMovieScene>();
    if (!MovieScene)
    {
        ErrorMessage = TEXT("Could not resolve MovieScene from track.");
        return 0;
    }

    TSharedPtr<ISequencer> Sequencer = EnsureOpenSequencerForTrack(Track, ErrorMessage);
    if (!Sequencer.IsValid())
    {
        return 0;
    }

    const TRange<FFrameNumber> SelectionRange = MovieScene->GetSelectionRange();
    const int32 NumRemoved = DeselectKeyHandlesForChannels(
        Sequencer,
        Track,
        SelectionRange,
        [&Names, bMatchContains](const TSharedPtr<UE::Sequencer::FChannelModel>& Channel)
        {
            if (!Channel)
            {
                return false;
            }

            FName RequestedName = NAME_None;
            return DoesChannelMatchAnyRequestedName(Channel->GetChannelName(), Names, bMatchContains, RequestedName);
        });

    if (NumRemoved == 0)
    {
        const int32 RawStartFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeStart();
        const int32 RawEndFrame = ULevelSequenceEditorBlueprintLibrary::GetSelectionRangeEnd();
        const int32 StartFrame = FMath::Min(RawStartFrame, RawEndFrame);
        const int32 EndFrame = FMath::Max(RawStartFrame, RawEndFrame);
        ErrorMessage = FString::Printf(
            TEXT("No matching keys were removed from selection between selection frames %d and %d."),
            StartFrame,
            EndFrame);
    }

    return NumRemoved;
#endif
}

TArray<FName> USequencerAbstractionBPLibrary::GetSelectedNamesInTrack(
    UMovieSceneTrack* Track,
    const TArray<FName>& Names,
    bool bMatchContains,
    FString& ErrorMessage)
{
    TArray<FName> OutNames;
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return OutNames;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return OutNames;
    }

    ULevelSequence* Sequence = GetLevelSequenceFromTrack(Track);
    if (Sequence && GetCurrentOpenedLevelSequence() != Sequence)
    {
        ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence);
    }

    TSet<FName> UniqueNames;
    const TArray<FSequencerChannelProxy> ChannelsWithSelectedKeys =
        ULevelSequenceEditorBlueprintLibrary::GetChannelsWithSelectedKeys();

    for (const FSequencerChannelProxy& ChannelProxy : ChannelsWithSelectedKeys)
    {
        UMovieSceneSection* Section = ChannelProxy.Section;
        if (!Section)
        {
            continue;
        }

        if (Section->GetTypedOuter<UMovieSceneTrack>() != Track)
        {
            continue;
        }

        FName RequestedName = NAME_None;
        if (DoesChannelMatchAnyRequestedName(ChannelProxy.ChannelName, Names, bMatchContains, RequestedName))
        {
            UniqueNames.Add(Names.Num() > 0 ? RequestedName : ChannelProxy.ChannelName);
        }
    }

    OutNames.Reserve(UniqueNames.Num());
    for (const FName& Name : UniqueNames)
    {
        OutNames.Add(Name);
    }
    OutNames.Sort([](const FName& A, const FName& B)
    {
        return A.LexicalLess(B);
    });

    if (OutNames.Num() == 0)
    {
        ErrorMessage = TEXT("No selected keys found on this track for the requested names.");
    }

    return OutNames;
#endif
}

int32 USequencerAbstractionBPLibrary::FocusSequencerRowsForNames(
    UMovieSceneTrack* Track,
    const TArray<FName>& Names,
    bool bMatchContains,
    bool bSelectParentInstead,
    bool bClearExistingRowSelection,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();
    (void)bClearExistingRowSelection;

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return 0;
    }

    TSharedPtr<ISequencer> Sequencer = EnsureOpenSequencerForTrack(Track, ErrorMessage);
    if (!Sequencer.IsValid())
    {
        return 0;
    }

    TArray<TSharedPtr<UE::Sequencer::FChannelModel>> Channels;
    GatherChannelsForTrack(Sequencer, Track, Channels);

    TMap<UMovieSceneSection*, TArray<FName>> ChannelNamesBySection;
    TSet<FName> UniqueMatchedChannelNames;
    for (const TSharedPtr<UE::Sequencer::FChannelModel>& Channel : Channels)
    {
        if (!Channel)
        {
            continue;
        }

        UMovieSceneSection* Section = Channel->GetSection();
        if (!Section)
        {
            continue;
        }

        FName RequestedName = NAME_None;
        bool bMatches = DoesChannelMatchAnyRequestedName(Channel->GetChannelName(), Names, bMatchContains, RequestedName);

        if (!bMatches)
        {
            if (UMovieSceneControlRigParameterSection* ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(Section))
            {
                if (FMovieSceneChannel* MovieSceneChannel = Channel->GetChannel())
                {
                    const UE::MovieScene::FControlRigChannelMetaData ControlRigMetaData =
                        ControlRigSection->GetChannelMetaData(MovieSceneChannel);
                    bMatches = ControlRigMetaData &&
                        DoesChannelMatchAnyRequestedName(ControlRigMetaData.GetControlName(), Names, bMatchContains, RequestedName);
                }
            }
        }

        if (!bMatches)
        {
            continue;
        }

        ChannelNamesBySection.FindOrAdd(Section).AddUnique(Channel->GetChannelName());
        UniqueMatchedChannelNames.Add(Channel->GetChannelName());
    }

    if (ChannelNamesBySection.Num() == 0)
    {
        ErrorMessage = TEXT("No Sequencer rows matched the requested names.");
        return 0;
    }

    TSharedPtr<UE::Sequencer::FSequencerSelection> Selection =
        Sequencer->GetViewModel().IsValid() ? Sequencer->GetViewModel()->GetSelection() : nullptr;

    TArray<TPair<FKeyHandle, UE::Sequencer::TViewModelPtr<UE::Sequencer::FChannelModel>>> PreviousKeySelection;
    TSet<UE::Sequencer::TWeakViewModelPtr<UE::Sequencer::IOutlinerExtension>> PreviousOutlinerSelection;
    if (Selection)
    {
        PreviousKeySelection.Reserve(Selection->KeySelection.Num());
        for (const FKeyHandle KeyHandle : Selection->KeySelection.GetSelected())
        {
            UE::Sequencer::TViewModelPtr<UE::Sequencer::FChannelModel> ChannelModel =
                Selection->KeySelection.GetModelForKey(KeyHandle);
            if (ChannelModel)
            {
                PreviousKeySelection.Emplace(KeyHandle, ChannelModel);
            }
        }

        PreviousOutlinerSelection = Selection->Outliner.GetSelected();
    }

    for (const TPair<UMovieSceneSection*, TArray<FName>>& SectionAndNames : ChannelNamesBySection)
    {
        Sequencer->SelectByChannels(
            SectionAndNames.Key,
            SectionAndNames.Value,
            bSelectParentInstead,
            true);
    }

    if (Selection)
    {
        UE::Sequencer::FSelectionEventSuppressor EventSuppressor = Selection->SuppressEvents();

        Selection->Outliner.Empty();
        Selection->Outliner.SelectRange(PreviousOutlinerSelection);

        UE::Sequencer::TUniqueFragmentSelectionSet<FKeyHandle, UE::Sequencer::FChannelModel>& BaseKeySelection = Selection->KeySelection;
        BaseKeySelection.Empty();
        for (const TPair<FKeyHandle, UE::Sequencer::TViewModelPtr<UE::Sequencer::FChannelModel>>& KeyAndChannel : PreviousKeySelection)
        {
            if (KeyAndChannel.Value)
            {
                BaseKeySelection.Select(KeyAndChannel.Value, KeyAndChannel.Key);
            }
        }
    }

    return UniqueMatchedChannelNames.Num();
#endif
}

int32 USequencerAbstractionBPLibrary::FocusSequencerSectionsForNames(
    UMovieSceneTrack* Track,
    const TArray<FName>& Names,
    bool bMatchContains,
    bool bClearExistingSelection,
    bool bThrobSelection,
    bool bFrameMatchedSections,
    double ViewPaddingSeconds,
    FString& ErrorMessage)
{
    ErrorMessage.Empty();

#if !WITH_EDITOR
    ErrorMessage = TEXT("Editor only.");
    return 0;
#else
    if (!Track)
    {
        ErrorMessage = TEXT("Track is null.");
        return 0;
    }

    TSharedPtr<ISequencer> Sequencer = EnsureOpenSequencerForTrack(Track, ErrorMessage);
    if (!Sequencer.IsValid())
    {
        return 0;
    }

    TArray<TSharedPtr<UE::Sequencer::FChannelModel>> Channels;
    GatherChannelsForTrack(Sequencer, Track, Channels);

    TSet<UMovieSceneSection*> MatchedSections;
    TSet<FName> UniqueMatchedChannelNames;
    for (const TSharedPtr<UE::Sequencer::FChannelModel>& Channel : Channels)
    {
        if (!Channel)
        {
            continue;
        }

        UMovieSceneSection* Section = Channel->GetSection();
        if (!Section)
        {
            continue;
        }

        FName RequestedName = NAME_None;
        bool bMatches = DoesChannelMatchAnyRequestedName(Channel->GetChannelName(), Names, bMatchContains, RequestedName);

        if (!bMatches)
        {
            if (UMovieSceneControlRigParameterSection* ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(Section))
            {
                if (FMovieSceneChannel* MovieSceneChannel = Channel->GetChannel())
                {
                    const UE::MovieScene::FControlRigChannelMetaData ControlRigMetaData =
                        ControlRigSection->GetChannelMetaData(MovieSceneChannel);
                    bMatches = ControlRigMetaData &&
                        DoesChannelMatchAnyRequestedName(ControlRigMetaData.GetControlName(), Names, bMatchContains, RequestedName);
                }
            }
        }

        if (!bMatches)
        {
            continue;
        }

        MatchedSections.Add(Section);
        UniqueMatchedChannelNames.Add(Channel->GetChannelName());
    }

    if (MatchedSections.Num() == 0)
    {
        ErrorMessage = TEXT("No Sequencer sections matched the requested names.");
        return 0;
    }

    if (bClearExistingSelection)
    {
        TSharedPtr<UE::Sequencer::FSequencerSelection> Selection =
            Sequencer->GetViewModel().IsValid() ? Sequencer->GetViewModel()->GetSelection() : nullptr;
        if (Selection)
        {
            Selection->TrackArea.Empty();
            Selection->Outliner.Empty();
            UE::Sequencer::TUniqueFragmentSelectionSet<FKeyHandle, UE::Sequencer::FChannelModel>& BaseKeySelection = Selection->KeySelection;
            BaseKeySelection.Empty();
        }
    }

    bool bHasSectionBounds = false;
    FFrameNumber MinFrame(TNumericLimits<int32>::Max());
    FFrameNumber MaxFrame(TNumericLimits<int32>::Lowest());

    for (UMovieSceneSection* Section : MatchedSections)
    {
        if (!Section)
        {
            continue;
        }

        Sequencer->SelectSection(Section);

        const TRange<FFrameNumber> SectionRange = Section->GetRange();
        if (SectionRange.HasLowerBound() && SectionRange.HasUpperBound())
        {
            MinFrame = FMath::Min(MinFrame, SectionRange.GetLowerBoundValue());
            MaxFrame = FMath::Max(MaxFrame, SectionRange.GetUpperBoundValue());
            bHasSectionBounds = true;
        }
    }

    if (bThrobSelection)
    {
        Sequencer->ThrobSectionSelection();
    }

    if (bFrameMatchedSections && bHasSectionBounds && MinFrame < MaxFrame)
    {
        const FFrameRate TickResolution = Sequencer->GetFocusedTickResolution();
        const double StartSeconds = TickResolution.AsSeconds(MinFrame);
        const double EndSeconds = TickResolution.AsSeconds(MaxFrame);
        const double PaddingSeconds = FMath::Max(0.0, ViewPaddingSeconds);
        Sequencer->SetViewRange(
            TRange<double>(
                StartSeconds - PaddingSeconds,
                EndSeconds + PaddingSeconds),
            EViewRangeInterpolation::Animated);
    }

    return UniqueMatchedChannelNames.Num();
#endif
}
