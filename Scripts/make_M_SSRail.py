# SSBeamRailFix 0.4.10: builds /SSBeamRailFix/Materials/M_SSRail in the SML editor project.
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
