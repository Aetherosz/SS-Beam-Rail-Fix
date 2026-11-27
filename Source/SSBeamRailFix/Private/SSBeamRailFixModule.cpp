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
#include "UObject/UObjectIterator.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

DEFINE_LOG_CATEGORY_STATIC(LogSSBeamRailFix, Log, All);

/*
 * 0.4.11: dismantle highlight, real cause. In-game logs 2026-11-27 (0.4.10 probe): the dismantle outline
 * is drawn by InstancedStaticMeshComponents on the player character (not on the rail actor), with
 * SM_TrainRail_New and SS's MI_Tack_SS, which can't bend. Every 0.05 s, such components on the local
 * player's pawn get our M_SSRail (a second instance with ClipRail 0, so the whole piece incl. rail is drawn
 * and outlined), which bends from the same per-instance spline data. Only done when the component carries
 * at least 27 custom floats; the material also leaves pieces without spline data unbent.
 * The 0.4.9 spline-mesh swap of rail-actor components is removed (those components never appear).
 * 0.4.10: paint finishes. The game writes a finish as per-instance data: index 9 metallic (0 default,
 * 1 for finishes like copper) and index 10 roughness (1 default, 0.5 copper), in-game logs 2026-11-27.
 * M_SSRail now reads both for the beam. The vanilla rail overlay no longer receives the SS piece's
 * paint data (indices 0-14 stay at their initial values), so a finish no longer tints the steel rail;
 * spline and state data (15+) are still mirrored.
 * Probe: in 0.4.9 no highlight component ever appeared on a tracked SS rail actor. Now every second,
 * every SM_TrainRail_New mesh component anywhere in the world is logged once (owner, class, spline or
 * not, visibility, material), with a heartbeat so a stopped timer shows. SS piece hides are logged too.
 * PAINTWRITE logs indices 0-14 only when they change for that piece.
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
	TWeakObjectPtr<UMaterialInstanceDynamic> HighlightReplacement;
	TWeakObjectPtr<UWorld> ProbeWorld;
	int32 HighlightSwapsLogged = 0;
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

	// 0.4.10: indices 0-14 are paint/finish (0-5 colours, 6 power, 7-8 pattern, 9 metallic, 10 roughness,
	// 11-13 light colour). The vanilla rail overlay keeps its own values for these.
	constexpr int32 PaintSlots = 15;
	TMap<const FInstanceOwnershipHandle*, TArray<float>> LastPaint;
	int32 PaintLogged = 0;
	int32 HidesLogged = 0;
	TSet<FString> RailCompsLogged;
	int32 ProbeTicks = 0;

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

	TAutoConsoleVariable<float> CVarRoughness(TEXT("ssrail.Roughness"), 0.6f,
		TEXT("Base roughness of the I-beam; the paint finish's roughness multiplies it."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarFinish(TEXT("ssrail.Finish"), 1.f,
		TEXT("1: apply paint finishes (metallic/roughness) to the I-beam, 0: ignore them."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarPaintMode(TEXT("ssrail.PaintMode"), 6.f,
		TEXT("SS rail paint area: 0 whole mesh, 1-4 AOMasks R/G/B/A, 5 coloured parts of the albedo, 6 below RailZ (I-beam body), 7 debug split view."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));
	TAutoConsoleVariable<float> CVarPaintGain(TEXT("ssrail.PaintGain"), 2.f,
		TEXT("SS rail paint brightness."),
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
			// The highlight copy draws the whole piece (rail too), so the outline covers all of it.
			MID->SetScalarParameterValue(TEXT("ClipRail"), k == 0 ? CVarClipRail.GetValueOnGameThread() : 0.f);
			MID->SetScalarParameterValue(TEXT("Roughness"), CVarRoughness.GetValueOnGameThread());
			MID->SetScalarParameterValue(TEXT("FinishAmt"), CVarFinish.GetValueOnGameThread());
		}
		if (Replacement.IsValid())
		{
			UE_LOG(LogSSBeamRailFix, Display, TEXT("PaintMode %.0f, PaintGain %.2f, RailZ %.1f, PaintDetail %.2f, RailBright %.2f, RailSat %.2f, ClipRail %.0f, Roughness %.2f, Finish %.0f"), CVarPaintMode.GetValueOnGameThread(),
				CVarPaintGain.GetValueOnGameThread(), CVarRailZ.GetValueOnGameThread(), CVarDetail.GetValueOnGameThread(),
				CVarRailBright.GetValueOnGameThread(), CVarRailSat.GetValueOnGameThread(), CVarClipRail.GetValueOnGameThread(),
				CVarRoughness.GetValueOnGameThread(), CVarFinish.GetValueOnGameThread());
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
	bool bProbeStarted = false;

	/** Give the dismantle-outline ISMs on the player (SM_TrainRail_New with MI_Tack_SS) our bending material. */
	void FixConstruction()
	{
		static UStaticMesh* RailMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/SS_Mod/Mesh/Steel/SM_TrainRail_New.SM_TrainRail_New"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UWorld* World = ProbeWorld.Get();
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
			bool bHasBroken = false;
			for (int32 Slot = 0; Slot < C->GetNumMaterials(); ++Slot)
			{
				bHasBroken |= C->GetMaterial(Slot) == BrokenMaterial;
			}
			if (!bHasBroken)
			{
				continue;
			}
			const bool bHasSpline = C->NumCustomDataFloats >= 27;
			if (HighlightSwapsLogged < 10)
			{
				++HighlightSwapsLogged;
				FString First;
				if (C->PerInstanceSMCustomData.Num() >= C->NumCustomDataFloats && C->NumCustomDataFloats > 0)
				{
					TArray<float> Inst(C->PerInstanceSMCustomData.GetData(), C->NumCustomDataFloats);
					First = FirstFloats(Inst, 28);
				}
				UE_LOG(LogSSBeamRailFix, Display, TEXT("HIGHLIGHTSWAP %s.%s: %d instances, %d custom floats, %s. Instance 0: [%s]"), *Pawn->GetName(), *C->GetName(),
					C->GetInstanceCount(), C->NumCustomDataFloats, bHasSpline ? TEXT("swapped to bending material") : TEXT("NOT swapped (no spline data)"), *First);
			}
			if (!bHasSpline)
			{
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

	void StartHighlightProbe(UWorld* World)
	{
		if (bProbeStarted || !World)
		{
			return;
		}
		bProbeStarted = true;
		ProbeWorld = World;
		FTimerHandle FastHandle;
		World->GetTimerManager().SetTimer(FastHandle, FTimerDelegate::CreateLambda([]() { FixConstruction(); }), 0.05f, true);
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([]()
		{
			static UStaticMesh* RailMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/SS_Mod/Mesh/Steel/SM_TrainRail_New.SM_TrainRail_New"), nullptr, LOAD_NoWarn | LOAD_Quiet);
			++ProbeTicks;
			int32 Found = 0, Visible = 0;
			for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
			{
				UStaticMeshComponent* C = *It;
				if (!C || C->GetStaticMesh() != RailMesh || C->IsTemplate() || !C->IsRegistered())
				{
					continue;
				}
				++Found;
				Visible += C->IsVisible() ? 1 : 0;
				const FString Key = C->GetPathName();
				if (RailCompsLogged.Num() >= 80 || RailCompsLogged.Contains(Key))
				{
					continue;
				}
				RailCompsLogged.Add(Key);
				AActor* A = C->GetOwner();
				const bool bOurs = BuildSwaps.ContainsByPredicate([C](const FBuildSwap& S) { return S.Bent.Get() == C; });
				UE_LOG(LogSSBeamRailFix, Display, TEXT("RAILCOMP %s owner=%s (%s) tracked=%d %s ours=%d visible=%d loc=%s mat=%s(parent %s)"), *C->GetName(),
					*GetNameSafe(A), A ? *A->GetClass()->GetName() : TEXT("-"), A && RailSplines.Contains(A) ? 1 : 0,
					Cast<USplineMeshComponent>(C) ? TEXT("spline") : TEXT("static"), bOurs ? 1 : 0, C->IsVisible() ? 1 : 0,
					*C->GetComponentLocation().ToString(), *GetNameSafe(C->GetMaterial(0)),
					Cast<UMaterialInstance>(C->GetMaterial(0)) ? *GetNameSafe(Cast<UMaterialInstance>(C->GetMaterial(0))->Parent) : TEXT("-"));
			}
			if (ProbeTicks == 1 || ProbeTicks % 60 == 0)
			{
				UE_LOG(LogSSBeamRailFix, Display, TEXT("PROBE tick %d: %d SM_TrainRail_New components (%d visible), %d tracked SS actors, %d bent swaps"),
					ProbeTicks, Found, Visible, RailSplines.Num(), BuildSwaps.Num());
			}
		}), 1.0f, true);
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
		UMaterialInstanceDynamic* HMID = UMaterialInstanceDynamic::Create(Ours, GetTransientPackage(), TEXT("MID_SSBeamRailFix_Highlight"));
		for (const FTextureParameterValue& P : SSMaterial->TextureParameterValues)
		{
			HMID->SetTextureParameterValue(P.ParameterInfo.Name, P.ParameterValue);
		}
		HMID->AddToRoot();
		HighlightReplacement = HMID;
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
	UE_LOG(LogSSBeamRailFix, Display, TEXT("SSBeamRailFix 0.4.11 hooks registered."));

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
						TArray<float> Rail = Data;
						for (int32 i = 0; i < PaintSlots && i < Rail.Num(); ++i)
						{
							Rail[i] = 0.f;
						}
						(*M)->SetPrimitiveDataByArray(Rail, bMarkDirty);
					}
				}
			}
			if (bMirroring)
			{
				return;
			}
			const int32 Index = PaintLogged < 150 ? RailIndex(Self) : INDEX_NONE;
			if (Index != INDEX_NONE)
			{
				TArray<float> Paint(Data.GetData(), FMath::Min(Data.Num(), PaintSlots));
				TArray<float>& Last = LastPaint.FindOrAdd(Self);
				if (Last != Paint)
				{
					Last = Paint;
					++PaintLogged;
					UE_LOG(LogSSBeamRailFix, Display, TEXT("PAINTWRITE array on rail piece %d of %d: [%s]"), Index, RailHandles.Num(), *FirstFloats(Data, PaintSlots));
				}
			}
		});

	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByID,
		[](const FInstanceOwnershipHandle* Self, float Value, const int32 Index, bool bMarkDirty)
		{
			if (!bMirroring)
			{
				if (FInstanceOwnerHandlePtr* M = Mirror.Find(Self))
				{
					if (M->IsValid() && Index >= PaintSlots)
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

	// 0.4.10: log (not mirror) hides of SS pieces, to see when the game hides them for the highlight.
	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::HideInstance,
		[](FInstanceOwnershipHandle* Self, bool)
		{
			if (bMirroring || HidesLogged >= 30 || !Mirror.Contains(Self))
			{
				return;
			}
			++HidesLogged;
			UE_LOG(LogSSBeamRailFix, Display, TEXT("HIDE rail piece %d"), RailIndex(Self));
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
