#define LUMIX_NO_CUSTOM_CRT
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"
#include "engine/universe.h"
#include "../visual_script.h"

#include "imgui/imgui.h"


using namespace Lumix;

struct VisualScriptEditorPlugin : StudioApp::IPlugin, AssetCompiler::IPlugin, AssetBrowser::IPlugin {
	VisualScriptEditorPlugin (StudioApp& app) : m_app(app) {}

	~VisualScriptEditorPlugin () {
		m_app.getAssetCompiler().removePlugin(*this);
		m_app.getAssetBrowser().removePlugin(*this);
	}

	bool showGizmo(struct UniverseView& view, ComponentUID) override { return false; }

	bool compile(const Path& src) override {
		return false;
	}

	void init() override {
		m_app.getAssetCompiler().registerExtension("lvc", VisualScript::TYPE);
		const char* exts[] = { "lvc", nullptr };
		m_app.getAssetCompiler().addPlugin(*this, exts);
		m_app.getAssetBrowser().addPlugin(*this);
	}

	virtual void onGUI(Span<struct Resource*> resource) override {}
	virtual void onResourceUnloaded(Resource* resource) override {}
	virtual ResourceType getResourceType() const override { return VisualScript::TYPE; }

	const char* getName() const override { return "visualscript"; }

	StudioApp& m_app;
};


LUMIX_STUDIO_ENTRY(visualscript)
{
	IAllocator& allocator = app.getAllocator();
	return LUMIX_NEW(allocator, VisualScriptEditorPlugin )(app);
}
