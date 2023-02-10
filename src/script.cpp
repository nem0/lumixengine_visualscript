#include "engine/hash_map.h"
#include "engine/engine.h"
#include "engine/input_system.h"
#include "engine/log.h"
#include "engine/plugin.h"
#include "engine/reflection.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "engine/stream.h"
#include "engine/universe.h"
#include "script.h"
#include "../external/wasm3.h"

namespace Lumix {

ResourceType ScriptResource::TYPE("script");
static const ComponentType SCRIPT_TYPE = reflection::getComponentType("script");

void ScriptResource::unload() {
	m_bytecode.clear();
}

ScriptResource::ScriptResource(const Path& path, ResourceManager& resource_manager, IAllocator& allocator)
	: Resource(path, resource_manager, allocator)
	, m_bytecode(allocator)
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

	u32 bytecode_size = u32(blob.size() - blob.getPosition());
	m_bytecode.resize(bytecode_size);
	blob.read(m_bytecode.getMutableData(), bytecode_size);
	return true;
}

Script::Script(IAllocator& allocator)
{}

Script::Script(Script&& script)
{
	m_runtime = script.m_runtime;
	m_module = script.m_module;
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
		, m_mouse_move_scripts(allocator)
		, m_key_input_scripts(allocator)
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
		m_mouse_move_scripts.clear();
		m_key_input_scripts.clear();
	}

	void startGame() override {
		m_is_game_running = true;
		m_environment = m3_NewEnvironment();
	}

	void onKeyEvent(const InputSystem::Event& event) {
		for (EntityRef e : m_key_input_scripts) {
			Script& script = m_scripts[e];
			/*KVM vm;
			kvm_init(&vm, (u32*)script.m_environment.getMutableData(), (u32)script.m_environment.size());
			kvm_push(&vm, event.data.button.key_id);
			const OutputMemoryStream& bytecode = script.m_resource->m_bytecode;
			kvm_call(&vm, bytecode.data(), syscall, script.m_resource->m_key_input_label);*/
		}
	}

	void onMouseEvent(const InputSystem::Event& event) {
		for (EntityRef e : m_mouse_move_scripts) {
			Script& script = m_scripts[e];
			/*KVM vm;
			kvm_init(&vm, (u32*)script.m_environment.getMutableData(), (u32)script.m_environment.size());
			kvm_push_float(&vm, event.data.axis.x);
			kvm_push_float(&vm, event.data.axis.y);
			const OutputMemoryStream& bytecode = script.m_resource->m_bytecode;
			kvm_call(&vm, bytecode.data(), syscall, script.m_resource->m_mouse_move_label);*/
		}
	}

	void update(float time_delta, bool paused) override {
		if (paused) return;
		if (!m_is_game_running) return;
		
		InputSystem& input = m_engine.getInputSystem();
		const InputSystem::Event* events = input.getEvents();
		for (u32 i = 0, c = input.getEventsCount(); i < c; ++i) {
			switch(events[i].type) {
				case InputSystem::Event::BUTTON:
					if (events[i].device->type == InputSystem::Device::KEYBOARD) {
						onKeyEvent(events[i]);
					}
					break;
				case InputSystem::Event::AXIS:
					if (events[i].device->type == InputSystem::Device::MOUSE) {
						onMouseEvent(events[i]);
					}
					break;
				default: break;
			}
		}

		for (auto iter = m_scripts.begin(), end = m_scripts.end(); iter != end; ++iter) {
			Script& script = iter.value();
			if (script.m_init_failed) continue;
			if (!script.m_resource) continue;
			if (!script.m_resource->isReady()) continue;

			bool start = false;
			if (!script.m_runtime) {
				script.m_runtime = m3_NewRuntime(m_environment, 32*1024, nullptr);
				const M3Result parse_res = m3_ParseModule(m_environment, &script.m_module, script.m_resource->m_bytecode.data(), (u32)script.m_resource->m_bytecode.size());
				if (parse_res != m3Err_none) {
					logError(script.m_resource->getPath(), ": ", parse_res);
					// TODO reset m_init_failed if resource is reloaded
					script.m_init_failed = true;
					m3_FreeRuntime(script.m_runtime);
					script.m_runtime = nullptr;
					continue;
				}
				const M3Result load_res = m3_LoadModule(script.m_runtime, script.m_module);
				if (load_res != m3Err_none) {
					logError(script.m_resource->getPath(), ": ", load_res);
					script.m_init_failed = true;
					m3_FreeRuntime(script.m_runtime);
					script.m_runtime = nullptr;
					continue;
				}

#if 0
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
				if (script.m_resource->m_mouse_move_label != KVM_INVALID_LABEL) {
					m_mouse_move_scripts.push(iter.key());
				}
				if (script.m_resource->m_key_input_label != KVM_INVALID_LABEL) {
					m_key_input_scripts.push(iter.key());
				}
#endif
			}

			IM3Function update_fn;
			M3Result find_update_res = m3_FindFunction(&update_fn, script.m_runtime, "update");
			if (find_update_res == m3Err_none) {
				m3_CallV(update_fn, time_delta);
			}
			else if (find_update_res != m3Err_functionLookupFailed) {
				logError(script.m_resource->getPath(), ": ", find_update_res);
				script.m_init_failed = true;
			}
		}
	}

	void destroyScript(EntityRef entity) {
		m_mouse_move_scripts.eraseItem(entity);
		m_key_input_scripts.eraseItem(entity);
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
	Array<EntityRef> m_mouse_move_scripts;
	Array<EntityRef> m_key_input_scripts;
	bool m_is_game_running = false;
	IM3Environment m_environment = nullptr;
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
		OutputMemoryStream wasm_bin(engine.getAllocator());
		(void)engine.getFileSystem().getContentSync(Path("test.wasm"), wasm_bin);

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

