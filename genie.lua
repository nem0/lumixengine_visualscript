project "visualscript"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"external/**.c",
		"external/**.h",
		"genie.lua"
	}
	defines { "BUILDING_VISUALSCRIPT" }
	links { "engine", "core" }
	if build_studio then
		links { "editor" }
	end
	useLua()
	defaultConfigurations()

linkPlugin("visualscript")