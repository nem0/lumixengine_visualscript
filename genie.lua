project "visualscript"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_VISUALSCRIPT" }
	links { "engine" }
	useLua()
	defaultConfigurations()

linkPlugin("visualscript")