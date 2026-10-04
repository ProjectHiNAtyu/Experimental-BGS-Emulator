newoption {
	trigger = "copy-to",
	description = "Optional, copy the built DLL to a custom folder after build.",
	value = "PATH"
}

newoption {
	trigger = "dev-build",
	description = "Enable development builds."
}

dependencies = {
	basePath = "./deps"
}

function dependencies.load()
	local dir = path.join(dependencies.basePath, "premake/*.lua")
	local deps = os.matchfiles(dir)
	for i, dep in pairs(deps) do
		dep = dep:gsub(".lua", "")
		require(dep)
	end
end

function dependencies.imports()
	for i, proj in pairs(dependencies) do
		if type(i) == 'number' then
			proj.import()
		end
	end
end

function dependencies.projects()
	for i, proj in pairs(dependencies) do
		if type(i) == 'number' then
			proj.project()
		end
	end
end

dependencies.load()

workspace "bgs_emu"
	startproject "bgs_emu"
	location "./build"
	objdir "%{wks.location}/obj"
	targetdir "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"

	configurations { "Debug", "Release" }

	language "C++"
	cppdialect "C++20"

	architecture "x86_64"
	platforms "x64"

	systemversion "latest"
	symbols "On"
	staticruntime "On"
	editandcontinue "Off"
	warnings "Extra"
	characterset "ASCII"

	if _OPTIONS["dev-build"] then
		defines { "DEV_BUILD" }
	end

	flags { "NoIncrementalLink", "NoMinimalRebuild", "MultiProcessorCompile", "No64BitChecks" }

	filter "platforms:x64"
		defines { "_WINDOWS", "WIN32" }
	filter {}

	filter "configurations:Release"
		optimize "Size"
		buildoptions { "/GL" }
		linkoptions { "/IGNORE:4702", "/LTCG" }
		defines { "NDEBUG" }
		warnings "Off"
	filter {}

	filter "configurations:Debug"
		optimize "Debug"
		defines { "DEBUG", "_DEBUG" }
	filter {}

project "shared"
	kind "StaticLib"
	language "C++"

	files { "./source/shared/**.hpp", "./source/shared/**.cpp" }
	includedirs { "./source/shared" }

	dependencies.imports()

project "bgs_emu"
	kind "SharedLib"
	language "C++"
	targetname "bgs_emu"

	pchheader "std_include.hpp"
	pchsource "source/dll/std_include.cpp"

	files {
		"./source/dll/**.h",
		"./source/dll/**.hpp",
		"./source/dll/**.cpp",
	}

	includedirs {
		"./source/dll",
		"./source/shared",
	}

	links { "shared" }

	dependencies.imports()

	if _OPTIONS["copy-to"] then
		postbuildcommands {
			"copy /y \"$(TargetPath)\" \"" .. _OPTIONS["copy-to"] .. "\""
		}
	end

group "Dependencies"
	dependencies.projects()
