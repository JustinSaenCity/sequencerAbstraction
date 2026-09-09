#include "SequencerAbstractionNotifierSubsystem.h"

#include "ISequencer.h"
#include "ISequencerModule.h"
#include "LevelSequence.h"
#include "Modules/ModuleManager.h"

void USequencerAbstractionNotifierSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (ISequencerModule* SequencerModule = FModuleManager::LoadModulePtr<ISequencerModule>("Sequencer"))
	{
		SequencerCreatedHandle = SequencerModule->RegisterOnSequencerCreated(
			FOnSequencerCreated::FDelegate::CreateUObject(this, &USequencerAbstractionNotifierSubsystem::HandleSequencerCreated));
	}
}

void USequencerAbstractionNotifierSubsystem::Deinitialize()
{
	StopTrackingSequencer();

	if (SequencerCreatedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded("Sequencer"))
	{
		ISequencerModule& SequencerModule = FModuleManager::GetModuleChecked<ISequencerModule>("Sequencer");
		SequencerModule.UnregisterOnSequencerCreated(SequencerCreatedHandle);
		SequencerCreatedHandle.Reset();
	}

	Super::Deinitialize();
}

bool USequencerAbstractionNotifierSubsystem::IsTrackingSequencer() const
{
	return TrackedSequencer.IsValid();
}

ULevelSequence* USequencerAbstractionNotifierSubsystem::GetTrackedLevelSequence() const
{
	TSharedPtr<ISequencer> Sequencer = TrackedSequencer.Pin();
	if (!Sequencer)
	{
		return nullptr;
	}

	return Cast<ULevelSequence>(Sequencer->GetRootMovieSceneSequence());
}

void USequencerAbstractionNotifierSubsystem::HandleSequencerCreated(TSharedRef<ISequencer> Sequencer)
{
	TrackSequencer(Sequencer);
	OnSequencerOpened.Broadcast(GetTrackedLevelSequence());
}

void USequencerAbstractionNotifierSubsystem::HandleSequencerClosed(TSharedRef<ISequencer> Sequencer)
{
	ULevelSequence* ClosingSequence = Cast<ULevelSequence>(Sequencer->GetRootMovieSceneSequence());
	StopTrackingSequencer();
	OnSequencerClosed.Broadcast(ClosingSequence);
}

void USequencerAbstractionNotifierSubsystem::HandleMovieSceneDataChanged(EMovieSceneDataChangeType ChangeType)
{
	OnSequencerMovieSceneDataChanged.Broadcast(
		GetTrackedLevelSequence(),
		MovieSceneDataChangeTypeToString(ChangeType),
		static_cast<int32>(ChangeType));
}

void USequencerAbstractionNotifierSubsystem::HandleGlobalTimeChanged()
{
	TSharedPtr<ISequencer> Sequencer = TrackedSequencer.Pin();
	if (!Sequencer)
	{
		return;
	}

	OnSequencerGlobalTimeChanged.Broadcast(
		GetTrackedLevelSequence(),
		Sequencer->GetGlobalTime().Time.FloorToFrame().Value);
}

void USequencerAbstractionNotifierSubsystem::TrackSequencer(TSharedRef<ISequencer> Sequencer)
{
	StopTrackingSequencer();

	TrackedSequencer = Sequencer;
	SequencerClosedHandle = Sequencer->OnCloseEvent().AddUObject(this, &USequencerAbstractionNotifierSubsystem::HandleSequencerClosed);
	MovieSceneDataChangedHandle = Sequencer->OnMovieSceneDataChanged().AddUObject(this, &USequencerAbstractionNotifierSubsystem::HandleMovieSceneDataChanged);
	GlobalTimeChangedHandle = Sequencer->OnGlobalTimeChanged().AddUObject(this, &USequencerAbstractionNotifierSubsystem::HandleGlobalTimeChanged);
}

void USequencerAbstractionNotifierSubsystem::StopTrackingSequencer()
{
	TSharedPtr<ISequencer> Sequencer = TrackedSequencer.Pin();
	if (Sequencer)
	{
		if (SequencerClosedHandle.IsValid())
		{
			Sequencer->OnCloseEvent().Remove(SequencerClosedHandle);
		}

		if (MovieSceneDataChangedHandle.IsValid())
		{
			Sequencer->OnMovieSceneDataChanged().Remove(MovieSceneDataChangedHandle);
		}

		if (GlobalTimeChangedHandle.IsValid())
		{
			Sequencer->OnGlobalTimeChanged().Remove(GlobalTimeChangedHandle);
		}
	}

	TrackedSequencer.Reset();
	SequencerClosedHandle.Reset();
	MovieSceneDataChangedHandle.Reset();
	GlobalTimeChangedHandle.Reset();
}

FString USequencerAbstractionNotifierSubsystem::MovieSceneDataChangeTypeToString(EMovieSceneDataChangeType ChangeType)
{
	switch (ChangeType)
	{
		case EMovieSceneDataChangeType::TrackValueChanged:
			return TEXT("TrackValueChanged");
		case EMovieSceneDataChangeType::TrackValueChangedRefreshImmediately:
			return TEXT("TrackValueChangedRefreshImmediately");
		case EMovieSceneDataChangeType::MovieSceneStructureItemAdded:
			return TEXT("MovieSceneStructureItemAdded");
		case EMovieSceneDataChangeType::MovieSceneStructureItemRemoved:
			return TEXT("MovieSceneStructureItemRemoved");
		case EMovieSceneDataChangeType::MovieSceneStructureItemsChanged:
			return TEXT("MovieSceneStructureItemsChanged");
		case EMovieSceneDataChangeType::ActiveMovieSceneChanged:
			return TEXT("ActiveMovieSceneChanged");
		case EMovieSceneDataChangeType::RefreshAllImmediately:
			return TEXT("RefreshAllImmediately");
		case EMovieSceneDataChangeType::Unknown:
			return TEXT("Unknown");
		case EMovieSceneDataChangeType::RefreshTree:
			return TEXT("RefreshTree");
		default:
			return TEXT("Unhandled");
	}
}
