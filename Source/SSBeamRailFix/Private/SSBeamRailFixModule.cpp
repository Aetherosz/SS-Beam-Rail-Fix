#include "SSBeamRailFixModule.h"

#include "AbstractInstanceManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "InstanceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Hologram/FGHologram.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Patching/NativeHookManager.h"
#include "Buildables/FGBuildable.h"

DEFINE_LOG_CATEGORY_STATIC(LogSSBeamRailFix, Log, All);

/*
 * 0.4.9: dismantle highlight. While highlighted, the game hides the SS pieces and draws straight copies
 * (plain StaticMeshComponents, SM_TrainRail_New, Factory_Materialize MID). 0.4.8 also hid our vanilla
 * rail then, so the straight copies showed fully. Now: hide is no longer mirrored (only the initial hidden
 * state, unhide and data writes), and the straight highlight copies are replaced by spline meshes bent with
 * each piece's spline data (ssrail.BendHighlight 1, 0 to turn off).
 * 0.4.8: the added vanilla track instance now mirrors its SS piece exactly: same initial hidden state,
 * every hide/unhide, and every per-instance data write the game makes to the SS piece (construction and
 * paint state). Without that, it showed the wrong state during the construction effect, which D4rk's
 * original never did. 0.4.7's construction-mesh swap is removed: those meshes were the dismantle highlight.
 * 0.4.7 (removed): construction animation. While an SS rail is being built, the game draws each piece as a plain
 * StaticMeshComponent (SM_TrainRail_New, Factory_Materialize MID, no instance data), so it shows straight,
 * unbent chunks. Each such component is hidden and replaced by a SplineMeshComponent with the same mesh and
 * the same materialize MID, bent with that piece's own spline data. Ours is destroyed when the game's goes.
 * 0.4.6: hologram. SS's Holo_RailRoadTrack_Custom draws SM_TrainRail_New on a StaticMeshComponent plus
 * SplineMeshComponents with the broken MI_Tack_SS. While an SS rail hologram exists, its MI_Tack_SS slots
 * are switched to the game's own MI_Tack_01 (what the vanilla track hologram uses on its spline meshes).
 * Logging adds each hologram material's parent and its "Bypass WPO" value, for SS and vanilla.
 * Probe: the dismantle highlight shows straight, unbent SS chunks. Every 0.5 s, log any mesh components
 * that appear on SS rail actors and on a few vanilla track actors (that is what the highlight draws with).
 * 0.4.5: the steel rail and deck must look identical to vanilla track. SS's mesh is a vanilla track
 * section with an I-beam added underneath, and it reuses vanilla's normal and mask textures. So for every
 * SS rail piece we add a second lightweight instance with the game's own track mesh SM_TrainTrack_02 and
 * its own material MI_Tack_01 (cooked, bends on the same per-instance spline data), and M_SSRail clips
 * its rail region (ClipRail). Overlay instances are removed when the SS piece ends play.
 * ssrail.ClipRail 0 shows our old rail again (for comparison).
 * 0.4.4: unpainted steel rail darkened and desaturated to match the game's track steel
 * (ssrail.RailBright, ssrail.RailSat).
 * 0.4.3: paint mask uses the pre-bend local position (vertex interpolator), so the steel rail stays
 * unpainted on curved pieces too. ssrail.PaintMode 7 shows the beam/rail split (red/green).
 * 0.4.2: paint only the I-beam body (height mask, PaintMode 6, RailZ), swatch colour with light texture
 * detail to match the straight I-beams, and logging of every SS rail piece NOT swapped (tone-step check)
 * plus a per-piece index on paint writes.
 * 0.4.1: 0.4.0 plus logging of every paint write the game makes to a rail piece (PAINTWRITE),
 * to tell "material ignores paint" apart from "paint gun never writes to the rail".
 * 0.4.0
 * SS's MI_Tack_SS needs a variant of the game's MM_FactoryBaked_Spline that was never cooked, and a mod
 * can't add one ("Can't compile MM_FactoryBaked_Spline with cooked content"). So this version ships
 * its own base material, /SSBeamRailFix/Materials/M_SSRail, built by make_M_SSRail.py. It bends
 * each rail piece from the game's per-instance spline data (floats 15-26) and paints from the paint
 * data the game already writes (floats 0-5). SS's textures are set at runtime; none are shipped.
 * Console: ssrail.PaintMode 0-5 (0 whole mesh, 1-4 AOMasks R/G/B/A, 5 coloured parts of the albedo),
 * ssrail.PaintGain.
 */
namespace
{
	const TCHAR* BrokenMaterialPath = TEXT("/SS_Mod/Materials/MI_Tack_SS.MI_Tack_SS");
	const TCHAR* OurMaterialPath = TEXT("/SSBeamRailFix/Materials/M_SSRail.M_SSRail");

	TWeakObjectPtr<UMaterialInterface> Broken;
	TWeakObjectPtr<UMaterialInstanceDynamic> Replacement;
	bool bTriedBuild = false;
	TMap<TWeakObjectPtr<UClass>, bool> IsSSClass;
	int32 Swapped = 0;
	int32 HologramsLogged = 0;

	// 0.4.1: rail piece handles, so we can see whether the paint gun writes new paint to them.
	TArray<FInstanceOwnerHandlePtr> RailHandles;

	// 0.4.5: vanilla track overlay instances, per SS rail actor.
	const TCHAR* VanillaMeshPath = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Track/Mesh/SM_TrainTrack_02.SM_TrainTrack_02");
	struct FOverlay
	{
		TWeakObjectPtr<AAbstractInstanceManager> Manager;
		FInstanceOwnerHandlePtr Handle;
	};
	TMap<TWeakObjectPtr<AActor>, TArray<FOverlay>> Overlays;
	bool bAddingOverlay = false;

	// 0.4.8: SS piece handle -> its vanilla overlay handle, so hide/unhide and data writes can be mirrored.
	TMap<const FInstanceOwnershipHandle*, FInstanceOwnerHandlePtr> Mirror;
	bool bMirroring = false;
	// 0.4.7: per-piece spline data (world space, from instance floats 15-26) for the construction animation.
	struct FRailSpline
	{
		FVector P0, T0, P1, T1;
	};
	TMap<TWeakObjectPtr<AActor>, TArray<FRailSpline>> RailSplines;
	struct FBuildSwap
	{
		TWeakObjectPtr<UStaticMeshComponent> Original;
		TWeakObjectPtr<USplineMeshComponent> Bent;
	};
	TArray<FBuildSwap> BuildSwaps;
	int32 BuildSwapsLogged = 0;
	int32 MirrorLogged = 0;

	int32 OverlaysAdded = 0;

	int32 RailIndex(const FInstanceOwnershipHandle* Self)
	{
		for (int32 i = 0; i < RailHandles.Num(); ++i)
		{
			if (RailHandles[i].Get() == Self)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	FString FirstFloats(const TArray<float>& Data, int32 Count)
	{
		FString Out;
		for (int32 i = 0; i < Data.Num() && i < Count; ++i)
		{
			Out += FString::Printf(TEXT("%s%d:%.3f"), i ? TEXT(" ") : TEXT(""), i, Data[i]);
		}
		return Out;
	}

	void ApplyConsoleValues();

	TAutoConsoleVariable<float> CVarRailZ(TEXT("ssrail.RailZ"), 0.f,
		TEXT("Local height where the steel rail starts (mode 6). Mesh spans -400..135."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarRailBright(TEXT("ssrail.RailBright"), 0.45f,
		TEXT("Brightness of the unpainted steel rail, 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarRailSat(TEXT("ssrail.RailSat"), 0.3f,
		TEXT("Colour saturation of the unpainted steel rail, 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarClipRail(TEXT("ssrail.ClipRail"), 1.f,
		TEXT("1: hide our rail region and show the game's own track on top, 0: show ours."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarBendHighlight(TEXT("ssrail.BendHighlight"), 1.f,
		TEXT("1: bend the dismantle-highlight copies of SS rail pieces, 0: leave the game's straight copies."));
	TAutoConsoleVariable<float> CVarDetail(TEXT("ssrail.PaintDetail"), 0.25f,
		TEXT("How much texture detail shows through the paint, 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));

	TAutoConsoleVariable<float> CVarPaintMode(TEXT("ssrail.PaintMode"), 6.f,
		TEXT("SS rail paint area: 0 whole mesh, 1-4 AOMasks R/G/B/A, 5 coloured parts of the albedo, 6 below RailZ (I-beam body), 7 debug split view."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarPaintGain(TEXT("ssrail.PaintGain"), 2.f,
		TEXT("SS rail paint brightness."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));

	void ApplyConsoleValues()
	{
		if (UMaterialInstanceDynamic* MID = Replacement.Get())
		{
			MID->SetScalarParameterValue(TEXT("PaintMode"), CVarPaintMode.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("PaintGain"), CVarPaintGain.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("RailZ"), CVarRailZ.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("PaintDetail"), CVarDetail.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("RailBright"), CVarRailBright.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("RailSat"), CVarRailSat.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("ClipRail"), CVarClipRail.GetValueOnGameThread());
			UE_LOG(LogSSBeamRailFix, Display, TEXT("PaintMode %.0f, PaintGain %.2f, RailZ %.1f, PaintDetail %.2f, RailBright %.2f, RailSat %.2f, ClipRail %.0f"), CVarPaintMode.GetValueOnGameThread(),
				CVarPaintGain.GetValueOnGameThread(), CVarRailZ.GetValueOnGameThread(), CVarDetail.GetValueOnGameThread(),
				CVarRailBright.GetValueOnGameThread(), CVarRailSat.GetValueOnGameThread(), CVarClipRail.GetValueOnGameThread());
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

	int32 HoloSwaps = 0;

	// Highlight probe state.
	TSet<TWeakObjectPtr<AActor>> VanillaTracks;
	TSet<FString> ComponentsLogged;
	bool bProbeStarted = false;

	void LogActorComponents(AActor* Actor, const TCHAR* Kind)
	{
		TInlineComponentArray<UMeshComponent*> Meshes(Actor);
		for (UMeshComponent* M : Meshes)
		{
			const FString Key = M->GetPathName();
			if (ComponentsLogged.Contains(Key) || ComponentsLogged.Num() >= 40)
			{
				continue;
			}
			ComponentsLogged.Add(Key);
			FString Mesh, Mats, Data;
			if (USplineMeshComponent* SM = Cast<USplineMeshComponent>(M))
			{
				Mesh = FString::Printf(TEXT("spline mesh=%s"), *GetPathNameSafe(SM->GetStaticMesh()));
			}
			else if (UStaticMeshComponent* S = Cast<UStaticMeshComponent>(M))
			{
				Mesh = FString::Printf(TEXT("mesh=%s"), *GetPathNameSafe(S->GetStaticMesh()));
			}
			for (int32 Slot = 0; Slot < M->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Mat = M->GetMaterial(Slot);
				UMaterialInstance* MI = Cast<UMaterialInstance>(Mat);
				Mats += FString::Printf(TEXT("%s%s(parent %s)"), Slot ? TEXT(", ") : TEXT(""), *GetNameSafe(Mat), MI ? *GetNameSafe(MI->Parent) : TEXT("-"));
			}
			const TArray<float>& CPD = M->GetCustomPrimitiveData().Data;
			for (int32 i = 0; i < CPD.Num() && i < 28; ++i)
			{
				Data += FString::Printf(TEXT("%s%d:%.1f"), i ? TEXT(" ") : TEXT(""), i, CPD[i]);
			}
			UE_LOG(LogSSBeamRailFix, Display, TEXT("HIGHLIGHT %s %s: %s (%s) %s visible=%d mats=[%s] cpd%d=[%s]"), Kind, *Actor->GetName(), *M->GetName(),
				*M->GetClass()->GetName(), *Mesh, M->IsVisible() ? 1 : 0, *Mats, CPD.Num(), *Data);
		}
	}

	/** Replace the game's straight dismantle-highlight meshes on SS rails with bent spline meshes. */
	void FixConstruction()
	{
		static UStaticMesh* RailMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/SS_Mod/Mesh/Steel/SM_TrainRail_New.SM_TrainRail_New"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		// Clean up ours once the game's component is gone.
		for (int32 i = BuildSwaps.Num() - 1; i >= 0; --i)
		{
			UStaticMeshComponent* O = BuildSwaps[i].Original.Get();
			if (!O || !O->IsRegistered() || O->IsBeingDestroyed())
			{
				if (USplineMeshComponent* B = BuildSwaps[i].Bent.Get())
				{
					B->DestroyComponent();
				}
				BuildSwaps.RemoveAtSwap(i);
			}
		}
		if (!RailMesh || CVarBendHighlight.GetValueOnGameThread() < 0.5f)
		{
			return;
		}
		for (auto& Pair : RailSplines)
		{
			AActor* A = Pair.Key.Get();
			if (!A || Pair.Value.Num() == 0)
			{
				continue;
			}
			TInlineComponentArray<UStaticMeshComponent*> Comps(A);
			for (UStaticMeshComponent* C : Comps)
			{
				if (Cast<USplineMeshComponent>(C) || C->GetStaticMesh() != RailMesh || !C->IsVisible())
				{
					continue;
				}
				// The piece whose start is nearest this component.
				const FVector Loc = C->GetComponentLocation();
				const FRailSpline* Best = nullptr;
				double BestDist = TNumericLimits<double>::Max();
				for (const FRailSpline& RS : Pair.Value)
				{
					const double Dist = FVector::DistSquared(Loc, RS.P0);
					if (Dist < BestDist)
					{
						BestDist = Dist;
						Best = &RS;
					}
				}
				if (!Best)
				{
					continue;
				}
				const FTransform T = C->GetComponentTransform();
				USplineMeshComponent* B = NewObject<USplineMeshComponent>(A);
				B->SetMobility(EComponentMobility::Movable);
				B->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				B->SetStaticMesh(RailMesh);
				B->SetupAttachment(A->GetRootComponent());
				B->RegisterComponent();
				B->SetWorldTransform(T);
				B->SetStartAndEnd(T.InverseTransformPosition(Best->P0), T.InverseTransformVector(Best->T0),
					T.InverseTransformPosition(Best->P1), T.InverseTransformVector(Best->T1), true);
				for (int32 Slot = 0; Slot < C->GetNumMaterials(); ++Slot)
				{
					B->SetMaterial(Slot, C->GetMaterial(Slot));
				}
				C->SetVisibility(false);
				BuildSwaps.Add({C, B});
				if (BuildSwapsLogged++ < 6)
				{
					UE_LOG(LogSSBeamRailFix, Display, TEXT("HIGHLIGHTSWAP %s.%s -> bent spline mesh (start offset %.0f, mat %s)"), *A->GetName(), *C->GetName(),
						FMath::Sqrt(BestDist), *GetNameSafe(C->GetMaterial(0)));
				}
			}
		}
	}

	void StartHighlightProbe(UWorld* World)
	{
		if (bProbeStarted || !World)
		{
			return;
		}
		bProbeStarted = true;
		FTimerHandle FastHandle;
		World->GetTimerManager().SetTimer(FastHandle, FTimerDelegate::CreateLambda([]() { FixConstruction(); }), 0.05f, true);
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]()
		{
			if (ComponentsLogged.Num() >= 40)
			{
				return;
			}
			for (const auto& Pair : Overlays)
			{
				if (AActor* A = Pair.Key.Get())
				{
					LogActorComponents(A, TEXT("SS"));
				}
			}
			for (const TWeakObjectPtr<AActor>& W : VanillaTracks)
			{
				if (AActor* A = W.Get())
				{
					LogActorComponents(A, TEXT("VANILLA"));
				}
			}
		}), 0.5f, true);
	}

	/** Switch any MI_Tack_SS on an SS rail hologram to the game's MI_Tack_01. Spline meshes are added while dragging, so this runs repeatedly. */
	void FixHologram(AFGHologram* Holo)
	{
		static UMaterialInterface* Vanilla = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FactoryGame/Buildable/Factory/Train/Track/Material/MI_Tack_01.MI_Tack_01"), nullptr, LOAD_NoWarn | LOAD_Quiet);
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
					if (HoloSwaps++ < 5)
					{
						UE_LOG(LogSSBeamRailFix, Display, TEXT("HOLOSWAP %s.%s slot %d -> MI_Tack_01"), *Holo->GetName(), *M->GetName(), Slot);
					}
				}
			}
		}
	}

	void DescribeHologram(AFGHologram* Holo)
	{
		TInlineComponentArray<UMeshComponent*> Meshes(Holo);
		UE_LOG(LogSSBeamRailFix, Display, TEXT("HOLOGRAM %s (%s) builds %s: %d mesh components"), *Holo->GetName(), *Holo->GetClass()->GetPathName(),
			*GetPathNameSafe(Holo->GetBuildClass()), Meshes.Num());
		for (UMeshComponent* M : Meshes)
		{
			FString Mats;
			for (int32 Slot = 0; Slot < M->GetNumMaterials(); ++Slot)
			{
				Mats += FString::Printf(TEXT("%s%s"), Slot ? TEXT(", ") : TEXT(""), *GetPathNameSafe(M->GetMaterial(Slot)));
			}
			FString Mesh;
			if (USplineMeshComponent* SM = Cast<USplineMeshComponent>(M))
			{
				Mesh = FString::Printf(TEXT("spline mesh=%s"), *GetPathNameSafe(SM->GetStaticMesh()));
			}
			else if (UStaticMeshComponent* S = Cast<UStaticMeshComponent>(M))
			{
				Mesh = FString::Printf(TEXT("mesh=%s"), *GetPathNameSafe(S->GetStaticMesh()));
			}
			UE_LOG(LogSSBeamRailFix, Display, TEXT("  %s (%s) %s mats=[%s]"), *M->GetName(), *M->GetClass()->GetName(), *Mesh, *Mats);
			for (int32 Slot = 0; Slot < M->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Mat = M->GetMaterial(Slot);
				UMaterialInstance* MI = Cast<UMaterialInstance>(Mat);
				float Bypass = -1.f;
				if (Mat)
				{
					Mat->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Bypass WPO")), Bypass);
				}
				UE_LOG(LogSSBeamRailFix, Display, TEXT("    slot %d: %s (%s) parent=%s base=%s BypassWPO=%.1f"), Slot, *GetNameSafe(Mat),
					Mat ? *Mat->GetClass()->GetName() : TEXT("none"), MI ? *GetPathNameSafe(MI->Parent) : TEXT("-"),
					Mat ? *GetNameSafe(Mat->GetBaseMaterial()) : TEXT("-"), Bypass);
			}
		}
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
			UE_LOG(LogSSBeamRailFix, Warning, TEXT("No replacement: SS material %s, our material %s. SS rail left as is."),
				*GetPathNameSafe(SSMaterial), *GetPathNameSafe(Ours));
			return nullptr;
		}
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Ours, GetTransientPackage(), TEXT("MID_SSBeamRailFix"));
		for (const FTextureParameterValue& P : SSMaterial->TextureParameterValues)
		{
			UTexture* Tex = P.ParameterValue;
			MID->SetTextureParameterValue(P.ParameterInfo.Name, Tex);
			UE_LOG(LogSSBeamRailFix, Display, TEXT("TEXTURE %s = %s compression=%d sRGB=%d"), *P.ParameterInfo.Name.ToString(), *GetPathNameSafe(Tex),
				Tex ? (int32)Tex->CompressionSettings.GetValue() : -1, Tex ? (Tex->SRGB ? 1 : 0) : -1);
		}
		MID->AddToRoot();
		Broken = SSMaterial;
		Replacement = MID;
		ApplyConsoleValues();
		UE_LOG(LogSSBeamRailFix, Display, TEXT("Replacement for MI_Tack_SS built on %s."), *Ours->GetPathName());
		return MID;
	}
}

void FSSBeamRailFixModule::StartupModule()
{
#if !WITH_EDITOR
	UE_LOG(LogSSBeamRailFix, Display, TEXT("SSBeamRailFix 0.4.9 hooks registered."));

	SUBSCRIBE_METHOD(AAbstractInstanceManager::SetInstanced,
		[](auto& Scope, AAbstractInstanceManager*, AActor* Owner, const FTransform&, const FInstanceData& Instance,
			FInstanceOwnerHandlePtr&, bool)
		{
			if (!bAddingOverlay && Owner && VanillaTracks.Num() < 10 && Owner->GetClass()->GetName() == TEXT("Build_RailroadTrack_C"))
			{
				VanillaTracks.Add(Owner);
			}
			if (bAddingOverlay || !Owner || !Instance.StaticMesh || !IsFromSS(Owner->GetClass()))
			{
				return;
			}
			StartHighlightProbe(Owner->GetWorld());
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
				if (Current != BrokenMaterial && Current != MID)
				{
					static int32 NotSwapped = 0;
					if (NotSwapped++ < 20)
					{
						UE_LOG(LogSSBeamRailFix, Display, TEXT("NOTSWAPPED %s mesh=%s slot %d mat=%s"), *Owner->GetName(),
							*Data.StaticMesh->GetPathName(), Slot, *GetPathNameSafe(Current));
					}
				}
				if (Current == BrokenMaterial)
				{
					if (Data.OverridenMaterials.Num() < Slots)
					{
						Data.OverridenMaterials.SetNum(Slots);
					}
					Data.OverridenMaterials[Slot] = MID;
					if (Swapped++ < 3 || Swapped % 50 == 0)
					{
						UE_LOG(LogSSBeamRailFix, Display, TEXT("Swapped MI_Tack_SS on %s of %s, slot %d."),
							*Data.StaticMesh->GetName(), *Owner->GetClass()->GetName(), Slot);
					}
				}
			}
		});

	SUBSCRIBE_METHOD_AFTER(AAbstractInstanceManager::SetInstanced,
		[](AAbstractInstanceManager* Manager, AActor* Owner, const FTransform& ActorTransform, const FInstanceData& Instance,
			FInstanceOwnerHandlePtr& Handle, bool bInitializeHidden)
		{
			if (bAddingOverlay || !Owner || Owner->GetClass()->GetName() != TEXT("Build_SteelBeam_TrainRail_C") || !Handle.IsValid())
			{
				return;
			}
			RailHandles.Add(Handle);
			{
				const TArray<float>& F = Instance.DefaultPerInstanceCustomData;
				if (F.Num() >= 27)
				{
					FRailSpline& RS = RailSplines.FindOrAdd(Owner).AddDefaulted_GetRef();
					RS.P0 = FVector(F[15], F[16], F[17]);
					RS.T0 = FVector(F[18], F[19], F[20]);
					RS.P1 = FVector(F[21], F[22], F[23]);
					RS.T1 = FVector(F[24], F[25], F[26]);
				}
			}
			if (!bAddingOverlay && Manager)
			{
				static UStaticMesh* VanillaMesh = LoadObject<UStaticMesh>(nullptr, VanillaMeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (VanillaMesh)
				{
					FInstanceData Copy = Instance;
					Copy.StaticMesh = VanillaMesh;
					Copy.OverridenMaterials.Reset();
					FOverlay& O = Overlays.FindOrAdd(Owner).AddDefaulted_GetRef();
					O.Manager = Manager;
					TGuardValue<bool> Guard(bAddingOverlay, true);
					Manager->SetInstanced(Owner, ActorTransform, Copy, O.Handle, bInitializeHidden);
					if (O.Handle.IsValid())
					{
						Mirror.Add(Handle.Get(), O.Handle);
					}
					if (OverlaysAdded++ < 3)
					{
						UE_LOG(LogSSBeamRailFix, Display, TEXT("OVERLAY vanilla track instance added for %s (handle valid %d, initially hidden %d)."), *Owner->GetName(),
							O.Handle.IsValid() ? 1 : 0, bInitializeHidden ? 1 : 0);
					}
				}
				else if (OverlaysAdded == 0)
				{
					OverlaysAdded = -1000;
					UE_LOG(LogSSBeamRailFix, Warning, TEXT("OVERLAY: vanilla mesh %s not found."), VanillaMeshPath);
				}
			}
			static int32 Logged = 0;
			if (Logged++ < 3)
			{
				UE_LOG(LogSSBeamRailFix, Display, TEXT("RAILPIECE %s created, paint floats [%s]"), *Owner->GetName(), *FirstFloats(Instance.DefaultPerInstanceCustomData, 15));
			}
		});

	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByArray,
		[](const FInstanceOwnershipHandle* Self, const TArray<float>& Data, bool bMarkDirty)
		{
			if (!bMirroring)
			{
				if (FInstanceOwnerHandlePtr* M = Mirror.Find(Self))
				{
					if (M->IsValid())
					{
						TGuardValue<bool> Guard(bMirroring, true);
						(*M)->SetPrimitiveDataByArray(Data, bMarkDirty);
					}
				}
			}
			if (bMirroring)
			{
				return;
			}
			static int32 Logged = 0;
			const int32 Index = Logged < 120 ? RailIndex(Self) : INDEX_NONE;
			if (Index != INDEX_NONE)
			{
				++Logged;
				UE_LOG(LogSSBeamRailFix, Display, TEXT("PAINTWRITE array on rail piece %d of %d: [%s]"), Index, RailHandles.Num(), *FirstFloats(Data, 28));
			}
		});

	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByID,
		[](const FInstanceOwnershipHandle* Self, float Value, const int32 Index, bool bMarkDirty)
		{
			if (!bMirroring)
			{
				if (FInstanceOwnerHandlePtr* M = Mirror.Find(Self))
				{
					if (M->IsValid())
					{
						TGuardValue<bool> Guard(bMirroring, true);
						(*M)->SetPrimitiveDataByID(Value, Index, bMarkDirty);
					}
				}
			}
			if (bMirroring)
			{
				return;
			}
			static int32 Logged = 0;
			const int32 Piece = Logged < 60 ? RailIndex(Self) : INDEX_NONE;
			if (Piece != INDEX_NONE)
			{
				++Logged;
				UE_LOG(LogSSBeamRailFix, Display, TEXT("PAINTWRITE id on rail piece %d: %d = %f"), Piece, Index, Value);
			}
		});

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
					if (MirrorLogged++ < 12)
					{
						UE_LOG(LogSSBeamRailFix, Display, TEXT("MIRROR unhide"));
					}
				}
			}
		});

	// Remove our overlay instances when an SS rail is dismantled or unloaded.
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildable::EndPlay, GetMutableDefault<AFGBuildable>(),
		[](auto& Scope, AFGBuildable* Self, const EEndPlayReason::Type Reason)
		{
			TArray<FOverlay> List;
			if (Self)
			{
				RailSplines.Remove(Self);
			}
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

	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGHologram::BeginPlay, GetMutableDefault<AFGHologram>(),
		[](AFGHologram* Self)
		{
			if (!Self)
			{
				return;
			}
			UClass* Build = Self->GetBuildClass();
			const FString Name = Build ? Build->GetName() : FString();
			if (Name != TEXT("Build_SteelBeam_TrainRail_C") && Name != TEXT("Build_RailroadTrack_C"))
			{
				return;
			}
			UWorld* World = Self->GetWorld();
			if (Name == TEXT("Build_SteelBeam_TrainRail_C") && World)
			{
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
			}
			if (HologramsLogged >= 6)
			{
				return;
			}
			++HologramsLogged;
			FTimerHandle Handle;
			TWeakObjectPtr<AFGHologram> Weak(Self);
			World->GetTimerManager().SetTimer(Handle, [Weak]()
			{
				if (AFGHologram* H = Weak.Get())
				{
					DescribeHologram(H);
				}
			}, 2.0f, false);
		});
#endif
}

IMPLEMENT_MODULE(FSSBeamRailFixModule, SSBeamRailFix)
