#include "SSBeamRailFixModule.h"

#include "AbstractInstanceManager.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Hologram/FGHologram.h"
#include "InstanceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Patching/NativeHookManager.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogSSBeamRailFix, Log, All);

/*
 * SS Beam Rail Fix: makes Structural Solutions' "Train Rail I-Beam" render correctly.
 *
 * SS's rail material MI_Tack_SS needs a variant of the game's MM_FactoryBaked_Spline that is not cooked,
 * so the game falls back to its default material: grey, straight, unpaintable pieces. This mod:
 * - swaps MI_Tack_SS on SS rail pieces for its own material M_SSRail (shipped in this mod, built by
 *   make_M_SSRail.py). It bends each piece on the GPU from the per-instance spline data the game already
 *   writes (floats 15-26), paints the I-beam from the paint data (0-5) and applies finishes (9 metallic,
 *   10 roughness). SS's textures are used at runtime; none of SS's files are shipped.
 * - draws the game's own track mesh and material on top of each piece, so the steel rail looks exactly
 *   like vanilla track. M_SSRail clips its own rail region away. These copies follow the SS piece's
 *   visibility and spline data, but not its paint, and are removed when the piece is dismantled.
 * - switches MI_Tack_SS on the SS rail hologram to the game's track material.
 * - gives the build gun's dismantle outline (instanced meshes on the player) the bending material.
 * Console: ssrail.Finish, ssrail.Roughness, ssrail.PaintDetail, ssrail.BendHighlight and a few tuning values.
 */
namespace
{
	const TCHAR* BrokenMaterialPath = TEXT("/SS_Mod/Materials/MI_Tack_SS.MI_Tack_SS");
	const TCHAR* RailMeshPath = TEXT("/SS_Mod/Mesh/Steel/SM_TrainRail_New.SM_TrainRail_New");
	const TCHAR* OurMaterialPath = TEXT("/SSBeamRailFix/Materials/M_SSRail.M_SSRail");
	const TCHAR* VanillaMeshPath = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Track/Mesh/SM_TrainTrack_02.SM_TrainTrack_02");
	const TCHAR* VanillaMaterialPath = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Track/Material/MI_Tack_01.MI_Tack_01");

	TWeakObjectPtr<UMaterialInterface> Broken;
	TWeakObjectPtr<UMaterialInstanceDynamic> Replacement;
	TWeakObjectPtr<UMaterialInstanceDynamic> HighlightReplacement;
	bool bTriedBuild = false;
	TMap<TWeakObjectPtr<UClass>, bool> IsSSClass;

	// Vanilla track copies drawn on top of each SS rail piece, per SS rail actor.
	struct FOverlay
	{
		TWeakObjectPtr<AAbstractInstanceManager> Manager;
		FInstanceOwnerHandlePtr Handle;
	};
	TMap<TWeakObjectPtr<AActor>, TArray<FOverlay>> Overlays;
	bool bAddingOverlay = false;

	// SS piece handle -> its vanilla copy, so unhide and data writes can be passed on.
	TMap<const FInstanceOwnershipHandle*, FInstanceOwnerHandlePtr> Mirror;
	bool bMirroring = false;

	// Per-instance floats 0-14 are paint and finish. The vanilla copy keeps its own (zero) values for these.
	constexpr int32 PaintSlots = 15;

	TWeakObjectPtr<UWorld> TimerWorld;
	bool bWarnedNoSplineData = false;

	void ApplyConsoleValues();

	TAutoConsoleVariable<float> CVarRailZ(TEXT("ssrail.RailZ"), 0.f,
		TEXT("Local height where the steel rail starts. Mesh spans -400..135."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarRailBright(TEXT("ssrail.RailBright"), 0.45f,
		TEXT("Brightness of our own steel rail (only seen with ClipRail 0 and in the dismantle outline), 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarRailSat(TEXT("ssrail.RailSat"), 0.3f,
		TEXT("Colour saturation of our own steel rail, 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarClipRail(TEXT("ssrail.ClipRail"), 1.f,
		TEXT("1: hide our rail region and show the game's own track on top, 0: show ours."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarBendHighlight(TEXT("ssrail.BendHighlight"), 1.f,
		TEXT("1: bend the dismantle outline of SS rail pieces, 0: leave SS's straight outline."));
	TAutoConsoleVariable<float> CVarDetail(TEXT("ssrail.PaintDetail"), 0.25f,
		TEXT("How much texture detail shows through the paint, 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarRoughness(TEXT("ssrail.Roughness"), 0.6f,
		TEXT("Base roughness of the I-beam; the paint finish's roughness multiplies it."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarFinish(TEXT("ssrail.Finish"), 1.f,
		TEXT("1: apply paint finishes (metallic/roughness) to the I-beam, 0: ignore them."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarPaintMode(TEXT("ssrail.PaintMode"), 6.f,
		TEXT("Paint area: 6 I-beam body (default), 0 whole mesh, 1-4 AOMasks R/G/B/A, 5 coloured parts of the albedo, 7 debug split view."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarPaintGain(TEXT("ssrail.PaintGain"), 2.f,
		TEXT("Paint brightness."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));

	void ApplyConsoleValues()
	{
		UMaterialInstanceDynamic* MIDs[] = { Replacement.Get(), HighlightReplacement.Get() };
		for (int32 k = 0; k < 2; ++k)
		{
			UMaterialInstanceDynamic* MID = MIDs[k];
			if (!MID)
			{
				continue;
			}
			MID->SetScalarParameterValue(TEXT("PaintMode"), CVarPaintMode.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("PaintGain"), CVarPaintGain.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("RailZ"), CVarRailZ.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("PaintDetail"), CVarDetail.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("RailBright"), CVarRailBright.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("RailSat"), CVarRailSat.GetValueOnGameThread());
			// The outline copy draws the whole piece (rail too), so the outline covers all of it.
			MID->SetScalarParameterValue(TEXT("ClipRail"), k == 0 ? CVarClipRail.GetValueOnGameThread() : 0.f);
			MID->SetScalarParameterValue(TEXT("Roughness"), CVarRoughness.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("FinishAmt"), CVarFinish.GetValueOnGameThread());
		}
	}

	bool IsFromSS(UClass* Class)
	{
		if (const bool* Cached = IsSSClass.Find(Class))
		{
			return *Cached;
		}
		const bool bSS = Class->GetPathName().StartsWith(TEXT("/SS_Mod/"));
		IsSSClass.Add(Class, bSS);
		return bSS;
	}

	UMaterialInstanceDynamic* MakeMID(UMaterial* Base, UMaterialInstance* Source, const TCHAR* Name)
	{
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, GetTransientPackage(), Name);
		for (const FTextureParameterValue& P : Source->TextureParameterValues)
		{
			MID->SetTextureParameterValue(P.ParameterInfo.Name, P.ParameterValue);
		}
		MID->AddToRoot();
		return MID;
	}

	UMaterialInstanceDynamic* GetReplacement()
	{
		if (Replacement.IsValid() || bTriedBuild)
		{
			return Replacement.Get();
		}
		bTriedBuild = true;
		UMaterialInstance* SSMaterial = LoadObject<UMaterialInstance>(nullptr, BrokenMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterial* Ours = LoadObject<UMaterial>(nullptr, OurMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!SSMaterial || !Ours)
		{
			UE_LOG(LogSSBeamRailFix, Warning, TEXT("Not active: SS material %s, our material %s. SS rail left as is."),
				*GetPathNameSafe(SSMaterial), *GetPathNameSafe(Ours));
			return nullptr;
		}
		Replacement = MakeMID(Ours, SSMaterial, TEXT("MID_SSBeamRailFix"));
		HighlightReplacement = MakeMID(Ours, SSMaterial, TEXT("MID_SSBeamRailFix_Highlight"));
		Broken = SSMaterial;
		ApplyConsoleValues();
		UE_LOG(LogSSBeamRailFix, Display, TEXT("SS rail material replaced with %s."), *Ours->GetPathName());
		return Replacement.Get();
	}

	/** Give the build gun's dismantle-outline meshes on the player (SM_TrainRail_New with MI_Tack_SS) the bending material. */
	void FixHighlight()
	{
		static UStaticMesh* RailMesh = LoadObject<UStaticMesh>(nullptr, RailMeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		UWorld* World = TimerWorld.Get();
		UMaterialInterface* BrokenMaterial = Broken.Get();
		UMaterialInstanceDynamic* HMID = HighlightReplacement.Get();
		if (!RailMesh || !World || !BrokenMaterial || !HMID || CVarBendHighlight.GetValueOnGameThread() < 0.5f)
		{
			return;
		}
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			return;
		}
		TInlineComponentArray<UInstancedStaticMeshComponent*> Comps(Pawn);
		for (UInstancedStaticMeshComponent* C : Comps)
		{
			if (C->GetStaticMesh() != RailMesh)
			{
				continue;
			}
			if (C->NumCustomDataFloats < 27)
			{
				if (!bWarnedNoSplineData)
				{
					bWarnedNoSplineData = true;
					UE_LOG(LogSSBeamRailFix, Warning, TEXT("Dismantle outline %s has %d custom floats (needs 27); left as is."), *C->GetName(), C->NumCustomDataFloats);
				}
				continue;
			}
			for (int32 Slot = 0; Slot < C->GetNumMaterials(); ++Slot)
			{
				if (C->GetMaterial(Slot) == BrokenMaterial)
				{
					C->SetMaterial(Slot, HMID);
				}
			}
		}
	}

	/** One fast timer per game world (restarted when a save is loaded). */
	void EnsureTimer(UWorld* World)
	{
		if (!World || TimerWorld.Get() == World)
		{
			return;
		}
		TimerWorld = World;
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]() { FixHighlight(); }), 0.05f, true);
	}

	/** Switch any MI_Tack_SS on an SS rail hologram to the game's track material. Spline meshes are added while dragging, so this runs repeatedly. */
	void FixHologram(AFGHologram* Holo)
	{
		static UMaterialInterface* Vanilla = LoadObject<UMaterialInterface>(nullptr, VanillaMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterialInterface* BrokenMaterial = Broken.Get();
		if (!BrokenMaterial)
		{
			BrokenMaterial = LoadObject<UMaterialInterface>(nullptr, BrokenMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Vanilla || !BrokenMaterial)
		{
			return;
		}
		TInlineComponentArray<UMeshComponent*> Meshes(Holo);
		for (UMeshComponent* M : Meshes)
		{
			for (int32 Slot = 0; Slot < M->GetNumMaterials(); ++Slot)
			{
				if (M->GetMaterial(Slot) == BrokenMaterial)
				{
					M->SetMaterial(Slot, Vanilla);
				}
			}
		}
	}
}

void FSSBeamRailFixModule::StartupModule()
{
#if !WITH_EDITOR
	UE_LOG(LogSSBeamRailFix, Display, TEXT("SSBeamRailFix 1.0.0 hooks registered."));

	// Swap SS's broken rail material for ours before the piece's instance is created.
	SUBSCRIBE_METHOD(AAbstractInstanceManager::SetInstanced,
		[](auto& Scope, AAbstractInstanceManager*, AActor* Owner, const FTransform&, const FInstanceData& Instance,
			FInstanceOwnerHandlePtr&, bool)
		{
			if (bAddingOverlay || !Owner || !Instance.StaticMesh || !IsFromSS(Owner->GetClass()))
			{
				return;
			}
			EnsureTimer(Owner->GetWorld());
			UMaterialInstanceDynamic* MID = GetReplacement();
			UMaterialInterface* BrokenMaterial = Broken.Get();
			if (!MID || !BrokenMaterial)
			{
				return;
			}
			FInstanceData& Data = const_cast<FInstanceData&>(Instance);
			const int32 Slots = Data.StaticMesh->GetStaticMaterials().Num();
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				UMaterialInterface* Current = Data.OverridenMaterials.IsValidIndex(Slot) ? Data.OverridenMaterials[Slot].Get() : nullptr;
				if (!Current)
				{
					Current = Data.StaticMesh->GetMaterial(Slot);
				}
				if (Current == BrokenMaterial)
				{
					if (Data.OverridenMaterials.Num() < Slots)
					{
						Data.OverridenMaterials.SetNum(Slots);
					}
					Data.OverridenMaterials[Slot] = MID;
				}
			}
		});

	// Add the game's own track on top of each SS rail piece.
	SUBSCRIBE_METHOD_AFTER(AAbstractInstanceManager::SetInstanced,
		[](AAbstractInstanceManager* Manager, AActor* Owner, const FTransform& ActorTransform, const FInstanceData& Instance,
			FInstanceOwnerHandlePtr& Handle, bool bInitializeHidden)
		{
			if (bAddingOverlay || !Manager || !Owner || Owner->GetClass()->GetName() != TEXT("Build_SteelBeam_TrainRail_C") || !Handle.IsValid())
			{
				return;
			}
			static UStaticMesh* VanillaMesh = LoadObject<UStaticMesh>(nullptr, VanillaMeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!VanillaMesh)
			{
				static bool bWarned = false;
				if (!bWarned)
				{
					bWarned = true;
					UE_LOG(LogSSBeamRailFix, Warning, TEXT("Vanilla track mesh %s not found; steel rail not drawn on top."), VanillaMeshPath);
				}
				return;
			}
			FInstanceData Copy = Instance;
			Copy.StaticMesh = VanillaMesh;
			Copy.OverridenMaterials.Reset();
			for (int32 i = 0; i < PaintSlots && i < Copy.DefaultPerInstanceCustomData.Num(); ++i)
			{
				Copy.DefaultPerInstanceCustomData[i] = 0.f;
			}
			FOverlay& O = Overlays.FindOrAdd(Owner).AddDefaulted_GetRef();
			O.Manager = Manager;
			TGuardValue<bool> Guard(bAddingOverlay, true);
			Manager->SetInstanced(Owner, ActorTransform, Copy, O.Handle, bInitializeHidden);
			if (O.Handle.IsValid())
			{
				Mirror.Add(Handle.Get(), O.Handle);
			}
		});

	// Pass the game's data writes (spline, build state) to the vanilla copy, but not paint/finish (0-14).
	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByArray,
		[](const FInstanceOwnershipHandle* Self, const TArray<float>& Data, bool bMarkDirty)
		{
			if (bMirroring)
			{
				return;
			}
			if (FInstanceOwnerHandlePtr* M = Mirror.Find(Self))
			{
				if (M->IsValid())
				{
					TGuardValue<bool> Guard(bMirroring, true);
					TArray<float> Rail = Data;
					for (int32 i = 0; i < PaintSlots && i < Rail.Num(); ++i)
					{
						Rail[i] = 0.f;
					}
					(*M)->SetPrimitiveDataByArray(Rail, bMarkDirty);
				}
			}
		});

	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByID,
		[](const FInstanceOwnershipHandle* Self, float Value, const int32 Index, bool bMarkDirty)
		{
			if (bMirroring || Index < PaintSlots)
			{
				return;
			}
			if (FInstanceOwnerHandlePtr* M = Mirror.Find(Self))
			{
				if (M->IsValid())
				{
					TGuardValue<bool> Guard(bMirroring, true);
					(*M)->SetPrimitiveDataByID(Value, Index, bMarkDirty);
				}
			}
		});

	// The game creates pieces hidden and shows them when the build effect ends; the vanilla copy follows.
	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::UnHideInstance,
		[](FInstanceOwnershipHandle* Self, bool bMarkDirty)
		{
			if (bMirroring)
			{
				return;
			}
			if (FInstanceOwnerHandlePtr* M = Mirror.Find(Self))
			{
				if (M->IsValid())
				{
					TGuardValue<bool> Guard(bMirroring, true);
					(*M)->UnHideInstance(bMarkDirty);
				}
			}
		});

	// Remove the vanilla copies when an SS rail is dismantled or unloaded.
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildable::EndPlay, GetMutableDefault<AFGBuildable>(),
		[](auto& Scope, AFGBuildable* Self, const EEndPlayReason::Type Reason)
		{
			TArray<FOverlay> List;
			if (!Self || !Overlays.RemoveAndCopyValue(Self, List))
			{
				return;
			}
			for (auto It = Mirror.CreateIterator(); It; ++It)
			{
				for (const FOverlay& O : List)
				{
					if (It.Value() == O.Handle)
					{
						It.RemoveCurrent();
						break;
					}
				}
			}
			for (FOverlay& O : List)
			{
				if (AAbstractInstanceManager* M = O.Manager.Get())
				{
					if (O.Handle.IsValid())
					{
						M->RemoveInstance(O.Handle);
					}
				}
			}
		});

	// SS rail hologram: use the game's track material instead of the broken one while it exists.
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGHologram::BeginPlay, GetMutableDefault<AFGHologram>(),
		[](AFGHologram* Self)
		{
			UClass* Build = Self ? Self->GetBuildClass() : nullptr;
			UWorld* World = Self ? Self->GetWorld() : nullptr;
			if (!Build || !World || Build->GetName() != TEXT("Build_SteelBeam_TrainRail_C"))
			{
				return;
			}
			FixHologram(Self);
			TSharedRef<FTimerHandle> Repeat = MakeShared<FTimerHandle>();
			TWeakObjectPtr<AFGHologram> WeakHolo(Self);
			TWeakObjectPtr<UWorld> WeakWorld(World);
			World->GetTimerManager().SetTimer(*Repeat, FTimerDelegate::CreateLambda([WeakHolo, WeakWorld, Repeat]()
			{
				if (AFGHologram* H = WeakHolo.Get())
				{
					FixHologram(H);
				}
				else if (UWorld* W = WeakWorld.Get())
				{
					W->GetTimerManager().ClearTimer(*Repeat);
				}
			}), 0.1f, true);
		});
#endif
}

IMPLEMENT_MODULE(FSSBeamRailFixModule, SSBeamRailFix)
