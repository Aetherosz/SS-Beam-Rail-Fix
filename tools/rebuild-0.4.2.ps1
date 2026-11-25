# SSBeamRailFix 0.4.2: writes source, uplugin and the UPDATED material script. Stops if the Unreal Editor is running.
# Then: 1) rerun the material script (M_SSRail changed: beam-only paint mask, new params):
#          UnrealEditor-Cmd.exe "<starter FactoryGame.uproject>" -run=pythonscript -script="$mod\Scripts\make_M_SSRail.py"
#          success line "M_SSRail created and saved" is in SML\Saved\Logs\FactoryGame.log
#       2) the usual PackagePlugin -DLCName=SSBeamRailFix.
param([string]$mod = (Split-Path -Parent $PSScriptRoot))  # the mod folder (repo root); pass -mod to write elsewhere
$ErrorActionPreference = 'Stop'
if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) { throw 'Close the Unreal Editor, then rerun.' }
New-Item -ItemType Directory -Force "$mod\Scripts","$mod\Content\Materials" | Out-Null
@'
{
	"FileVersion": 3,
	"Version": 0,
	"VersionName": "0.4.2",
	"SemVersion": "0.4.2",
	"GameVersion": ">=520140",
	"FriendlyName": "SS Beam Rail Fix (private test)",
	"Description": "Private test build. Redraws Structural Solutions beam railway with its own bending, paintable material, using SS's textures at runtime. Ships none of Structural Solutions' files.",
	"Category": "Modding",
	"CreatedBy": "Aetherosz",
	"CanContainContent": true,
	"IsBetaVersion": true,
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
			"Optional": true,
			"SemVersion": ">=1.1.47"
		}
	]
}
'@ | Set-Content -Encoding UTF8 "$mod\SSBeamRailFix.uplugin"
@'
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

DEFINE_LOG_CATEGORY_STATIC(LogSSBeamRailFix, Log, All);

/*
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
	TAutoConsoleVariable<float> CVarDetail(TEXT("ssrail.PaintDetail"), 0.25f,
		TEXT("How much texture detail shows through the paint, 0..1."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ApplyConsoleValues(); }));

	TAutoConsoleVariable<float> CVarPaintMode(TEXT("ssrail.PaintMode"), 6.f,
		TEXT("SS rail paint area: 0 whole mesh, 1-4 AOMasks R/G/B/A, 5 coloured parts of the albedo, 6 below RailZ (I-beam body)."),
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
			UE_LOG(LogSSBeamRailFix, Display, TEXT("PaintMode %.0f, PaintGain %.2f, RailZ %.1f, PaintDetail %.2f"), CVarPaintMode.GetValueOnGameThread(),
				CVarPaintGain.GetValueOnGameThread(), CVarRailZ.GetValueOnGameThread(), CVarDetail.GetValueOnGameThread());
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
	UE_LOG(LogSSBeamRailFix, Display, TEXT("SSBeamRailFix 0.4.2 hooks registered."));

	SUBSCRIBE_METHOD(AAbstractInstanceManager::SetInstanced,
		[](auto& Scope, AAbstractInstanceManager*, AActor* Owner, const FTransform&, const FInstanceData& Instance,
			FInstanceOwnerHandlePtr&, bool)
		{
			if (!Owner || !Instance.StaticMesh || !IsFromSS(Owner->GetClass()))
			{
				return;
			}
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
		[](AAbstractInstanceManager*, AActor* Owner, const FTransform&, const FInstanceData& Instance,
			FInstanceOwnerHandlePtr& Handle, bool)
		{
			if (!Owner || Owner->GetClass()->GetName() != TEXT("Build_SteelBeam_TrainRail_C") || !Handle.IsValid())
			{
				return;
			}
			RailHandles.Add(Handle);
			static int32 Logged = 0;
			if (Logged++ < 3)
			{
				UE_LOG(LogSSBeamRailFix, Display, TEXT("RAILPIECE %s created, paint floats [%s]"), *Owner->GetName(), *FirstFloats(Instance.DefaultPerInstanceCustomData, 15));
			}
		});

	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByArray,
		[](const FInstanceOwnershipHandle* Self, const TArray<float>& Data, bool)
		{
			static int32 Logged = 0;
			const int32 Index = Logged < 120 ? RailIndex(Self) : INDEX_NONE;
			if (Index != INDEX_NONE)
			{
				++Logged;
				UE_LOG(LogSSBeamRailFix, Display, TEXT("PAINTWRITE array on rail piece %d of %d: [%s]"), Index, RailHandles.Num(), *FirstFloats(Data, 6));
			}
		});

	SUBSCRIBE_METHOD_AFTER(FInstanceOwnershipHandle::SetPrimitiveDataByID,
		[](const FInstanceOwnershipHandle* Self, float Value, const int32 Index, bool)
		{
			static int32 Logged = 0;
			const int32 Piece = Logged < 60 ? RailIndex(Self) : INDEX_NONE;
			if (Piece != INDEX_NONE)
			{
				++Logged;
				UE_LOG(LogSSBeamRailFix, Display, TEXT("PAINTWRITE id on rail piece %d: %d = %f"), Piece, Index, Value);
			}
		});

	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGHologram::BeginPlay, GetMutableDefault<AFGHologram>(),
		[](AFGHologram* Self)
		{
			if (!Self || HologramsLogged >= 6)
			{
				return;
			}
			UClass* Build = Self->GetBuildClass();
			const FString Name = Build ? Build->GetName() : FString();
			if (Name != TEXT("Build_SteelBeam_TrainRail_C") && Name != TEXT("Build_RailroadTrack_C"))
			{
				return;
			}
			++HologramsLogged;
			FTimerHandle Handle;
			TWeakObjectPtr<AFGHologram> Weak(Self);
			Self->GetWorld()->GetTimerManager().SetTimer(Handle, [Weak]()
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
'@ | Set-Content -Encoding UTF8 "$mod\Source\SSBeamRailFix\Private\SSBeamRailFixModule.cpp"
@'
# SSBeamRailFix 0.4.2: builds /SSBeamRailFix/Materials/M_SSRail in the SML editor project.
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

if EAL.does_asset_exist(PATH):
    EAL.delete_asset(PATH)
mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(NAME, PKG, unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('used_with_instanced_static_meshes', True)


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
float3 painted = prim * lerp(1, saturate(lum * Gain), Detail);
return lerp(Alb, painted, m);
'''
paint = custom(-400, -100, PAINT, ['Alb', 'Masks', 'Prim', 'Mode', 'Gain', 'Detail', 'LP', 'RailZ', 'RailBlend'], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
alb = texture('Albedo', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, '/Engine/EngineResources/DefaultTexture', -900, -300)
masks = texture('AOMasks', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, '/Engine/EngineResources/WhiteSquareTexture', -900, -100)
link(alb, 'RGB', paint, 'Alb')
link(masks, 'RGBA', paint, 'Masks')
link(data3(0, -900, 100), '', paint, 'Prim')
link(scalar('PaintMode', 6.0, -900, 160), '', paint, 'Mode')
link(scalar('PaintGain', 2.0, -900, 220), '', paint, 'Gain')
link(scalar('PaintDetail', 0.25, -900, 280), '', paint, 'Detail')
link(node(unreal.MaterialExpressionLocalPosition, -900, 340), '', paint, 'LP')
link(scalar('RailZ', 0.0, -900, 400), '', paint, 'RailZ')
link(scalar('RailBlend', 4.0, -900, 460), '', paint, 'RailBlend')
MEL.connect_material_property(paint, '', unreal.MaterialProperty.MP_BASE_COLOR)

normal = texture('Normal', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, '/Engine/EngineMaterials/DefaultNormal', -900, 800)
MEL.connect_material_property(normal, 'RGB', unreal.MaterialProperty.MP_NORMAL)
MEL.connect_material_property(scalar('Roughness', 0.6, -400, 900), '', unreal.MaterialProperty.MP_ROUGHNESS)
MEL.connect_material_property(scalar('Metallic', 0.0, -400, 960), '', unreal.MaterialProperty.MP_METALLIC)

MEL.recompile_material(mat)
EAL.save_asset(PATH)
unreal.log('SSBeamRailFix: M_SSRail created and saved at ' + PATH)
'@ | Set-Content -Encoding UTF8 "$mod\Scripts\make_M_SSRail.py"
Write-Host 'Source is 0.4.2. Run make_M_SSRail.py in the editor commandlet, then PackagePlugin (-DLCName=SSBeamRailFix).'
