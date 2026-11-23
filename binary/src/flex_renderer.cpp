#include "flex_renderer.h"
#include "cdll_client_int.h"
#include "tier1/callqueue.h"
#include "ThreadPool.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef GMOD_MAIN
extern IMaterialSystem* materials = nullptr;
#endif
extern IVEngineClient* engine = nullptr;

IMatRenderContext* gwater_render_context() {
#ifdef GWATER2_CGH
	// Main Win64 omits four IAppSystem entries. The SDK slot calls Lock(),
	// switching the material system away from the active render context.
	constexpr size_t get_render_context_slot = 98;
	using GetRenderContext = IMatRenderContext* (*)(IMaterialSystem*);
	const auto* vtable = *reinterpret_cast<GetRenderContext* const*>(materials);
	return vtable[get_render_context_slot](materials);
#else
	return materials->GetRenderContext();
#endif
}

VMatrix gwater_view_projection(IMatRenderContext& context) {
	VMatrix view, projection, view_projection;
	context.GetMatrix(MATERIAL_VIEW, &view);
	context.GetMatrix(MATERIAL_PROJECTION, &projection);
	MatrixMultiply(projection, view, view_projection);
	return view_projection;
}

namespace {
#ifdef GWATER2_CGH
using MaterialQueueSubmit = void (*)(ICallQueue*, CFunctor*);

MaterialQueueSubmit material_queue_submit(IMatRenderContext& context) {
	// Main Win64 returns a concrete CMatCallQueue. Its Flush implementation
	// tail-calls the queue's append routine. Validate both before calling it.
	const auto* const* vtable = *reinterpret_cast<const unsigned char* const* const*>(&context);
	const auto* flush = vtable[2];
	const unsigned char get_queue[] = {0x48, 0x8d, 0x81, 0xb0, 0x02, 0x00, 0x00, 0xc3};
	const unsigned char flush_prefix[] = {
		0x4c, 0x8b, 0x91, 0xd8, 0x02, 0x00, 0x01, 0x4c, 0x8d, 0x81, 0xb0, 0x02, 0x00, 0x00
	};
	constexpr size_t jump_offset = 0x59;
	constexpr size_t get_call_queue_slot = 145;
	if (std::memcmp(vtable[get_call_queue_slot], get_queue, sizeof(get_queue)) != 0 ||
		std::memcmp(flush, flush_prefix, sizeof(flush_prefix)) != 0 || flush[jump_offset] != 0xe9)
		return nullptr;

	int32_t relative;
	std::memcpy(&relative, flush + jump_offset + 1, sizeof(relative));
	const auto* append = flush + jump_offset + 5 + relative;
	const unsigned char append_prefix[] = {
		0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x41, 0x10, 0x4c, 0x8d, 0x49, 0x10
	};
	const unsigned char append_node[] = {0x4c, 0x89, 0x00, 0x4c, 0x89, 0x41, 0x08};
	if (std::memcmp(append, append_prefix, sizeof(append_prefix)) != 0 ||
		std::memcmp(append + 0x33, append_node, sizeof(append_node)) != 0)
		return nullptr;
	return reinterpret_cast<MaterialQueueSubmit>(append);
}
#endif

template <typename Callback>
struct MaterialCommand final : CFunctorBase {
	Callback callback;

	explicit MaterialCommand(Callback call) : callback(std::move(call)) {}
	void operator()() override { callback(); }
};

template <typename Callback>
bool submit_material_command(Callback callback) {
	auto& context = *gwater_render_context();
	if (auto* queue = context.GetCallQueue()) {
#ifdef GWATER2_CGH
		const auto append = material_queue_submit(context);
		if (!append) return false;
		append(queue, new MaterialCommand<Callback>(std::move(callback)));
#else
		queue->QueueFunctor(new MaterialCommand<Callback>(std::move(callback)));
#endif
	} else {
		callback();
	}
	return true;
}

struct FlexRenderVertex {
	Vector position;
	Vector normal;
	float u;
	float v;
};
static_assert(sizeof(FlexRenderVertex) == 8 * sizeof(float));

using Vertices = std::vector<FlexRenderVertex>;
using VertexStreams = std::array<Vertices, 3>;
constexpr int max_sections = 8;
constexpr float sqrt3 = 1.73205081f;
constexpr float water_u[] = {0.5f - sqrt3 / 2, 0.5f, 0.5f + sqrt3 / 2};
constexpr float water_v[] = {1, -0.5f, 1};
constexpr float cloth_u[] = {0, 0, 1, 1, 0, 1};
constexpr float cloth_v[] = {1, 0, 1, 1, 0, 0};

float project(const float* row, const Vector& position) {
	return row[0] * position.x + row[1] * position.y + row[2] * position.z + row[3];
}

bool inside_frustum(const Vector& position, const VMatrix& view_projection, float margin) {
	if (project(view_projection[2], position) < margin) return false;
	const float clip_w = project(view_projection[3], position);
	return std::abs(project(view_projection[0], position)) - clip_w <= margin &&
		std::abs(project(view_projection[1], position)) - clip_w <= margin;
}

Vector normalize(const Vector& vector) {
	return vector * (1.f / (std::sqrt(vector.LengthSqr()) + FLT_EPSILON));
}

Vector anisotropy_axis(const Vector4D& axis, float scale) {
	return axis.AsVector3D() * (scale * axis.w);
}

struct ViewInput {
	const FlexHosts& hosts;
	const VMatrix& projection;
	const Vector& eye;
	float radius;
	float diffuse_scale;
	bool anisotropy;
};

void build_water(Vertices& vertices, const ViewInput& input, const int* visible, int first, int last) {
	const auto& hosts = input.hosts;
	const float radius = input.radius;
	const float anisotropy_scale = 5.f / radius;
	vertices.clear();
	vertices.reserve(static_cast<size_t>(last - first) * 3);
	for (int i = first; i < last; ++i) {
		const int index = visible ? visible[i] : hosts.particle_active[i];
		const Vector& position = hosts.particle_smooth[index].AsVector3D();
		if (!visible && (hosts.particle_phase[index] != FlexPhase::WATER ||
			!inside_frustum(position, input.projection, radius))) continue;

		const Vector forward = normalize(position - input.eye);
		const Vector right = normalize(Vector(forward.y, -forward.x, 0));
		Vector horizontal = right * (radius * (0.1f * sqrt3));
		Vector vertical = forward.Cross(right) * (radius * 0.1f);
		if (input.anisotropy) {
			const Vector axis0 = anisotropy_axis(hosts.particle_ani0[index], anisotropy_scale);
			const Vector axis1 = anisotropy_axis(hosts.particle_ani1[index], anisotropy_scale);
			const Vector axis2 = anisotropy_axis(hosts.particle_ani2[index], anisotropy_scale);
			// The deformation is linear, so each billboard needs just two
			// anisotropy transforms for its three vertices.
			horizontal += axis0 * horizontal.Dot(axis0) + axis1 * horizontal.Dot(axis1) + axis2 * horizontal.Dot(axis2);
			vertical += axis0 * vertical.Dot(axis0) + axis1 * vertical.Dot(axis1) + axis2 * vertical.Dot(axis2);
		}
		const Vector bottom = position + vertical;
		const Vector normal = -forward;
		vertices.push_back({bottom - horizontal, normal, water_u[0], water_v[0]});
		vertices.push_back({position - vertical * 2.f, normal, water_u[1], water_v[1]});
		vertices.push_back({bottom + horizontal, normal, water_u[2], water_v[2]});
	}
}

void build_diffuse(Vertices& vertices, const ViewInput& input, const int* visible, int first, int last) {
	const auto& hosts = input.hosts;
	vertices.clear();
	vertices.reserve(static_cast<size_t>(last - first) * 3);
	for (int i = first; i < last; ++i) {
		const int index = visible ? visible[i] : i;
		const auto& particle = hosts.diffuse_pos[index];
		const Vector& position = particle.AsVector3D();
		if (!visible && !inside_frustum(position, input.projection, 0)) continue;

		Vector stretch = hosts.diffuse_vel[index].AsVector3D() * 0.03f;
		if (stretch.LengthSqr() > 4) stretch = normalize(stretch) * 2.f;
		const Vector forward = normalize(position - input.eye);
		const Vector right = normalize(Vector(forward.y, -forward.x, 0));
		const Vector up = forward.Cross(right);
		const float scale = input.diffuse_scale * particle.w;
		for (int vertex = 0; vertex < 3; ++vertex) {
			const float u = water_u[vertex], v = water_v[vertex];
			Vector local = (right * (u - 0.5f) + up * (v - 0.5f)) * scale;
			const Vector normal = normalize(local) - forward;
			local += stretch * local.Dot(stretch);
			vertices.push_back({position + local, normal, u, v});
		}
	}
}

void build_cloth(Vertices& vertices, const FlexHosts& hosts, int first, int last) {
	vertices.clear();
	vertices.reserve(static_cast<size_t>(last - first) * 3);
	for (int triangle = first; triangle < last; ++triangle) {
		for (int vertex = 0; vertex < 3; ++vertex) {
			const int index = hosts.triangle_indices[triangle * 3 + vertex];
			const int uv_index = triangle % 2 * 3 + vertex;
			vertices.push_back({
				hosts.particle_smooth[index].AsVector3D(),
				-hosts.triangle_normals[index].AsVector3D(),
				cloth_u[uv_index], cloth_v[uv_index]
			});
		}
	}
}

void write_vertices(CMeshBuilder& builder, const FlexRenderVertex* vertices, int count, bool model_format) {
	const auto position = reinterpret_cast<uintptr_t>(builder.m_pPosition);
	constexpr int stride = sizeof(FlexRenderVertex);
	// Some Source backends pad or rearrange attributes.
	if (!model_format && builder.m_CompressionType == VERTEX_COMPRESSION_NONE &&
		builder.m_ActualVertexSize == stride && builder.m_VertexSize_Position == stride &&
		builder.m_VertexSize_Normal == stride && builder.m_VertexSize_TexCoord[0] == stride &&
		reinterpret_cast<uintptr_t>(builder.m_pNormal) == position + offsetof(FlexRenderVertex, normal) &&
		reinterpret_cast<uintptr_t>(builder.m_pTexCoord[0]) == position + offsetof(FlexRenderVertex, u)) {
		std::memcpy(builder.m_pPosition, vertices, count * sizeof(FlexRenderVertex));
		builder.AdvanceVertices(count);
		return;
	}

	const float userdata[4] = {};
	for (int i = 0; i < count; ++i) {
		const auto& vertex = vertices[i];
		builder.Position3fv(vertex.position.Base());
		builder.Normal3fv(vertex.normal.Base());
		builder.TexCoord2f(0, vertex.u, vertex.v);
		if (model_format) {
			builder.UserData(userdata);
			builder.Color4ub(255, 255, 255, 255);
		}
		builder.AdvanceVertex();
	}
}

void draw_dynamic(IMatRenderContext& context, const Vertices& vertices, VertexFormat_t format, bool model_format) {
	const int batch_size = Min(12288, context.GetMaxIndicesToRender()) / 3 * 3;
	if (batch_size <= 0) return;
	for (size_t first = 0; first < vertices.size(); first += batch_size) {
		const int count = Min(static_cast<int>(vertices.size() - first), batch_size);
		IMesh* mesh = context.GetDynamicMeshEx(format);
		if (!mesh) return;
		CMeshBuilder builder;
		builder.Begin(mesh, MATERIAL_TRIANGLES, count / 3);
		write_vertices(builder, vertices.data() + first, count, model_format);
		builder.End(false, true);
	}
}
}

struct FlexRenderGeometry {
	std::array<VertexStreams, max_sections> sections;
	int section_count = 0;
	std::array<bool, 3> visible{};

	bool matches(const FlexRenderGeometry& other, size_t stream) const {
		if (section_count != other.section_count) return false;
		for (int section = 0; section < section_count; ++section) {
			const auto& current = sections[section][stream];
			const auto& previous = other.sections[section][stream];
			if (current.size() != previous.size() || (!current.empty() &&
				std::memcmp(current.data(), previous.data(), current.size() * sizeof(FlexRenderVertex)) != 0))
				return false;
		}
		return true;
	}
};

struct FlexRenderGeometryPool : std::enable_shared_from_this<FlexRenderGeometryPool> {
	std::mutex mutex;
	std::array<std::unique_ptr<FlexRenderGeometry>, 3> available;

	std::shared_ptr<FlexRenderGeometry> acquire() {
		std::unique_ptr<FlexRenderGeometry> geometry;
		{
			std::lock_guard lock(mutex);
			for (auto& entry : available) {
				if (!entry) continue;
				geometry = std::move(entry);
				break;
			}
		}
		if (!geometry) geometry = std::make_unique<FlexRenderGeometry>();
		return {geometry.release(), [pool = shared_from_this()](FlexRenderGeometry* data) {
			std::unique_ptr<FlexRenderGeometry> owned(data);
			std::lock_guard lock(pool->mutex);
			for (auto& entry : pool->available) {
				if (entry) continue;
				entry = std::move(owned);
				return;
			}
		}};
	}
};

// Only the material command consumer mutates the GPU meshes. Draw commands
// retain the immutable view data while Lua prepares the next view.
struct FlexRenderMeshes {
	struct Batch {
		IMesh* mesh = nullptr;
		int capacity = 0;
		int count = 0;
	};

	struct MeshSet {
		std::vector<Batch> batches;
		std::shared_ptr<const FlexRenderGeometry> uploaded;
		size_t live_batches = 0;
	};

	std::array<MeshSet, 3> sets;
	static constexpr int batch_size = 32766;

	static bool upload_section(IMatRenderContext& context, MeshSet& set, const Vertices& vertices,
		VertexFormat_t format, bool model_format) {
		for (size_t first = 0; first < vertices.size(); first += batch_size) {
			if (set.live_batches == set.batches.size()) set.batches.emplace_back();
			auto& batch = set.batches[set.live_batches++];
			batch.count = Min(static_cast<int>(vertices.size() - first), batch_size);
			CMeshBuilder builder;
			if (batch.capacity < batch.count) {
				if (batch.mesh) context.DestroyStaticMesh(batch.mesh);
				batch.mesh = context.CreateStaticMesh(format, "GWater2FCGH");
				batch.capacity = 0;
				if (!batch.mesh) return false;

				// Preserve sequential indices and leave space for the last batch to grow.
				constexpr int growth = 1536;
				batch.capacity = Min((batch.count + growth - 1) / growth * growth, batch_size);
				builder.Begin(batch.mesh, MATERIAL_TRIANGLES, batch.capacity / 3);
				write_vertices(builder, vertices.data() + first, batch.count, model_format);
				builder.AdvanceVertices(batch.capacity - batch.count);
				builder.End();
			} else {
				builder.BeginModify(batch.mesh, 0, batch.count, 0, 0);
				write_vertices(builder, vertices.data() + first, batch.count, model_format);
				builder.EndModify();
			}
		}
		return true;
	}

	void draw(const std::shared_ptr<FlexRenderGeometry>& geometry, FlexRenderer::MeshKind kind) {
		const size_t index = static_cast<size_t>(kind);
		auto& set = sets[index];
		const bool model_format = kind != FlexRenderer::MeshKind::water;
		const VertexFormat_t format = model_format ? MATERIAL_VERTEX_FORMAT_MODEL : (VERTEX_GWATER2);
		auto& context = *gwater_render_context();
		if (set.uploaded != geometry && (!set.uploaded || !geometry->matches(*set.uploaded, index))) {
			set.uploaded.reset();
			set.live_batches = 0;
			for (int section = 0; section < geometry->section_count; ++section) {
				if (upload_section(context, set, geometry->sections[section][index], format, model_format)) continue;
				for (int fallback = 0; fallback < geometry->section_count; ++fallback)
					draw_dynamic(context, geometry->sections[fallback][index], format, model_format);
				return;
			}
		}
		set.uploaded = geometry;
		for (size_t i = 0; i < set.live_batches; ++i) {
			const auto& batch = set.batches[i];
			batch.mesh->Draw(0, batch.count);
		}
	}

	void clear() {
		auto& context = *gwater_render_context();
		for (auto& set : sets) {
			for (const auto& batch : set.batches) {
				if (batch.mesh) context.DestroyStaticMesh(batch.mesh);
			}
			set = {};
		}
	}
};

FlexRenderer::FlexRenderer() = default;

std::array<bool, 3> FlexRenderer::build_meshes(FlexSolver* flex, float diffuse_radius, bool cull) {
	geometry.reset();
	const int active_particles = flex->get_active_particles();
	if (active_particles == 0) return {};

	auto& context = *gwater_render_context();
	Vector eye;
	context.GetWorldSpaceCameraPosition(&eye);
	const VMatrix projection = gwater_view_projection(context);
	const float radius = flex->get_parameter("radius");
	const int active_diffuse = flex->get_active_diffuse();
	const int active_triangles = flex->get_active_triangles();
	const ViewInput input{
		flex->hosts, projection, eye, radius,
		active_diffuse > 0 ? radius / flex->get_parameter("diffuse_lifetime") * diffuse_radius : 0.f,
		flex->get_parameter("anisotropy_scale") != 0
	};

	// Read Source's PVS from the Lua thread and keep the same particle order.
	if (cull) {
		pvs_water.clear();
		pvs_diffuse.clear();
		pvs_water.reserve(active_particles);
		pvs_diffuse.reserve(active_diffuse);
		for (int i = 0; i < active_particles; ++i) {
			const int index = input.hosts.particle_active[i];
			const Vector& position = input.hosts.particle_smooth[index].AsVector3D();
			if (input.hosts.particle_phase[index] == FlexPhase::WATER &&
				inside_frustum(position, projection, radius) && engine->IsBoxVisible(position, position))
				pvs_water.push_back(index);
		}
		for (int i = 0; i < active_diffuse; ++i) {
			const Vector& position = input.hosts.diffuse_pos[i].AsVector3D();
			if (inside_frustum(position, projection, 0) && engine->IsBoxVisible(position, position))
				pvs_diffuse.push_back(i);
		}
	}
	const int water_count = cull ? static_cast<int>(pvs_water.size()) : active_particles;
	const int diffuse_count = cull ? static_cast<int>(pvs_diffuse.size()) : active_diffuse;
	if (water_count == 0 && diffuse_count == 0 && active_triangles == 0) return {};

	if (!geometry_pool) geometry_pool = std::make_shared<FlexRenderGeometryPool>();
	geometry = geometry_pool->acquire();

	static const int max_workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 2, 1, max_sections);
	const int work = Max(Max(water_count, diffuse_count), active_triangles);
	constexpr int particles_per_section = 8192;
	const int sections = std::clamp(work / particles_per_section, 1, max_workers);
	geometry->section_count = sections;
	const auto build_section = [&](int section) {
		const auto boundary = [sections](int count, int index) {
			return static_cast<int>(static_cast<int64_t>(count) * index / sections);
		};
		auto& [water, diffuse, cloth] = geometry->sections[section];
		build_water(water, input, cull ? pvs_water.data() : nullptr,
			boundary(water_count, section), boundary(water_count, section + 1));
		build_diffuse(diffuse, input, cull ? pvs_diffuse.data() : nullptr,
			boundary(diffuse_count, section), boundary(diffuse_count, section + 1));
		build_cloth(cloth, input.hosts, boundary(active_triangles, section), boundary(active_triangles, section + 1));
	};

	if (sections > 1) {
		if (!workers) workers = std::make_unique<ThreadPool>(max_workers - 1);
		std::array<std::future<void>, max_sections - 1> jobs;
		try {
			for (int section = 1; section < sections; ++section)
				jobs[section - 1] = workers->enqueue(build_section, section);
			build_section(0);
			for (int section = 1; section < sections; ++section)
				jobs[section - 1].get();
		} catch (...) {
			// Submitted jobs still borrow the solver and view until they finish.
			for (auto& job : jobs) {
				if (job.valid()) job.wait();
			}
			throw;
		}
	} else {
		build_section(0);
	}

	geometry->visible = {};
	for (int section = 0; section < sections; ++section) {
		for (size_t stream = 0; stream < geometry->visible.size(); ++stream)
			geometry->visible[stream] |= !geometry->sections[section][stream].empty();
	}
	return geometry->visible;
}

FlexRenderer::~FlexRenderer() {
	if (!meshes) return;
	if (!submit_material_command([cache = meshes] { cache->clear(); })) meshes->clear();
}

void FlexRenderer::draw(MeshKind kind) {
	if (!geometry || !geometry->visible[static_cast<size_t>(kind)]) return;
	if (!meshes) meshes = std::make_shared<FlexRenderMeshes>();
	submit_material_command([cache = meshes, data = geometry, kind] { cache->draw(data, kind); });
}

void FlexRenderer::draw_water() {
	draw(MeshKind::water);
}

void FlexRenderer::draw_diffuse() {
	draw(MeshKind::diffuse);
}

void FlexRenderer::draw_cloth() {
	draw(MeshKind::cloth);
}

#ifdef GWATER2_CGH
bool FlexRenderer::is_render_context_queued() {
	return gwater_render_context()->GetCallQueue() != nullptr;
}

bool FlexRenderer::can_render_in_current_context() {
	auto& context = *gwater_render_context();
	return !context.GetCallQueue() || material_queue_submit(context);
}
#endif
