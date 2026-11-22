#pragma once

#include <materialsystem/imesh.h>
#include <istudiorender.h>
#include "meshutils.h"		// Fixes linker errors
#include "flex_solver.h"
#include <vector>

#define VERTEX_GWATER2 VERTEX_POSITION | VERTEX_NORMAL | VERTEX_TEXCOORD0_2D

struct FlexRenderVertex {
	Vector position;
	Vector normal;
	float u;
	float v;
};

class FlexRenderer {
private:
	std::vector<FlexRenderVertex> water_vertices;
	std::vector<FlexRenderVertex> diffuse_vertices;
	std::vector<FlexRenderVertex> cloth_vertices;
public:
	void draw_water();
	void draw_diffuse();
	void draw_cloth();

	void build_meshes(FlexSolver* flex, float diffuse_radius, bool cull);
#ifdef GWATER2_CGH
	static bool is_render_context_queued();
#endif
};
