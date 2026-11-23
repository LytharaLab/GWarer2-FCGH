#pragma once

#include <materialsystem/imesh.h>
#include <istudiorender.h>
#include "meshutils.h"		// Fixes linker errors
#include "flex_solver.h"
#include <array>
#include <memory>
#include <vector>

#define VERTEX_GWATER2 VERTEX_POSITION | VERTEX_NORMAL | VERTEX_TEXCOORD0_2D

IMatRenderContext* gwater_render_context();
VMatrix gwater_view_projection(IMatRenderContext& context);

struct FlexRenderGeometry;
struct FlexRenderGeometryPool;
struct FlexRenderMeshes;
class ThreadPool;

class FlexRenderer {
private:
	enum class MeshKind : size_t { water, diffuse, cloth };

	std::shared_ptr<FlexRenderGeometryPool> geometry_pool;
	std::shared_ptr<FlexRenderGeometry> geometry;
	std::shared_ptr<FlexRenderMeshes> meshes;
	std::unique_ptr<ThreadPool> workers;
	std::vector<int> pvs_water;
	std::vector<int> pvs_diffuse;

	void draw(MeshKind kind);
	friend struct FlexRenderMeshes;

public:
	FlexRenderer();
	~FlexRenderer();
	void draw_water();
	void draw_diffuse();
	void draw_cloth();

	std::array<bool, 3> build_meshes(FlexSolver* flex, float diffuse_radius, bool cull);
#ifdef GWATER2_CGH
	static bool is_render_context_queued();
	static bool can_render_in_current_context();
#endif
};
