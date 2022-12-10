#include "engine/hash_map.h"
#include "engine/engine.h"
#include "engine/plugin.h"
#include "engine/reflection.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "engine/stream.h"
#include "engine/universe.h"
#include "script.h"
#include "../external/kvm.h"

namespace Lumix {

ResourceType ScriptResource::TYPE("script");
static const ComponentType SCRIPT_TYPE = reflection::getComponentType("script");

void ScriptResource::unload() {
	m_bytecode.clear();
	m_variables.clear();
}

ScriptResource::ScriptResource(const Path& path, ResourceManager& resource_manager, IAllocator& allocator)
	: Resource(path, resource_manager, allocator)
	, m_bytecode(allocator)
	, m_variables(allocator)
	, m_allocator(allocator)
{}

ScriptResource::Variable::Variable(IAllocator& allocator)
	: name(allocator)
{}

bool ScriptResource::load(u64 size, const u8* mem) {
	InputMemoryStream blob(mem, size);
	Header header;
	blob.read(header);
	if (header.magic != Header::MAGIC) return false;
	if (header.version > 0) return false;

	blob.read(m_update_label);
	blob.read(m_start_label);
	u32 var_count;
	blob.read(var_count);
	m_variables.reserve(var_count);
	for (u32 i = 0; i < var_count; ++i) {
		Variable& var = m_variables.emplace(m_allocator);
		var.name = blob.readString();
		blob.read(var.type);
	}
	u32 bytecode_size;
	blob.read(bytecode_size);
	m_bytecode.resize(bytecode_size);
	blob.read(m_bytecode.getMutableData(), bytecode_size);
	return true;
}

Script::Script(IAllocator& allocator)
	: m_environment(allocator) 
{}

Script::Script(Script&& script)
	: m_environment(static_cast<OutputMemoryStream&&>(script.m_environment))
{
	m_resource = script.m_resource;
	script.m_resource = nullptr;
}

Script::~Script() {
	if (m_resource) m_resource->decRefCount();
}

struct ScriptSceneImpl : ScriptScene {
	ScriptSceneImpl(IPlugin& plugin, Engine& engine, Universe& universe, IAllocator& allocator)
		: m_plugin(plugin)
		, m_universe(universe)
		, m_engine(engine)
		, m_allocator(allocator)
		, m_scripts(allocator)
	{}

	void serialize(OutputMemoryStream& blob) override {
		blob.write(m_scripts.size());
		for (auto iter = m_scripts.begin(), end = m_scripts.end(); iter != end; ++iter) {
			blob.write(iter.key());
			ScriptResource* res = iter.value().m_resource;
			blob.writeString(res ? res->getPath().c_str() : "");
		}
	}

	void deserialize(InputMemoryStream& blob, const EntityMap& entity_map, i32 version) override {
		u32 count;
		blob.read(count);
		ResourceManagerHub& rm = m_engine.getResourceManager();
		for (u32 i = 0; i < count; ++i) {
			EntityRef e;
			blob.read(e);
			e = entity_map.get(e);
			const char* path = blob.readString();
			Script script(m_allocator);
			script.m_resource = path[0] ? rm.load<ScriptResource>(Path(path)) : nullptr;
			m_scripts.insert(e, static_cast<Script&&>(script));
			m_universe.onComponentCreated(e, SCRIPT_TYPE, this);
		}
	}

	IPlugin& getPlugin() const override { return m_plugin; }
	Universe& getUniverse() override { return m_universe; }
	void clear() override {}

	void stopGame() override {
		m_is_game_running = false;
	}

	void startGame() override {
		m_is_game_running = true;
	}

	void update(float time_delta, bool paused) override {
		if (paused) return;
		if (!m_is_game_running) return;
		
		for (auto iter = m_scripts.begin(), end = m_scripts.end(); iter != end; ++iter) {
			Script& script = iter.value();
			if (!script.m_resource) continue;
			if (!script.m_resource->isReady()) continue;

			auto syscall = [](KVM* vm, kvm_u32 args_count){
				if (args_count == 0) return;

				ScriptSyscalls id = (ScriptSyscalls)kvm_get(vm, -(int)args_count);
				switch (id) {
					case ScriptSyscalls::SET_YAW: {
						Universe* univ = (Universe*)kvm_get_ptr(vm, (kvm_i32)EnvironmentIndices::UNIVERSE);
						EntityRef e = {(i32)kvm_get(vm, -(i32)args_count + 1)};
						float angle = kvm_get_float(vm, -(i32)args_count + 2);
						Quat rot(Vec3(0, 1, 0), angle);
						univ->setRotation(e, rot);
						break;
					}
					case ScriptSyscalls::SET_PROPERTY: {
						ASSERT(args_count == 6);
					
						kvm_u64 prop_hash = kvm_get64(vm, -(i32)args_count + 1);
						auto* prop = (reflection::Property<float>*)reflection::getPropertyFromHash(StableHash::fromU64(prop_hash));
					
						Universe* univ = (Universe*)kvm_get_ptr(vm, (kvm_i32)EnvironmentIndices::UNIVERSE);
						ComponentType cmp_type = {(i32)kvm_get(vm, -(i32)args_count + 3)};
						IScene* scene = univ->getScene(cmp_type);
					
						int entity_index = kvm_get(vm, -(i32)args_count + 4);
						float v = kvm_get_float(vm, -(i32)args_count + 5);
					
						prop->setter(scene, {entity_index}, 0, v);
						break;
					}
				}
			};

			const OutputMemoryStream& bytecode = script.m_resource->m_bytecode;
			bool start = false;
			if (script.m_environment.empty()) {
				script.m_environment.write(iter.key());
				script.m_environment.write(&m_universe);
				script.m_environment.write(u32(0));
				for (const ScriptResource::Variable& var : script.m_resource->m_variables) {
					switch (var.type) {
						case ScriptValueType::ENTITY:
						case ScriptValueType::I32:
						case ScriptValueType::U32:
						case ScriptValueType::FLOAT:
							script.m_environment.write(0.f);
							break;
					}
				}
				start = true;
			}
			KVM vm;
			memcpy(script.m_environment.getMutableData() + 12, &time_delta, sizeof(time_delta));
			kvm_init(&vm, (u32*)script.m_environment.getMutableData(), (u32)script.m_environment.size());
			if (start) {
				kvm_call(&vm, bytecode.data(), syscall, script.m_resource->m_start_label);
			}
			kvm_call(&vm, bytecode.data(), syscall, script.m_resource->m_update_label);
		}
	}

	void destroyScript(EntityRef entity) {
		m_scripts.erase(entity);
		m_universe.onComponentDestroyed(entity, SCRIPT_TYPE, this);
	}

	void createScript(EntityRef entity) {
		Script script(m_allocator);
		m_scripts.insert(entity, static_cast<Script&&>(script));
		m_universe.onComponentCreated(entity, SCRIPT_TYPE, this);
	}

	Script& getScript(EntityRef entity) override {
		return m_scripts[entity];
	}

	void setScriptResource(EntityRef entity, const Path& path) {
		Script& script = m_scripts[entity];
		if (script.m_resource) script.m_resource->decRefCount();
		if (path.isEmpty()) {
			script.m_resource = nullptr;
			return;
		}
		script.m_resource = m_engine.getResourceManager().load<ScriptResource>(path);
	}

	Path getScriptResource(EntityRef entity) {
		Script& script = m_scripts[entity];
		ScriptResource* res = script.m_resource;
		return res ? res->getPath() : Path();
	}

	IAllocator& m_allocator;
	Engine& m_engine;
	IPlugin& m_plugin;
	Universe& m_universe;
	HashMap<EntityRef, Script> m_scripts;
	bool m_is_game_running = false;
};

struct ScriptManager : ResourceManager {
	ScriptManager(IAllocator& allocator)
		: ResourceManager(allocator)
	{}

	Resource* createResource(const Path& path) override {
		return LUMIX_NEW(m_allocator, ScriptResource)(path, *this, m_allocator);
	}

	void destroyResource(Resource& resource) override {
		LUMIX_DELETE(m_allocator, &resource);
	}
};

struct VisualScriptPlugin : IPlugin {
	VisualScriptPlugin(Engine& engine)
		: m_engine(engine)
		, m_allocator(engine.getAllocator())
		, m_script_manager(engine.getAllocator())
	{
		m_script_manager.create(ScriptResource::TYPE, engine.getResourceManager());
	
		LUMIX_SCENE(ScriptSceneImpl, "script")
			.LUMIX_CMP(Script, "script", "Script")
				.LUMIX_PROP(ScriptResource, "Script").resourceAttribute(ScriptResource::TYPE);
	}

	const char* getName() const override { return "script"; }
	u32 getVersion() const override { return 0; }
	void serialize(OutputMemoryStream& serializer) const override {}
	bool deserialize(u32 version, InputMemoryStream& serializer) override { return true; }

	void createScenes(Universe& universe) override {
		UniquePtr<ScriptScene> scene = UniquePtr<ScriptSceneImpl>::create(m_allocator, *this, m_engine, universe, m_allocator);
		universe.addScene(scene.move());
	}

	IAllocator& m_allocator;
	Engine& m_engine;
	ScriptManager m_script_manager;
};

LUMIX_PLUGIN_ENTRY(visualscript) {
	return LUMIX_NEW(engine.getAllocator(), VisualScriptPlugin)(engine);
}

} // namespace Lumix

