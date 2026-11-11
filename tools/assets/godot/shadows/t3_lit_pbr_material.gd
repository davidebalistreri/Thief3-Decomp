extends RefCounted
## Converts the explicitly opted-in subset of StandardMaterial3D used by the remaster.
## Unsupported settings are rejected rather than silently changing the material.
const SHADER := preload('res://t3_tools/shadows/t3_lit_pbr.gdshader')
const UNSUPPORTED := [BaseMaterial3D.FEATURE_RIM, BaseMaterial3D.FEATURE_CLEARCOAT,
	BaseMaterial3D.FEATURE_ANISOTROPY, BaseMaterial3D.FEATURE_SUBSURFACE_SCATTERING,
	BaseMaterial3D.FEATURE_SUBSURFACE_TRANSMITTANCE, BaseMaterial3D.FEATURE_BACKLIGHT,
	BaseMaterial3D.FEATURE_REFRACTION, BaseMaterial3D.FEATURE_DETAIL,
	BaseMaterial3D.FEATURE_BENT_NORMAL_MAPPING, BaseMaterial3D.FEATURE_EMISSION]

static func make(b: StandardMaterial3D, require_tag := true) -> ShaderMaterial:
	if require_tag and not b.get_meta('t3_remaster_pbr', false):
		return null
	if b.shading_mode != BaseMaterial3D.SHADING_MODE_PER_PIXEL or b.cull_mode != BaseMaterial3D.CULL_BACK \
			or b.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED \
			or b.blend_mode != BaseMaterial3D.BLEND_MODE_MIX \
			or b.depth_draw_mode != BaseMaterial3D.DEPTH_DRAW_OPAQUE_ONLY or b.no_depth_test \
			or b.disable_ambient_light or b.disable_fog or b.disable_specular_occlusion \
			or b.disable_receive_shadows or b.shadow_to_opacity or b.fixed_size or b.use_point_size \
			or b.use_particle_trails or b.use_z_clip_scale or b.use_fov_override \
			or b.albedo_texture_force_srgb or b.albedo_texture_msdf \
			or b.diffuse_mode != BaseMaterial3D.DIFFUSE_BURLEY \
			or b.specular_mode != BaseMaterial3D.SPECULAR_SCHLICK_GGX \
			or b.billboard_mode != BaseMaterial3D.BILLBOARD_DISABLED \
			or b.texture_filter != BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS or not b.texture_repeat \
			or b.metallic_texture != null or b.uv1_triplanar or b.uv2_triplanar or b.ao_on_uv2 \
			or b.grow or b.proximity_fade_enabled or b.distance_fade_mode != BaseMaterial3D.DISTANCE_FADE_DISABLED \
			or b.next_pass != null:
		return null
	for feature in UNSUPPORTED:
		if b.get_feature(feature):
			return null
	if (b.heightmap_enabled and b.heightmap_texture == null) or (b.ao_enabled and b.ao_texture == null):
		return null
	var sm := ShaderMaterial.new()
	sm.shader = SHADER
	sm.resource_name = b.resource_name
	sm.render_priority = b.render_priority
	for k in b.get_meta_list():
		sm.set_meta(k, b.get_meta(k))
	sm.set_shader_parameter('albedo', b.albedo_color)
	if b.albedo_texture != null:
		sm.set_shader_parameter('albedo_texture', b.albedo_texture)
	sm.set_shader_parameter('use_vertex_color', b.vertex_color_use_as_albedo)
	sm.set_shader_parameter('vertex_color_srgb', b.vertex_color_is_srgb)
	if b.normal_enabled and b.normal_texture != null:
		sm.set_shader_parameter('normal_texture', b.normal_texture)
		sm.set_shader_parameter('normal_scale', b.normal_scale)
	if b.roughness_texture != null:
		sm.set_shader_parameter('roughness_texture', b.roughness_texture)
		sm.set_shader_parameter('roughness_channel', _channel(b.roughness_texture_channel))
	if b.ao_enabled:
		sm.set_shader_parameter('ao_texture', b.ao_texture)
		sm.set_shader_parameter('ao_channel', _channel(b.ao_texture_channel))
		sm.set_shader_parameter('ao_light_affect', b.ao_light_affect)
	if b.heightmap_enabled:
		sm.set_shader_parameter('height_enabled', true)
		sm.set_shader_parameter('height_texture', b.heightmap_texture)
		sm.set_shader_parameter('heightmap_scale', b.heightmap_scale)
		sm.set_shader_parameter('heightmap_min_layers', b.heightmap_min_layers)
		sm.set_shader_parameter('heightmap_max_layers', b.heightmap_max_layers)
		sm.set_shader_parameter('heightmap_deep_parallax', b.heightmap_deep_parallax)
		sm.set_shader_parameter('heightmap_flip_texture', b.heightmap_flip_texture)
		sm.set_shader_parameter('heightmap_flip', Vector2(-1.0 if b.heightmap_flip_tangent else 1.0,
			-1.0 if b.heightmap_flip_binormal else 1.0))
	sm.set_shader_parameter('roughness', b.roughness)
	sm.set_shader_parameter('metallic', b.metallic)
	sm.set_shader_parameter('specular', b.metallic_specular)
	sm.set_shader_parameter('uv1_scale', b.uv1_scale)
	sm.set_shader_parameter('uv1_offset', b.uv1_offset)
	return sm

static func _channel(channel: int) -> Vector4:
	match channel:
		BaseMaterial3D.TEXTURE_CHANNEL_RED: return Vector4(1, 0, 0, 0)
		BaseMaterial3D.TEXTURE_CHANNEL_GREEN: return Vector4(0, 1, 0, 0)
		BaseMaterial3D.TEXTURE_CHANNEL_BLUE: return Vector4(0, 0, 1, 0)
		BaseMaterial3D.TEXTURE_CHANNEL_ALPHA: return Vector4(0, 0, 0, 1)
		BaseMaterial3D.TEXTURE_CHANNEL_GRAYSCALE: return Vector4(0.333333, 0.333333, 0.333333, 0)
	return Vector4(1, 0, 0, 0)
