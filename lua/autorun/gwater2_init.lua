AddCSLuaFile()

-- do return end

gwater2 = nil

if SERVER then
	include("gwater2_net.lua")
	include("gwater2_interactions.lua")

	AddCSLuaFile("menu/gwater2_params.lua")
	AddCSLuaFile("menu/gwater2_paramstabs.lua")
	AddCSLuaFile("menu/gwater2_presets.lua")
	AddCSLuaFile("menu/gwater2_styling.lua")
	AddCSLuaFile("menu/gwater2_util.lua")

	AddCSLuaFile("gwater2_interactions.lua")
	AddCSLuaFile("gwater2_net.lua")
	AddCSLuaFile("gwater2_shaders.lua")
	AddCSLuaFile("gwater2_menu.lua")
	AddCSLuaFile("gwater2_client.lua")
	return
end

-- multi language support
local lang = GetConVar("gmod_language"):GetString()
local function load_language(lang)
	local strings = file.Read("data_static/gwater2/locale/gwater2_".. lang .. ".txt", "THIRDPARTY")
	if not strings then return false end
	/*
	matches strings like this:
		"KEY"=[[
		VALUE
		]]
	*/
	for k, v in string.gmatch(strings, '"(.-)"=%[%[%s*(.-)%s*%]%]') do
		language.Add(k, v)
	end
	return true
end

load_language("en") -- fallback
if not load_language(lang) then
	print("[GWater2] Your language is unsupported. Falling back to english.")
end

local function gw2_error(text)
	ErrorNoHalt(text) -- log to problem menu
	chat.AddText(
		color_black, "[",
		Color(50, 255, 50), "G",
		color_white, "Water",
		Color(50, 150, 255), "2",
		color_black, "]: ",
		Color(250, 230, 20), language.GetPhrase("gwater2.error.chatlog")
	)
end

-- Sandbox builds its tool menu before CGH-LAPI finishes attaching on Think.
hook.Add("PopulateToolMenu", "gwater2_menu", function()
    spawnmenu.AddToolMenuOption("Utilities", "GWater2", "gwater2_menu", "Menu Options", "", "", function(panel)
		panel:ClearControls()
		panel:Button("Open Menu", "gwater2_menu")
        panel:KeyBinder("Menu Key", "gwater2_menukey")
	end)
end)

local hook_name = "GWater2FCGH.BindClient"
local deadline = SysTime() + 10
local next_attempt = 0

local function bind_client()
	local now = SysTime()
	if now < next_attempt then return end
	next_attempt = now + 0.25

	local result, err
	if cgh and cgh.Call then
		result, err = cgh.Call("gwater2.fcgh", "bind.client", "gwater2fcgh/bind.client/v1")
	end

	if result == "ready" then
		hook.Remove("Think", hook_name)
		print("[GWater2FCGH]: Loaded for Con's GMod Host with language: " .. lang)
		include("gwater2_client.lua")
	elseif result or now >= deadline then
		hook.Remove("Think", hook_name)
		local detail = result or "CGH-LAPI client bridge unavailable (" .. tostring(err) .. ")"
		gw2_error("[GWater2FCGH] " .. detail .. "\n")
	end
end

hook.Add("Think", hook_name, bind_client)
bind_client()
