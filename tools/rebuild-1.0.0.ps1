# SSBeamRailFix 1.0.0 (release): writes source, uplugin and material script. Stops if the Unreal Editor is running.
# Diagnostics removed; SS_Mod is now a required dependency. The material script is unchanged since 0.4.11
# (only its header comment), so rerunning it is optional.
# Then: the usual PackagePlugin -DLCName=SSBeamRailFix.
param([string]$mod = (Split-Path -Parent $PSScriptRoot))  # the mod folder (repo root); pass -mod to write elsewhere
$ErrorActionPreference = 'Stop'
if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) { throw 'Close the Unreal Editor, then rerun.' }
New-Item -ItemType Directory -Force "$mod\Scripts","$mod\Content\Materials" | Out-Null
@'
{
	"FileVersion": 3,
	"Version": 1,
	"VersionName": "1.0.0",
	"SemVersion": "1.0.0",
	"GameVersion": ">=520140",
	"FriendlyName": "SS Beam Rail Fix",
	"Description": "Fixes Structural Solutions' Train Rail I-Beam: it bends along curves, takes paint and finishes on the beam, and its steel rail looks like vanilla track. Requires Structural Solutions; ships none of its files.",
	"Category": "Modding",
	"CreatedBy": "Aetherosz",
	"CanContainContent": true,
	"IsBetaVersion": false,
	"IsExperimentalVersion": false,
	"Installed": false,
	"RequiredOnRemote": false,
	"Modules": [
		{
			"Name": "SSBeamRailFix",
			"Type": "Runtime",
			"LoadingPhase": "PostDefault"
		}
	],
	"Plugins": [
		{
			"Name": "SML",
			"Enabled": true,
			"SemVersion": "^3.12.1"
		},
		{
			"Name": "SS_Mod",
			"Enabled": true,
			"SemVersion": ">=1.1.47"
		}
	]
}
'@ | Set-Content -Encoding UTF8 "$mod\SSBeamRailFix.uplugin"
@'
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
'@ | Set-Content -Encoding UTF8 "$mod\Source\SSBeamRailFix\Private\SSBeamRailFixModule.cpp"
@'
# SSBeamRailFix 1.0.0: builds /SSBeamRailFix/Materials/M_SSRail in the SML editor project.
# Run headless: UnrealEditor-Cmd.exe "<FactoryGame.uproject>" -run=pythonscript -script="<this file>"
# Needs the editor's Python Editor Script Plugin enabled in the starter project.
#
# A brand-new base material (not a child of the game's MM_FactoryBaked_Spline), so it is compiled
# and cooked into our own pak. It bends each rail piece on the GPU from the per-instance data the
# game already writes (in-game logs 2026-11-24):
#   floats 0-2  primary paint (FICSIT orange 0.953 0.302 0.067), 3-5 secondary
#   floats 15-17 start position (world), 18-20 start tangent, 21-23 end position, 24-26 end tangent
#   mesh SM_TrainRail_New runs X 0..1200 (= Length)
import unreal

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary
PKG = '/SSBeamRailFix/Materials'
NAME = 'M_SSRail'
PATH = PKG + '/' + NAME

# Reuse the existing asset. In the commandlet does_asset_exist is False until the registry scans the
# plugin, and delete-and-recreate fails unattended, so load first and create only if that fails.
mat = unreal.load_asset(PATH)
if mat is not None:
    MEL.delete_all_material_expressions(mat)
else:
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(NAME, PKG, unreal.Material, unreal.MaterialFactoryNew())
if mat is None:
    raise RuntimeError('M_SSRail could not be loaded or created')
mat.set_editor_property('used_with_instanced_static_meshes', True)
# 0.4.5: the steel rail and deck are drawn by the game's own track mesh and material on top of ours,
# so this material clips its own rail region away (ClipRail 1) and only draws the I-beam.
mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)


def node(cls, x, y, **props):
    n = MEL.create_material_expression(mat, cls, x, y)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def custom(x, y, code, inputs, out_type):
    n = node(unreal.MaterialExpressionCustom, x, y)
    n.set_editor_property('code', code)
    n.set_editor_property('output_type', out_type)
    ins = []
    for name in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property('input_name', name)
        ins.append(ci)
    n.set_editor_property('inputs', ins)
    return n


def link(a, a_out, b, b_in):
    if not MEL.connect_material_expressions(a, a_out, b, b_in):
        raise RuntimeError('link failed: %s.%s -> %s.%s' % (a.get_name(), a_out, b.get_name(), b_in))


def data3(index, x, y):
    return node(unreal.MaterialExpressionPerInstanceCustomData3Vector, x, y, data_index=index)


def scalar(name, value, x, y):
    return node(unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=value)


def texture(name, sampler, default, x, y):
    return node(unreal.MaterialExpressionTextureSampleParameter2D, x, y, parameter_name=name,
                sampler_type=sampler, texture=unreal.load_asset(default))


# ---- bend (World Position Offset) ----
BEND = '''
// 0.4.11: no spline data (both tangents zero) -> leave the piece unbent instead of collapsing it.
if (dot(T0, T0) + dot(T1, T1) < 1e-6) return float3(0, 0, 0);
float t = saturate(LP.x / Len);
float t2 = t * t, t3 = t2 * t;
float3 P = (2*t3 - 3*t2 + 1) * P0 + (t3 - 2*t2 + t) * T0 + (-2*t3 + 3*t2) * P1 + (t3 - t2) * T1;
float3 D = (6*t2 - 6*t) * P0 + (3*t2 - 4*t + 1) * T0 + (-6*t2 + 6*t) * P1 + (3*t2 - 2*t) * T1;
float3 F = normalize(D + float3(1e-4, 0, 0));
float3 R = normalize(cross(float3(0, 0, 1), F));
float3 U = cross(F, R);
return (P + R * LP.y + U * LP.z) - WP;
'''
bend = custom(-400, 400, BEND, ['LP', 'WP', 'P0', 'T0', 'P1', 'T1', 'Len'], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
link(node(unreal.MaterialExpressionLocalPosition, -900, 300), '', bend, 'LP')
link(node(unreal.MaterialExpressionWorldPosition, -900, 360,
          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS), '', bend, 'WP')
link(data3(15, -900, 420), '', bend, 'P0')
link(data3(18, -900, 480), '', bend, 'T0')
link(data3(21, -900, 540), '', bend, 'P1')
link(data3(24, -900, 600), '', bend, 'T1')
link(scalar('Length', 1200.0, -900, 660), '', bend, 'Len')
MEL.connect_material_property(bend, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

# ---- colour and paint ----
# PaintMode picks which part gets painted:
# 0 whole mesh, 1-4 AOMasks R/G/B/A, 5 coloured parts of the albedo,
# 7 debug: red below RailZ, green above, banded every 50 units of local height.
# 6 (default, 0.4.2) everything below local height RailZ: the I-beam body. The mesh spans Z -400..135,
#   so the beam is presumed to be -400..0 and the steel rail on top 0..135. RailZ/RailBlend are tunable live.
# Painted colour = swatch colour with light texture detail (Detail), so it matches the straight I-beams.
PAINT = '''
float3 prim = Prim;
if (dot(prim, float3(1, 1, 1)) < 0.001) prim = float3(0.953, 0.302, 0.067);
float lum = dot(Alb, float3(0.299, 0.587, 0.114));
int mode = (int)round(Mode);
float m = 1;
if (mode == 1) m = Masks.r;
else if (mode == 2) m = Masks.g;
else if (mode == 3) m = Masks.b;
else if (mode == 4) m = Masks.a;
else if (mode == 5) m = saturate((max(Alb.r, max(Alb.g, Alb.b)) - min(Alb.r, min(Alb.g, Alb.b))) * 4);
else if (mode == 6) m = 1 - saturate((LP.z - RailZ) / max(RailBlend, 0.01) + 0.5);
else if (mode == 7) return lerp(float3(1, 0, 0), float3(0, 1, 0), step(RailZ, LP.z)) * (0.6 + 0.4 * frac(LP.z / 50));
float3 painted = prim * lerp(1, saturate(lum * Gain), Detail);
float3 steel = lerp(lum.xxx, Alb, RailSat) * RailBright;
return lerp(steel, painted, m);
'''
paint = custom(-400, -100, PAINT, ['Alb', 'Masks', 'Prim', 'Mode', 'Gain', 'Detail', 'LP', 'RailZ', 'RailBlend', 'RailBright', 'RailSat'], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
alb = texture('Albedo', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, '/Engine/EngineResources/DefaultTexture', -900, -300)
masks = texture('AOMasks', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, '/Engine/EngineResources/WhiteSquareTexture', -900, -100)
link(alb, 'RGB', paint, 'Alb')
link(masks, 'RGBA', paint, 'Masks')
link(data3(0, -900, 100), '', paint, 'Prim')
link(scalar('PaintMode', 6.0, -900, 160), '', paint, 'Mode')
link(scalar('PaintGain', 2.0, -900, 220), '', paint, 'Gain')
link(scalar('PaintDetail', 0.25, -900, 280), '', paint, 'Detail')
# 0.4.3: take the mesh's own local position in the vertex shader, before the bend, and carry it to the
# pixel shader. In 0.4.2 the pixel-shader LocalPosition was rebuilt from the bent position, so on curved
# pieces the beam/rail split moved and the steel rail took paint.
vi = node(unreal.MaterialExpressionVertexInterpolator, -700, 340)
link(node(unreal.MaterialExpressionLocalPosition, -900, 340), '', vi, '')
link(vi, '', paint, 'LP')
CLIP = '''
return (Clip > 0.5 && LP.z > RailZ) ? 0 : 1;
'''
clip = custom(-400, 1100, CLIP, ['LP', 'RailZ', 'Clip'], unreal.CustomMaterialOutputType.CMOT_FLOAT1)
link(vi, '', clip, 'LP')
link(scalar('RailZ', 0.0, -900, 1100), '', clip, 'RailZ')
link(scalar('ClipRail', 1.0, -900, 1160), '', clip, 'Clip')
MEL.connect_material_property(clip, '', unreal.MaterialProperty.MP_OPACITY_MASK)
link(scalar('RailZ', 0.0, -900, 400), '', paint, 'RailZ')
link(scalar('RailBlend', 4.0, -900, 460), '', paint, 'RailBlend')
# 0.4.4: the unpainted steel rail showed SS's raw albedo, lighter and warmer than the game's dark steel
# track. Darken and desaturate it; both are tunable live (ssrail.RailBright, ssrail.RailSat).
link(scalar('RailBright', 0.45, -900, 520), '', paint, 'RailBright')
link(scalar('RailSat', 0.3, -900, 580), '', paint, 'RailSat')
MEL.connect_material_property(paint, '', unreal.MaterialProperty.MP_BASE_COLOR)

# 0.4.5: bend the normals too. Before, every piece kept the lighting of its unbent start direction, so
# shading stepped at each piece boundary along a curve (hard bands). Now the normal map is taken into the
# mesh's own space and turned with the same spline frame the bend uses, output in world space.
mat.set_editor_property('tangent_space_normal', False)
normal = texture('Normal', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, '/Engine/EngineMaterials/DefaultNormal', -900, 800)
to_local = node(unreal.MaterialExpressionTransform, -700, 800,
                transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_TANGENT,
                transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_INSTANCE)
link(normal, 'RGB', to_local, '')
BENTN = '''
float t = saturate(LP.x / Len);
float t2 = t * t;
float3 D = (6*t2 - 6*t) * P0 + (3*t2 - 4*t + 1) * T0 + (-6*t2 + 6*t) * P1 + (3*t2 - 2*t) * T1;
float3 F = normalize(D + float3(1e-4, 0, 0));
float3 R = normalize(cross(float3(0, 0, 1), F));
float3 U = cross(F, R);
return normalize(F * N.x + R * N.y + U * N.z);
'''
bentn = custom(-400, 800, BENTN, ['N', 'LP', 'P0', 'T0', 'P1', 'T1', 'Len'], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
link(to_local, '', bentn, 'N')
link(vi, '', bentn, 'LP')
link(data3(15, -900, 1300), '', bentn, 'P0')
link(data3(18, -900, 1360), '', bentn, 'T0')
link(data3(21, -900, 1420), '', bentn, 'P1')
link(data3(24, -900, 1480), '', bentn, 'T1')
link(scalar('Length', 1200.0, -900, 1540), '', bentn, 'Len')
MEL.connect_material_property(bentn, '', unreal.MaterialProperty.MP_NORMAL)
# 0.4.10: paint finishes. The game writes metallic to float 9 (0 default, 1 for metal finishes such as
# copper) and roughness to float 10 (1 default, 0.5 copper), in-game logs 2026-11-27. Pieces whose paint
# data is still all zero (not yet written) fall back to the default look. Roughness = our base roughness
# (0.6, the value Sir approved for the default paint) times the finish's float 10, so default paint is unchanged.
FINISH = '''
bool unset = dot(Prim, float3(1, 1, 1)) < 0.001 && Rough < 0.001;
float metal = unset ? 0 : saturate(Metal) * FinishAmt;
float rough = unset ? 1 : saturate(Rough);
return float2(metal, saturate(DefRough * lerp(1, rough, FinishAmt)));
'''
finish = custom(-400, 900, FINISH, ['Prim', 'Metal', 'Rough', 'DefRough', 'FinishAmt'], unreal.CustomMaterialOutputType.CMOT_FLOAT2)
link(data3(0, -900, 1700), '', finish, 'Prim')
link(node(unreal.MaterialExpressionPerInstanceCustomData, -900, 1760, data_index=9), '', finish, 'Metal')
link(node(unreal.MaterialExpressionPerInstanceCustomData, -900, 1820, data_index=10), '', finish, 'Rough')
link(scalar('Roughness', 0.6, -900, 1880), '', finish, 'DefRough')
link(scalar('FinishAmt', 1.0, -900, 1940), '', finish, 'FinishAmt')
mask_r = node(unreal.MaterialExpressionComponentMask, -200, 900, r=True, g=False, b=False, a=False)
mask_g = node(unreal.MaterialExpressionComponentMask, -200, 960, r=False, g=True, b=False, a=False)
link(finish, '', mask_r, '')
link(finish, '', mask_g, '')
MEL.connect_material_property(mask_r, '', unreal.MaterialProperty.MP_METALLIC)
MEL.connect_material_property(mask_g, '', unreal.MaterialProperty.MP_ROUGHNESS)

MEL.recompile_material(mat)
EAL.save_asset(PATH)
unreal.log('SSBeamRailFix: M_SSRail created and saved at ' + PATH)
'@ | Set-Content -Encoding UTF8 "$mod\Scripts\make_M_SSRail.py"
Write-Host 'Source is 1.0.0. Run PackagePlugin (-DLCName=SSBeamRailFix).'
