if plugin "visualscript" then
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"external/**.c",
		"external/**.h",
		"genie.lua"
	}
	defines { "BUILDING_VISUALSCRIPT" }
	dynamic_link_plugin { "engine", "core" }
	if build_studio then
		dynamic_link_plugin { "editor" }
	end
end