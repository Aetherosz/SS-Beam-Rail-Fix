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
