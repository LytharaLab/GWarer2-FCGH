-- The GWater 2 Render Pipeline

local function get_gwater_rt(name, mult, depth) 
	mult = mult or 1
	return GetRenderTargetEx(name, ScrW() * mult, ScrH() * mult,
		RT_SIZE_DEFAULT,
		depth or 0,
		2 + 4 + 8 + 256,
		0,
		IMAGE_FORMAT_RGBA16161616F
	)
end

local cache_screen0 = render.GetScreenEffectTexture()
local cache_screen1 = render.GetScreenEffectTexture(1)
local cache_mipmap = get_gwater_rt("gwater2_mipmap", 1 / 1)
local cache_absorption = get_gwater_rt("gwater2_absorption", 1 / 4, MATERIAL_RT_DEPTH_NONE)
local cache_normals = get_gwater_rt("gwater2_normals", 1 / 1, MATERIAL_RT_DEPTH_SEPARATE)
local cache_blur = get_gwater_rt("gwater2_blur", 1 / 2)

local water = Material("gwater2/finalpass")
local water_blur = Material("gwater2/smooth")
local water_volumetric = Material("gwater2/volumetric")
local water_normals = Material("gwater2/normals")
local water_bubble = Material("gwater2/bubble")	-- bubbles
local water_mist = Material("gwater2/mist")
local black = Material("gwater2/black")
local cloth = Material("gwater2/cloth")

local blur_passes = CreateClientConVar("gwater2_blur_passes", "3", true)
local blur_scale = CreateClientConVar("gwater2_blur_scale", "1", true)

local debug_absorption = CreateClientConVar("gwater2_debug_absorption", "0", false)
local debug_normals = CreateClientConVar("gwater2_debug_normals", "0", false)
local debug_mipmap = CreateClientConVar("gwater2_debug_mipmap", "0", false)

local queue_mode = GetConVar("mat_queue_mode")
local saved_queue_mode
local restore_queue_at

local function can_render()
	local renderer = gwater2.renderer
	return not renderer.CanRenderInCurrentContext or renderer:CanRenderInCurrentContext()
end

local function restore_queue()
	if saved_queue_mode ~= nil and queue_mode:GetInt() == 0 then
		RunConsoleCommand("mat_queue_mode", tostring(saved_queue_mode))
	end
	saved_queue_mode, restore_queue_at = nil, nil
end

-- The stock queue stays enabled on supported builds. Keep the serial fallback
-- for a material-system revision with an unrecognized queue implementation.
hook.Add("Think", "gwater2_material_queue", function()
	if not queue_mode then return end
	if gwater2.solver:GetActiveParticles() > 0 then
		restore_queue_at = nil
		if saved_queue_mode == nil and queue_mode:GetInt() ~= 0 and not can_render() then
			saved_queue_mode = queue_mode:GetInt()
			RunConsoleCommand("mat_queue_mode", "0")
		end
	elseif saved_queue_mode ~= nil then
		restore_queue_at = restore_queue_at or SysTime() + 1
		if SysTime() >= restore_queue_at then restore_queue() end
	end
end)
hook.Add("ShutDown", "gwater2_material_queue", restore_queue)

local clear_color = Color(0, 0, 0, 0)
local cubemap_model = {model = "models/shadertest/envballs.mdl"}
local light_model = {model = "models/shadertest/vertexlit.mdl"}

local function setup_lighting(pos)
	local angle = EyeAngles()
	cubemap_model.pos, cubemap_model.angle = pos, angle
	light_model.pos, light_model.angle = pos, angle
	render.OverrideColorWriteEnable(true, false)
	render.OverrideDepthEnable(true, false)
	render.Model(cubemap_model)
	render.Model(light_model)
	render.OverrideDepthEnable(false, false)
	render.OverrideColorWriteEnable(false, false)
end

local function draw_water() gwater2.renderer:DrawWater() end
local function draw_diffuse() gwater2.renderer:DrawDiffuse() end
local function draw_cloth() gwater2.renderer:DrawCloth() end

local function do_cloth()
	setup_lighting(gwater2.cloth_pos)
	render.SetMaterial(cloth)
	draw_cloth()
	render.RenderFlashlights(draw_cloth)
end

local function do_absorption(alpha)
	render.UpdateScreenEffectTexture()	-- _rt_framebuffer is used in refraction shader

	-- depth absorption (disabled when opaque liquids are enabled)
	if water_volumetric:GetFloat("$alpha") != 0 and alpha > 0 and alpha < 255 then
		-- ANTIALIAS FIX! (courtesy of Xenthio)
			-- how it works: 
			-- Clear the main rendertarget, keeping depth
			-- Render to main buffer (still has depth), and copy the contents to another rendertarget
			-- Restore the main buffer

		-- clear screen w/o intruding translucents depth buffer
		render.SetMaterial(black)
		render.DrawScreenQuad()

		render.SetMaterial(water_volumetric)
		draw_water()
		render.CopyTexture(render.GetRenderTarget(), cache_absorption)
		render.DrawTextureToScreen(cache_screen0)

	else
		-- no absorption calculations, so just use solid color
		render.PushRenderTarget(cache_absorption)
		render.Clear(15, 15, 15, 10)
		render.PopRenderTarget()
	end
end

local function do_diffuse_inside()
	-- The refraction texture includes bubbles, while the main scene does not.
	render.SetMaterial(water_bubble)
	render.UpdateScreenEffectTexture(1)
	draw_diffuse()
	render.RenderFlashlights(draw_diffuse)
	render.CopyTexture(render.GetRenderTarget(), cache_screen0)
	render.DrawTextureToScreen(cache_screen1)
end

local function do_normals(radius, passes)
	-- stencils are used to only blur the pixels we want
	render.SetStencilWriteMask(0xFF)
	render.SetStencilTestMask(0xFF)
	render.SetStencilReferenceValue(1)
	render.SetStencilCompareFunction(STENCIL_ALWAYS)
	render.SetStencilFailOperation(STENCIL_KEEP)
	render.SetStencilZFailOperation(STENCIL_KEEP)
	render.SetStencilPassOperation(STENCIL_REPLACE)
	render.ClearStencil()
	render.SetStencilEnable(true)

	-- grab normals
	water_normals:SetFloat("$radius", radius / 2)
	render.SetMaterial(water_normals)
	render.PushRenderTarget(cache_normals)
	render.SetRenderTargetEx(1, cache_mipmap)
	render.ClearDepth()
	draw_water()
	render.PopRenderTarget()
	render.SetRenderTargetEx(1, nil)

	render.SetStencilCompareFunction(STENCIL_EQUAL)
	render.SetStencilEnable(false)
	
	if passes < 1 then return end

	local scrw = 1 / ScrW()
	local scrh = 1 / ScrH()
	local scale_base = 0.3 * blur_scale:GetFloat()

	water_blur:SetFloat("$radius", radius)
	water_blur:SetTexture("$depthtexture", cache_mipmap)
	render.SetMaterial(water_blur)
	for i = 1, passes do
		local scale = scale_base / i

		-- Blur X
		water_blur:SetTexture("$normaltexture", cache_normals)
		water_blur:SetVector("$scrs", Vector(scale * scrw, 0))
		render.SetStencilEnable(false)	-- cache_blur doesn't have a stencil buffer set up, so we cant use it
		render.PushRenderTarget(cache_blur)
		render.DrawScreenQuad()
		render.PopRenderTarget()
		
		-- Blur Y
		
		water_blur:SetTexture("$normaltexture", cache_blur)
		water_blur:SetVector("$scrs", Vector(0, scale * scrh))
		render.SetStencilEnable(i < passes)	-- disable stencils on last pass
		render.PushRenderTarget(cache_normals)
		render.DrawScreenQuad()
		render.PopRenderTarget()
	end
	
	render.SetStencilEnable(false)
end

local function do_finalpass(radius, passes)
	-- Setup water material parameters
	water:SetFloat("$radius", radius * 1.5)
	water:SetFloat("$reflectance", passes ~= 0 and 0.7 or 0)
	water:SetTexture("$normaltexture", cache_normals)
	water:SetTexture("$depthtexture", cache_absorption)
	render.SetMaterial(water)
	draw_water()
	render.RenderFlashlights(draw_water)
end

-- vrmod does not render to the main RT, force enable mirror rendering
hook.Add("VRMod_Start", "gwater2_vrmodsupport", function(ply)
	if ply != LocalPlayer() then return end

	gwater2.options.render_mirrors:SetInt(1)
end)

-- gwater2 shader pipeline
local vrmod_material = Material("models/wireframe")
hook.Remove("RenderScene", "gwater2_render")
hook.Add("PostDrawOpaqueRenderables", "gwater2_render", function(depth, sky, sky3d)
	if depth or sky3d then return end
	if gwater2.solver:GetActiveParticles() < 1 then return end
	if not can_render() then return end

	local mirrors = gwater2.options.render_mirrors:GetInt()
	if mirrors == 0 and render.GetRenderTarget() then return end

	local has_water, has_diffuse, has_cloth = gwater2.renderer:BuildMeshes(gwater2.solver, 0.25, mirrors ~= 1)
	if not (has_water or has_diffuse or has_cloth) then return end
	if has_cloth then do_cloth() end
	setup_lighting(EyePos())
	if not (has_water or has_diffuse) then return end

	if vrmod and vrmod.IsPlayerInVR(LocalPlayer()) then
		render.SetMaterial(vrmod_material)
		draw_water()
		return
	end

	if has_water then
		render.ClearRenderTarget(cache_normals, clear_color)
		render.ClearRenderTarget(cache_mipmap, clear_color)
		local _, _, _, alpha = water:GetVector4D("$color2")
		do_absorption(alpha)
		if has_diffuse and alpha < 255 then do_diffuse_inside() end
		local radius, passes = gwater2.solver:GetParameter("radius"), blur_passes:GetInt()
		do_normals(radius, passes)
		do_finalpass(radius, passes)
	end
	if has_diffuse then
		render.SetMaterial(water_mist)
		draw_diffuse()
		render.RenderFlashlights(draw_diffuse)
	end
	if not has_water then return end

	-- Debug Draw
	local dbg = 0
	if debug_absorption:GetBool() then render.DrawTextureToScreenRect(cache_absorption, ScrW() * 0.75, (ScrH() / 4) * dbg, ScrW() / 4, ScrH() / 4); dbg = dbg + 1 end
	if debug_normals:GetBool() then render.DrawTextureToScreenRect(cache_normals, ScrW() * 0.75, (ScrH() / 4) * dbg, ScrW() / 4, ScrH() / 4); dbg = dbg + 1 end
	if debug_mipmap:GetBool() then render.DrawTextureToScreenRect(cache_mipmap, ScrW() * 0.75, (ScrH() / 4) * dbg, ScrW() / 4, ScrH() / 4); dbg = dbg + 1 end

	--render.DrawTextureToScreenRect(cache_absorption, 0, 0, ScrW() / 4, ScrH() / 4)
end)

--hook.Add("NeedsDepthPass", "gwater2_depth", function()
--	DOFModeHack(true)	-- fixes npcs and stuff dissapearing
--	return true
--end)
