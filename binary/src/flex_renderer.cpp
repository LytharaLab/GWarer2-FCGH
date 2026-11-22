#include "flex_renderer.h"
#include "cdll_client_int.h"

#ifdef GMOD_MAIN
extern IMaterialSystem* materials = nullptr;
#endif
extern IVEngineClient* engine = nullptr;

namespace {
constexpr float sqrt3 = 1.73205081f;
constexpr float water_u[] = {0.5f - sqrt3 / 2, 0.5f, 0.5f + sqrt3 / 2};
constexpr float water_v[] = {1, -0.5f, 1};
constexpr float cloth_u[] = {0, 0, 1, 1, 0, 1};
constexpr float cloth_v[] = {1, 0, 1, 1, 0, 0};

IMatRenderContext* get_render_context() {
#ifdef GWATER2_CGH
	// GMod Main Win64's VMaterialSystem080 has four fewer IAppSystem slots
	// than the SDK header. The SDK's GetRenderContext dispatches to Lock(),
	// which switches away from the queued context in the middle of the view.
	constexpr size_t get_render_context_slot = 98;
	using GetRenderContext = IMatRenderContext* (*)(IMaterialSystem*);
	const auto* vtable = *reinterpret_cast<GetRenderContext* const*>(materials);
	return vtable[get_render_context_slot](materials);
#else
	return materials->GetRenderContext();
#endif
}

bool particle_visible(const Vector& position, const VMatrix& view_projection, float margin, bool cull) {
	Vector4D clip;
	Vector4DMultiply(view_projection, Vector4D(position.x, position.y, position.z, 1), clip);
	if (clip.z < margin || -clip.x - clip.w > margin || clip.x - clip.w > margin ||
		-clip.y - clip.w > margin || clip.y - clip.w > margin)
		return false;

	return !cull || engine->IsBoxVisible(position, position);
}

Vector anisotropy_axis(const Vector4D& axis, float scale) {
	return axis.AsVector3D() * (scale * axis.w);
}

void draw_vertices(const std::vector<FlexRenderVertex>& vertices, VertexFormat_t format) {
	if (vertices.empty()) return;

	IMatRenderContext* context = get_render_context();
#ifdef GWATER2_CGH
	if (context->GetCallQueue()) return;
#endif
	// Source's dynamic index buffer limits each draw. Each triangle uses three indices.
	const int batch_size = Min(12288, context->GetMaxIndicesToRender()) / 3 * 3;
	if (batch_size <= 0) return;

	const bool model_format = format == MATERIAL_VERTEX_FORMAT_MODEL;
	const float userdata[4] = {};
	for (size_t first = 0; first < vertices.size();) {
		const int count = Min(static_cast<int>(vertices.size() - first), batch_size);
		IMesh* mesh = context->GetDynamicMeshEx(format);
		if (!mesh) return;

		CMeshBuilder builder;
		builder.Begin(mesh, MATERIAL_TRIANGLES, count / 3);
		for (int i = 0; i < count; ++i) {
			const auto& vertex = vertices[first + i];
			builder.Position3fv(vertex.position.Base());
			builder.Normal3fv(vertex.normal.Base());
			builder.TexCoord2f(0, vertex.u, vertex.v);
			if (model_format) {
				builder.UserData(userdata);
				builder.Color4ub(255, 255, 255, 255);
			}
			builder.AdvanceVertex();
		}
		builder.End(false, true);
		first += count;
	}
}
}

void FlexRenderer::build_meshes(FlexSolver* flex, float diffuse_radius, bool cull) {
	water_vertices.clear();
	diffuse_vertices.clear();
	cloth_vertices.clear();

	const int active_particles = flex->get_active_particles();
	if (active_particles == 0) return;

	IMatRenderContext* context = get_render_context();
#ifdef GWATER2_CGH
	if (context->GetCallQueue()) return;
#endif
	Vector eye_pos;
	context->GetWorldSpaceCameraPosition(&eye_pos);
	VMatrix view, projection, view_projection;
	context->GetMatrix(MATERIAL_VIEW, &view);
	context->GetMatrix(MATERIAL_PROJECTION, &projection);
	MatrixMultiply(projection, view, view_projection);

	const auto& hosts = flex->hosts;
	const float radius = flex->get_parameter("radius");
	const bool use_anisotropy = flex->get_parameter("anisotropy_scale") != 0;
	const float anisotropy_scale = 5.f / radius;
	for (int i = 0; i < active_particles; ++i) {
		const int index = hosts.particle_active[i];
		if (hosts.particle_phase[index] != FlexPhase::WATER) continue;
		const Vector position = hosts.particle_smooth[index].AsVector3D();
		if (!particle_visible(position, view_projection, radius, cull)) continue;

		const Vector forward = (position - eye_pos).Normalized();
		const Vector right = forward.Cross(Vector(0, 0, 1)).Normalized();
		const Vector up = forward.Cross(right);
		Vector axis0(0, 0, 0), axis1(0, 0, 0), axis2(0, 0, 0);
		if (use_anisotropy) {
			axis0 = anisotropy_axis(hosts.particle_ani0[index], anisotropy_scale);
			axis1 = anisotropy_axis(hosts.particle_ani1[index], anisotropy_scale);
			axis2 = anisotropy_axis(hosts.particle_ani2[index], anisotropy_scale);
		}
		for (int vertex = 0; vertex < 3; ++vertex) {
			const float u = water_u[vertex], v = water_v[vertex];
			Vector local = (right * (u - 0.5f) + up * (v - 0.5f)) * radius * 0.2f;
			local += axis0 * local.Dot(axis0) + axis1 * local.Dot(axis1) + axis2 * local.Dot(axis2);
			water_vertices.push_back({position + local, -forward, u, v});
		}
	}

	const int active_diffuse = flex->get_active_diffuse();
	if (active_diffuse > 0) {
		const float scale = radius / flex->get_parameter("diffuse_lifetime") * diffuse_radius;
		for (int i = 0; i < active_diffuse; ++i) {
			const auto& particle = hosts.diffuse_pos[i];
			const Vector position = particle.AsVector3D();
			if (!particle_visible(position, view_projection, 0, cull)) continue;

			Vector stretch = hosts.diffuse_vel[i].AsVector3D() * 0.03f;
			if (stretch.LengthSqr() > 4) stretch = stretch.Normalized() * 2;
			const Vector forward = (position - eye_pos).Normalized();
			const Vector right = forward.Cross(Vector(0, 0, 1)).Normalized();
			const Vector up = forward.Cross(right);
			for (int vertex = 0; vertex < 3; ++vertex) {
				const float u = water_u[vertex], v = water_v[vertex];
				Vector local = (right * (u - 0.5f) + up * (v - 0.5f)) * (scale * particle.w);
				const Vector normal = local.Normalized() - forward;
				local += stretch * local.Dot(stretch);
				diffuse_vertices.push_back({position + local, normal, u, v});
			}
		}
	}

	const int active_triangles = flex->get_active_triangles();
	for (int triangle = 0; triangle < active_triangles; ++triangle) {
		for (int vertex = 0; vertex < 3; ++vertex) {
			const int index = hosts.triangle_indices[triangle * 3 + vertex];
			const int uv_index = triangle % 2 * 3 + vertex;
			cloth_vertices.push_back({
				hosts.particle_smooth[index].AsVector3D(),
				-hosts.triangle_normals[index].AsVector3D(),
				cloth_u[uv_index], cloth_v[uv_index]
			});
		}
	}
}

void FlexRenderer::draw_water() {
	draw_vertices(water_vertices, VERTEX_GWATER2);
}

void FlexRenderer::draw_diffuse() {
	draw_vertices(diffuse_vertices, MATERIAL_VERTEX_FORMAT_MODEL);
}

void FlexRenderer::draw_cloth() {
	draw_vertices(cloth_vertices, MATERIAL_VERTEX_FORMAT_MODEL);
}

#ifdef GWATER2_CGH
bool FlexRenderer::is_render_context_queued() {
	return get_render_context()->GetCallQueue() != nullptr;
}
#endif
