#define LUMIX_NO_CUSTOM_CRT
#include "engine/engine.h"
#include "engine/plugin.h"
#include "engine/reflection.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "engine/universe.h"
#include "visual_script.h"

namespace Lumix {

ResourceType VisualScript::TYPE = ResourceType("visualscript");

void VisualScript::unload() {
}

bool VisualScript::load(u64 size, const u8* mem) {
	return false;
}

struct VisualScriptManager : ResourceManager {
	VisualScriptManager(IAllocator& allocator)
		: ResourceManager(allocator)
	{}

	Resource* createResource(const Path& path) override {
		return LUMIX_NEW(m_allocator, VisualScript)(path, *this, m_allocator);
	}

	void destroyResource(Resource& resource) override {
		LUMIX_DELETE(m_allocator, &resource);
	}
};

static ComponentType VISUAL_SCRIPT_TYPE = reflection::getComponentType("visualscript");

struct VisualScriptComponent {
	VisualScript* resource = nullptr;
};

struct VisualScriptScene : IScene {
	VisualScriptScene(Engine& engine, IPlugin& plugin, Universe& universe) 
		: m_universe(universe)
		, m_plugin(plugin)
		, m_scripts(engine.getAllocator())
		, m_engine(engine)
	{}

	void serialize(struct OutputMemoryStream& serializer) override {}
	void deserialize(struct InputMemoryStream& serialize, const struct EntityMap& entity_map, i32 version) override {}
	IPlugin& getPlugin() const override { return m_plugin; }
	void update(float time_delta, bool paused) override {}
	struct Universe& getUniverse() override { return m_universe; }
	i32 getVersion() const override { return -1; }
	void clear() override {}

	void createVisualScript(EntityRef entity) {
		m_scripts.insert(entity);
		m_universe.onComponentCreated(entity, VISUAL_SCRIPT_TYPE, this);
	}

	void destroyVisualScript(EntityRef entity) {
		VisualScriptComponent& cmp =  m_scripts[entity];
		if (cmp.resource) cmp.resource->decRefCount();
		m_scripts.erase(entity);
		m_universe.onComponentDestroyed(entity, VISUAL_SCRIPT_TYPE, this);
	}

	Path getScriptPath(EntityRef e) const {
		const VisualScriptComponent& cmp =  m_scripts[e];
		return cmp.resource ? cmp.resource->getPath() : Path();
	}

	void setScriptPath(EntityRef e, const Path& path) {
		VisualScriptComponent& cmp =  m_scripts[e];
		if (cmp.resource) {
			if (cmp.resource->getPath() == path) return;
			cmp.resource->decRefCount();
		}

		cmp.resource = nullptr;
		
		if (!path.isEmpty()) {
			cmp.resource = m_engine.getResourceManager().load<VisualScript>(path);
		}
	}

	HashMap<EntityRef, VisualScriptComponent> m_scripts;
	Universe& m_universe;
	IPlugin& m_plugin;
	Engine& m_engine;
};

struct VisualScriptSystem final : IPlugin {
	VisualScriptSystem(Engine& engine)
		: m_engine(engine)
		, m_resource_manager(engine.getAllocator())
	{
		m_resource_manager.create(VisualScript::TYPE, engine.getResourceManager());
		reflect();
	}

	~VisualScriptSystem() {
		m_resource_manager.destroy();
	}

	void reflect() {
		LUMIX_SCENE(VisualScriptScene, "visualscript")
			.LUMIX_CMP(VisualScript, "visualscript", "Visual script")
				.LUMIX_PROP(ScriptPath, "Path").resourceAttribute(VisualScript::TYPE);
				//.LUMIX_PROP(BoneAttachmentBone, "Bone").attribute<BoneEnum>() 
	}

	void init() override {}
	const char* getName() const override { return "visualscript"; }
	u32 getVersion() const override { return 0; }
	void serialize(OutputMemoryStream& serializer) const override {}
	bool deserialize(u32 version, InputMemoryStream& serializer) override { return true; }
	
	void createScenes(Universe& universe) override {
		IAllocator& allocator = m_engine.getAllocator();
		UniquePtr<VisualScriptScene> scene = UniquePtr<VisualScriptScene>::create(allocator, m_engine, *this, universe);
		universe.addScene(scene.move());
	}

	Engine& m_engine;
	VisualScriptManager m_resource_manager;
};

LUMIX_PLUGIN_ENTRY(visualscript)
{
	return LUMIX_NEW(engine.getAllocator(), VisualScriptSystem)(engine);
}

}

