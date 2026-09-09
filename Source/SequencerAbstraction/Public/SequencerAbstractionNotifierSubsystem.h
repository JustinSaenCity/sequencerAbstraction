#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "SequencerAbstractionNotifierSubsystem.generated.h"

class ISequencer;
class ULevelSequence;
enum class EMovieSceneDataChangeType;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSequencerAbstractionSequenceEvent, ULevelSequence*, Sequence);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSequencerAbstractionDataChangedEvent, ULevelSequence*, Sequence, const FString&, ChangeType, int32, ChangeTypeValue);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSequencerAbstractionTimeChangedEvent, ULevelSequence*, Sequence, int32, GlobalFrame);

UCLASS()
class SEQUENCERABSTRACTION_API USequencerAbstractionNotifierSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "SequencerAbstraction|Sequencer Notifier")
	FSequencerAbstractionSequenceEvent OnSequencerOpened;

	UPROPERTY(BlueprintAssignable, Category = "SequencerAbstraction|Sequencer Notifier")
	FSequencerAbstractionSequenceEvent OnSequencerClosed;

	UPROPERTY(BlueprintAssignable, Category = "SequencerAbstraction|Sequencer Notifier")
	FSequencerAbstractionDataChangedEvent OnSequencerMovieSceneDataChanged;

	UPROPERTY(BlueprintAssignable, Category = "SequencerAbstraction|Sequencer Notifier")
	FSequencerAbstractionTimeChangedEvent OnSequencerGlobalTimeChanged;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintPure, Category = "SequencerAbstraction|Sequencer Notifier")
	bool IsTrackingSequencer() const;

	UFUNCTION(BlueprintPure, Category = "SequencerAbstraction|Sequencer Notifier")
	ULevelSequence* GetTrackedLevelSequence() const;

private:
	void HandleSequencerCreated(TSharedRef<ISequencer> Sequencer);
	void HandleSequencerClosed(TSharedRef<ISequencer> Sequencer);
	void HandleMovieSceneDataChanged(EMovieSceneDataChangeType ChangeType);
	void HandleGlobalTimeChanged();

	void TrackSequencer(TSharedRef<ISequencer> Sequencer);
	void StopTrackingSequencer();

	static FString MovieSceneDataChangeTypeToString(EMovieSceneDataChangeType ChangeType);

	TWeakPtr<ISequencer> TrackedSequencer;
	FDelegateHandle SequencerCreatedHandle;
	FDelegateHandle SequencerClosedHandle;
	FDelegateHandle MovieSceneDataChangedHandle;
	FDelegateHandle GlobalTimeChangedHandle;
};
